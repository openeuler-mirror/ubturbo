cmake_minimum_required(VERSION 3.13)

project(SMAP VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 11)

find_package(Threads REQUIRED)

set(SMAP_SRC ${CMAKE_CURRENT_LIST_DIR}/dt_src)
set(CMAKE_CXX_FLAGS " ${CMAKE_CXX_FLAGS} -fprofile-arcs -ftest-coverage")
set(CMAKE_C_FLAGS " ${CMAKE_CXX_FLAGS} -fprofile-arcs -ftest-coverage")
add_link_options(-lgcov --coverage)
set(CMAKE_EXE_LINKER_FLAGS " -lgcov --coverage ")
set(CMAKE_SHARED_LINKER_FLAGS " -lgcov --coverage ")

add_subdirectory(3rdparty)

add_compile_definitions(USE_DT CONFIG_MIGRATE_PAGES_DMA_OFFLOADING)

# include all source files
aux_source_directory(${CMAKE_CURRENT_LIST_DIR}/migrate TEST_SRC)
aux_source_directory(${CMAKE_CURRENT_LIST_DIR}/scan TEST_SRC)
aux_source_directory(${CMAKE_CURRENT_LIST_DIR}/user TEST_SRC)
aux_source_directory(${CMAKE_CURRENT_LIST_DIR}/user/manage TEST_SRC)
aux_source_directory(${CMAKE_CURRENT_LIST_DIR}/user/strategy TEST_SRC)
aux_source_directory(${CMAKE_CURRENT_LIST_DIR}/user/advanced-strategy TEST_SRC)

# stub c files
aux_source_directory(${CMAKE_CURRENT_LIST_DIR}/depends/tools/lib TEST_SRC)
aux_source_directory(${CMAKE_CURRENT_LIST_DIR}/depends/drivers/base TEST_SRC)
aux_source_directory(${CMAKE_CURRENT_LIST_DIR}/depends/drivers/firmware TEST_SRC)
aux_source_directory(${CMAKE_CURRENT_LIST_DIR}/depends/drivers/pci TEST_SRC)
aux_source_directory(${CMAKE_CURRENT_LIST_DIR}/depends/drivers/acpi/acpica TEST_SRC)
aux_source_directory(${CMAKE_CURRENT_LIST_DIR}/depends/drivers/acpi/numa TEST_SRC)
aux_source_directory(${CMAKE_CURRENT_LIST_DIR}/depends/drivers/char TEST_SRC)
aux_source_directory(${CMAKE_CURRENT_LIST_DIR}/depends/drivers/usb/core TEST_SRC)
aux_source_directory(${CMAKE_CURRENT_LIST_DIR}/depends/kernel TEST_SRC)
aux_source_directory(${CMAKE_CURRENT_LIST_DIR}/depends/kernel/time TEST_SRC)
aux_source_directory(${CMAKE_CURRENT_LIST_DIR}/depends/kernel/locking TEST_SRC)
aux_source_directory(${CMAKE_CURRENT_LIST_DIR}/depends/kernel/sched TEST_SRC)
aux_source_directory(${CMAKE_CURRENT_LIST_DIR}/depends/kernel/cgroup TEST_SRC)
aux_source_directory(${CMAKE_CURRENT_LIST_DIR}/depends/kvm TEST_SRC)
aux_source_directory(${CMAKE_CURRENT_LIST_DIR}/depends/lib TEST_SRC)
aux_source_directory(${CMAKE_CURRENT_LIST_DIR}/depends/mm TEST_SRC)
aux_source_directory(${CMAKE_CURRENT_LIST_DIR}/depends/fs TEST_SRC)
aux_source_directory(${CMAKE_CURRENT_LIST_DIR}/depends/fs/debugfs TEST_SRC)
aux_source_directory(${CMAKE_CURRENT_LIST_DIR}/depends/fs/sysfs TEST_SRC)
aux_source_directory(${CMAKE_CURRENT_LIST_DIR}/depends/fs/proc TEST_SRC)
aux_source_directory(${CMAKE_CURRENT_LIST_DIR}/depends/init TEST_SRC)
aux_source_directory(${CMAKE_CURRENT_LIST_DIR}/depends/virt/kvm TEST_SRC)
aux_source_directory(${CMAKE_CURRENT_LIST_DIR}/depends/arch/arm64/kernel TEST_SRC)
aux_source_directory(${CMAKE_CURRENT_LIST_DIR}/depends/arch/arm64/mm TEST_SRC)
aux_source_directory(${CMAKE_CURRENT_LIST_DIR}/3rdparty/src TEST_SRC)
aux_source_directory(${CMAKE_CURRENT_LIST_DIR}/user/strategy/migration_stub.c TEST_SRC)
aux_source_directory(${CMAKE_CURRENT_LIST_DIR}/user/manage/thread_stub.c TEST_SRC)

