# UBTurbo 构建依赖镜像
# 预装全部构建依赖（与 ubturbo.spec BuildRequires 一致），用于源码编译/UT，
# 避免每次进入环境重复安装工具链。
# 使用方法见 doc/ubturbo_installation.md "（可选）容器镜像部署" 章节。
ARG BASE_IMAGE=openeuler/openeuler:24.03-lts
FROM ${BASE_IMAGE}

RUN dnf install -y \
        make gcc gcc-c++ cmake ninja-build dos2unix \
        chrpath patchelf findutils git \
        libboundscheck libvirt-devel \
    && dnf clean all \
    && rm -rf /var/cache/dnf

WORKDIR /workspace
CMD ["/bin/bash"]
