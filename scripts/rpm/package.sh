#!/bin/bash
#
# 基于 ubturbo.spec 通过 rpmbuild 构建 RPM 包。
#
# 用法:
#   package.sh [ubturbo|ubturbo-rmrs]
#
#   ubturbo        仅收集主包 ubturbo-*.rpm 到 output/（默认）
#   ubturbo-rmrs   仅收集子包 ubturbo-rmrs-*.rpm 到 output/
#
# 说明: rpmbuild -bb 会一次性构建 spec 中定义的全部二进制包（ubturbo 与
#       ubturbo-rmrs），本脚本按传入的目标包名将对应 RPM 收集到 output/。
#
set -e

SCRIPTS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
PROJECT_DIR="$(dirname "$(dirname "$SCRIPTS_DIR")")"
PROJECT_BASENAME="$(basename "$PROJECT_DIR")"

SPEC_NAME="ubturbo"
SOURCE_PACKAGE="${SPEC_NAME}.tar.gz"
SPEC_FILE="${HOME}/rpmbuild/SPECS/${SPEC_NAME}.spec"

# 目标 RPM 包名：ubturbo（主包）或 ubturbo-rmrs（子包），默认 ubturbo
TARGET_PACKAGE="${1:-ubturbo}"
case "$TARGET_PACKAGE" in
    ubturbo)
        # 主包文件名形如 ubturbo-<version>-<release>.aarch64.rpm，
        # 用 ubturbo-[0-9]* 排除 ubturbo-rmrs 等子包
        RPM_GLOB="${SPEC_NAME}-[0-9]*.rpm"
        ;;
    ubturbo-rmrs)
        RPM_GLOB="${TARGET_PACKAGE}-*.rpm"
        ;;
    *)
        echo "Error: unknown package target '$TARGET_PACKAGE' (expected 'ubturbo' or 'ubturbo-rmrs')" >&2
        exit 1
        ;;
esac

mkdir -p "${HOME}"/rpmbuild/{BUILD,RPMS,SOURCES,SPECS,SRPMS}

# 打包源码（排除构建产物与版本控制目录）
cd "$(dirname "$PROJECT_DIR")"
tar -czf "$SOURCE_PACKAGE" \
    --exclude="$PROJECT_BASENAME/.git" \
    --exclude="$PROJECT_BASENAME/dist" \
    --exclude="$PROJECT_BASENAME/output" \
    "$PROJECT_BASENAME"
mv "$SOURCE_PACKAGE" "${HOME}"/rpmbuild/SOURCES/

# 准备 spec：对齐源码目录名与 tar 包名，并注释掉 BuildRequires（依赖由本地环境提供）
cp "$PROJECT_DIR/$SPEC_NAME.spec" "${HOME}"/rpmbuild/SPECS/
sed -i "s/%{name}-%{version}.tar.gz/$SOURCE_PACKAGE/" "$SPEC_FILE"
sed -i "s/%global build_subdir %{name}-%{version}/%global build_subdir $PROJECT_BASENAME/" "$SPEC_FILE"
sed -i "s/^BuildRequires/#BuildRequires/" "$SPEC_FILE"

# 清理旧的目标 RPM，避免误收集历史产物
find "${HOME}"/rpmbuild/RPMS/ -type f -name "$RPM_GLOB" -delete 2>/dev/null || true

rpmbuild -bb --clean "$SPEC_FILE"

# 收集目标 RPM 到 output/
mkdir -p "$PROJECT_DIR/output"
find "$PROJECT_DIR/output" -maxdepth 1 -type f -name "$RPM_GLOB" -delete 2>/dev/null || true
cp -p "${HOME}"/rpmbuild/RPMS/*/$RPM_GLOB "$PROJECT_DIR/output/"

echo "RPM package(s) built for target '$TARGET_PACKAGE':"
ls -1 "$PROJECT_DIR"/output/$RPM_GLOB