# dump_info
list(APPEND TEST_SRC ${SMAP_SRC}/src/kernel/migrate/dump_info.c)

# numa
list(APPEND TEST_SRC ${SMAP_SRC}/src/kernel/migrate/numa.c)

# obmm_monitor
list(APPEND TEST_SRC ${SMAP_SRC}/src/kernel/migrate/obmm_monitor.c)

# smap_migrate_wrapper
list(APPEND TEST_SRC ${SMAP_SRC}/src/kernel/migrate/smap_migrate_wrapper.c)

# smap_migrate_page
list(APPEND TEST_SRC ${SMAP_SRC}/src/kernel/migrate/smap_migrate_pages.c)

# smap_debugfs
list(APPEND TEST_SRC ${SMAP_SRC}/src/kernel/migrate/smap_debugfs.c)

#migrate_main
list(APPEND TEST_SRC ${SMAP_SRC}/src/kernel/migrate/migrate_main.c)
# acpi_helper
list(APPEND TEST_SRC ${SMAP_SRC}/src/kernel/comm/acpi_helper.c)

# pid
list(APPEND TEST_SRC ${SMAP_SRC}/src/kernel/migrate/migrate_back_ioctl.c)
list(APPEND TEST_SRC ${SMAP_SRC}/src/kernel/migrate/migrate_task.c)
list(APPEND TEST_SRC ${SMAP_SRC}/src/kernel/migrate/work.c)
list(APPEND TEST_SRC ${SMAP_SRC}/src/kernel/migrate/migrate_back_debugfs.c)
list(APPEND TEST_SRC ${SMAP_SRC}/src/kernel/migrate/migrate_dev.c)

# ham
list(APPEND TEST_SRC ${SMAP_SRC}/src/kernel/migrate/basic.c)
list(APPEND TEST_SRC ${SMAP_SRC}/src/kernel/migrate/coherence_maintain.c)
list(APPEND TEST_SRC ${SMAP_SRC}/src/kernel/migrate/ham_migration.c)
list(APPEND TEST_SRC ${SMAP_SRC}/src/kernel/migrate/ham_tasks_mgr.c)

# access_mmu
list(APPEND TEST_SRC ${SMAP_SRC}/src/kernel/scan/access_mmu.c)
# scan ioctl
list(APPEND TEST_SRC ${SMAP_SRC}/src/kernel/scan/scan_ioctl.c)
# scan
list(APPEND TEST_SRC ${SMAP_SRC}/src/kernel/scan/accessed_bit.c)
# access_acpi_mem
list(APPEND TEST_SRC ${SMAP_SRC}/src/kernel/comm/acpi_mem.c)

list(APPEND TEST_SRC ${SMAP_SRC}/src/kernel/comm/iomem.c)

list(APPEND TEST_SRC ${SMAP_SRC}/src/kernel/scan/kvm_pgtable.c)

# access_pid
list(APPEND TEST_SRC ${SMAP_SRC}/src/kernel/scan/access_pid.c)

list(APPEND TEST_SRC ${SMAP_SRC}/src/kernel/scan/scan_main.c)
list(APPEND TEST_SRC ${SMAP_SRC}/src/kernel/scan/access_tracking_wrapper.c)

list(APPEND TEST_SRC ${SMAP_SRC}/src/kernel/scan/ub_hist.c)
list(APPEND TEST_SRC ${SMAP_SRC}/src/kernel/scan/hist_ops.c)

list(APPEND TEST_SRC ${SMAP_SRC}/src/kernel/scan/hist_tracking.c)

