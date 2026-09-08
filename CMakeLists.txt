cmake_minimum_required(VERSION 3.22)
project(UBTurbo VERSION 1.0 LANGUAGES C CXX)

# 默认使用 C11
set(CMAKE_C_STANDARD_DEFAULT 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS NO)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
# 默认使用 C++17
set(CMAKE_CXX_STANDARD_DEFAULT 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS NO)

message(STATUS "CMAKE_CXX_STANDARD: c++${CMAKE_CXX_STANDARD}")

set(SRC_DIR ${PROJECT_SOURCE_DIR}/src)
set(RMRS_SRC_DIR ${PROJECT_SOURCE_DIR}/plugins/rmrs/src)
set(3RDPARTY_DIR ${PROJECT_SOURCE_DIR}/3rdparty)

# 安全编译选项控制
set(UBTURBO_COMMON_FLAGS "")
set(SECURE_LD_FLAGS "-Wl,-z,noexecstack -Wl,-z,relro -Wl,-z,now -pie -Wl,--build-id=none")
set(SECURE_C_FLAGS "-fstack-protector-strong")
set(COMMON_FLAGS "-fno-omit-frame-pointer ${SECURE_C_FLAGS} ${SECURE_LD_FLAGS} ${UBTURBO_COMMON_FLAGS}")

if (CMAKE_BUILD_TYPE MATCHES "^[Dd]ebug")
    set(DEBUG_FLAGS "-O0 -ggdb3 -fPIE")
    set(CMAKE_CXX_FLAGS_DEBUG "${COMMON_FLAGS} ${DEBUG_FLAGS} -lrt")
else()
    set(RELEASE_FLAGS "-O2 -D_FORTIFY_SOURCE=2 -DNDEBUG=1 -fPIE -fPIC -s")
    set(CMAKE_CXX_FLAGS_RELEASE "${COMMON_FLAGS} ${RELEASE_FLAGS} -lrt")
endif()


# 设置可执行文件和库的输出目录
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# 禁用 RPATH
set(CMAKE_SKIP_RPATH ON)

include(GNUInstallDirs)
set(CMAKE_INSTALL_LIBDIR lib)

message(DEBUG "CMAKE_INSTALL_PREFIX: ${CMAKE_INSTALL_PREFIX}")
message(DEBUG "CMAKE_INSTALL_BINDIR: ${CMAKE_INSTALL_BINDIR}")
message(DEBUG "CMAKE_INSTALL_LIBDIR: ${CMAKE_INSTALL_LIBDIR}")
message(DEBUG "CMAKE_INSTALL_INCLUDEDIR: ${CMAKE_INSTALL_INCLUDEDIR}")

set(CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/build/cmake" ${CMAKE_MODULE_PATH})
include(building)
include(ccache)
include(utils)

# 设置所有共享库的输出目录
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
# 设置所有可执行的输出目录
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

message(STATUS "current platform: ${CMAKE_SYSTEM_NAME}")
message(STATUS "CMAKE_BUILD_TYPE: ${CMAKE_BUILD_TYPE}")
message(DEBUG "CMAKE_CXX_FLAGS: ${CMAKE_CXX_FLAGS}")
message(DEBUG "CXX_FLAGS_DEBUG: ${CXX_FLAGS_DEBUG}")
message(DEBUG "CXX_FLAGS_RELEASE: ${CXX_FLAGS_RELEASE}")
message(DEBUG "CMAKE_CXX_FLAGS_DEBUG: ${CMAKE_CXX_FLAGS_DEBUG}")
message(DEBUG "CMAKE_CXX_FLAGS_RELWITHDEBINFO: ${CMAKE_CXX_FLAGS_RELWITHDEBINFO}")
message(DEBUG "CMAKE_CXX_FLAGS_RELEASE: ${CMAKE_CXX_FLAGS_RELEASE}")
message(DEBUG "CMAKE_CXX_FLAGS_MINSIZEREL: ${CMAKE_CXX_FLAGS_MINSIZEREL}")

# 加上构建过程日志的颜色输出
option (FORCE_COLORED_OUTPUT "Always produce ANSI-colored output (GNU/Clang only)." OFF)
if (${FORCE_COLORED_OUTPUT})
    if ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "Clang")
        add_compile_options(-fcolor-diagnostics)
    elseif ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU")
        add_compile_options(-fdiagnostics-color=always)
    endif ()
else ()
    if ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU")
        add_compile_options(-fdiagnostics-color=auto)
    endif ()
endif ()

