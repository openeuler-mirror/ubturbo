#!/bin/bash
# UBTurbo + SMAP 完整样例运行验证脚本（在 --privileged 特权容器内执行）
# 流程：SMAP 内核模块构建 -> 安装 -> insmod -> 运行 ub_turbo_exec -> 结果判定 -> 清理
set -e
T0=$SECONDS
step() { echo "[$((SECONDS-T0))s] === $* ==="; }

step "1/8 环境检查：容器内核=宿主内核，内核开发目录就绪"
KVER=$(uname -r)
echo "    宿主机内核版本: ${KVER}"
cat /etc/openEuler-release
KDIR="/lib/modules/${KVER}/build"
if [ ! -e "${KDIR}/Makefile" ]; then
    echo "[FATAL] ${KDIR} 无效。请在 docker run 时挂载宿主机内核头："
    echo "        -v /usr/src/kernels/${KVER}:/usr/src/kernels/${KVER}:ro"
    echo "        -v /lib/modules/${KVER}:/lib/modules/${KVER}:ro"
    exit 1
fi
echo "    内核开发目录就绪: ${KDIR}"
if lsmod | grep -q smap; then
    echo "    [WARN] 内核已存在 smap 模块（宿主侧预加载或其它会话遗留），将跳过步骤5的 insmod 直接复用"
fi

step "2/8 构建 SMAP 内核模块与用户态库（按 smap.spec %build 流程）"
cd /workspace/plugins/smap
make -C src/drivers KERNEL_VERSION=openeuler -j"$(nproc)" > /tmp/smap_build.log 2>&1 \
    || { tail -20 /tmp/smap_build.log; exit 1; }
cp -f src/drivers/Module.symvers src/tiering/depends
make -C src/tiering KERNEL_VERSION=openeuler -j"$(nproc)" >> /tmp/smap_build.log 2>&1 \
    || { tail -20 /tmp/smap_build.log; exit 1; }
make -C src/ucache -j"$(nproc)" >> /tmp/smap_build.log 2>&1 \
    || { tail -20 /tmp/smap_build.log; exit 1; }
cmake -DCMAKE_BUILD_TYPE=Release . > /tmp/smap_cmake.log 2>&1 \
    && make -j"$(nproc)" install >> /tmp/smap_cmake.log 2>&1 \
    || { tail -20 /tmp/smap_cmake.log; exit 1; }
echo "    .ko vermagic: $(modinfo -F vermagic src/drivers/smap_tracking_core.ko)"

step "3/8 安装 SMAP 产物（按 smap.spec %install 布局）"
install -D -m 0500 src/drivers/smap_tracking_core.ko      /lib/modules/smap/smap_tracking_core.ko
install -D -m 0500 src/drivers/smap_histogram_tracking.ko /lib/modules/smap/smap_histogram_tracking.ko
install -D -m 0500 src/drivers/smap_access_tracking.ko    /lib/modules/smap/smap_access_tracking.ko
install -D -m 0500 src/tiering/smap_tiering.ko            /lib/modules/smap/smap_tiering.ko
install -D -m 0500 src/ucache/ucache.ko                   /lib/modules/ucache/ucache.ko
install -D -m 0500 output/smap/lib/libsmap.so             /usr/lib64/libsmap.so
install -D -m 0640 99-smap.rules                          /etc/udev/rules.d/99-smap.rules
echo "    $(ls /lib/modules/smap/ | tr '\n' ' ')"

step "4/8 按文档前置条件清理 /dev/shm 残留状态"
rm -f /dev/shm/smap_config /dev/shm/ubturbo_page_type.dat
echo "    done"

step "5/8 加载 SMAP 内核模块（文档规定顺序，不可调整）"
if lsmod | grep -q smap_tracking_core; then
    echo "    内核模块已由宿主侧加载（SELinux enforcing 环境的推荐方式），跳过容器内 insmod"
else
    cd /lib/modules/smap
    if ! insmod smap_tracking_core.ko; then
        echo "[FAIL] 容器内 insmod 被拒绝（SELinux enforcing 下容器进程域无 module_load 权限，"
        echo "       见 audit 日志 AVC denied { module_load }）。"
        echo "       处理方式二选一："
        echo "       a) 临时 setenforce 0 后重跑本脚本（验证完毕还原 setenforce 1）；"
        echo "       b) 在宿主机按文档顺序 insmod 四个模块后重跑本脚本（推荐，生产形态）："
        echo "          cd /lib/modules/smap && insmod smap_tracking_core.ko && \"
        echo "          insmod smap_histogram_tracking.ko && insmod smap_access_tracking.ko && \"
        echo "          insmod smap_tiering.ko smap_pgsize=1"
        exit 1
    fi
    insmod smap_histogram_tracking.ko
    insmod smap_access_tracking.ko
    insmod smap_tiering.ko smap_pgsize=1
fi
echo "    /proc/modules: $(grep -c smap /proc/modules) 个 smap 模块已加载"
echo "    设备节点: $(ls /dev/ | grep -c smap) 个（/dev/smap_device 等）"

step "6/8 启动 UBTurbo 样例 ub_turbo_exec"
cd /workspace/dist/release/bin
chmod +x ub_turbo_exec
nohup ./ub_turbo_exec > /var/log/ubturbo/stdout.log 2>&1 &
UPID=$!
echo "    pid=${UPID}"

step "7/8 等待 IPC socket 就绪（/opt/ubturbo/ubturbo_ipc，最多30秒）"
SOCKET_OK=0
for i in $(seq 1 30); do
    if [ -S /opt/ubturbo/ubturbo_ipc ]; then SOCKET_OK=1; break; fi
    kill -0 "${UPID}" 2>/dev/null || break
    sleep 1
done
sleep 2
PASS=1
kill -0 "${UPID}" 2>/dev/null || { echo "[FAIL] ub_turbo_exec 进程已退出"; PASS=0; }
[ "${SOCKET_OK}" = 1 ] && echo "    IPC socket 就绪: /opt/ubturbo/ubturbo_ipc" || { echo "[FAIL] IPC socket 未就绪"; PASS=0; }
if grep -q "Start module failed" /var/log/ubturbo/stdout.log; then
    echo "[FAIL] 存在模块启动失败"; PASS=0
fi
grep -E "StartModule end|Start module failed" /var/log/ubturbo/stdout.log | head -5
lsmod | grep smap || true

step "8/8 清理：停止服务并卸载内核模块（恢复内核干净状态）"
if [ "${PASS}" = 1 ]; then
    kill "${UPID}" 2>/dev/null && sleep 3 || true
fi
rmmod smap_tiering 2>/dev/null || true; rmmod smap_access_tracking 2>/dev/null || true
rmmod smap_histogram_tracking 2>/dev/null || true; rmmod smap_tracking_core 2>/dev/null || true
LEFT=$(grep -c smap /proc/modules || true)
echo "    /proc/modules 残留 smap 模块: ${LEFT}（非 0 表示模块被占用，请检查是否有进程打开 /dev/smap* 设备）"

echo "==============================================="
if [ "${PASS}" = 1 ]; then
    echo "验证结果: PASS —— 容器方式完整样例运行验证通过（总耗时 $((SECONDS-T0))s）"
else
    echo "验证结果: FAIL（总耗时 $((SECONDS-T0))s）"
    tail -30 /var/log/ubturbo/stdout.log 2>/dev/null
    tail -30 /var/log/ubturbo/ubturbo.log 2>/dev/null
    exit 1
fi
