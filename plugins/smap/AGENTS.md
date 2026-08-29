# AGENTS.md

## Project Overview

SMAP (Smart Memory Accelerator Processor) - Memory tiering and page migration plugin for UBTurbo. Uses hardware-enhanced hot/cold page identification (requires chip support for remote memory cold/hot detection) to migrate hot data to local memory and cold data to remote memory, accelerating application performance end-to-end. C (C11/kernel C) CMake + Kbuild project targeting openEuler Linux (ARM64).

## Build Commands

```shell
# Build user-space library (default Debug)
./build.sh

# Release build
./build.sh -t release

# Debug build
./build.sh -t debug

# Clean build and output directories
./build.sh -t clean

# Build kernel modules (drivers)
cd src/drivers && make KERNEL_VERSION=openeuler -j$(nproc)
cp -f src/drivers/Module.symvers src/tiering/depends

# Build kernel modules (tiering)
cd src/tiering && make KERNEL_VERSION=openeuler -j$(nproc)

# Build kernel modules (ucache)
cd src/ucache && make -j$(nproc)

# Build and package as RPM
rpmbuild -bb smap.spec
```

## Test Commands

```shell
# Run all DT (Design Test) tests with coverage report
./test/run_dt.sh

# Build tests without running (manual)
cd test/build && cmake -DCMAKE_BUILD_TYPE=Debug .. && make -j$(nproc)

# Run specific test binary
./test/build/smap_dt

# Coverage report is generated in test/build/gcovr_report/
```

## Code Style

- C11 standard (user-space), kernel C (drivers/tiering/ucache modules)
- Two distinct code styles:
  - **Kernel code** (`src/drivers/`, `src/tiering/`, `src/ucache/`): Linux kernel style, tabs, 8-column indent, 80 column limit (`src/.clang-format`)
  - **User-space code** (`src/user/`): 4-space indent, 120 column limit, no tabs (`src/user/.clang-format`)
- clang-format for formatting, clang-tidy for static analysis
  - Kernel: `src/.clang--tidy` (minimal checks, no C++ rules)
  - User-space: `src/user/.clang-tidy` (CERT C, clang-analyzer, bugprone, readability)
- Naming: `lower_case` for functions/variables, `UPPER_CASE` for macros
- Comments in Chinese or English as appropriate

## Dev Environment Tips

- Default build type is Debug; tests use Debug with coverage (`-fprofile-arcs -ftest-coverage`)
- User-space library builds into `build/`, outputs to `output/smap/` (lib, bin, include)
- Kernel modules build `.ko` files in their respective source directories
- Main output: `libsmap.so` shared library (from `src/user/`)
- Kernel version support via `KERNEL_VERSION` variable: `openeuler` (default, enables HAM), `ocos`, `velinux`
- `libboundscheck` is a required dependency; build script auto-builds it from `3rdparty/` if not installed
- Test framework uses GoogleTest + mockcpp; `run_dt.sh` performs source transformations (removes `static`/`inline`, renames symbols) before building tests
- Coverage reports generated via `lcov` + `genhtml` in `test/build/gcovr_report/`
- Device permissions managed by udev rules (`99-smap.rules`), owned by `ubturbo:ubturbo` with mode 0600
- Secure build flags enforced: `-fstack-protector-strong`, `-Wl,-z,noexecstack`, `-Wl,-z,relro`, `-Wl,-z,now`, `-fPIE`, `-fPIC`; Release adds `-D_FORTIFY_SOURCE=2`

## Architecture

```
src/
├── drivers/              # Kernel tracking drivers (smap_tracking_core.ko, smap_access_tracking.ko, smap_histogram_tracking.ko)
│   ├── core.c/bus.c      # Tracking core and bus infrastructure
│   ├── access_tracking.c # Software access tracking (page table AF bit scanning)
│   ├── hist_tracking.c   # Hardware histogram tracking
│   ├── access_mmu.c      # MMU/page table walking
│   ├── access_iomem.c    # IOMEM / ACPI memory access
│   ├── access_pid.c      # PID-based access tracking
│   └── kvm_pgtable.c     # KVM page table operations
├── tiering/              # Kernel tiering module (smap_tiering.ko)
│   ├── smap_migrate_*.c  # Page migration core
│   ├── rmap.c            # Reverse mapping
│   ├── numa.c            # NUMA topology
│   ├── ham_migration.c   # HAM (Hot Access Memory) migration (openEuler only)
│   ├── tracking_manage.c # Tracking management
│   └── smap_debugfs.c    # Debugfs interface
├── ucache/               # Kernel ucache module (ucache.ko)
│   ├── core.c            # Ucache core
│   └── ucache_migrate.c   # Cache migration
└── user/                 # User-space library (libsmap.so)
    ├── smap_interface.c  # Public API (ubturbo_smap_* functions)
    ├── smap_inner_interface.c # Internal interface
    ├── smap_env.h        # Environment, types, mutex/atomic utilities
    ├── manage/           # Management (device, thread, config, OOM migrate)
    ├── strategy/         # Migration strategy (grouped, separate, config)
    ├── advanced-strategy/# Scene-based strategy
    └── user_log/         # Logging (smap_log_core, smap_user_log)

test/
├── drivers/              # Driver unit tests (18 test files)
├── tiering/              # Tiering unit tests (25 files, includes stubs)
├── ucache/               # Ucache unit tests
├── user/                 # User-space unit tests (manage, strategy, advanced-strategy)
├── depends/              # Kernel stub/mock implementations
├── 3rdparty/             # Test dependencies (mockcpp, gtest)
├── run_dt.sh             # Test build & run script
└── smap_main.cpp         # Test main entry
```

## Public API

Key interfaces in `src/user/smap_interface.h`:

- `ubturbo_smap_start/stop` - Module lifecycle
- `ubturbo_smap_migrate_out/migrate_out_grouped/migrate_out_sync` - Migrate pages to remote NUMA
- `ubturbo_smap_migrate_back` - Migrate pages back from remote
- `ubturbo_smap_urgent_migrate_out` - Emergency migration on local memory pressure
- `ubturbo_smap_process_tracking_add/remove` - Process hot/cold scanning management
- `ubturbo_smap_process_migrate_enable` - Enable/disable per-process migration
- `ubturbo_smap_freq_query` - Query page hot/cold frequency data
- `ubturbo_smap_remote_numa_migrate/same_remote_numa_migrate/pid_remote_numa_migrate` - Remote-to-remote migration

## Security Guidelines

**禁止以下行为（红线规则）：**

1. **禁止提交敏感信息**
   - API 密钥、密码、Token、证书私钥
   - 数据库连接字符串含凭证
   - SSH 私钥、GPG 密钥

2. **禁止硬编码凭证**
   - 用户名/密码
   - Access Key/Secret Key
   - 认证 Token

3. **禁止绕过安全检查**
   - 禁用 SSL/TLS 验证
   - 注释或删除安全相关代码
   - 关闭认证/授权机制

4. **禁止不安全日志**
   - 记录敏感数据（密码、Token、个人信息）
   - 明文记录凭证

**必须遵守：**

- 使用环境变量或配置文件管理凭证（配置文件需 `.gitignore`）
- 敏感操作需代码审查
- 定期轮换密钥和凭证
- 报告安全漏洞不公开披露

## Commit Guidelines

- Run `./test/run_dt.sh` before committing (ensures tests pass)
- Ensure clang-format and clang-tidy checks pass for modified files
- Kernel code: follow Linux kernel coding style
- User-space code: follow `src/user/.clang-format` and `src/user/.clang-tidy`
- Add or update tests for code changes
- Kernel modules and user-space library may be committed independently