# smap user
list(APPEND TEST_SRC ${SMAP_SRC}/src/user/smap_interface.c)
list(APPEND TEST_SRC ${SMAP_SRC}/src/user/smap_inner_interface.c)
list(APPEND TEST_SRC ${SMAP_SRC}/src/user/manage/manage.c)
list(APPEND TEST_SRC ${SMAP_SRC}/src/user/manage/manage_pair.c)
list(APPEND TEST_SRC ${SMAP_SRC}/src/user/manage/manage_numa.c)
list(APPEND TEST_SRC ${SMAP_SRC}/src/user/manage/manage_remote.c)
list(APPEND TEST_SRC ${SMAP_SRC}/src/user/manage/manage_grouped.c)
list(APPEND TEST_SRC ${SMAP_SRC}/src/user/manage/manage_scan.c)
list(APPEND TEST_SRC ${SMAP_SRC}/src/user/manage/thread.c)
list(APPEND TEST_SRC ${SMAP_SRC}/src/user/manage/device.c)
list(APPEND TEST_SRC ${SMAP_SRC}/src/user/user_log/smap_user_log/smap_user_log.c)
list(APPEND TEST_SRC ${SMAP_SRC}/src/user/user_log/smap_log_core.c)
list(APPEND TEST_SRC ${SMAP_SRC}/src/user/manage/access_ioctl.c)
list(APPEND TEST_SRC ${SMAP_SRC}/src/user/manage/oom_migrate.c)
list(APPEND TEST_SRC ${SMAP_SRC}/src/user/manage/smap_config.c)
list(APPEND TEST_SRC ${SMAP_SRC}/src/user/strategy/strategy_config.c)
list(APPEND TEST_SRC ${SMAP_SRC}/src/user/strategy/migration.c)
list(APPEND TEST_SRC ${SMAP_SRC}/src/user/strategy/separate_strategy.c)
list(APPEND TEST_SRC ${SMAP_SRC}/src/user/strategy/strategy.c)
list(APPEND TEST_SRC ${SMAP_SRC}/src/user/strategy/grouped_strategy.c)
list(APPEND TEST_SRC ${SMAP_SRC}/src/user/advanced-strategy/scene.c)
list(APPEND TEST_SRC ${CMAKE_CURRENT_LIST_DIR}/smap_main.cpp)

add_executable(smap_dt ${TEST_SRC})

TARGET_MOCKCPP(smap_dt)
TARGET_GTEST(smap_dt)
target_link_libraries(smap_dt Threads::Threads ${CMAKE_DL_LIBS})

# include headers
target_include_directories(smap_dt PUBLIC
    ${SMAP_SRC}/src
    ${SMAP_SRC}/src/user
    ${SMAP_SRC}/src/user/user_log
    ${SMAP_SRC}/src/user/user_log/smap_user_log
    ${SMAP_SRC}/src/kernel/comm
    ${SMAP_SRC}/src/kernel/migrate
    ${SMAP_SRC}/src/kernel/scan
    ${SMAP_SRC}/src/ucache
    ${CMAKE_CURRENT_LIST_DIR}/ucache
    ${CMAKE_CURRENT_LIST_DIR}/3rdparty/include
    ${CMAKE_CURRENT_LIST_DIR}/user_depends/include
    ${CMAKE_CURRENT_LIST_DIR}/depends/include
    ${CMAKE_CURRENT_LIST_DIR}/depends/lib
    ${CMAKE_CURRENT_LIST_DIR}/depends/arch/arm64/include
)

target_compile_definitions(smap_dt PUBLIC
    SKIP_STATIC
    CONFIG_NUMA
    CONFIG_NUMA_BALANCING
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
get_target_property(COMPILE_FLAGS smap_dt COMPILE_OPTIONS)
get_target_property(LINK_FLAGS smap_dt LINK_OPTIONS)
message(STATUS "Compiler id: ${CMAKE_CXX_COMPILER_ID}")
message(STATUS "Compile flags: ${COMPILE_FLAGS}")
message(STATUS "Link flags: ${LINK_FLAGS}")
message(STATUS "Build Type: ${CMAKE_BUILD_TYPE}")
