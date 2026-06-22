#!/bin/bash
#******************************************************************
# Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
# Description: ub-dma run dt script
#******************************************************************

set -euo pipefail

remove_static()
{
    local dir=$1
    find ${dir} -type f -name "*.c" | xargs -i sed -i "s/\bstatic\b//g" {}
}

remove_inline()
{
    local dir=$1
    find ${dir} -type f -name "*.c" | xargs -i sed -i "s/\binline\b//g" {}
}

rename_class()
{
    local dir=$1
    find ${dir} -type f -name "*.c" | xargs -i sed -i "s/\bclass\b/class_stub/g" {}
}

rename_min()
{
    local dir=$1
    find ${dir} -type f -name "*.c" | xargs -i sed -i "s/\bmin\b/min_stub/g" {}
}

replace_string() {
    filename="$1"
    search_string="$2"
    replace_string="$3"
    replace_without_exist="${4:-true}"

    count=$(grep -c "$replace_string" "$filename" || true)
    if [ $count -gt 0 ]; then
        if [[ "$replace_without_exist" == "true" ]]; then
            return 0
        fi
    fi

    sed -i "s/\b$search_string\b/$replace_string/g" "$filename"
}

add_extern()
{
    local dir=$1
    local header_files=""
    local lineno=""
    local tmp=""

    (grep -rn -E '^extern "C"' --include="*.h" ${dir} || echo "1") > tmp
    if [ "$(cat tmp)" = "1" ]; then
        echo "none" > skip_files
    else
        cat tmp | cut -f1 -d: --output-delimiter="\n" > skip_files
    fi
    if [ "$(cat skip_files)" = "none" ]; then
        header_files=$(find ${dir} -type f -name "*.h")
    else
        header_files=$(find ${dir} -type f -name "*.h" | grep -vFf skip_files)
    fi
    for f in ${header_files}
    do
        sed -i -r '0,/^\w/s/(^\w.*$)/#ifdef __cplusplus\nextern "C" {\n#endif\n\1/' ${f}
        (grep -n -E "^#endif" ${f} || echo "no") > tmp
        if [ "$(cat tmp)" = "no" ]; then
            continue
        fi
        lineno=$(cat tmp | tail -n 1 | cut -f1 -d:)
        sed -i "${lineno}i\#ifdef __cplusplus\n}\n#endif" ${f}
    done
}

CURRENT_PATH=$(cd "$(dirname "$0")"; pwd)
echo "${CURRENT_PATH:?}"
cd "${CURRENT_PATH:?}"

DT_SRC_DIR=$CURRENT_PATH/dt_src
if [[ ! -d "$DT_SRC_DIR" ]]
then
    mkdir -p $DT_SRC_DIR
else
    rm -rf ${DT_SRC_DIR}/*
fi
cp -r ${CURRENT_PATH}/../src $DT_SRC_DIR

code_dir=$(cd ${DT_SRC_DIR} && pwd)

remove_static ${code_dir}/src
remove_inline ${code_dir}/src
rename_class ${code_dir}/src
rename_min ${code_dir}/src

add_extern ${code_dir}/src

set -e

BUILD_DIR=$CURRENT_PATH/build
if [[ ! -d "$BUILD_DIR" ]]
then
    mkdir -p $BUILD_DIR
fi

rm -rf $CURRENT_PATH/build/*

cd $BUILD_DIR || {
    echo "Fatal! Cannot enter $BUILD_DIR."
    exit 1;
}

mock_patch_path=$CURRENT_PATH/3rdparty/mockcpp/mockcpp_support_arm64.patch
if [[ ! -e "$mock_patch_path" ]]
then
    dos2unix $CURRENT_PATH/3rdparty/mockcpp_support_arm64.patch
    cp -r $CURRENT_PATH/3rdparty/mockcpp_support_arm64.patch $CURRENT_PATH/3rdparty/mockcpp
    cd $CURRENT_PATH/3rdparty/mockcpp
    dos2unix src/UnixCodeModifier.cpp
    git apply mockcpp_support_arm64.patch
fi
cd $CURRENT_PATH/build

N_CPUS=$(grep processor /proc/cpuinfo | wc -l)
echo "$N_CPUS processors detected."

CMAKE_CMD="cmake -DCMAKE_BUILD_TYPE=Debug $CURRENT_PATH"
BUILD_CMD="make -j $((N_CPUS-2))"

echo $CMAKE_CMD
$CMAKE_CMD || {
    echo "Failed to configure ubdma_dt build!"
    exit 1;
}
echo
echo "Done configuring ubdma_dt build"
echo
echo $BUILD_CMD
$BUILD_CMD || {
    echo "Failed to build ubdma_dt"
    exit 1;
}
echo
echo Success

./ubdma_dt

mkdir -p build/gcovr_report
lcov --d ./ --c --output-file test.info --rc lcov_branch_coverage=1
lcov -e test.info "*/dt_src/src/*" -output-file coverage.info --rc lcov_branch_coverage=1
genhtml -o gcovr_report coverage.info --show-details --legend --rc lcov_branch_coverage=1
