#!/usr/bin/env python3
"""SMAP IPC helper — 通过 Unix Domain Socket 与 ubturbo daemon 通信.

封装 smap_interface.h 中全部 IPC 接口。payload 布局严格对照 ubturbo 源码
src/smap/smap_handler_msg.cpp 的 codec 实现，结构体尺寸经 sizeof 探针核实
（见 smap_interface.h 常量：REMOTE_NUMA_NUM=18 / MAX_NR_MIGOUT=40 / ...）。

协议：请求帧 [>II total_len, func_name_len][func_name+" "][payload]，
响应帧 [>II total_len, retCode][resp_data]，func 头部大端、payload/resp 小端。
"""

import socket
import struct
import sys
import ctypes
from pathlib import Path


class OldProcessPayload(ctypes.Structure):
    """Process configuration layout returned by the IPC query API."""

    _fields_ = [
        ("pid", ctypes.c_int),
        ("ratio", ctypes.c_uint8),
        ("scanType", ctypes.c_uint8),
        ("type", ctypes.c_uint8),
        ("state", ctypes.c_uint8),
        ("migrateMode", ctypes.c_uint8),
        ("l1Node", ctypes.c_int16 * 4),
        ("l2Node", ctypes.c_int16 * 4),
        ("scanTime", ctypes.c_uint32),
        ("memSize", ctypes.c_uint64),
    ]

UDS_PATH = "/opt/ubturbo/ubturbo_ipc"
IPC_OK = 0
DEFAULT_IPC_TIMEOUT = 120

# ---- 结构体尺寸（sizeof 探针核实）----
SZ_MIGRATE_OUT_MSG = 17928        # MigrateOutMsg: int count + pad + payload[40] (448 each)
SZ_GROUPED_MIG_OUT_MSG = 118088   # GroupedMigrateOutMsg: count + payload[40] (2952 each)
SZ_MIGRATE_BACK_MSG = 816        # MigrateBackMsg: taskID + count + pad + payload[50] (16 each)
SZ_REMOVE_MSG = 3204             # RemoveMsg: count + payload[40] (80 each, align 4 无内聚 pad)
SZ_MIGRATE_NUMA_MSG = 416        # MigrateNumaMsg: src/dest/count + pad + memids[50] (Q each)
SZ_MIGRATE_ESCAPE_MSG = 9608     # MigrateEscapeMsg: count + pad + payload[300] (32 each)
SZ_PROCESS_PAYLOAD = 40          # ProcessPayload

# ---- 数组容量（smap_interface.h）----
N_REMOTE_NUMA = 18
N_MAX_MIGOUT = 40
N_MAX_MIGBACK = 50
N_MAX_MIGNUMA = 50
N_MAX_MIGRATE_ESCAPE = 300
N_MAX_GROUP_LOCAL = 4
N_MAX_GROUP_REMOTE = 18
N_MAX_MIGRATION_GROUP = 8


# ============================================================
# IPC 传输层
# ============================================================
class SmapIpcError(RuntimeError):
    """Unix socket transport or protocol failure."""


def _send_ipc(func_name: str, payload: bytes, socket_path: str = UDS_PATH,
              timeout: float = DEFAULT_IPC_TIMEOUT) -> tuple[int, bytes]:
    """发送 IPC 请求，返回 (status, resp_data)。status=0 表示 IPC 层成功。"""
    func_name_bytes = (func_name + " ").encode()
    func_name_len = len(func_name_bytes)
    total_len = 8 + func_name_len + len(payload)

    header = struct.pack(">II", total_len, func_name_len)
    message = header + func_name_bytes + payload

    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sock.settimeout(timeout)
    try:
        sock.connect(socket_path)
        sock.sendall(message)

        resp_header = _recv_exact(sock, 8)
        if len(resp_header) != 8:
            raise SmapIpcError("IPC response header is truncated")
        resp_total_len, status = struct.unpack(">II", resp_header)
        if resp_total_len < 8:
            raise SmapIpcError(f"invalid IPC response length: {resp_total_len}")
        resp_data = _recv_exact(sock, resp_total_len - 8) if resp_total_len > 8 else b""
        if len(resp_data) != resp_total_len - 8:
            raise SmapIpcError("IPC response payload is truncated")
    except OSError as exc:
        raise SmapIpcError(f"IPC request {func_name} failed: {exc}") from exc
    finally:
        sock.close()

    return status, resp_data


def _recv_exact(sock: socket.socket, n: int) -> bytes:
    data = b""
    while len(data) < n:
        chunk = sock.recv(n - len(data))
        if not chunk:
            break
        data += chunk
    return data


def _result_int(status: int, resp_data: bytes) -> int:
    """取业务返回值：codec 的 EncodeResponse 把 returnValue 打包在 resp_data 前 4 字节。
    resp_data 不足 4 字节（如 urgent_migrate 无 returnValue）则回退到 IPC 层 status。"""
    if len(resp_data) >= 4:
        return struct.unpack("<i", resp_data[:4])[0]
    return status


