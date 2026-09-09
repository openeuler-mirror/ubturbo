### 负责采集：谁申请了大页、地址是多少、时间是多少
### 收到 event -> 维护按地址的 outstanding 台账 -> 周期分析 + 快照

import ctypes
import json
import os
import signal
import tempfile
import time
from bpfcc import BPF

from bpf_program import BPF_PROGRAM
from analyzer import analyze
from snapshot import save_snapshot, MAX_SNAPSHOT_ALLOCATIONS
from symbols import KernelSymbols, resolve_kernel_stack, resolve_user_stack

# 路径锚定到本文件所在目录，避免从其它 CWD 运行时产物写到错误位置
TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
SESSION_FILE = os.path.join(TOOLS_DIR, "runtime", "session.json")

# ============================================================
# 【运行参数】可通过环境变量覆盖：
#   MEMDIAG_ANALYZE_INTERVAL   分析/快照周期，秒（默认 5）
#   MEMDIAG_FLUSH_INTERVAL     session.json 落盘间隔，秒（默认 1）
#   MEMDIAG_STACK_CLEAR_INTERVAL 栈表清理周期，秒（默认 3600）
# ============================================================
ANALYZE_INTERVAL = float(os.environ.get("MEMDIAG_ANALYZE_INTERVAL", "5"))
FLUSH_INTERVAL = float(os.environ.get("MEMDIAG_FLUSH_INTERVAL", "1"))
STACK_CLEAR_INTERVAL = float(os.environ.get("MEMDIAG_STACK_CLEAR_INTERVAL", "3600"))


def _read_hugepage_size():
    """从 /proc/meminfo 读取真实大页大小（字节），读不到按 2MB 兜底。

    BPF 侧 kretprobe 拿不到 folio 的 size，event.size 为 2MB 占位值；
    在用户态按系统真实 Hugepagesize 覆盖，避免 512MB/1GB 大页环境下
    字节数统计失真（同 PR 的 demo 已动态适配，采集侧需一致）。
    """
    try:
        with open("/proc/meminfo", "r", errors="replace") as f:
            for line in f:
                if line.startswith("Hugepagesize:"):
                    return int(line.split()[1]) * 1024
    except OSError:
        pass
    return 2 * 1024 * 1024


class Event(ctypes.Structure):
    _fields_ = [
        ("pid", ctypes.c_uint),
        ("ts_ns", ctypes.c_ulonglong),
        ("addr", ctypes.c_ulonglong),
        ("size", ctypes.c_ulonglong),
        ("action", ctypes.c_uint),

        # 对应 BPF 中的 int
        ("user_stack_id", ctypes.c_int),
        ("kernel_stack_id", ctypes.c_int),

        ("comm", ctypes.c_char * 16),
    ]


