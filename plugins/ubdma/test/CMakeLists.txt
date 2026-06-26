cmake_minimum_required(VERSION 3.13)

project(UB_DMA VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 11)

set(UB_DMA_SRC ${CMAKE_CURRENT_LIST_DIR}/dt_src)
set(TEST_DIR ${CMAKE_CURRENT_LIST_DIR})
set(CMAKE_CXX_FLAGS " ${CMAKE_CXX_FLAGS} -fprofile-arcs -ftest-coverage -DACPI_NOT_READY -Wno-all")
set(CMAKE_C_FLAGS " ${CMAKE_C_FLAGS} -fprofile-arcs -ftest-coverage -DACPI_NOT_READY -Wno-all")
add_link_options(-lgcov --coverage)
set(CMAKE_EXE_LINKER_FLAGS " -lgcov --coverage ")
set(CMAKE_SHARED_LINKER_FLAGS " -lgcov --coverage ")
add_definitions(-DDT_ENABLE)

add_subdirectory(3rdparty)

add_compile_definitions(USE_DT)

# include all test source files
aux_source_directory(${TEST_DIR}/drivers TEST_SRC)

# stub c files
aux_source_directory(${TEST_DIR}/depends/drivers/base TEST_SRC)
aux_source_directory(${TEST_DIR}/depends/drivers/mm TEST_SRC)
aux_source_directory(${TEST_DIR}/depends/drivers/dma TEST_SRC)
aux_source_directory(${TEST_DIR}/depends/kernel TEST_SRC)
aux_source_directory(${TEST_DIR}/depends/kernel/time TEST_SRC)
aux_source_directory(${TEST_DIR}/depends/kernel/locking TEST_SRC)
list(APPEND TEST_SRC ${TEST_DIR}/main.cpp)

# ub-dma source files (from dt_src copy)
list(APPEND TEST_SRC ${UB_DMA_SRC}/src/urma.c)
list(APPEND TEST_SRC ${UB_DMA_SRC}/src/ub_dma_main.c)
list(APPEND TEST_SRC ${UB_DMA_SRC}/src/ub_virt_dma.c)
list(APPEND TEST_SRC ${UB_DMA_SRC}/src/segment_manager.c)
list(APPEND TEST_SRC ${UB_DMA_SRC}/src/ub_dma_iomem.c)
list(APPEND TEST_SRC ${UB_DMA_SRC}/src/memory_notifier.c)

add_executable(ubdma_dt ${TEST_SRC})

# include headers
target_include_directories(ubdma_dt PUBLIC
    ${UB_DMA_SRC}/src
    ${TEST_DIR}/3rdparty/umdk
    ${TEST_DIR}/user_depends/include
    ${TEST_DIR}/depends/include
    ${TEST_DIR}/depends/lib
    ${TEST_DIR}/depends/arch/arm64/include
)

target_compile_definitions(ubdma_dt PUBLIC
    SKIP_STATIC
    ENV_USER
    CONFIG_NUMA
    CONFIG_MODULES
    CONFIG_NODES_SHIFT=7
    CONFIG_PHYS_ADDR_T_64BIT
    CONFIG_PGTABLE_LEVELS=4
    CONFIG_ARM64_CONT_PTE_SHIFT=4
    CONFIG_ARM64_CONT_PMD_SHIFT=4
    CONFIG_NR_CPUS=4096
    CONFIG_SPARSEMEM=y
    CONFIG_SPARSEMEM_VMEMMAP=y
    CONFIG_ARM64_VA_BITS=48
    CONFIG_ARM64_PAGE_SHIFT=12
    CONFIG_CPUSETS=y
    CONFIG_64BIT=y
    CONFIG_MEMORY_HOTPLUG=true
    CONFIG_CMA
)

# print compile and link flags
get_target_property(COMPILE_FLAGS ubdma_dt COMPILE_OPTIONS)
get_target_property(LINK_FLAGS ubdma_dt LINK_OPTIONS)
message(STATUS "Compiler id: ${CMAKE_CXX_COMPILER_ID}")
message(STATUS "Compile flags: ${COMPILE_FLAGS}")
message(STATUS "Link flags: ${LINK_FLAGS}")
message(STATUS "Build Type: ${CMAKE_BUILD_TYPE}")
TARGET_MOCKCPP(ubdma_dt)
TARGET_GTEST(ubdma_dt)