def _log(msg: str) -> None:
    """诊断日志走 stderr（与 stdout 结果数据分离，避免缓冲交错）。"""
    print(msg, file=sys.stderr, flush=True)


def _out(msg: str) -> None:
    """结果数据走 stdout。"""
    print(msg, flush=True)


def _report(func: str, result: int, extra: str = "") -> int:
    if result == IPC_OK:
        _log(f"[smap_ipc] {func} OK{extra}")
    else:
        _log(f"[smap_ipc] {func} failed ret={result}{extra}")
    return result


class SmapIpcClient:
    """SMAP API client backed by the ubturbo Unix-domain socket.

    All requests cross the daemon IPC boundary. ctypes structures are used
    only to preserve the binary layouts expected by the IPC codec.
    """

    def __init__(self, socket_path: str = UDS_PATH, timeout: float = DEFAULT_IPC_TIMEOUT):
        self.socket_path = str(Path(socket_path))
        self.timeout = timeout

    def _request(self, name: str, payload: bytes) -> tuple[int, bytes]:
        return _send_ipc(name, payload, self.socket_path, self.timeout)

    def _int_request(self, name: str, payload: bytes) -> int:
        status, response = self._request(name, payload)
        return _result_int(status, response)

    @staticmethod
    def _struct_bytes(message) -> bytes:
        return ctypes.string_at(ctypes.addressof(message), ctypes.sizeof(message))

    def start(self, page_type: int = 0, log_func=None) -> int:
        del log_func
        return self._int_request("ubturbo_smap_start", struct.pack("<I", page_type))

    def stop(self) -> int:
        return self._int_request("ubturbo_smap_stop", b"\x00" * 4)

    def is_running(self) -> bool:
        status, response = self._request("ubturbo_smap_is_running", b"\x00" * 4)
        if status != IPC_OK:
            return False
        return bool(response[0]) if response else False

    def migrate_out(self, message, pid_type: int) -> int:
        payload = struct.pack("<i", pid_type) + self._struct_bytes(message)
        return self._int_request("ubturbo_smap_migrate_out", payload)

    def migrate_back(self, message) -> int:
        return self._int_request("ubturbo_smap_migrate_back", self._struct_bytes(message))

    def migrate_out_sync(self, message, pid_type: int, max_wait_time: int) -> int:
        payload = struct.pack("<iQ", pid_type, max_wait_time) + self._struct_bytes(message)
        return self._int_request("ubturbo_smap_migrate_out_sync", payload)

    def remove(self, message, pid_type: int) -> int:
        payload = struct.pack("<i", pid_type) + self._struct_bytes(message)
        return self._int_request("ubturbo_smap_remove", payload)

    def node_enable(self, message) -> int:
        return self._int_request("ubturbo_smap_node_enable", self._struct_bytes(message))

    def urgent_migrate_out(self, size: int) -> None:
        status, _ = self._request("ubturbo_smap_urgent_migrate_out", struct.pack("<Q", size))
        if status != IPC_OK:
            raise SmapIpcError(f"urgent migrate failed: {status}")

    def remote_numa_info_set(self, message) -> int:
        return self._int_request("ubturbo_smap_remote_numa_info_set", self._struct_bytes(message))

    def run_mode_set(self, run_mode: int) -> int:
        return self._int_request("ubturbo_smap_run_mode_set", struct.pack("<i", run_mode))

    def process_tracking_add(self, pids, scan_times, durations, scan_type: int) -> int:
        count = len(pids)
        payload = struct.pack("<ii", count, scan_type)
        payload += struct.pack(f"<{count}i", *pids)
        payload += struct.pack(f"<{count}I", *scan_times)
        payload += struct.pack(f"<{count}I", *durations)
        return self._int_request("ubturbo_smap_process_tracking_add", payload)

    def process_tracking_remove(self, pids, flag: int) -> int:
        count = len(pids)
        payload = struct.pack("<ii", count, flag) + struct.pack(f"<{count}i", *pids)
        return self._int_request("ubturbo_smap_process_tracking_remove", payload)

    def process_migrate_enable(self, pids, enable: int, flags: int) -> int:
        count = len(pids)
        payload = struct.pack("<iii", count, enable, flags) + struct.pack(f"<{count}i", *pids)
        return self._int_request("ubturbo_smap_process_migrate_enable", payload)

    def freq_query(self, pid: int, data: list, data_source: int) -> tuple[int, int]:
        payload = struct.pack("<iII", pid, len(data), data_source)
        status, response = self._request("ubturbo_smap_freq_query", payload)
        if status != IPC_OK:
            return status, 0
        if len(response) < 8:
            raise SmapIpcError("freq_query response is too short")
        result, count = struct.unpack("<iI", response[:8])
        required = 8 + count * 2
        if len(response) < required:
            raise SmapIpcError("freq_query data is truncated")
        values = struct.unpack_from(f"<{count}H", response, 8)
        data[:min(len(data), count)] = values[:len(data)]
        return result, count

    def process_config_query(self, nid: int, in_len: int) -> tuple[int, list, int]:
        status, response = self._request(
            "ubturbo_smap_process_config_query", struct.pack("<ii", nid, in_len)
        )
        if status != IPC_OK:
            return status, [], 0
        if len(response) < 8:
            raise SmapIpcError("process_config_query response is too short")
        result, count = struct.unpack("<ii", response[:8])
        if count < 0:
            raise SmapIpcError(f"invalid process_config_query length: {count}")
        item_size = ctypes.sizeof(OldProcessPayload)
        required = 8 + count * item_size
        if len(response) < required:
            raise SmapIpcError("process_config_query data is truncated")
        values = [OldProcessPayload.from_buffer_copy(response, 8 + index * item_size)
                  for index in range(count)]
        return result, values, count

    def remote_numa_migrate(self, message) -> int:
        return self._int_request("ubturbo_smap_remote_numa_migrate", self._struct_bytes(message))

    def same_remote_numa_migrate(self, message) -> int:
        return self._int_request("ubturbo_smap_same_remote_numa_migrate", self._struct_bytes(message))

    def pid_remote_numa_migrate(self, message) -> int:
        return self._int_request("ubturbo_smap_pid_remote_numa_migrate", self._struct_bytes(message))

    def remote_numa_freq_query(self, numas: list) -> tuple[int, list]:
        count = len(numas)
        payload = struct.pack("<H", count) + struct.pack(f"<{count}H", *numas)
        status, response = self._request("ubturbo_smap_remote_numa_freq_query", payload)
        if status != IPC_OK:
            return status, []
        if len(response) < 6:
            raise SmapIpcError("remote_numa_freq_query response is too short")
        result, out_len = struct.unpack("<iH", response[:6])
        required = 6 + out_len * 8
        if len(response) < required:
            raise SmapIpcError("remote_numa_freq_query data is truncated")
        return result, list(struct.unpack_from(f"<{out_len}Q", response, 6))


