#!/usr/bin/env bash
#
# hotscan_run.sh — HotScan 一键采集 + 出图
#
# 按 README 的流程执行:
#   1) 检查环境(root / aarch64 / Python 依赖 / 目标进程行为)
#   2) 需要时关闭透明大页(脚本退出时恢复)
#   3) (可选)编译 hotscan.ko
#   4) insmod 加载模块并采集指定时长
#   5) rmmod 卸载模块(中断时也会自动卸载)
#   6) 调用 parse_smap.py 生成 img/<test_tag>/ 下的热区图
#
# 用法:
#   sudo ./hotscan_run.sh -p <PID> [-d 30] [-i 100] [-w 4] [-t TAG] [-s 20]
#
# 示例:
#   sudo ./hotscan_run.sh -p 12345 -d 30
#   sudo ./hotscan_run.sh -p 12345 -d 60 -i 500 -w 8 -t redis_run
#   sudo ./hotscan_run.sh -p 12345 -d 60 -t redis_run -s 5   # 5 轮合并成 1 个布尔周期
#
set -euo pipefail

HS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KO="$HS_DIR/hotscan.ko"
PARSER="$HS_DIR/parse_smap.py"
THP_FILE="/sys/kernel/mm/transparent_hugepage/enabled"

PID=""
DURATION=30          # 采集时长(秒)
INTERVAL=100         # scan_period_ms
WORKERS=4            # num_workers
TAG=""
SIM_PERIOD=20        # 出图合并步长；默认 20 轮 → 300 轮约 15 个周期
DO_BUILD=1
KEEP_THP=0
FORCE=0

usage() {
    cat <<'EOF'
HotScan 一键采集 + 出图

参数:
  -p, --pid <PID>        必填,目标进程 PID(必须是宿主进程,不能在虚机/容器内)
  -d, --duration <秒>    采集时长,默认 30
  -i, --interval <毫秒>  scan_period_ms,默认 100
  -w, --workers <数量>   num_workers(1-64),默认 4
  -t, --tag <名字>       test_tag / 输出目录名,默认按时间戳生成
  -s, --sim-period <N>   出图时每 N 个原始轮合并成 1 个布尔周期,默认 20(300 轮约 15 格;逐轮不合并请用 -s 1)
  -k, --keep-thp         不修改透明大页设置(默认会自动关闭并在结束时恢复)
  -B, --no-build         跳过编译(要求 hotscan.ko 已存在)
  -f, --force            允许覆盖已存在的 /tmp/<tag>;模块已加载时先卸载
  -h, --help             显示本帮助

示例:
  sudo ./hotscan_run.sh -p 12345 -d 30
  sudo ./hotscan_run.sh -p 12345 -d 60 -i 500 -w 8 -t redis_run
EOF
}

log()  { printf '[hotscan] %s\n' "$*"; }
warn() { printf '[hotscan][警告] %s\n' "$*" >&2; }
die()  { printf '[hotscan][错误] %s\n' "$*" >&2; exit 1; }

# ---------- 解析参数 ----------
while [ $# -gt 0 ]; do
    case "$1" in
        -p|--pid)        PID="${2:-}"; shift 2 ;;
        -d|--duration)   DURATION="${2:-}"; shift 2 ;;
        -i|--interval)   INTERVAL="${2:-}"; shift 2 ;;
        -w|--workers)    WORKERS="${2:-}"; shift 2 ;;
        -t|--tag)        TAG="${2:-}"; shift 2 ;;
        -s|--sim-period) SIM_PERIOD="${2:-}"; shift 2 ;;
        -k|--keep-thp)   KEEP_THP=1; shift ;;
        -B|--no-build)   DO_BUILD=0; shift ;;
        -f|--force)      FORCE=1; shift ;;
        -h|--help)       usage; exit 0 ;;
        *)               die "未知参数:$1(用 -h 查看帮助)" ;;
    esac
    if [ $# -gt 0 ] && [ "${1:-}" = "" ]; then
        die "参数缺少取值(用 -h 查看帮助)"
    fi
done

[ -n "$PID" ] || { usage; die "缺少 -p/--pid"; }

case "$PID" in
    ''|*[!0-9]*) die "target_pid 必须是正整数:$PID" ;;
