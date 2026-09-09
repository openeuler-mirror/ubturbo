# -*- coding: utf-8 -*-
"""
reporter.py - HugePage 内存异常诊断报告生成器
"""

import json
import os
import subprocess
import time

from analyzer import analyze
from snapshot import save_snapshot

# ============================================================
# 【配置区】配置从 config.py 读取（含 API Key，勿提交版本库）
# 首次部署：cp config.example.py config.py 并填写 API_KEY
# ============================================================
try:
    from config import API_KEY, API_URL, MODEL_NAME
except ImportError:
    # config.py 不存在时降级：AI 诊断跳过，其余报告功能不受影响
    API_KEY = ""
    API_URL = "https://computing.huawei.com/lingshu/api/v1/chat/completions"
    MODEL_NAME = "GLM-5.2-w4a8"

# 代理配置（从环境变量读取）
HTTP_PROXY = os.environ.get("http_proxy", "")
HTTPS_PROXY = os.environ.get("https_proxy", "")
# TLS 校验：默认校验；MEMDIAG_VERIFY_TLS=0 仅限 TLS 拦截(MITM)代理环境加 -k 放行
VERIFY_TLS = os.environ.get("MEMDIAG_VERIFY_TLS", "1") != "0"
# ============================================================


TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
REPORT_FILE = os.path.join(TOOLS_DIR, "runtime", "diagnosis_report.json")


# ============================================================
# 辅助函数
# ============================================================
def mb(value):
    return value / 1024 / 1024


def find_snapshots():
    """查找所有快照文件"""
    snapshot_dir = os.path.join(TOOLS_DIR, "snapshots")
    if not os.path.exists(snapshot_dir):
        return []

    snapshots = []
    for filename in os.listdir(snapshot_dir):
        if filename.startswith("hugepage_pid_") and filename.endswith(".json"):
            snapshots.append(os.path.join(snapshot_dir, filename))

    snapshots.sort(key=lambda x: os.path.getmtime(x), reverse=True)
    return snapshots


# ============================================================
# LLM API 调用（支持代理）
# ============================================================

def _extract_json(text):
    """剥离模型输出中可能存在的 markdown 代码围栏（```json ... ```）"""
    text = text.strip()
    if text.startswith("```"):
        text = text.strip("`")
        if text.startswith("json"):
            text = text[4:]
        text = text.strip()
    return text


def call_llm(prompt):
    """调用 LLM API（curl；Bearer 经 --config stdin 注入，不进命令行/ps）"""
    if not API_KEY:
        print("[AI] 未配置 API Key（config.py），跳过 AI 诊断")
        return None

    data = json.dumps({
        "model": MODEL_NAME,
        "messages": [{"role": "user", "content": prompt}],
        "stream": False,
    })

    # Authorization 经 stdin(--config -)注入，不进进程参数，防 ps 肩窥
    auth_config = f'header = "Authorization: Bearer {API_KEY}"\n'

    cmd = ["curl", "-s", "-S"]
    if not VERIFY_TLS:
        cmd.append("-k")  # MITM 代理环境放行 TLS（MEMDIAG_VERIFY_TLS=0）
    proxy = HTTPS_PROXY or HTTP_PROXY or ""
    if proxy:
        cmd += ["--proxy", proxy]
    cmd += ["-X", "POST",
            "-H", "Content-Type: application/json",
            "-d", data,
            "--config", "-",
            API_URL]

    try:
        result = subprocess.run(
            cmd, input=auth_config, capture_output=True, text=True, timeout=600)
        response = json.loads(result.stdout)
        if "choices" in response:
            return response["choices"][0]["message"]["content"]
    except Exception as e:
        print(f"[AI] 调用失败: {e}")
    return None


def _extract_field(text, field):
    """从 /proc 文本中提取指定字段值，如 VmRSS: 1234 kB"""
    for line in text.splitlines():
        if line.startswith(field):
            return line.strip()
    return None


def build_prompt(snapshot):
    """构建诊断 prompt"""
    p = snapshot.get("process", {})
    s = snapshot.get("system", {})
    details = snapshot.get("allocation_details", {}).get("items", [])
    proc_detail = snapshot.get("process_detail", {})

    # --- 栈信息（符号化后：形如 "symbol+0x12" / "libc.so.6+0x1a2b3c"）---
    stack_info = ""
    for i, d in enumerate(details[:3]):
        if d.get("user_stack"):
            stack_info += f"\n分配{i + 1}用户栈: {' <- '.join(d['user_stack'][:5])}"
        if d.get("kernel_stack"):
            stack_info += f"\n分配{i + 1}内核栈: {' <- '.join(d['kernel_stack'][:5])}"

    # --- 完整命令行（比 comm 更能区分进程身份）---
    cmdline = proc_detail.get("cmdline", "").strip()[:200]

    # --- maps 只保留 hugetlb 相关行（全量几百行会撑爆 token）---
    huge_maps = [
        line.strip()
        for line in proc_detail.get("maps", "").splitlines()
        if "hugetlb" in line.lower()
    ][:10]
    huge_maps_info = "\n".join(huge_maps) if huge_maps else "无 hugetlb 映射"

    # --- status 摘要（只取关键字段）---
    status_text = proc_detail.get("status", "")
    status_fields = [
        f for f in (
            _extract_field(status_text, "VmRSS"),
            _extract_field(status_text, "VmHWM"),
            _extract_field(status_text, "Threads"),
            _extract_field(status_text, "VmData"),
        ) if f
    ]
    status_info = "; ".join(status_fields) if status_fields else "未知"

    # --- smaps_rollup 关键字段 ---
    smaps_text = proc_detail.get("smaps_rollup", "")
    smaps_fields = [
        f for f in (
            _extract_field(smaps_text, "Rss"),
            _extract_field(smaps_text, "Anonymous"),
        ) if f
    ]
    smaps_info = "; ".join(smaps_fields) if smaps_fields else "未知"

    return f"""你是内存诊断专家，分析 HugePage 内存异常问题。

## 进程信息
- PID: {p.get('pid', '未知')}
- 进程名: {p.get('comm', '未知')}
- 命令行: {cmdline if cmdline else '未知'}
- 持有大页: {p.get('hugepage_count', 0)} 个
- 占用内存: {p.get('hugepage_bytes', 0) / 1024 / 1024:.1f} MB
- 最老分配: {p.get('oldest_age_sec', 0)} 秒
- 进程内存摘要: {status_info}
- 进程 RSS 摘要: {smaps_info}
- 异常原因: {snapshot.get('reason', '未知')}

## 大页映射（/proc/<pid>/maps 中 hugetlb 相关行）
{huge_maps_info}

## 系统内存
{s.get('meminfo', '未知')}

## 大页配置
{s.get('hugepages_nr', '未知')}

## 调用栈（已符号化为 模块/函数+偏移）
{stack_info if stack_info else '无'}

请输出 JSON 格式诊断结果：
{{
    "root_cause": "根因分析",
    "impact_assessment": "影响评估",
    "suggestions": ["建议1", "建议2"],
    "fix_steps": ["步骤1", "步骤2"],
    "severity": "high/medium/low"
}}
"""


