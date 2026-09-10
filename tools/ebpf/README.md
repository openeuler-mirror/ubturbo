# memdiag — HugePage 内存异常诊断工具

基于 eBPF（BCC）的 HugePage 内存诊断工具。通过在 hugetlb 子系统统一入口
`alloc_hugetlb_folio` / `free_huge_folio` 上挂载 kprobe/kretprobe，实时采集每一笔
大页的申请与释放，按地址维护 outstanding 台账，周期性分析可疑进程并抓取现场快照，
可选调用大模型生成自然语言诊断报告。

---

## 一、工作原理

### 1.1 采集层（collector.py + bpf_program.py）

BPF 程序挂载两个探针：

| 探针类型 | 内核函数 | 触发时机 | action | 采集内容 |
|---|---|---|---|---|
| kretprobe | `alloc_hugetlb_folio` | 大页分配**返回** | 1 | 返回值(地址) + 用户栈/内核栈 ID |
| kprobe | `free_huge_folio` | 大页释放**入口** | 2 | 参数1(地址) |

- **分配事件过滤**：取返回值 `PT_REGS_RC(ctx)` 作为地址；当 `addr == 0`（NULL）或
  `addr >= (u64)-4095`（ERR_PTR(-1..-4095) 区间，如 `ERR_PTR(-ENOMEM)`）时直接 `return 0`，
  失败分配不进入统计。
- **调用栈**：`BPF_STACK_TRACE(stack_traces, 4096)` 容量 4096 份，分配时同时抓
  `BPF_F_USER_STACK`（用户栈）与 `0`（内核栈）两个 stackid。释放事件不抓栈。
- **大页 size**：BPF 中硬编码为 `2 * 1024 * 1024`（2MB）。
- 事件经 `BPF_PERF_OUTPUT(events)` perf buffer 送到用户态。

### 1.2 用户态处理（HugePageCollector）

`handle_event(cpu, data, size)`：
- `action == 1`（分配）：以地址字符串为 key 写入台账，记录
  `pid / comm / addr / size / ts_ns / time`，并**即时符号化**用户栈与内核栈。
- `action == 2`（释放）：按地址 key 从台账 `pop` 删除。

主循环 `run()` 中三个周期任务（周期均可由环境变量覆盖）：

| 任务 | 环境变量 | 默认 | 说明 |
|---|---|---|---|
| session 落盘 | `MEMDIAG_FLUSH_INTERVAL` | 1s | 原子写 `runtime/session.json`（tempfile + `os.replace`，读者看不到半截 JSON） |
| 分析 + 抓快照 | `MEMDIAG_ANALYZE_INTERVAL` | 5s | 调 `analyze()`，对可疑进程抓快照 |
| 清栈表 | `MEMDIAG_STACK_CLEAR_INTERVAL` | 3600s | 清空 BPF `stack_traces` Map（不清会导致 `get_stackid` 失败） |

主循环每次 `perf_buffer_poll(timeout=1000)` 后检查这三个周期。SIGTERM 由
`_handle_sigterm` 置 `_stop` 标志，循环退出前再 `save_session()` 兜底，保证最后状态不丢。

### 1.3 分析层（analyzer.py）

`analyze()` 读 `runtime/session.json`，按 PID 聚合每个进程的
`count / bytes / oldest_age / allocations[]`，并对 `allocations` 按 `age_sec` 降序排序。

**可疑判定**（满足任一即判可疑）：
- `count >= MEMDIAG_SUSPICIOUS_COUNT`（默认 **5**）
- `oldest_age >= MEMDIAG_SUSPICIOUS_AGE`（默认 **60** 秒）

`reason` 文案形如 `holds 6 HugePages; oldest allocation age is 75s`。
`load_session()` 对坏 JSON / IO 错误统一降级为空会话，保证 `report` 不中断。

### 1.4 快照层（snapshot.py）

