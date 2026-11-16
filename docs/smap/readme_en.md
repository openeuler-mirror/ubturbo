# SMAP

[简体中文](README.md) | [English](readme_en.md)

SMAP is the tiered-memory scanning and page-migration component integrated by UBTurbo. It consists of the
`libsmap.so` user-space library and the unified `smap.ko` kernel module for access tracking and migration. It supports ordinary 4 KiB
process pages and virtual machines backed by static 2 MiB huge pages.

## Capabilities

- Software access tracking based on Linux page-table Access Flags.
- Optional HIST-based remote-page access statistics on supported hardware.
- Selection and exchange of local cold pages and remote hot pages.
- Migrate-out targets, migrate-back, urgent migration, and remote-to-remote migration.
- Multiple local and remote NUMA nodes and grouped migration policy.

The `smap.remote.scan.mode` setting in `smap_strategy.conf` selects software-only (`0`), combined software/hardware
(`1`, default), or hardware-only (`2`) remote scanning. HIST is supported only on Kunpeng 950; unavailable hardware
falls back to software scanning. Module initialization creates `/dev/smap_scan_dev` and initializes software scanning.
The hardware subsystem is initialized on demand by user-space configuration. The management layer sends PID,
page-table scanning, and frequency-query commands through the scan device and receives the results.

## Architecture

![SMAP layered architecture](images/smap-architecture.svg)

The detailed architecture and operating notes are maintained in the synchronized [Chinese entry](README.md).

## Build

```bash
cd plugins/smap
./build.sh -t release
make -C src/kernel KERNEL_VERSION=openeuler -j"$(nproc)"
make -C src/ucache -j"$(nproc)"
```

Kernel development files must match the running kernel. See the [Chinese entry](README.md) for module loading and
troubleshooting.

## Documentation

- [API reference](api_reference.md)
- [Algorithm design](performance_algorithm.md)
- [Release notes](release_notes.md)

Detailed documents are maintained in Chinese; this README is the synchronized English entry.