# ============================================================
# 结构体构造辅助
# ============================================================
def _build_migrate_out_msg(pid: int, dest_nid: int, ratio: int,
                           mem_size: int = 0, migrate_mode: int = 0) -> bytes:
    """MigrateOutMsg (17928B)：count=1，只填 payload[0].inner[0]。"""
    msg = bytearray(SZ_MIGRATE_OUT_MSG)
    struct.pack_into("<i", msg, 0, 1)                       # count
    base = 8                                                # payload[0]（count 后 4B padding）
    struct.pack_into("<iii", msg, base, -1, pid, 1)        # srcNid=-1 / pid / count
    inner = base + 16                                       # inner[0]（srcNid+pid+count + 4 pad）
    # inner: destNid / ratio / memSize(Q) / migrateMode + 4 pad
    struct.pack_into("<iiQi4x", msg, inner, dest_nid, ratio, mem_size, migrate_mode)
    assert len(msg) == SZ_MIGRATE_OUT_MSG
    return bytes(msg)


def _build_grouped_mig_out_msg(pid: int, locals_list, targets_list) -> bytes:
    """GroupedMigrateOutMsg (118088B)：count=1，payload[0].groups[0]。"""
    if len(locals_list) > N_MAX_GROUP_LOCAL:
        raise ValueError(f"locals 最多 {N_MAX_GROUP_LOCAL} 个")
    if len(targets_list) > N_MAX_GROUP_REMOTE:
        raise ValueError(f"targets 最多 {N_MAX_GROUP_REMOTE} 个")

    msg = bytearray(SZ_GROUPED_MIG_OUT_MSG)
    struct.pack_into("<i", msg, 0, 1)                       # count
    payload0 = 8                                            # payload[0]
    struct.pack_into("<ii", msg, payload0, pid, 1)         # pid / groupCount=1
    group0 = payload0 + 8                                   # groups[0]
    struct.pack_into("<i", msg, group0, len(locals_list))  # localCount
    # locals[4] @ group0+8（每个 MigrationNode=16B: <i4xQ>）
    for i, (nid, size) in enumerate(locals_list):
        struct.pack_into("<i4xQ", msg, group0 + 8 + 16 * i, nid, size)
    target_count_off = group0 + 8 + 16 * N_MAX_GROUP_LOCAL  # targetCount
    struct.pack_into("<i", msg, target_count_off, len(targets_list))
    targets_off = target_count_off + 4 + 4                 # targets[18]（targetCount 后 4B padding）
    for i, (nid, size) in enumerate(targets_list):
        struct.pack_into("<i4xQ", msg, targets_off + 16 * i, nid, size)
    assert len(msg) == SZ_GROUPED_MIG_OUT_MSG
    return bytes(msg)


