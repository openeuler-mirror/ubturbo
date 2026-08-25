# AGENTS.md

## Project Overview

UBTurbo - an open-source in-node resource management framework providing configuration loading, plugin loading, logging, and IPC capabilities, with integrated SMAP for multi-level (tiered) memory scheduling. C++17/C11 CMake project targeting openEuler Linux on aarch64 only.

Typical use case: the RMRS memory migration tool is developed on the UBTurbo framework and runs inside the UBTurbo daemon, exposing memory migration decisions and execution to external processes via IPC. RMRS configuration is read from config files, and logs are emitted through the UBTurbo logging facility.

## Build Commands

```shell
# Initialize third-party submodules first (libboundscheck, rapidjson, googletest, mockcpp)
git submodule update --init --recursive
# On a fresh checkout (especially from Windows), fix line endings
dos2unix build.sh

# Release build (default) -> dist/release/
./build.sh

# Debug build -> dist/debug/
./build.sh -D

# Release with debug info
./build.sh -T RelWithDebInfo

# Build and package as RPM (via CPack)
./build.sh package

# Clean build directory
./build.sh -c

# Sanitizer builds
./build.sh --asan      # AddressSanitizer
./build.sh --lsan      # LeakSanitizer
./build.sh --tsan      # ThreadSanitizer
./build.sh --ubsan     # UndefinedBehaviorSanitizer
```

Build artifacts:

- Binary: `dist/release/bin/ub_turbo_exec` (UBTurbo daemon, from `src/main.cpp`)
- Library: `dist/release/lib/libubturbo_client.so` (client SDK)
- Config: `dist/release/conf/` (`ubturbo.conf`, `ubturbo_plugin_admission.conf`)

## Test Commands

```shell
# Build + run all UBTurbo core & RMRS unit tests (also generates an lcov coverage report)
./build.sh -t test
# or run the test script directly:
sh test/run_ut.sh

# SMAP plugin unit tests
cd plugins/smap/test && sh run_dt.sh

# Run a specific test case via gtest_filter (invoke the compiled binary directly)
./test/build/ubturbo_ut --gtest_filter="SuiteName.CaseName"
./test/build/rmrs_ut --gtest_filter="SuiteName.CaseName"

# Build tests without running
./build.sh ut --skip-run-tests
```

| Test binary | Scope |
|-------------|-------|
| `ubturbo_ut` | UBTurbo core modules (config, log, ipc, plugin, smap) |
| `rmrs_ut` | RMRS plugin (migrate, smap_helper, ucache) |
| `smap_dt` | SMAP plugin (drivers, tiering, user, ucache) |

> `test/run_ut.sh` and `plugins/smap/test/run_dt.sh` auto-generate a coverage report via lcov/genhtml. lcov is not in the openEuler 24.03 official repo and must be installed separately (e.g. from source).

## Code Style

- C++17 / C11 standard (enforced in `CMakeLists.txt`, `CMAKE_CXX_EXTENSIONS NO`)
- clang-format for formatting: Google-based, 4-space indent, 120-column limit (see `.clang-format`)
- clang-tidy for static analysis (see `.clang-tidy`); Release config for `src/`, Debug for `test/`
- pre-commit hooks (`.pre-commit-config.yaml`) enforce: Release build, test build, clang-format, and clang-tidy — all must pass
- Secure compile flags: `-fstack-protector-strong`, `-Wl,-z,noexecstack -Wl,-z,relro -Wl,-z,now`, `-pie`, `-D_FORTIFY_SOURCE=2`
- Comments in Chinese or English as appropriate

## Dev Environment Tips

- aarch64 only; build hard-depends on openEuler (`CMakeLists.txt` reads `/etc/openEuler-release`); recommended openEuler 24.03 LTS
- Build uses Ninja if available, otherwise falls back to Unix Makefiles
- `compile_commands.json` lives in the build directory (`dist/release/` or `dist/debug/`) — point your LSP/clangd there
- Auto-generated headers are placed in the build directory's `include/` (referenced via `${CMAKE_BINARY_DIR}/include`)
- Config files in `conf/` are copied into the build directory at CMake configure time
- Default build type is Release; the `test` target auto-switches to Debug for coverage
- Main executable: `ub_turbo_exec` daemon (from `src/main.cpp`); client SDK: `libubturbo_client.so`
- Install build deps (openEuler):

  ```bash
  dnf install -y make gcc gcc-c++ cmake ninja-build dos2unix chrpath patchelf \
      libboundscheck libvirt-devel findutils git
  ```

## Architecture

```
src/
├── include/        # Internal shared headers
├── config/         # Config module (parses ubturbo.conf etc.)
├── ipc/            # IPC module (UDS, Reactor pattern)
├── log/            # Async ring-buffer logging
├── main.cpp        # Process entry -> ub_turbo_exec
├── main/           # TurboMain daemon lifecycle
├── plugin/         # Plugin manager (dlopen/dlclose)
├── sdk/            # UBTurbo client SDK (libubturbo_client.so)
├── smap/           # SMAP encode/decode
└── utils/          # Utilities

plugins/
├── rmrs/           # RMRS — resource migration/scheduling (needs libvirt-devel)
├── smap/           # SMAP — tiered memory (kernel modules + libsmap.so, aarch64 only)
├── ubdma/          # UBDMA plugin
└── ucache/         # ucache plugin

3rdparty/           # libboundscheck, rapidjson (git submodules)
conf/               # ubturbo.conf, ubturbo_plugin_admission.conf
doc/                # Documentation and architecture diagrams
test/
├── 3rdparty/       # googletest, mockcpp (submodules)
└── testcase/       # Unit tests -> ubturbo_ut, rmrs_ut
plugins/smap/test/  # SMAP tests -> smap_dt
```

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

4. **禁止记录安全日志**
   - 记录敏感数据（密码、Token、个人信息）
   - 明文记录凭证

**必须遵守：**

- 使用环境变量或配置文件管理凭证（配置文件需加入 `.gitignore`）
- 敏感操作需代码审查
- 定期轮换密钥和凭证
- 报告安全漏洞不公开披露

> 项目特定约束：UBTurbo 以节点内存资源管理员权限运行；内存迁移的 `PID`、`srcNid`、`destNid` 等参数须由集群资源管理中心安全下发，UBTurbo 不校验 PID 有效性，需在整体解决方案中保证其传输与存储安全。迁出与迁入内存地址须保证安全性一致，且虚机/容器用户权限与远端内存用户权限一致。

## Commit Guidelines

- pre-commit hooks (`.pre-commit-config.yaml`) enforce Release build, test build, clang-format, and clang-tidy — all must pass
- Run `./build.sh -t test` before committing
- Add or update tests for code changes (`ubturbo_ut` / `rmrs_ut` / `smap_dt`)
- Follow existing commit message conventions: prefix with type such as `Feature:`, `fix:`, `doc:` (Chinese or English), matching recent history (e.g. `Feature: ...`, `doc: ...`, `smap: ...`)