class HugePageCollector:
    def __init__(self):
        self.bpf = None
        self.allocations = {}
        self._stop = False
        self._dirty = False
        self.kernel_symbols = KernelSymbols()
        self.hugepage_size = _read_hugepage_size()

    def load(self):
        self.bpf = BPF(text=BPF_PROGRAM)

        # 内核符号表加载一次，后续内核栈符号化全用二分查找
        symbol_count = self.kernel_symbols.load()
        print(f"Loaded {symbol_count} kernel symbols from kallsyms.", flush=True)
        print(f"HugePageSize={self.hugepage_size // 1024}KB", flush=True)

        self.bpf.attach_kretprobe(
            event="alloc_hugetlb_folio",
            fn_name="trace_alloc_return"
        )

        self.bpf.attach_kprobe(
            event="free_huge_folio",
            fn_name="trace_free_entry"
        )

        self.bpf["events"].open_perf_buffer(self.handle_event)

    def handle_event(self, cpu, data, size):
        event = ctypes.cast(data, ctypes.POINTER(Event)).contents

        comm = event.comm.decode(
            "utf-8",
            errors="replace",
        ).rstrip("\x00")

        key = str(event.addr)

        if event.action == 1:
            # 热路径只做 BPF Map walk 取栈地址（无 /proc IO），
            # 符号化延迟到 check_and_snapshot 的快照阶段，避免高频分配时
            # perf buffer 溢出丢事件导致台账漂移
            self.allocations[key] = {
                "pid": int(event.pid),
                "comm": comm,
                "addr": hex(event.addr),
                "size": self.hugepage_size,
                "ts_ns": int(event.ts_ns),
                "time": time.time(),

                # 同时保留 ID 和实际地址列表
                "user_stack_id": int(event.user_stack_id),
                "kernel_stack_id": int(event.kernel_stack_id),

                "user_stack": self.read_stack_addresses(event.user_stack_id),
                "kernel_stack": self.read_stack_addresses(event.kernel_stack_id),
            }

        elif event.action == 2:
            # perf buffer 跨 CPU 无序：地址复用 + 乱序时旧 free 可能迟到，
            # 直接按地址盲删会误删新分配的记录；先校验归属再删
            existing = self.allocations.get(key)
            if existing is None:
                pass  # 迟到的 free（记录已删/未见过），忽略
            elif (existing["pid"] == int(event.pid)
                  or existing["comm"] == comm
                  or comm.startswith(existing["comm"])):
                # 同 pid，或 fork 子进程（comm 继承）释放父进程映射
                self.allocations.pop(key, None)
            else:
                # 地址已被新分配复用而旧 free 迟到：保留新记录，避免误删
                print(f"[warn] stale free for addr {key}: "
                      f"ledger pid={existing['pid']}, free pid={event.pid}", flush=True)

        self._dirty = True

    def save_session(self):
        """原子写 session.json：先写临时文件再 os.replace，读者永远看不到半截 JSON"""
        runtime_dir = os.path.join(TOOLS_DIR, "runtime")
        os.makedirs(runtime_dir, exist_ok=True)

        data = {
            "updated_at": time.time(),
            "allocations": self.allocations,
        }

        fd, tmp_path = tempfile.mkstemp(
            dir=runtime_dir,
            prefix=".session_",
            suffix=".tmp",
        )

        try:
            with os.fdopen(fd, "w", encoding="utf-8") as f:
                json.dump(data, f, indent=2)

            os.replace(tmp_path, SESSION_FILE)

        except Exception:
            if os.path.exists(tmp_path):
                os.unlink(tmp_path)
            raise

    def check_and_snapshot(self):
        result = analyze()

        # 快照前统一符号化（每周期仅对可疑进程、至多 MAX_SNAPSHOT_ALLOCATIONS 条，
        # /proc IO 开销可控）。栈地址已在事件热路径取出，不会因 stack_traces
        # 滚动/清理而丢失。
        for proc in result.get("suspicious", []):
            for allocation in proc.get("allocations", [])[:MAX_SNAPSHOT_ALLOCATIONS]:
                if isinstance(allocation.get("user_stack"), list):
                    allocation["user_stack"] = resolve_user_stack(
                        allocation["user_stack"], proc["pid"])
                    allocation["kernel_stack"] = resolve_kernel_stack(
                        allocation["kernel_stack"], self.kernel_symbols)
            save_snapshot(proc, result)

    def read_stack_addresses(self, stack_id):
        """
        从 BPF stack_traces Map 中读取调用栈地址。

        返回示例：
        [
            "0xffff800080123456",
            "0xffff800080234567"
        ]

        如果 stack_id 为负数，说明采集失败。
        """
        if stack_id < 0:
            return []

        try:
            stack_table = self.bpf["stack_traces"]

            return [
                f"0x{address:x}"
                for address in stack_table.walk(stack_id)
            ]

        except Exception as exc:
            self.log_stack_error(stack_id, exc)
            return []

    def log_stack_error(self, stack_id, exc):
        print(
            f"Failed to read stack ID {stack_id}: {exc}",
            flush=True,
        )

    def run(self):
        signal.signal(signal.SIGTERM, self._handle_sigterm)
        signal.signal(signal.SIGINT, self._handle_sigterm)

        self.load()
        self.save_session()

        last_check = 0.0
        last_flush = 0.0
        last_stack_clear = time.time()

        while not self._stop:
            self.bpf.perf_buffer_poll(timeout=1000)

            if self._stop:
                break

            now = time.time()

            # session.json 节流落盘（原子写，仅在有事件变更时写，避免空转写盘）
            if now - last_flush >= FLUSH_INTERVAL and self._dirty:
                self.save_session()
                last_flush = now
                self._dirty = False

            # 周期分析 + 对可疑进程抓快照（含延迟符号化）
            if now - last_check >= ANALYZE_INTERVAL:
                self.check_and_snapshot()
                last_check = now

            # 周期清理 BPF 栈表（容量 4096 份，不清会导致
            # get_stackid 失败、新分配拿不到调用栈）
            if now - last_stack_clear >= STACK_CLEAR_INTERVAL:
                self.bpf["stack_traces"].clear()
                last_stack_clear = now

        # 退出前兜底落盘，保证 SIGTERM/SIGINT 时最后状态不丢
        self.save_session()
        print("memdiag collector stopped gracefully.", flush=True)

    def _handle_sigterm(self, signum, frame):
        self._stop = True