# coverage option
option(ENABLE_COVERAGE "Use gcov" OFF)
message(DEBUG ENABLE_COVERAGE=${ENABLE_COVERAGE})
if (ENABLE_COVERAGE)
    setup_coverage()
endif ()

# 查找系统 libboundscheck 头文件 (securec.h)
find_path(SECUREC_INCLUDE_DIR securec.h)
if(NOT SECUREC_INCLUDE_DIR)
    message(FATAL_ERROR "securec.h not found. Install: sudo yum install libboundscheck")
endif()

# 查找系统 rapidjson 头文件；未安装时从 src-openeuler 自动下载
find_path(RAPIDJSON_INCLUDE_DIR rapidjson/document.h)
if(NOT RAPIDJSON_INCLUDE_DIR)
    set(RAPIDJSON_VERSION "1.1.0")
    set(RAPIDJSON_TARBALL_NAME "rapidjson-${RAPIDJSON_VERSION}.tar.gz")
    set(RAPIDJSON_TARBALL_SHA256 "bf7ced29704a1e696fbccf2a2b4ea068e7774fa37f6d7dd4039d0787f8bed98e")
    set(RAPIDJSON_DOWNLOAD_DIR ${CMAKE_BINARY_DIR}/3rdparty/rapidjson-download)
    set(RAPIDJSON_OPENEULER_RAW_URL "https://gitcode.com/api/v5/repos/src-openeuler/rapidjson/raw/${RAPIDJSON_TARBALL_NAME}?ref=master")
    set(RAPIDJSON_INCLUDE_DIR ${RAPIDJSON_DOWNLOAD_DIR}/rapidjson-${RAPIDJSON_VERSION}/include)
    if(NOT EXISTS ${RAPIDJSON_INCLUDE_DIR})
        message(STATUS "rapidjson: fetch from src-openeuler ${RAPIDJSON_TARBALL_NAME}")
        file(DOWNLOAD ${RAPIDJSON_OPENEULER_RAW_URL} ${RAPIDJSON_DOWNLOAD_DIR}/${RAPIDJSON_TARBALL_NAME}
            SHOW_PROGRESS TIMEOUT 120)
        file(SHA256 ${RAPIDJSON_DOWNLOAD_DIR}/${RAPIDJSON_TARBALL_NAME} RAPIDJSON_ACTUAL_SHA256)
        if(NOT RAPIDJSON_ACTUAL_SHA256 STREQUAL RAPIDJSON_TARBALL_SHA256)
            message(FATAL_ERROR "rapidjson SHA256 mismatch: expected ${RAPIDJSON_TARBALL_SHA256}, got ${RAPIDJSON_ACTUAL_SHA256}")
        endif()
        execute_process(COMMAND ${CMAKE_COMMAND} -E make_directory ${RAPIDJSON_DOWNLOAD_DIR})
        execute_process(COMMAND ${CMAKE_COMMAND} -E chdir ${RAPIDJSON_DOWNLOAD_DIR}
            ${CMAKE_COMMAND} -E tar xzf ${RAPIDJSON_DOWNLOAD_DIR}/${RAPIDJSON_TARBALL_NAME})
    endif()
    message(STATUS "rapidjson: using downloaded source ${RAPIDJSON_INCLUDE_DIR}")
endif()

# 全局作用域的头文件定义，不依赖具体模块
include_directories(
        ${PROJECT_SOURCE_DIR}/include   # 外部共享头文件
        ${SRC_DIR}/include              # 内部共享头文件
        ${RMRS_SRC_DIR}/include         # rmrs插件共享头文件
        ${CMAKE_BINARY_DIR}/include     # 自动生成头文件
        ${SECUREC_INCLUDE_DIR}
        ${RAPIDJSON_INCLUDE_DIR}
)

add_subdirectory(src)
add_subdirectory(plugins/rmrs)

option(BUILD_TESTS "Build tests" OFF)
if (BUILD_TESTS)
    add_subdirectory(test)
endif ()

file(COPY ${PROJECT_SOURCE_DIR}/conf/
        DESTINATION ${CMAKE_BINARY_DIR}/conf
        FILE_PERMISSIONS OWNER_READ OWNER_WRITE
        PATTERN "README.md" EXCLUDE
)

# ------------------------------
# RPM 打包说明
# ------------------------------
# RPM 打包不再使用 CPack，改为基于仓库根目录的 ubturbo.spec 通过 rpmbuild 构建。
# 入口：bash build.sh ubturbo 或 bash build.sh ubturbo-rmrs
#（内部调用 scripts/rpm/package.sh），产物位于 output/。