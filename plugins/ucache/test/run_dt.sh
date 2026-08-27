#!/bin/bash
# ***********************************************************************
# Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
# Description: smap run dt script
# ***********************************************************************

CURRENT_PATH=$(cd "$(dirname "$0")"; pwd)
echo "${CURRENT_PATH:?}"
cd "${CURRENT_PATH:?}"
code_dir=$(cd ${CURRENT_PATH}/../ && pwd)

# 默认使用本系统的kernel path
kernel_dir="/lib/modules/$(uname -r)/build"

# 脚本参数解析
function parse_args() {
    trans_params=()
    local trans_flag=false
    while [[ $# -gt 0 ]]; do
        case "$1" in
        -c | --clean)
            if [ -d "build" ]; then
                echo "rm test build directory"
                rm -rf ${CURRENT_PATH}/build/
            fi
            shift
            ;;
        -k | --kernel-dir)
            if [[ $# -gt 1 && "$2" != "-"* ]]; then
                kernel_dir="$2"
                shift 2
            else
                log_info "Error: Argument required after -k|--kernel-dir."
                exit 1
            fi
            ;;
        esac
    done
}

remove_static()
{
    local dir=$1
    find ${dir} -type f -name "*.c" | xargs -i sed -i "s/^static \b//g" {}
    find ${dir} -type f -name "*.cpp" | xargs -i sed -i "s/^static \b//g" {}
}

replace_log_header()
{
    local dir=$1
    src_files=$(find ${dir} -type f \( -name "*.cpp" -o -name "*.h" \))
    for file in $src_files; do
        sed -i 's/"turbo_logger\.h\s*"/"turbo_logger_mock.h"/g' $file
    done
}

parse_args "$@" # 解析脚本参数
rm -rf ${code_dir}/test/build
mkdir -p ${code_dir}/test/build/src
cp -r ${code_dir}/src ${code_dir}/test/build

remove_static ${code_dir}/test/build/src
replace_log_header ${code_dir}/test/build/src

cp -r $CURRENT_PATH/3rdparty/mockcpp_support_arm64.patch $CURRENT_PATH/3rdparty/mockcpp
cd $CURRENT_PATH/3rdparty/mockcpp
dos2unix src/UnixCodeModifier.cpp
git apply mockcpp_support_arm64.patch
cd $CURRENT_PATH/build

cmake .. -DENABLE_TEST=OFF
cmake --build . -j$(nproc)
# mockcpp 依赖 4K 内存页对齐（mprotect 修改代码段页属性实现打桩），
# 非 4K 页环境（如鲲鹏 64K 页）下 mprotect 失败会导致测试进程崩溃。
# 此处检测页大小，不兼容时跳过 UT 执行，保证整体流程正常完成，
# 详见 doc/3rdparty_open_source_components.md（2.2 mockcpp 兼容性限制）。
UT_PAGE_SIZE="${MOCKCPP_PAGE_SIZE_OVERRIDE:-$(getconf PAGESIZE 2>/dev/null || echo 4096)}"
if [ "${UT_PAGE_SIZE}" != "4096" ]; then
    echo "[SKIP] 当前系统内存页大小为 ${UT_PAGE_SIZE}（非 4K），mockcpp 不兼容，跳过 UT 执行，不影响整体流程；详见 doc/3rdparty_open_source_components.md"
    exit 0
fi

./ucache_dt

mkdir -p $CURRENT_PATH/build/gcovr_report
lcov --d ./ --c --output-file test.info --rc lcov_branch_coverage=1
lcov -e test.info "*/src/*" -output-file coverage.info --rc lcov_branch_coverage=1
genhtml -o gcovr_report coverage.info --show-details --legend --rc lcov_branch_coverage=1
