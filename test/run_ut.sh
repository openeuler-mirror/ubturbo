#!/bin/bash
 
CURRENT_PATH=$(cd "$(dirname "$0")"; pwd)
echo "${CURRENT_PATH:?}"
cd "${CURRENT_PATH:?}"
code_dir=$(cd ${CURRENT_PATH}/../ && pwd)

# 设置时区为东八区，避免 TestGenerateCompressedFilename 测试因 UTC 时区失败
export TZ=Asia/Shanghai

remove_static()
{
    local dir=$1
    find ${dir} -type f -name "*.cpp" | xargs -i sed -i "s/\bstatic\b//g" {}
}

remove_static ${code_dir}/src/smap

build_dir=build

cd ${code_dir}/test
pwd
[ ! -d ${build_dir} ] && mkdir -p ${build_dir}
rm -rf ${build_dir}/*

cp -r $CURRENT_PATH/3rdparty/mockcpp_support_arm64.patch $CURRENT_PATH/3rdparty/mockcpp
cd $CURRENT_PATH/3rdparty/mockcpp
dos2unix src/UnixCodeModifier.cpp
PATCH_FILE="mockcpp_support_arm64.patch"
dos2unix $PATCH_FILE
# 检查补丁是否能应用
if git apply --check "$PATCH_FILE" 2>/dev/null; then
    echo "Applying patch $PATCH_FILE..."
    git apply "$PATCH_FILE"
else
    echo "Patch $PATCH_FILE already applied or cannot be applied, skipping."
fi
 
cd ${code_dir}/test

cmake -S . -B ${build_dir}
echo "====== 开始编译 ubturbo_ut ======"
cmake --build ${build_dir} --target ubturbo_ut -j$(nproc) || exit 1
echo "====== ubturbo_ut 编译完成，开始编译 rmrs_ut ======"
cmake --build ${build_dir} --target rmrs_ut -j$(nproc) || exit 1
echo "====== rmrs_ut 编译完成 ======"

cd ${build_dir}
lcov --directory . --zerocounters 2>/dev/null || true
# mockcpp 依赖 4K 内存页对齐（mprotect 修改代码段页属性实现打桩），
# 非 4K 页环境（如鲲鹏 64K 页）下 mprotect 失败会导致测试进程崩溃。
# 此处检测页大小，不兼容时跳过 UT 执行，保证整体流程正常完成，
# 详见 doc/3rdparty_open_source_components.md（2.2 mockcpp 兼容性限制）。
UT_PAGE_SIZE="${MOCKCPP_PAGE_SIZE_OVERRIDE:-$(getconf PAGESIZE 2>/dev/null || echo 4096)}"
if [ "${UT_PAGE_SIZE}" != "4096" ]; then
    echo "[SKIP] 当前系统内存页大小为 ${UT_PAGE_SIZE}（非 4K），mockcpp 不兼容，跳过 UT 执行，不影响整体流程；详见 doc/3rdparty_open_source_components.md"
    exit 0
fi

echo "====== 开始执行 ubturbo_ut ======"
./ubturbo_ut
echo "====== 开始执行 rmrs_ut ======"
./rmrs_ut

# 覆盖率收集：lcov与gcc 12.3.1存在mismatched exception tag兼容问题，
# 导致lcov采集报错。此处忽略覆盖率收集阶段的错误，不影响测试结果。
# 如需获取覆盖率报告，请使用与gcc 12.3.1兼容的lcov版本。
set +e
lcov --d . --c --output-file ./test.info --rc lcov_branch_coverage=1
lcov -e ./test.info "*/src/*" -output-file ./coverage.info --rc lcov_branch_coverage=1

lcov --remove ./coverage.info "*/src/*.h" -o ../build/coverage.info --rc lcov_branch_coverage=1
lcov --remove ./coverage.info "*/test/3rdparty/googletest/googletest/src/*" -o ./coverage.info --rc lcov_branch_coverage=1

genhtml -o ./gcovr_report ./coverage.info --show-details --legend --rc lcov_branch_coverage=1
set -e