def _build_migrate_back_msg(task_id: int, entries) -> bytes:
    """MigrateBackMsg (816B)：taskID + count + payload[50]（srcNid/destNid/memid）。"""
    if len(entries) > N_MAX_MIGBACK:
        raise ValueError(f"migrate_back 最多 {N_MAX_MIGBACK} 条")
    msg = bytearray(SZ_MIGRATE_BACK_MSG)
    struct.pack_into("<Qi", msg, 0, task_id, len(entries))  # taskID / count
    for i, (src, dest, memid) in enumerate(entries):
        struct.pack_into("<iiQ", msg, 16 + 16 * i, src, dest, memid)
    assert len(msg) == SZ_MIGRATE_BACK_MSG
    return bytes(msg)


def _build_remove_msg(entries) -> bytes:
    """RemoveMsg (3204B)：count + payload[40]（pid / count / nid[18]）。"""
    if len(entries) > N_MAX_MIGOUT:
        raise ValueError(f"remove 最多 {N_MAX_MIGOUT} 条")
    msg = bytearray(SZ_REMOVE_MSG)
    struct.pack_into("<i", msg, 0, len(entries))           # count
    for i, (pid, nids) in enumerate(entries):
        if len(nids) > N_REMOTE_NUMA:
            raise ValueError(f"remove 条目 {i} 的 nid 数量超过 {N_REMOTE_NUMA}")
        base = 4 + 80 * i                                    # payload[i]
        nids_padded = list(nids) + [-1] * (N_REMOTE_NUMA - len(nids))
        struct.pack_into("<ii" + "i" * N_REMOTE_NUMA, msg, base,
                         pid, len(nids), *nids_padded)
    assert len(msg) == SZ_REMOVE_MSG
    return bytes(msg)


def _build_migrate_numa_msg(src_nid: int, dest_nid: int, memids) -> bytes:
    """MigrateNumaMsg (416B)：srcNid/destNid/count + pad + memids[50]。"""
    if len(memids) > N_MAX_MIGNUMA:
        raise ValueError(f"memids 最多 {N_MAX_MIGNUMA} 个")
    msg = bytearray(SZ_MIGRATE_NUMA_MSG)
    struct.pack_into("<iii", msg, 0, src_nid, dest_nid, len(memids))
    memids_padded = list(memids) + [0] * (N_MAX_MIGNUMA - len(memids))
    struct.pack_into("<" + "Q" * N_MAX_MIGNUMA, msg, 16, *memids_padded)
    assert len(msg) == SZ_MIGRATE_NUMA_MSG
    return bytes(msg)


def _build_migrate_escape_msg(entries) -> bytes:
    """MigrateEscapeMsg (9608B)：count + payload[300]（pid/src/dest/ratio/memSize/mode）。"""
    if len(entries) > N_MAX_MIGRATE_ESCAPE:
        raise ValueError(f"pid_remote_numa_migrate 最多 {N_MAX_MIGRATE_ESCAPE} 条")
    msg = bytearray(SZ_MIGRATE_ESCAPE_MSG)
    struct.pack_into("<i", msg, 0, len(entries))           # count
    for i, (pid, src, dest, ratio, mem_size, mode) in enumerate(entries):
        base = 8 + 32 * i                                    # payload[i]（count 后 4B padding）
        struct.pack_into("<iiiiQi4x", msg, base, pid, src, dest, ratio, mem_size, mode)
    assert len(msg) == SZ_MIGRATE_ESCAPE_MSG
    return bytes(msg)


# ============================================================
# CLI 参数解析辅助
# ============================================================
def _parse_int_list(s: str):
    """逗号/分号/空白分隔的整数列表。"""
    import re
    return [int(x, 0) for x in re.split(r"[,;\s]+", s.strip()) if x]


def _parse_pairs(s: str):
    """'0:1024,1:2048' -> [(0,1024),(1,2048)]。"""
    pairs = []
    for item in s.split(","):
        item = item.strip()
        if not item:
            continue
        nid, size = item.split(":")
        pairs.append((int(nid, 0), int(size, 0)))
    return pairs


def _parse_triples(s: str):
    """'src:dest:memid;...' -> [(src,dest,memid),...]。"""
    out = []
    for item in s.split(";"):
        item = item.strip()
        if not item:
            continue
        src, dest, memid = item.split(":")
        out.append((int(src, 0), int(dest, 0), int(memid, 0)))
    return out


def _parse_remove_entries(s: str):
    """'pid:nid1,nid2;pid2:nid3' -> [(pid,[nids]),...]。"""
    out = []
    for item in s.split(";"):
        item = item.strip()
        if not item:
            continue
        pid_part, nids_part = item.split(":")
        nids = _parse_int_list(nids_part)
        out.append((int(pid_part, 0), nids))
    return out