esac
[ "$PID" -gt 0 ] || die "target_pid 必须大于 0"
case "$DURATION" in ''|*[!0-9]*) die "duration 必须是正整数秒:$DURATION" ;; esac
case "$INTERVAL" in ''|*[!0-9]*) die "interval 必须是正整数毫秒:$INTERVAL" ;; esac
case "$WORKERS"  in ''|*[!0-9]*) die "workers 必须是 1-64 的整数:$WORKERS" ;; esac
case "$SIM_PERIOD" in ''|*[!0-9]*) die "sim-period 必须是正整数:$SIM_PERIOD" ;; esac
[ "$DURATION" -gt 0 ] || die "duration 必须大于 0"
[ "$INTERVAL" -gt 0 ] || die "interval 必须大于 0"
[ "$WORKERS" -ge 1 ] && [ "$WORKERS" -le 64 ] || die "workers 必须在 1-64 之间"
[ "$SIM_PERIOD" -ge 1 ] || die "sim-period 必须大于等于 1"

# ---------- 基本环境 ----------
if [ "$(uname -s)" != "Linux" ]; then
    die "本脚本只能在 Linux 上运行"
fi
[ "$(uname -m)" = "aarch64" ] || warn "当前架构是 $(uname -m),本模块只支持 aarch64"

if [ "$(id -u)" -eq 0 ]; then
    SUDO=""
else
    SUDO="sudo"
    command -v sudo >/dev/null 2>&1 || die "需要 root 权限,且系统没有 sudo"
fi

INSMOD="$(command -v insmod 2>/dev/null || true)"
RMMOD="$(command -v rmmod 2>/dev/null || true)"
[ -n "$INSMOD" ] || INSMOD="/sbin/insmod"
[ -n "$RMMOD" ]  || RMMOD="/sbin/rmmod"
[ -x "$INSMOD" ] || die "找不到 insmod(需要内核模块工具)"
[ -x "$RMMOD" ]  || die "找不到 rmmod"

command -v python3 >/dev/null 2>&1 || die "缺少 python3"
python3 - <<'PY' >/dev/null 2>&1 || { warn "缺少 Python 依赖,请执行:python3 -m pip install numpy matplotlib"; exit 1; }
import numpy
import matplotlib
PY

[ -d "/proc/$PID" ] || die "进程不存在:/proc/$PID(目标进程必须先启动)"
proc_name="$(awk '/^Name:/ {print $2; exit}' "/proc/$PID/status" 2>/dev/null || true)"
[ -n "$proc_name" ] && log "目标进程:pid=$PID name=$proc_name"

# ---------- 输出目录与 tag ----------
if [ -z "$TAG" ]; then
    TAG="hotscan_$(date +%Y%m%d_%H%M%S)"
fi
# 目录名只保留安全字符(避免 / 和空格)
TAG="$(printf '%s' "$TAG" | sed -E 's/[^A-Za-z0-9._-]/_/g')"
[ -n "$TAG" ] || die "test_tag 为空"
OUT_DIR="/tmp/$TAG"

if [ -e "$OUT_DIR" ]; then
    if [ "$FORCE" -eq 1 ]; then
        log "清理已存在的输出目录:$OUT_DIR"
        $SUDO rm -rf "$OUT_DIR"
    else
        die "输出目录已存在:$OUT_DIR(换一个 -t,或加 -f 覆盖)"
    fi
fi

# ---------- 模块加载状态 ----------
module_loaded() {
    grep -q '^hotscan ' /proc/modules 2>/dev/null
}

if module_loaded; then
    if [ "$FORCE" -eq 1 ]; then
        log "模块已加载,先卸载"
        $SUDO "$RMMOD" hotscan
    else
        die "hotscan 模块已加载,请先 rmmod hotscan(或加 -f 让脚本卸载)"
    fi
fi

# ---------- 编译 ----------
if [ "$DO_BUILD" -eq 1 ]; then
    need_build=0
    [ -f "$KO" ] || need_build=1
    [ -f "$HS_DIR/hotscan_main.c" ] && [ "$HS_DIR/hotscan_main.c" -nt "$KO" ] && need_build=1
    [ -f "$HS_DIR/Makefile" ] && [ "$HS_DIR/Makefile" -nt "$KO" ] && need_build=1
    if [ "$need_build" -eq 1 ]; then
        log "编译内核模块"
        make -C "$HS_DIR"
    else
        log "复用已有模块:$KO"
    fi
fi
[ -f "$KO" ] || die "找不到 $KO(先执行 make,或去掉 -B)"

