import os
import signal
import subprocess
import sys
import time

# 路径锚定到本文件所在目录，从任意 CWD 运行均落到工具目录下
TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
PID_FILE = os.path.join(TOOLS_DIR, "runtime", "memdiag.pid")
LOG_FILE = os.path.join(TOOLS_DIR, "logs", "memdiag.log")

# stop 发出 SIGTERM 后的等待时长。
# collector 优雅退出依赖 perf_buffer_poll(1000ms) 返回，最长约 1s 才能感知信号，
# 因此这里等待 2s 覆盖边界情况。
STOP_WAIT_SEC = 2.0


def _read_pid(pid_file=PID_FILE):
    """读取 PID 文件，返回 int；文件不存在/内容非法返回 None"""
    if not os.path.exists(pid_file):
        return None

    try:
        with open(pid_file, "r") as f:
            return int(f.read().strip())
    except (ValueError, OSError):
        return None


def _is_alive(pid):
    """信号 0 探活：进程存在且可发信号返回 True"""
    try:
        os.kill(pid, 0)
        return True
    except ProcessLookupError:
        return False
    except PermissionError:
        # 进程存在但属于其他用户（非 root 场景），仍视为存活
        return True
    except OSError:
        return False


def _is_memdiag_process(pid):
    """校验目标进程确为本工具拉起的采集进程，防止 PID 复用后误杀。

    "collector" 是高频普通词（grep collector、其它项目的 collector.py 等），
    仅凭它判定过宽；此处要求命令行同时含本工具独有的 HugePageCollector
    代码串与 sys.executable 双重校验。读不到 cmdline 时无法确认归属，
    拒绝 kill 交由人工确认（比放行 kill 更安全）。
    """
    try:
        with open(f"/proc/{pid}/cmdline", "rb") as f:
            cmdline = f.read().replace(b"\x00", b" ").decode("utf-8", "replace")
        return "HugePageCollector" in cmdline and sys.executable in cmdline
    except OSError:
        # 读不到 cmdline：无法确认归属，拒绝 kill，交由人工确认
        return False


def start_daemon():
    os.makedirs(os.path.join(TOOLS_DIR, "runtime"), exist_ok=True)
    os.makedirs(os.path.join(TOOLS_DIR, "logs"), exist_ok=True)

    if os.path.exists(PID_FILE):
        pid = _read_pid()

        if pid is not None and _is_alive(pid):
            print("memdiag daemon is already running.")
            print(f"PID file: {PID_FILE}")
            return

        # PID 文件存在但进程已死（僵尸文件）——自动清理后继续启动
        print("Stale PID file found (daemon not running), removing.")
        os.remove(PID_FILE)

    with open(LOG_FILE, "a") as log:
        proc = subprocess.Popen(
            [sys.executable, "-c", "from collector import HugePageCollector; HugePageCollector().run()"],
            cwd=TOOLS_DIR,        # 锚定 import 与运行时相对路径
            stdout=log,
            stderr=log,
            start_new_session=True,
        )

    with open(PID_FILE, "w") as f:
        f.write(str(proc.pid))

    print("Monitoring started.")
    print(f"PID: {proc.pid}")
    print(f"Log: {LOG_FILE}")
    print("Use: sudo python3 memdiag.py report")


def stop_daemon():
    pid = _read_pid()

    if pid is None:
        if os.path.exists(PID_FILE):
            os.remove(PID_FILE)
        print("memdiag daemon is not running.")
        return

    if not _is_alive(pid):
        print("Daemon process does not exist.")
        if os.path.exists(PID_FILE):
            os.remove(PID_FILE)
        return

    # 防误杀：PID 文件可能是陈旧的，目标进程已被内核复用
    if not _is_memdiag_process(pid):
        print(f"PID {pid} is not a memdiag collector process, refusing to kill.")
        print("Please verify manually and remove the PID file if appropriate:")
        print(f"  rm {PID_FILE}")
        return

    try:
        os.kill(pid, signal.SIGTERM)
        time.sleep(STOP_WAIT_SEC)
        print("Monitoring stopped.")
    except ProcessLookupError:
        print("Daemon process does not exist.")
    finally:
        if os.path.exists(PID_FILE):
            os.remove(PID_FILE)


def show_status():
    pid = _read_pid()

    if pid is None:
        print("memdiag daemon is not running.")
        return

    if _is_alive(pid):
        print("memdiag daemon is running.")
        print(f"PID: {pid}")
    else:
        print("PID file exists, but daemon process is not running.")