def _parse_tracking_entries(s: str):
    """'pid:scanTime:duration;...' -> [(pid,scanTime,duration),...]。"""
    out = []
    for item in s.split(";"):
        item = item.strip()
        if not item:
            continue
        pid, scan_time, duration = item.split(":")
        out.append((int(pid, 0), int(scan_time, 0), int(duration, 0)))
    return out


def _parse_escape_entries(s: str):
    """'pid:src:dest:ratio:memSize:mode;...' -> 6元组列表。"""
    out = []
    for item in s.split(";"):
        item = item.strip()
        if not item:
            continue
        pid, src, dest, ratio, mem_size, mode = item.split(":")
        out.append((int(pid, 0), int(src, 0), int(dest, 0),
                    int(ratio, 0), int(mem_size, 0), int(mode, 0)))
    return out


# ============================================================
# 命令实现
# ============================================================
def cmd_start(page_type: int) -> int:
    """ubturbo_smap_start(uint32_t pageType)。codec EncodeRequest 只打包 4B pageType。"""
    payload = struct.pack("<I", page_type)
    func = "ubturbo_smap_start"
    _log(f"[smap_ipc] {func} pageType={page_type}")
    status, resp = _send_ipc(func, payload)
    return _report(func, _result_int(status, resp))


def cmd_stop() -> int:
    """ubturbo_smap_stop(void)。EncodeRequest 占 4B 空占位，DecodeRequest 不读。"""
    func = "ubturbo_smap_stop"
    _log(f"[smap_ipc] {func}")
    status, resp = _send_ipc(func, b"\x00" * 4)
    return _report(func, _result_int(status, resp))


def cmd_urgent_migrate(size: int) -> int:
    """ubturbo_smap_urgent_migrate_out(uint64_t size)。无业务返回值，用 IPC status 判定。"""
    payload = struct.pack("<Q", size)
    func = "ubturbo_smap_urgent_migrate_out"
    _log(f"[smap_ipc] {func} size={size}")
    status, _ = _send_ipc(func, payload)
    return _report(func, status)


def cmd_remote_numa_info(src_nid: int, dest_nid: int, size_mib: int) -> int:
    """ubturbo_smap_remote_numa_info_set(SetRemoteNumaInfoMsg)。"""
    payload = struct.pack("<iiQ", src_nid, dest_nid, size_mib)
    func = "ubturbo_smap_remote_numa_info_set"
    _log(f"[smap_ipc] {func} srcNid={src_nid} destNid={dest_nid} size={size_mib}MiB")
    status, resp = _send_ipc(func, payload)
    return _report(func, _result_int(status, resp))


def cmd_migrate(pid: int, dest_nid: int, ratio: int, pid_type: int) -> int:
    """ubturbo_smap_migrate_out(MigrateOutMsg, pidType) — 比例模式。"""
    msg = _build_migrate_out_msg(pid, dest_nid, ratio)
    payload = struct.pack("<i", pid_type) + msg              # pidType 前置
    func = "ubturbo_smap_migrate_out"
    _log(f"[smap_ipc] {func} pid={pid} destNid={dest_nid} ratio={ratio}% pidType={pid_type}")
    status, resp = _send_ipc(func, payload)
    return _report(func, _result_int(status, resp))


def cmd_migrate_sync(pid: int, dest_nid: int, ratio: int, pid_type: int, max_wait_ms: int) -> int:
    """ubturbo_smap_migrate_out_sync(MigrateOutMsg, pageType, maxWaitTime)。

    codec 布局：pageType(4) + maxWaitTime(Q@4) + MigrateOutMsg(@12)，紧凑无对齐 padding。
    """
    msg = _build_migrate_out_msg(pid, dest_nid, ratio)
    payload = struct.pack("<iQ", pid_type, max_wait_ms) + msg
    func = "ubturbo_smap_migrate_out_sync"
    _log(f"[smap_ipc] {func} pid={pid} destNid={dest_nid} ratio={ratio}% "
          f"pidType={pid_type} maxWait={max_wait_ms}ms")
    status, resp = _send_ipc(func, payload)
    return _report(func, _result_int(status, resp))


def cmd_migrate_grouped(pid: int, page_type: int, locals_s: str, targets_s: str) -> int:
    """ubturbo_smap_migrate_out_grouped(GroupedMigrateOutMsg, pageType)。"""
    msg = _build_grouped_mig_out_msg(pid, _parse_pairs(locals_s), _parse_pairs(targets_s))
    payload = struct.pack("<i", page_type) + msg
    func = "ubturbo_smap_migrate_out_grouped"
    _log(f"[smap_ipc] {func} pid={pid} pageType={page_type} "
          f"locals={locals_s} targets={targets_s}")
    status, resp = _send_ipc(func, payload)
    return _report(func, _result_int(status, resp))


def cmd_migrate_back(task_id: int, entries_s: str) -> int:
    """ubturbo_smap_migrate_back(MigrateBackMsg)。"""
    msg = _build_migrate_back_msg(task_id, _parse_triples(entries_s))
    func = "ubturbo_smap_migrate_back"
    _log(f"[smap_ipc] {func} taskID={task_id} entries={entries_s}")
    status, resp = _send_ipc(func, msg)
    return _report(func, _result_int(status, resp))


