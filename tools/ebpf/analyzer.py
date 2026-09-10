### 负责分析：当前还有多少没释放、哪个 PID 最多、是否可疑
### 按 PID 统计、oldest_age 占用时间、可疑判断（count/age 阈值见下方常量）、build_reason
### 本文件由 analyzer_temp.py 与旧 analyzer.py 合并而来（collector/reporter 均引用本文件）

import json
import os
import time
from collections import defaultdict

SESSION_FILE = "runtime/session.json"

# ============================================================
# 【可疑判定规则】可通过环境变量覆盖：
#   MEMDIAG_SUSPICIOUS_COUNT  持有未释放大页数阈值（默认 5）
#   MEMDIAG_SUSPICIOUS_AGE    最早分配持有秒数阈值（默认 60）
# 进程满足任一条件即判可疑。
# ============================================================
SUSPICIOUS_MIN_COUNT = int(os.environ.get("MEMDIAG_SUSPICIOUS_COUNT", "5"))
SUSPICIOUS_MAX_AGE_SEC = int(os.environ.get("MEMDIAG_SUSPICIOUS_AGE", "60"))


def load_session():
    empty = {"updated_at": None, "allocations": {}}

    try:
        if not os.path.exists(SESSION_FILE):
            return empty

        if os.path.getsize(SESSION_FILE) == 0:
            return empty

        with open(SESSION_FILE, "r", encoding="utf-8") as f:
            return json.load(f)

    except (json.JSONDecodeError, OSError):
        # 坏 JSON / IO 错误统一降级为空会话，保证 report 不中断
        return empty


def analyze():
    session = load_session()
    allocations = session.get("allocations", {})

    by_pid = defaultdict(lambda: {
        "pid": None,
        "comm": "",
        "count": 0,
        "bytes": 0,
        "oldest_age": 0,

        # 保存这个进程的每一笔 outstanding allocation
        "allocations": [],
    })

    now = time.time()

    for item in allocations.values():
        pid = item["pid"]
        age = max(0, now - item.get("time", now))

        proc = by_pid[pid]

        proc["pid"] = pid
        proc["comm"] = item.get("comm", "")
        proc["count"] += 1
        proc["bytes"] += item.get("size", 0)
        proc["oldest_age"] = max(
            proc["oldest_age"],
            age,
        )

        proc["allocations"].append({
            "addr": item.get("addr", ""),
            "size": item.get("size", 0),
            "age_sec": int(age),

            "user_stack_id": item.get(
                "user_stack_id",
                -1,
            ),
            "kernel_stack_id": item.get(
                "kernel_stack_id",
                -1,
            ),

            "user_stack": item.get(
                "user_stack",
                [],
            ),
            "kernel_stack": item.get(
                "kernel_stack",
                [],
            ),
        })

    for proc in by_pid.values():
        proc["allocations"].sort(
            key=lambda allocation: allocation["age_sec"],
            reverse=True,
        )

    suspicious = []

    for proc in by_pid.values():
        if (proc["count"] >= SUSPICIOUS_MIN_COUNT
                or proc["oldest_age"] >= SUSPICIOUS_MAX_AGE_SEC):
            suspicious.append({
                **proc,
                "reason": build_reason(proc),
            })

    return {
        "updated_at": session.get("updated_at"),
        "total_allocations": len(allocations),
        "total_bytes": sum(item.get("size", 0) for item in allocations.values()),
        "by_pid": sorted(by_pid.values(), key=lambda x: x["count"], reverse=True),
        "suspicious": suspicious,
    }


def build_reason(proc):
    reasons = []

    if proc["count"] >= SUSPICIOUS_MIN_COUNT:
        reasons.append(f"holds {proc['count']} HugePages")

    if proc["oldest_age"] >= SUSPICIOUS_MAX_AGE_SEC:
        reasons.append(f"oldest allocation age is {int(proc['oldest_age'])}s")

    return "; ".join(reasons)
