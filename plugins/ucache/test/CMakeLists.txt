cmake_minimum_required(VERSION 3.13)

project(UCACHE VERSION 0.1.0 LANGUAGES C CXX)

message(STATUS "KDIR: ${KDIR}")

# 默认使用 c11,和源码对齐
set(CMAKE_CXX_STANDARD 11)

# 默认使用 c++17,和源码对齐
set(CMAKE_CXX_STANDARD 17)

set(CMAKE_CXX_FLAGS " ${CMAKE_CXX_FLAGS} -fprofile-arcs -ftest-coverage -DACPI_NOT_READY")
set(CMAKE_C_FLAGS " ${CMAKE_C_FLAGS} -fprofile-arcs -ftest-coverage -DACPI_NOT_READY")
add_link_options(-lgcov --coverage)
set(CMAKE_EXE_LINKER_FLAGS " -lgcov --coverage ")
set(CMAKE_SHARED_LINKER_FLAGS " -lgcov --coverage ")
set(DT_TARGET ucache_dt)
set(UCACHE_SRC_MOCK ucache_src_mock)
add_subdirectory(3rdparty)

set(UCACHE_SRC
    ${CMAKE_CURRENT_BINARY_DIR}/src/common/ucache_file_util.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/src/common/ucache_string_util.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/src/common/ucache_turbo_config.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/src/task_executor/task_executor.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/src/resource_collector/resource_collector.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/src/strategy_executor/driver_interaction.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/src/strategy_executor/migration_executor.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/src/strategy_executor/turbo_strategy_executor.cpp
)

set(MOCK_SRC
    3rdparty/src/memcpy_s.c
    mock/turbo_logger.cpp
    mock/turbo_ipc_server.cpp
    mock/turbo_conf.cpp
    mock/turbo_ipc_client.cpp
)

aux_source_directory(${CMAKE_CURRENT_SOURCE_DIR}/ TESTCASE_SRC)
aux_source_directory(${CMAKE_CURRENT_SOURCE_DIR}/src/ TESTCASE_UCACHE_OS_TURBO_PLUGIN_SRC)
aux_source_directory(${CMAKE_CURRENT_SOURCE_DIR}/src/resource_collector TESTCASE_RESOURCE_EXPORT_SRC)
aux_source_directory(${CMAKE_CURRENT_SOURCE_DIR}/src/task_executor TESTCASE_TASK_EXECUTOR_SRC)
aux_source_directory(${CMAKE_CURRENT_SOURCE_DIR}/src/common TESTCASE_UCACHE_OS_TURBO_PLUGIN_COMMON_SRC)

set(TEST_SRC
    ${UCACHE_SRC}
    ${MOCK_SRC}
    ${TESTCASE_SRC}
    ${TESTCASE_UCACHE_OS_TURBO_PLUGIN_SRC}
    ${TESTCASE_RESOURCE_EXPORT_SRC}
    ${TESTCASE_TASK_EXECUTOR_SRC}
    ${TESTCASE_UCACHE_OS_TURBO_PLUGIN_COMMON_SRC}
)

include_directories(PRIVATE
    # ucache src headers
    ${CMAKE_CURRENT_BINARY_DIR}/src/common
    ${CMAKE_CURRENT_BINARY_DIR}/src/include
    ${CMAKE_CURRENT_BINARY_DIR}/src/resource_collector
    ${CMAKE_CURRENT_BINARY_DIR}/src/task_executor
    ${CMAKE_CURRENT_BINARY_DIR}/src/strategy_executor
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/include
    ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/include

    # mock header
    ${CMAKE_CURRENT_SOURCE_DIR}/mock
)

add_executable(${DT_TARGET} ${TEST_SRC})

target_link_options(${DT_TARGET} PRIVATE -rdynamic)
target_compile_definitions(${DT_TARGET} PRIVATE UT_ELF_PATH=\"${CMAKE_CURRENT_BINARY_DIR}/${DT_TARGET}\")
TARGET_MOCKCPP(${DT_TARGET})
TARGET_GTEST(${DT_TARGET})

# print compile and link flags
get_target_property(COMPILE_FLAGS ${DT_TARGET} COMPILE_OPTIONS)
get_target_property(LINK_FLAGS ${DT_TARGET} LINK_OPTIONS)
message(STATUS "Compiler id: ${CMAKE_CXX_COMPILER_ID}")
message(STATUS "Compile flags: ${COMPILE_FLAGS}")
message(STATUS "Link flags: ${LINK_FLAGS}")
message(STATUS "Build Type: ${CMAKE_BUILD_TYPE}")