def cmd_remove(page_type: int, entries_s: str) -> int:
    """ubturbo_smap_remove(RemoveMsg, pageType)。"""
    msg = _build_remove_msg(_parse_remove_entries(entries_s))
    payload = struct.pack("<i", page_type) + msg
    func = "ubturbo_smap_remove"
    _log(f"[smap_ipc] {func} pageType={page_type} entries={entries_s}")
    status, resp = _send_ipc(func, payload)
    return _report(func, _result_int(status, resp))


def cmd_node_enable(enable: int, nid: int) -> int:
    """ubturbo_smap_node_enable(EnableNodeMsg)。"""
    payload = struct.pack("<ii", enable, nid)
    func = "ubturbo_smap_node_enable"
    _log(f"[smap_ipc] {func} enable={enable} nid={nid}")
    status, resp = _send_ipc(func, payload)
    return _report(func, _result_int(status, resp))


def cmd_process_tracking_add(scan_type: int, entries_s: str) -> int:
    """ubturbo_smap_process_tracking_add(pidArr, scanTime, duration, len, scanType)。

    codec 布局：int len + int scanType + pid_t[len] + uint32 scanTime[len] + uint32 duration[len]。
    """
    entries = _parse_tracking_entries(entries_s)
    pids = [e[0] for e in entries]
    scan_times = [e[1] for e in entries]
    durations = [e[2] for e in entries]
    n = len(entries)
    payload = struct.pack("<ii", n, scan_type)
    payload += struct.pack(f"<{n}i", *pids)
    payload += struct.pack(f"<{n}I", *scan_times)
    payload += struct.pack(f"<{n}I", *durations)
    func = "ubturbo_smap_process_tracking_add"
    _log(f"[smap_ipc] {func} scanType={scan_type} {n} pid(s)")
    status, resp = _send_ipc(func, payload)
    return _report(func, _result_int(status, resp))


def cmd_process_tracking_remove(flag: int, pids_s: str) -> int:
    """ubturbo_smap_process_tracking_remove(pidArr, len, flag)。

    codec 布局：int len + int flag + pid_t[len]。
    """
    pids = _parse_int_list(pids_s)
    payload = struct.pack("<ii", len(pids), flag)
    payload += struct.pack(f"<{len(pids)}i", *pids)
    func = "ubturbo_smap_process_tracking_remove"
    _log(f"[smap_ipc] {func} flag={flag} {len(pids)} pid(s)")
    status, resp = _send_ipc(func, payload)
    return _report(func, _result_int(status, resp))


def cmd_process_migrate_enable(enable: int, flags: int, pids_s: str) -> int:
    """ubturbo_smap_process_migrate_enable(pidArr, len, enable, flags)。

    codec 布局：int len + int enable + int flags + pid_t[len]。
    """
    pids = _parse_int_list(pids_s)
    payload = struct.pack("<iii", len(pids), enable, flags)
    payload += struct.pack(f"<{len(pids)}i", *pids)
    func = "ubturbo_smap_process_migrate_enable"
    _log(f"[smap_ipc] {func} enable={enable} flags={flags} {len(pids)} pid(s)")
    status, resp = _send_ipc(func, payload)
    return _report(func, _result_int(status, resp))


def cmd_run_mode(run_mode: int) -> int:
    """ubturbo_smap_run_mode_set(int runMode)。"""
    payload = struct.pack("<i", run_mode)
    func = "ubturbo_smap_run_mode_set"
    _log(f"[smap_ipc] {func} runMode={run_mode}")
    status, resp = _send_ipc(func, payload)
    return _report(func, _result_int(status, resp))


def cmd_is_running() -> int:
    """ubturbo_smap_is_running(void) -> bool。EncodeResponse 打包 1B bool。"""
    func = "ubturbo_smap_is_running"
    _log(f"[smap_ipc] {func}")
    status, resp = _send_ipc(func, b"\x00" * 4)
    if status != IPC_OK:
        return _report(func, status)
    running = bool(resp[0]) if resp else False
    _log(f"[smap_ipc] {func} running={running}")
    return _report(func, IPC_OK)


def cmd_freq_query(pid: int, length: int, data_source: int) -> int:
    """ubturbo_smap_freq_query(pid, data, lengthIn, lengthOut, dataSource)。

    响应：int returnValue + uint32 lengthOut + uint16[lengthOut] data。
    """
    payload = struct.pack("<iII", pid, length, data_source)
    func = "ubturbo_smap_freq_query"
    _log(f"[smap_ipc] {func} pid={pid} length={length} dataSource={data_source}")
    status, resp = _send_ipc(func, payload)
    if status != IPC_OK:
        return _report(func, status)
    if len(resp) < 8:
        return _report(func, -1, " resp too short")
    ret, out = struct.unpack("<iI", resp[:8])
    freqs = struct.unpack_from(f"<{out}H", resp, 8) if out else ()
    _log(f"[smap_ipc] {func} ret={ret} lengthOut={out} freq={list(freqs)}")
    return _report(func, ret)


