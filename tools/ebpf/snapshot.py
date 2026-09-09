import json
import os
import re
import tempfile
import time

# 路径锚定到本文件所在目录
TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
SNAPSHOT_DIR = os.path.join(TOOLS_DIR, "snapshots")
SNAPSHOT_INDEX = os.path.join(TOOLS_DIR, "runtime", "snapshot_index.json")
MAX_SNAPSHOT_ALLOCATIONS = 20

# ============================================================
# 【cmdline 脱敏开关】环境变量控制（默认开启）：
#   MEMDIAG_SANITIZE=1   开启（默认）：cmdline 中疑似密码/密钥的参数值替换为 ***
#   MEMDIAG_SANITIZE=0   关闭：保留原始 cmdline（仅限受控排障场景）
# 脱敏发生在采集源头，快照文件与 AI prompt 同时受保护。
# ============================================================
SANITIZE_CMDLINE = os.environ.get("MEMDIAG_SANITIZE", "1") == "1"

# 命中即脱敏的参数名模式（大小写不敏感）
_SENSITIVE_KEY_RE = re.compile(
    r"(?i)(password|passwd|pwd|secret|token|apikey|api_key|access_?key|"
    r"private_?key|credential|auth)"
)
# 形态1：--key=value（含 -key=value）
_KV_RE = re.compile(r"(--?\S*?)=(\S+)")
# 形态3：值内部的 KEY=value 赋值（如 --env PASSWORD=xxx、-e TOKEN=yyy）
_INNER_KV_RE = re.compile(
    r"(?i)\b([A-Za-z0-9_]*(?:password|passwd|pwd|secret|token|apikey|api_key)"
    r"[A-Za-z0-9_]*)=(\S+)"
)


def read_file(path, max_bytes=64 * 1024):
    try:
        with open(path, "r", errors="replace") as f:
            return f.read(max_bytes)
    except Exception as e:
        return f"<failed to read {path}: {e}>"


def load_snapshot_index():
    if not os.path.exists(SNAPSHOT_INDEX):
        return {}

    try:
        if os.path.getsize(SNAPSHOT_INDEX) == 0:
            return {}

        with open(SNAPSHOT_INDEX, "r") as f:
            return json.load(f)
    except Exception:
        return {}


def save_snapshot_index(index):
    """原子写快照索引：tmpfile + os.replace，与 save_session 风格一致。"""
    runtime_dir = os.path.join(TOOLS_DIR, "runtime")
    os.makedirs(runtime_dir, exist_ok=True)

    fd, tmp_path = tempfile.mkstemp(
        dir=runtime_dir, prefix=".snapshot_index_", suffix=".tmp")
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as f:
            json.dump(index, f, indent=2)
        os.replace(tmp_path, SNAPSHOT_INDEX)
    except Exception:
        if os.path.exists(tmp_path):
            os.unlink(tmp_path)
        raise


def sanitize_cmdline(cmdline):
    """对 cmdline 中疑似密码/密钥的凭证脱敏为 ***。

    覆盖四种形态（大小写不敏感）：
      1. --password=xxx              → --password=***
      2. --my_password xxx           → --my_password ***
      3. 值内部赋值 --env PASSWORD=xxx / -e TOKEN=yyy → KEY=***
      4. URL 凭证 https://user:pass@host 或 ?token=xxx → 凭证位 ***

    短选项形态（-p xxx）无法从语法判断语义，保守不处理；
    环境变量 MEMDIAG_SANITIZE=0 可关闭（默认开启）。
    """
    if not SANITIZE_CMDLINE or not cmdline:
        return cmdline

    def _mask_kv(match):
        key, _value = match.group(1), match.group(2)
        if _SENSITIVE_KEY_RE.search(key):
            return f"{key}=***"
        return match.group(0)

    # 形态1：--key=value（含 -key=value）
    result = _KV_RE.sub(_mask_kv, cmdline)

    # 形态3：值内部的 KEY=value 赋值（--env PASSWORD=xxx 等）
    def _mask_inner(match):
        if _SENSITIVE_KEY_RE.search(match.group(1)):
            return f"{match.group(1)}=***"
        return match.group(0)

    result = _INNER_KV_RE.sub(_mask_inner, result)

    # 形态2：--sensitive_key value（空格分隔，键命中敏感词）
    tokens = result.split()
    masked = []
    skip_next = False
    for i, token in enumerate(tokens):
        if skip_next:
            masked.append("***")
            skip_next = False
            continue
        if _SENSITIVE_KEY_RE.search(token) and i + 1 < len(tokens) \
                and "=" not in token:
            masked.append(token)
            skip_next = True
            continue
        masked.append(token)

    result = " ".join(masked)

    # 形态4：URL 中的凭证（https://user:pass@host 或 ?token=xxx）
    result = re.sub(r"(://[^/\s:]+:)[^@\s]+(@)", r"\1***\2", result)
    result = re.sub(
        r"(?i)([?&](?:token|apikey|api_key|access_key)=)\S+", r"\1***", result)

    return result


