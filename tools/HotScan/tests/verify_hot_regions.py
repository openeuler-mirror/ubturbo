"""Check HotScan rounds against hot_region_stimulus.c's VA layout."""

import argparse
from pathlib import Path
import struct

PAGES = 256
HOT_A = set(range(16, 48)) | set(range(96, 128))
HOT_B = set(range(160, 192)) | set(range(224, 256))


def interval(value):
    start, end = map(int, value.split(':'))
    if start < 0 or end <= start:
        raise argparse.ArgumentTypeError("expected START:END with END > START")
    return range(start, end)


def check_round(filename, base, page_size, expected, max_noise):
    data = filename.read_bytes()
    if len(data) % 8:
        raise AssertionError(f"{filename}: truncated u64 data")
    seen = set()
    actual = set()
    for (entry,) in struct.iter_unpack('<Q', data):
        va = entry & 0x0000fffffffff000
        if va in seen:
            raise AssertionError(f"{filename}: duplicate VA 0x{va:x}")
        seen.add(va)
        if base <= va < base + PAGES * page_size:
            index = (va - base) // page_size
            if entry >> 48 not in (0, 1) or (entry & 0xfff) != 0x100:
                raise AssertionError(f"{filename}: invalid metadata for page {index}")
            if entry >> 48:
                actual.add(index)
    target = {base + index * page_size for index in range(PAGES)}
    if not target <= seen:
        raise AssertionError(f"{filename}: missing {len(target - seen)} target pages")
    if expected - actual or len(actual - expected) > max_noise:
        raise AssertionError(f"{filename}: missing hot {sorted(expected - actual)[:8]}, "
                             f"unexpected hot {sorted(actual - expected)[:8]}")
    return len(actual - expected)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('scan_dir', type=Path)
    parser.add_argument('base', type=lambda value: int(value, 0))
    parser.add_argument('--page-size', type=int, default=4096)
    parser.add_argument('--max-noise', type=int, default=1,
                        help="allowed unexpectedly accessed pages per round")
    parser.add_argument('--phase-a', type=interval, required=True)
    parser.add_argument('--phase-b', type=interval, required=True)
    parser.add_argument('--idle', type=interval, required=True)
    args = parser.parse_args()
    checked = 0
    noisy = 0
    for rounds, expected in ((args.phase_a, HOT_A), (args.phase_b, HOT_B),
                             (args.idle, set())):
        for number in rounds:
            filename = args.scan_dir / f'scan_{number}.out'
            noisy += check_round(filename, args.base, args.page_size,
                                 expected, args.max_noise)
            checked += 1
    print(f"PASS: {checked} rounds, 256 mapped pages, two phase changes, "
          f"{noisy} unexpected page accesses")


if __name__ == '__main__':
    main()
