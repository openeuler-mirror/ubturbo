### 栈符号化（Level 1：地址 → "模块+偏移"）
### 内核栈：启动时加载一次 /proc/kallsyms，二分查找
### 用户栈：读取 /proc/<pid>/maps，找地址所属映射
### 解析失败一律回退原始地址，不影响采集主流程

import bisect
import os


class KernelSymbols:
    """内核符号表（kallsyms）。

    collector 启动时 load 一次（约几万行，亚秒级），
    之后每次内核地址查询都是二分查找（O(log n)）。
    """

    def __init__(self):
        self._addrs = []     # 排序后的符号起始地址列表
        self._names = []    # 与 _addrs 一一对应的符号名
        self._loaded = False

    def load(self, kallsyms_path="/proc/kallsyms"):
        """加载 kallsyms。失败（无权限/文件不存在）时置空，查询回退原地址。"""
        addrs = []
        names = []

        try:
            with open(kallsyms_path, "r", errors="replace") as f:
                for line in f:
                    parts = line.split(maxsplit=2)
                    if len(parts) < 3:
                        continue
                    # 只要代码段符号（T/t 文本段，W/w 弱符号）
                    if parts[1] not in ("T", "t", "W", "w"):
                        continue
                    try:
                        addrs.append(int(parts[0], 16))
                    except ValueError:
                        continue
                    names.append(parts[2].strip())

        except OSError:
            # 常见原因：非 root 读 kallsyms 显示全 0 地址，或文件不存在
            addrs = []
            names = []

        # 同一地址可能有多个符号，排序后保序即可
        pairs = sorted(zip(addrs, names))
        self._addrs = [a for a, _ in pairs]
        self._names = [n for _, n in pairs]
        self._loaded = True

        return len(self._addrs)

    def resolve(self, addr):
        """返回 'symbol+0x偏移'；查不到返回 None。"""
        if not self._loaded or not self._addrs:
            return None

        idx = bisect.bisect_right(self._addrs, addr) - 1
        if idx < 0:
            return None

        base = self._addrs[idx]
        offset = addr - base

        if offset == 0:
            return self._names[idx]
        return f"{self._names[idx]}+0x{offset:x}"


def parse_maps(maps_text):
    """解析 /proc/<pid>/maps 文本为 [(start, end, pathname)] 列表。"""
    regions = []

    for line in maps_text.splitlines():
        # 格式：start-end perms offset dev inode pathname
        fields = line.split(maxsplit=5)
        if len(fields) < 2:
            continue

        try:
            start, end = (int(x, 16) for x in fields[0].split("-"))
        except ValueError:
            continue

        pathname = fields[5].strip() if len(fields) == 6 else ""
        regions.append((start, end, pathname))

    return regions


def resolve_user_addr(addr, regions):
    """在 maps 区间中查找地址，返回 '文件名+0x偏移'；查不到返回 None。"""
    for start, end, pathname in regions:
        if start <= addr < end:
            label = pathname if pathname else "anon"
            offset = addr - start
            return f"{label}+0x{offset:x}"

    return None


def resolve_user_stack(stack, pid):
    """符号化一个用户栈（十六进制字符串地址列表）。

    每次读取该进程的 /proc/<pid>/maps——必须实时读：
    进程存活期间映射会变，且 report 时进程可能已退出。
    读不到 maps（进程退出/权限）时全部回退原地址。
    """
    if not stack:
        return list(stack) if stack else []

    try:
        with open(f"/proc/{pid}/maps", "r", errors="replace") as f:
            regions = parse_maps(f.read())
    except OSError:
        regions = []

    resolved = []
    for frame in stack:
        try:
            addr = int(frame, 16)
        except (TypeError, ValueError):
            resolved.append(frame)
            continue

        symbol = resolve_user_addr(addr, regions)
        resolved.append(symbol if symbol else frame)

    return resolved


def resolve_kernel_stack(stack, kernel_symbols):
    """符号化一个内核栈。kernel_symbols 为 KernelSymbols 实例。"""
    resolved = []

    for frame in stack:
        try:
            addr = int(frame, 16)
        except (TypeError, ValueError):
            resolved.append(frame)
            continue

        symbol = kernel_symbols.resolve(addr)
        resolved.append(symbol if symbol else frame)

    return resolved