对每个可疑进程抓取现场快照，保存到 `snapshots/hugepage_pid_<pid>_<timestamp>.json`：
- **去重**：同 PID 只抓一次，索引存 `runtime/snapshot_index.json`。
- **分配明细**：最多 `MAX_SNAPSHOT_ALLOCATIONS = 20` 条，超出标记 `truncated: true`。
- **系统现场**：`/proc/meminfo`、`/proc/vmstat`、`/proc/sys/vm/nr_hugepages`。
- **进程现场**：`/proc/<pid>/status`、`cmdline`、`maps`、`smaps_rollup`。
- **cmdline 脱敏**：`--password=xxx` / `--password xxx` 形态的疑似密钥参数值替换为 `***`，
  由 `MEMDIAG_SANITIZE`（默认 `1` 开启）控制；短选项 `-p xxx` 无法判断语义，保守不处理。

### 1.5 栈符号化（symbols.py）

- **内核栈**：启动时一次性加载 `/proc/kallsyms`（仅保留 `T/t/W/w` 代码段符号），
  之后每次查询走二分查找（`bisect`），输出 `symbol+0x偏移`。非 root 时 kallsyms 地址全 0，
  回退原地址。
- **用户栈**：每次**实时读** `/proc/<pid>/maps`（进程存活期间映射会变，且 report 时进程可能已退出），
  输出 `文件名+0x偏移`，查不到回退原地址。
- 解析失败一律回退原始地址，不影响采集主流程。

### 1.6 报告层（reporter.py）

`print_report()`：
1. 调 `analyze()`，打印总量摘要、Top 进程列表、可疑进程告警。
2. 对每个可疑进程，从 `snapshots/` 找对应快照，`build_prompt()` 构造诊断 prompt
   （含进程信息、`/proc/<pid>/maps` 中 hugetlb 相关行、系统内存、调用栈等）。
3. `call_llm()` 调用大模型 API：用系统 `curl`，`-k` 跳过 SSL 校验，
   `--proxy` 取自 `https_proxy`/`http_proxy` 环境变量，请求体含 `model / messages / stream=False`。
   **未配置 `API_KEY` 时跳过 AI 诊断**，其余报告功能不受影响。