# ============================================================
# 主函数
# ============================================================

def print_report():
    """生成并打印报告"""
    result = analyze()
    report_time = time.strftime("%Y-%m-%d %H:%M:%S")

    report_data = {
        "report_time": report_time,
        "summary": {
            "total_allocations": result["total_allocations"],
            "total_bytes_mb": round(result["total_bytes"] / 1024 / 1024, 2),
            "total_processes": len(result["by_pid"]),
            "suspicious_count": len(result["suspicious"]),
        },
        "top_processes": result["by_pid"],
        "suspicious_processes": result["suspicious"],
        "ai_diagnoses": []
    }

    print()
    print("HugePage Usage Report")
    print("=====================")
    print(f"Current Allocations: {result['total_allocations']}")
    print(f"Current Size       : {mb(result['total_bytes']):.1f} MB")
    print()

    if not result["by_pid"]:
        print("No outstanding HugePage allocations recorded.")
        save_report(report_data)
        return

    print("Top PIDs:")
    for proc in result["by_pid"]:
        print(f"PID {proc['pid']} {proc['comm']}: {proc['count']} allocations, "
              f"{mb(proc['bytes']):.1f} MB, oldest {int(proc['oldest_age'])}s")

    print()

    if result["suspicious"]:
        print("Suspicious Processes:")
        for proc in result["suspicious"]:
            print(f"[WARN] PID {proc['pid']} {proc['comm']} - {proc['reason']}")

        print()
        print("=" * 70)
        print("                    AI 诊断结果")
        print("=" * 70)

        suspicious_pids = {proc["pid"] for proc in result["suspicious"]}
        snapshots = find_snapshots()

        for snapshot_path in snapshots:
            try:
                with open(snapshot_path, "r", encoding="utf-8") as f:
                    snapshot = json.load(f)

                pid = snapshot.get("process", {}).get("pid")
                if pid not in suspicious_pids:
                    continue

                print(f"\n正在诊断: {os.path.basename(snapshot_path)}")

                prompt = build_prompt(snapshot)
                diagnosis = call_llm(prompt)

                diag_result = {
                    "snapshot_path": snapshot_path,
                    "process": snapshot.get("process", {}),
                    "reason": snapshot.get("reason", ""),
                    "diagnosis": diagnosis
                }
                report_data["ai_diagnoses"].append(diag_result)

                if diagnosis:
                    print(f"  PID {pid} ({snapshot['process'].get('comm', '未知')})")

                    try:
                        diag_json = json.loads(_extract_json(diagnosis))
                        print(f"  严重程度: {diag_json.get('severity', '未知')}")
                        print(f"  根因: {diag_json.get('root_cause', '无')}")
                        print(f"  建议:")
                        for i, s in enumerate(diag_json.get('suggestions', []), 1):
                            print(f"    {i}. {s}")
                        print(f"  修复步骤:")
                        for i, step in enumerate(diag_json.get('fix_steps', []), 1):
                            print(f"    {i}. {step}")

                        snapshot["ai_diagnosis"] = {
                            "timestamp": report_time,
                            "model": MODEL_NAME,
                            "result": diag_json
                        }
                        with open(snapshot_path, "w", encoding="utf-8") as f:
                            json.dump(snapshot, f, indent=2, ensure_ascii=False)

                    except json.JSONDecodeError:
                        print(f"  诊断结果: {diagnosis}")
                else:
                    print("  诊断失败")

                time.sleep(0.5)

            except Exception as e:
                print(f"  处理失败: {e}")

        print()
        print("=" * 70)
    else:
        print("Suspicious Processes: none")

    save_report(report_data)


def save_report(report_data):
    """保存结构化报告"""
    os.makedirs("runtime", exist_ok=True)
    with open(REPORT_FILE, "w", encoding="utf-8") as f:
        json.dump(report_data, f, indent=2, ensure_ascii=False)
    print(f"\n报告已保存: {REPORT_FILE}")


if __name__ == "__main__":
    print_report()