def collect_process_snapshot(pid):
    return {
        "status": read_file(f"/proc/{pid}/status"),
        "cmdline": sanitize_cmdline(
            read_file(f"/proc/{pid}/cmdline").replace("\x00", " ")
        ),
        "maps": read_file(f"/proc/{pid}/maps"),
        "smaps_rollup": read_file(f"/proc/{pid}/smaps_rollup"),
    }


def collect_system_snapshot():
    return {
        "meminfo": read_file("/proc/meminfo"),
        "vmstat": read_file("/proc/vmstat"),
        "hugepages_nr": read_file("/proc/sys/vm/nr_hugepages"),
    }


def save_snapshot(proc, result):
    os.makedirs(SNAPSHOT_DIR, exist_ok=True)

    pid = str(proc["pid"])
    index = load_snapshot_index()

    if pid in index:
        return index[pid]["path"]

    timestamp = time.strftime("%Y%m%d_%H%M%S")

    snapshot = {
        "snapshot_time": timestamp,
        "reason": proc.get("reason", ""),
        "process": {
            "pid": int(pid),
            "comm": proc.get("comm", ""),
            "hugepage_count": proc.get("count", 0),
            "hugepage_bytes": proc.get("bytes", 0),
            "oldest_age_sec": int(proc.get("oldest_age", 0)),
        },
        "summary": {
            "total_allocations": result.get("total_allocations", 0),
            "total_bytes": result.get("total_bytes", 0),
        },
        "allocation_details": build_allocation_details(proc),
        "system": collect_system_snapshot(),
        "process_detail": collect_process_snapshot(pid),
    }

    filename = f"hugepage_pid_{pid}_{timestamp}.json"
    path = os.path.join(SNAPSHOT_DIR, filename)

    with open(path, "w", encoding="utf-8") as f:
        json.dump(snapshot, f, indent=2, ensure_ascii=False)

    index[pid] = {
        "path": path,
        "created_at": timestamp,
        "reason": proc.get("reason", ""),
    }
    save_snapshot_index(index)

    return path


def build_allocation_details(proc):
    allocations = proc.get("allocations", [])

    details = []

    for allocation in allocations[:MAX_SNAPSHOT_ALLOCATIONS]:
        details.append({
            "addr": allocation.get("addr", ""),
            "size": allocation.get("size", 0),
            "age_sec": allocation.get("age_sec", 0),

            "user_stack_id": allocation.get(
                "user_stack_id",
                -1,
            ),
            "kernel_stack_id": allocation.get(
                "kernel_stack_id",
                -1,
            ),

            "user_stack": allocation.get(
                "user_stack",
                [],
            ),
            "kernel_stack": allocation.get(
                "kernel_stack",
                [],
            ),
        })

    return {
        "captured_count": len(details),
        "total_count": len(allocations),
        "truncated": len(allocations) > len(details),
        "items": details,
    }