4. 解析模型返回的 JSON（`_extract_json` 自动剥离 ` ```json ` 围栏），
   打印根因 / 影响 / 建议 / 修复步骤 / 严重程度，并把 `ai_diagnosis`（含 timestamp、model）写回快照文件。
5. `save_report()` 保存结构化报告到 `runtime/diagnosis_report.json`。

### 1.7 整体数据流

```
内核 alloc_hugetlb_folio / free_huge_folio
        │  (kretprobe / kprobe)
        ▼
 BPF 程序 (bpf_program.py) ──perf buffer──► HugePageCollector.handle_event
                                              │
                    ┌─────────────────────────┼──────────────────────────┐
                    ▼                          ▼                          ▼
              session.json            analyze() 周期分析            stack_traces 清理
            (1s 原子落盘)                   │                      (3600s)
                                     可疑进程抓快照
                                              ▼
                                   snapshots/*.json
                                              │
                          report 命令 ───────►│
                                              ▼
                          build_prompt → call_llm → diagnosis_report.json
```

---

## 二、目录结构

```
tools/ebpf/
├── memdiag.py            # 入口，转发到 cli.main()
├── cli.py                # argparse：start / stop / status / report
├── daemon.py             # 守护进程管理（PID 文件、防误杀、优雅停止）
├── collector.py           # BPF 采集器主循环
├── bpf_program.py         # BPF C 程序（探针与事件结构定义）
├── analyzer.py            # session 分析与可疑判定
├── snapshot.py            # 现场快照采集与 cmdline 脱敏
├── symbols.py             # 内核 / 用户栈符号化
├── reporter.py            # 报告生成与 AI 诊断
├── config.example.py      # 配置模板（复制为 config.py）
├── config.py              # 本地配置（含 API Key，已被 .gitignore 排除）
├── runtime/               # 运行时产物（已 gitignore）
│   ├── session.json            # 实时分配台账
│   ├── memdiag.pid             # 守护进程 PID
│   ├── snapshot_index.json     # 快照去重索引
│   └── diagnosis_report.json   # 结构化报告
├── logs/memdiag.log      # 守护进程日志（已 gitignore）
├── snapshots/            # 可疑进程现场快照（已 gitignore）
└── README.md             # 本文档
```

---

## 三、依赖与前置条件

- **OS**：Linux，内核需存在 `alloc_hugetlb_folio` 与 `free_huge_folio` 符号。
  确认：`grep -E 'alloc_hugetlb_folio|free_huge_folio' /proc/kallsyms`
- **权限**：root（BPF、kallsyms、`/proc/<pid>/*` 读取均需 root）。
- **Python 依赖**：`bpfcc`（BCC 的 Python 绑定）。
- **大页池**：`echo 20 > /proc/sys/vm/nr_hugepages` 预留大页。
- **AI 诊断（可选）**：系统 `curl`；`config.py` 中填入 `API_URL` / `API_KEY` / `MODEL_NAME`（Bearer 经 stdin 注入，不进命令行/ps）。

---

## 四、快速开始

```bash
# 1. 预留大页
sudo sh -c 'echo 20 > /proc/sys/vm/nr_hugepages'

# 2. 配置 AI 诊断（可选）
cp config.example.py config.py
#   编辑 config.py，填入 API_URL / API_KEY / MODEL_NAME

# 3. 启动监控（后台守护进程）
sudo python3 memdiag.py start

# 4. 另开终端运行任意使用大页的程序触发分配
#    （mmap MAP_HUGETLB / shmat SHM_HUGETLB / hugetlbfs 文件 mmap 等）

# 5. 查看报告（含可疑进程与 AI 诊断）
sudo python3 memdiag.py report

# 6. 查看状态 / 停止监控
sudo python3 memdiag.py status
sudo python3 memdiag.py stop
```

---

## 五、命令详解

入口 `memdiag.py` → `cli.py`，四个子命令：

| 命令 | 作用 |
|---|---|
| `start` | 启动采集守护进程 |
| `stop` | 停止守护进程 |
| `status` | 查看运行状态 |
| `report` | 生成并打印诊断报告 |

### start（daemon.py: start_daemon）
- 创建 `runtime/` 与 `logs/` 目录。
- 若 PID 文件存在且进程存活 → 提示 already running；若为陈旧 PID 文件 → 自动清理后继续。
- 用 `subprocess.Popen` + `start_new_session=True` 拉起一个独立的 collector 进程
  （`from collector import HugePageCollector; HugePageCollector().run()`），
  `cwd` 锚定到工具目录，stdout/stderr 重定向到 `logs/memdiag.log`。
- PID 写入 `runtime/memdiag.pid`。

### stop（daemon.py: stop_daemon）
- 读 PID；无 PID 文件 → 提示 not running；进程已死 → 清理 PID 文件。
- **防误杀**：`_is_memdiag_process` 校验 `/proc/<pid>/cmdline` 是否同时含
  `HugePageCollector` 与 `sys.executable`，不匹配则拒绝 kill，提示手动处理
  （防 PID 复用误杀无关进程）。
- 发 `SIGTERM`，等待 `STOP_WAIT_SEC = 2.0` 秒（覆盖 perf_buffer_poll 1000ms 返回边界），
  最后清理 PID 文件。

### status（daemon.py: show_status）
- 读 PID，用 `os.kill(pid, 0)` 探活，报告 running / not running / PID 文件残留但进程已死。

### report（reporter.py: print_report）
- 生成总量摘要、Top 进程、可疑进程告警；对可疑进程调 AI 诊断并打印；保存
  `runtime/diagnosis_report.json`。

---

## 六、配置

### 6.1 config.py（从 config.example.py 复制）

| 变量 | 说明 | 模板默认值 |
|---|---|---|
| `API_URL` | LLM 接口地址（OpenAI 兼容 `chat/completions`），换接口改这一行即可 | `https://computing.huawei.com/lingshu/api/v1/chat/completions` |
| `API_KEY` | 接口密钥；为空则跳过 AI 诊断 | `""` |
| `MODEL_NAME` | 模型名 | `GLM-5.2-w4a8`（模板） |

> `config.py` 不存在时，`reporter.py` 兜底默认：
> `API_URL` 同上、`API_KEY=""`、`MODEL_NAME="GLM-5.2-w4a8"`，AI 诊断被跳过。
> `config.py` 含敏感信息，已被 `.gitignore` 排除，请勿提交。

### 6.2 环境变量

| 环境变量 | 作用 | 默认 |
|---|---|---|
| `MEMDIAG_ANALYZE_INTERVAL` | 分析/快照周期（秒） | 5 |
| `MEMDIAG_FLUSH_INTERVAL` | session.json 落盘间隔（秒） | 1 |
| `MEMDIAG_STACK_CLEAR_INTERVAL` | BPF 栈表清理周期（秒） | 3600 |
| `MEMDIAG_SUSPICIOUS_COUNT` | 持有未释放大页数阈值 | 5 |
| `MEMDIAG_SUSPICIOUS_AGE` | 最早分配持有秒数阈值 | 60 |
| `MEMDIAG_SANITIZE` | cmdline 脱敏开关（1 开 / 0 关） | 1 |
| `http_proxy` / `https_proxy` | AI 调用走的代理 | 空 |
| `MEMDIAG_VERIFY_TLS` | AI 调用 TLS 校验开关（1 校验 / 0 放行，仅限 TLS 拦截代理环境） | 1 |

---

## 七、产物文件

| 文件 | 写入方 | 内容 |
|---|---|---|
| `runtime/session.json` | collector | `{updated_at, allocations:{addr:{pid,comm,addr,size,ts_ns,time,user_stack_id,kernel_stack_id,user_stack,kernel_stack}}}` |
| `runtime/memdiag.pid` | daemon | 采集进程 PID |
| `runtime/snapshot_index.json` | snapshot | `{pid:{path,created_at,reason}}` 去重索引 |
| `snapshots/hugepage_pid_<pid>_<ts>.json` | snapshot | 进程/系统现场 + 分配明细（≤20 条）|
| `runtime/diagnosis_report.json` | reporter | `{report_time, summary, top_processes, suspicious_processes, ai_diagnoses}` |
| `logs/memdiag.log` | daemon | 守护进程 stdout/stderr |

快照文件在 `report` 时会被回写 `ai_diagnosis`（`timestamp / model / result`）。

---

## 八、Demo 程序

场景验证 demo（正常配对 / 泄漏 / OOM / SHM_HUGETLB / hugetlbfs 文件 mmap / fork 继承 / THP 盲区反向验证）
**不随仓库发布**，需要时单独提供。验证时可用任意使用显式 hugetlb 大页的程序
（`mmap MAP_HUGETLB`、`shmat SHM_HUGETLB`、hugetlbfs 文件 `mmap` 等）触发采集。

---

## 九、故障排查

| 现象 | 排查 |
|---|---|
| `start` 后 `status` 显示 not running | 看 `logs/memdiag.log`；常见为 BPF 加载失败、内核无 `alloc_hugetlb_folio` 符号、非 root |
| `report` 显示 0 allocations | 确认被监控程序确实在用显式 hugetlb 大页；THP 透明大页不经 `alloc_hugetlb_folio`，不在本工具覆盖范围 |
| `report` 无 AI 诊断段 | `config.py` 未填 `API_KEY`（空则跳过）；或 `API_URL`/网络/代理不通 |
| `session.json` 不更新 | 守护进程是否在跑（`status`）；确认 PID 进程存活 |
| 快照不更新 | 同 PID 已有快照则不再重复抓（去重索引 `runtime/snapshot_index.json`），删除该文件可重抓 |
| `get_stackid` 失败、新分配无栈 | 栈表满 4096，等 `STACK_CLEAR_INTERVAL` 自动清，或重启 |
| 大页申请失败 ENOMEM | `cat /proc/meminfo | grep HugePages_Free`，不足则 `echo N > /proc/sys/vm/nr_hugepages` |