# ---------- 透明大页 ----------
THP_RESTORE=""
if [ -r "$THP_FILE" ]; then
    thp_cur="$(sed -n 's/.*\[\(.*\)\].*/\1/p' "$THP_FILE")"
    if [ -z "$thp_cur" ]; then
        warn "无法解析透明大页状态,跳过自动设置"
    elif [ "$thp_cur" != "never" ]; then
        if [ "$KEEP_THP" -eq 1 ]; then
            warn "透明大页当前为 [$thp_cur];模块按 4 KB PTE 扫描,建议关闭"
        else
            log "关闭透明大页(原值 [$thp_cur],脚本结束时恢复)"
            echo never | $SUDO tee "$THP_FILE" >/dev/null
            THP_RESTORE="$thp_cur"
        fi
    fi
else
    warn "读不到 $THP_FILE;若目标使用透明大页,结果会出现整块大页"
fi

# ---------- 退出清理 ----------
MODULE_INSMODED=0
cleanup() {
    set +e
    if [ "$MODULE_INSMODED" -eq 1 ]; then
        log "卸载模块"
        $SUDO "$RMMOD" hotscan 2>/dev/null
        MODULE_INSMODED=0
    fi
    if [ -n "$THP_RESTORE" ]; then
        log "恢复透明大页为 [$THP_RESTORE]"
        echo "$THP_RESTORE" | $SUDO tee "$THP_FILE" >/dev/null 2>&1
        THP_RESTORE=""
    fi
}
trap cleanup EXIT
trap 'exit 130' INT TERM

# ---------- 采集 ----------
expected_rounds=$(( DURATION * 1000 / INTERVAL ))
[ "$expected_rounds" -gt 0 ] || die "采集时长不足以产生 1 轮(duration*1000 < interval)"

log "加载模块:pid=$PID period=${INTERVAL}ms tag=$TAG workers=$WORKERS"
$SUDO "$INSMOD" "$KO" \
    target_pid="$PID" \
    scan_period_ms="$INTERVAL" \
    test_tag="$TAG" \
    num_workers="$WORKERS"
MODULE_INSMODED=1

if ! module_loaded; then
    die "模块未加载成功,请查看:dmesg | tail -20"
fi
log "开始采集 ${DURATION}s(预计 ${expected_rounds} 轮),Ctrl-C 会提前结束并自动卸载"

sleep "$DURATION"

log "卸载模块(等待最后几轮写入完成)"
$SUDO "$RMMOD" hotscan
MODULE_INSMODED=0

# ---------- 采集结果检查 ----------
shopt -s nullglob
files=( "$OUT_DIR"/scan_*.out )
shopt -u nullglob
[ "${#files[@]}" -gt 0 ] || die "没有生成扫描文件:$OUT_DIR/scan_*.out(检查 dmesg | grep hotscan)"

round_count="${#files[@]}"
first_round="$(printf '%s\n' "${files[@]}" | sed -E 's/.*scan_([0-9]+)\.out/\1/' | sort -n | head -1)"
last_round="$(printf '%s\n' "${files[@]}" | sed -E 's/.*scan_([0-9]+)\.out/\1/' | sort -n | tail -1)"
span=$(( last_round - first_round + 1 ))
skipped=$(( span - round_count ))

log "采集完成:${round_count} 轮(scan_${first_round} → scan_${last_round},预计 ${expected_rounds} 轮)"
if [ "$skipped" -gt 0 ]; then
    warn "存在 ${skipped} 个缺号轮次(上一轮未完成时触发会被跳过);有效周期可能大于标称值"
fi
if [ "$round_count" -lt "$(( expected_rounds * 8 / 10 ))" ]; then
    warn "实际轮数明显少于预计;可降低 console 日志级别后重采(见 README)"
fi

# ---------- 出图 ----------
log "生成热区图(img/$TAG/)"
cd "$HS_DIR"
if [ "$SIM_PERIOD" -gt 1 ]; then
    python3 "$PARSER" "$OUT_DIR/scan_*.out" zic --plot --sim-scan-period "$SIM_PERIOD"
else
    python3 "$PARSER" "$OUT_DIR/scan_*.out" zic --plot
fi

# ---------- 汇总 ----------
img_dir="$HS_DIR/img/$TAG"
echo
log "===== 完成 ====="
log "扫描数据:$OUT_DIR/(${round_count} 个 scan_*.out)"
log "图片目录:$img_dir/"
if [ -d "$img_dir" ]; then
    for f in "$img_dir"/*.png; do
        [ -e "$f" ] && log "  $(basename "$f")"
    done
fi
log "内核日志:dmesg | grep hotscan"
log "如需只做统计不出图,去掉 --plot 再执行解析脚本即可"