def cmd_process_config_query(nid: int, in_len: int) -> int:
    """ubturbo_smap_process_config_query(nid, result, inLen, outLen, dataSource)。

    响应：int returnValue + int out + ProcessPayload[out]（每条 40B）。
    """
    payload = struct.pack("<ii", nid, in_len)
    func = "ubturbo_smap_process_config_query"
    _log(f"[smap_ipc] {func} nid={nid} inLen={in_len}")
    status, resp = _send_ipc(func, payload)
    if status != IPC_OK:
        return _report(func, status)
    if len(resp) < 8:
        return _report(func, -1, " resp too short")
    ret, out = struct.unpack("<ii", resp[:8])
    _log(f"[smap_ipc] {func} ret={ret} out={out}")
    for i in range(out):
        off = 8 + SZ_PROCESS_PAYLOAD * i
        if off + SZ_PROCESS_PAYLOAD > len(resp):
            break
        (ppid, ratio, scan_type, ptype, state, mmode,
         l1_0, l1_1, l1_2, l1_3, l2_0, l2_1, l2_2, l2_3,
         scan_time, mem_size) = struct.unpack_from("<iBBBBB1x8h2xIQ", resp, off)
        _out(f"  [{i}] pid={ppid} ratio={ratio} scanType={scan_type} type={ptype} "
              f"state={state} migrateMode={mmode} l1={list((l1_0,l1_1,l1_2,l1_3))} "
              f"l2={list((l2_0,l2_1,l2_2,l2_3))} scanTime={scan_time} memSize={mem_size}")
    return _report(func, ret)


def cmd_remote_numa_migrate(src_nid: int, dest_nid: int, memids_s: str) -> int:
    """ubturbo_smap_remote_numa_migrate(MigrateNumaMsg)。"""
    msg = _build_migrate_numa_msg(src_nid, dest_nid, _parse_int_list(memids_s))
    func = "ubturbo_smap_remote_numa_migrate"
    _log(f"[smap_ipc] {func} srcNid={src_nid} destNid={dest_nid} memids={memids_s}")
    status, resp = _send_ipc(func, msg)
    return _report(func, _result_int(status, resp))


def cmd_pid_remote_numa_migrate(entries_s: str) -> int:
    """ubturbo_smap_pid_remote_numa_migrate(MigrateEscapeMsg)。"""
    msg = _build_migrate_escape_msg(_parse_escape_entries(entries_s))
    func = "ubturbo_smap_pid_remote_numa_migrate"
    _log(f"[smap_ipc] {func} entries={entries_s}")
    status, resp = _send_ipc(func, msg)
    return _report(func, _result_int(status, resp))


def cmd_remote_numa_freq_query(nids_s: str) -> int:
    """ubturbo_smap_remote_numa_freq_query(numa, freq, length)。

    请求：uint16 length + uint16[length] numa。
    响应：int returnValue + uint16 len + uint64[len]（紧凑无对齐 padding）。
    """
    nids = _parse_int_list(nids_s)
    n = len(nids)
    payload = struct.pack("<H", n) + struct.pack(f"<{n}H", *nids)
    func = "ubturbo_smap_remote_numa_freq_query"
    _log(f"[smap_ipc] {func} nids={nids}")
    status, resp = _send_ipc(func, payload)
    if status != IPC_OK:
        return _report(func, status)
    if len(resp) < 6:
        return _report(func, -1, " resp too short")
    ret = struct.unpack_from("<i", resp, 0)[0]
    out = struct.unpack_from("<H", resp, 4)[0]
    freqs = struct.unpack_from(f"<{out}Q", resp, 6) if out else ()
    for nid, freq in zip(nids, freqs):
        _out(f"  node{nid}: {freq}")
    return _report(func, ret)


