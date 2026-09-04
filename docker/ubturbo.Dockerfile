# UBTurbo 构建依赖与完整样例运行验证镜像
#
# 基础镜像与宿主机 OS 版本一致（openEuler 24.03 LTS-SP3，aarch64）。
# 已预装 UBTurbo 全部构建依赖（与 ubturbo.spec BuildRequires 一致）以及
# SMAP 构建运行依赖（kernel-devel/kmod/bc/ubturbo用户），并内置 UBTurbo
# Release 编译产物与完整样例验证脚本。
#
# 用途1：构建/UT 环境镜像——挂载源码进入编译
#   docker build -f docker/ubturbo.Dockerfile -t ubturbo-env:sp3 .
#   docker run --rm -it -v $(pwd):/workspace -w /workspace ubturbo-env:sp3
#   容器内执行：dos2unix build.sh && sh build.sh（UT：sh build.sh -t test）
#
# 用途2：完整样例运行验证——SMAP 内核模块(.ko)在容器启动时针对"宿主机内核
# 版本"构建（容器与宿主共享内核，uname -r 即宿主内核版本），因此必须以特权
# 模式启动，并挂载宿主机内核开发目录（<KVER> 为宿主机 `uname -r`）：
#   sudo docker run -d --privileged \
#     -v /usr/src/kernels/<KVER>:/usr/src/kernels/<KVER>:ro \
#     -v /lib/modules/<KVER>:/lib/modules/<KVER>:ro \
#     --name ttfhw-ubturbo-full ubturbo-env:sp3 sleep infinity
#   完整样例验证（容器内执行）：
#     docker exec ttfhw-ubturbo-full /opt/verify/run_verify.sh
#
# SELinux 说明（openEuler 24.03 默认 enforcing）：
#   容器进程域对非常规文件系统（overlayfs/tmpfs）的 module_load 被策略拒绝
#   （audit 日志：AVC denied { module_load } tclass=system），容器内 insmod 将失败。
#   二选一：
#   a) 验证期间临时 setenforce 0，验证后 setenforce 1 还原（容器内全自动）；
#   b) 宿主机按文档顺序 insmod 四个模块后重跑 run_verify.sh（推荐生产形态，
#      脚本检测到模块已加载会自动跳过 insmod 步骤）。
ARG BASE_IMAGE=hub.oepkgs.net/openeuler/openeuler:24.03-lts-sp3
FROM ${BASE_IMAGE}

# 包源修复：官方源 update 仓库 repomd.xml 校验异常（所需软件包均在 OS/everything 中），
# 重写为官方 OS/everything/EPOL 三源并剔除 update 段
# 依赖说明：
#   - UBTurbo 构建：make gcc gcc-c++ cmake ninja-build dos2unix chrpath patchelf findutils git
#     libboundscheck（securec.h）libvirt-devel（RMRS插件）bc（build.sh 计时）
#   - SMAP 构建/运行：kernel-devel（文档声明依赖，.ko 实际构建使用挂载的宿主机内核头，
#     保证 vermagic 与宿主内核严格一致）、kmod（insmod/rmmod）、ubturbo 用户（SMAP 配置属主）
RUN printf '%s\n' \
        '[OS]' 'name=OS' 'baseurl=https://repo.openeuler.org/openEuler-24.03-LTS-SP3/OS/$basearch/' 'enabled=1' 'gpgcheck=0' \
        '[everything]' 'name=everything' 'baseurl=https://repo.openeuler.org/openEuler-24.03-LTS-SP3/everything/$basearch/' 'enabled=1' 'gpgcheck=0' \
        '[EPOL]' 'name=EPOL' 'baseurl=https://repo.openeuler.org/openEuler-24.03-LTS-SP3/EPOL/main/$basearch/' 'enabled=1' 'gpgcheck=0' \
        > /etc/yum.repos.d/openEuler.repo \
    && dnf clean all && dnf makecache \
    && dnf install -y \
        make gcc gcc-c++ cmake ninja-build dos2unix \
        chrpath patchelf findutils git bc \
        libboundscheck libvirt-devel \
        kernel-devel \
        kmod \
    && dnf clean all && rm -rf /var/cache/dnf \
    && useradd -m -U ubturbo \
    && mkdir -p /opt/ubturbo /var/log/ubturbo /opt/verify

# 拷贝源码（context 根的 .dockerignore 已排除 .git/dist/test 等无关大目录）；
# 用途1使用时以 -v $(pwd):/workspace 挂载会覆盖此目录
WORKDIR /workspace
COPY CMakeLists.txt build.sh ubturbo.spec .gitmodules ./
COPY src ./src
COPY include ./include
COPY conf ./conf
COPY build ./build
COPY 3rdparty ./3rdparty
COPY plugins/rmrs ./plugins/rmrs
COPY plugins/smap ./plugins/smap

# 构建 UBTurbo 主框架（Release，Ninja；并发限制为容器核数的 60%）
RUN dos2unix build.sh \
    && JOBS=$(( $(nproc) * 6 / 10 )) \
    && sh build.sh -j ${JOBS} \
    && ls -l dist/release/bin/ub_turbo_exec dist/release/lib/

# 完整样例验证脚本（特权容器内执行 /opt/verify/run_verify.sh）
COPY docker/run_verify.sh /opt/verify/run_verify.sh

CMD ["/bin/bash"]