# ============================================================
# CLI
# ============================================================
USAGE = f"""Usage: {sys.argv[0]} <command> [args...]
  start <pageType>                                    启动 SMAP（0=4K进程, 1=2M虚机）
  stop                                                停止 SMAP
  urgent_migrate <size>                               紧急迁移 size 大小内存到远端
  remote_numa_info <srcNid> <destNid> <sizeMiB>      设置远端 NUMA 迁移配额
  migrate <pid> <destNid> <ratio> <pidType>           按比例迁出进程内存
  migrate_sync <pid> <destNid> <ratio> <pidType> <maxWaitMs>   同步迁出
  migrate_grouped <pid> <pageType> <local> <target>  组级迁移（local/target 形如 0:1024,1:2048 KB）
  migrate_back <taskID> <entries>                     迁回（entries: src:dest:memid;...）
  remove <pageType> <entries>                         移除进程冷热迁移（entries: pid:nid1,nid2;...）
  node_enable <enable> <nid>                          使能/禁用 NUMA 冷热迁移
  process_tracking_add <scanType> <entries>           添加进程到冷热扫描（entries: pid:scanTime:duration;...）
  process_tracking_remove <flag> <pids>               移除进程冷热扫描（pids: 1,2,3）
  process_migrate_enable <enable> <flags> <pids>      使能进程页面迁移
  freq_query <pid> <length> <dataSource>              查询进程页面冷热频次
  run_mode <runMode>                                  设置运行模式（0=动态, 1=固定比例）
  is_running                                          查询 SMAP 是否运行
  remote_numa_migrate <srcNid> <destNid> <memids>     迁移远端内存（memids: 0x..,0x..）
  pid_remote_numa_migrate <entries>                   迁移进程远端内存（entries: pid:src:dest:ratio:memSize:mode;...）
  process_config_query <nid> <inLen>                   查询远端 NUMA 上的进程配置
  remote_numa_freq_query <nids>                        查询 NUMA 频次统计（nids: 0,1,2）
"""


def main():
    if len(sys.argv) < 2 or sys.argv[1] in ("-h", "--help"):
        print(USAGE, file=sys.stderr)
        sys.exit(0 if len(sys.argv) >= 2 else 1)

    cmd = sys.argv[1]
    a = sys.argv  # 简写
    try:
        if cmd == "start":
            sys.exit(0 if cmd_start(int(a[2])) == IPC_OK else 1)
        elif cmd == "stop":
            sys.exit(0 if cmd_stop() == IPC_OK else 1)
        elif cmd == "urgent_migrate":
            sys.exit(0 if cmd_urgent_migrate(int(a[2], 0)) == IPC_OK else 1)
        elif cmd == "remote_numa_info":
            sys.exit(0 if cmd_remote_numa_info(int(a[2]), int(a[3]), int(a[4])) == IPC_OK else 1)
        elif cmd == "migrate":
            sys.exit(0 if cmd_migrate(int(a[2]), int(a[3]), int(a[4]), int(a[5])) == IPC_OK else 1)
        elif cmd == "migrate_sync":
            sys.exit(0 if cmd_migrate_sync(int(a[2]), int(a[3]), int(a[4]),
                                            int(a[5]), int(a[6], 0)) == IPC_OK else 1)
        elif cmd == "migrate_grouped":
            sys.exit(0 if cmd_migrate_grouped(int(a[2]), int(a[3]), a[4], a[5]) == IPC_OK else 1)
        elif cmd == "migrate_back":
            sys.exit(0 if cmd_migrate_back(int(a[2], 0), a[3]) == IPC_OK else 1)
        elif cmd == "remove":
            sys.exit(0 if cmd_remove(int(a[2]), a[3]) == IPC_OK else 1)
        elif cmd == "node_enable":
            sys.exit(0 if cmd_node_enable(int(a[2]), int(a[3])) == IPC_OK else 1)
        elif cmd == "process_tracking_add":
            sys.exit(0 if cmd_process_tracking_add(int(a[2]), a[3]) == IPC_OK else 1)
        elif cmd == "process_tracking_remove":
            sys.exit(0 if cmd_process_tracking_remove(int(a[2]), a[3]) == IPC_OK else 1)
        elif cmd == "process_migrate_enable":
            sys.exit(0 if cmd_process_migrate_enable(int(a[2]), int(a[3]), a[4]) == IPC_OK else 1)
        elif cmd == "freq_query":
            sys.exit(0 if cmd_freq_query(int(a[2]), int(a[3]), int(a[4])) == IPC_OK else 1)
        elif cmd == "run_mode":
            sys.exit(0 if cmd_run_mode(int(a[2])) == IPC_OK else 1)
        elif cmd == "is_running":
            sys.exit(0 if cmd_is_running() == IPC_OK else 1)
        elif cmd == "remote_numa_migrate":
            sys.exit(0 if cmd_remote_numa_migrate(int(a[2]), int(a[3]), a[4]) == IPC_OK else 1)
        elif cmd == "pid_remote_numa_migrate":
            sys.exit(0 if cmd_pid_remote_numa_migrate(a[2]) == IPC_OK else 1)
        elif cmd == "process_config_query":
            sys.exit(0 if cmd_process_config_query(int(a[2]), int(a[3])) == IPC_OK else 1)
        elif cmd == "remote_numa_freq_query":
            sys.exit(0 if cmd_remote_numa_freq_query(a[2]) == IPC_OK else 1)
        else:
            print(f"Unknown command: {cmd}", file=sys.stderr)
            print(USAGE, file=sys.stderr)
            sys.exit(1)
    except (IndexError, ValueError) as e:
        print(f"Argument error: {e}", file=sys.stderr)
        print(USAGE, file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
