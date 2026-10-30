#!/usr/bin/env python3
"""Run one HotScan VM trial and measure completed rounds from dmesg timestamps."""
import argparse
import re
import shutil
import statistics
import struct
import subprocess
import time
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument('--pid', type=int, required=True)
parser.add_argument('--workers', type=int, required=True)
parser.add_argument('--period-ms', type=int, default=2)
parser.add_argument('--seconds', type=float, default=4)
parser.add_argument('--module', default='/root/hotscan-bench/hotscan.ko')
args = parser.parse_args()
tag = f'bench_{int(time.time())}_{args.workers}'
marker = f'hotscan benchmark begin {tag}'
Path('/dev/kmsg').write_text(marker + '\n')
subprocess.run(['insmod', args.module, f'target_pid={args.pid}',
                f'num_workers={args.workers}', f'scan_period_ms={args.period_ms}',
                f'test_tag={tag}'], check=True)
try:
    time.sleep(args.seconds)
finally:
    subprocess.run(['rmmod', 'hotscan'], check=True)
log = subprocess.check_output(['dmesg'], text=True).split(marker, 1)[-1]
records = [(int(round_no), float(stamp), int(elapsed))
           for stamp, round_no, elapsed in re.findall(
               r'\[\s*([\d.]+)\] hotscan: round (\d+) scan completed, elapsed (\d+) ms', log)]
if len(records) < 3:
    raise SystemExit(f'only {len(records)} completion logs found')
# Drop first and last rounds to avoid warmup and unload effects.
records = records[1:-1]
intervals = [b[1] - a[1] for a, b in zip(records, records[1:])]
files = list(Path('/tmp', tag).glob('scan_*.out'))
sizes = [p.stat().st_size for p in files]
sample = min(files, key=lambda p: int(p.stem.split('_')[-1]))
entries = [entry[0] for entry in struct.iter_unpack('<Q', sample.read_bytes())]
addresses = [entry & 0x0000FFFFFFFFF000 for entry in entries]
hot_entries = sum(bool(entry & (1 << 48)) for entry in entries)
if len(addresses) != len(set(addresses)) or hot_entries == 0:
    raise SystemExit('invalid scan data: duplicate addresses or no accessed pages')
print(f'workers={args.workers} period_ms={args.period_ms} rounds={len(records)} '
      f'rate_hz={(len(records)-1)/(records[-1][1]-records[0][1]):.2f} '
      f'median_interval_ms={statistics.median(intervals)*1000:.2f} '
      f'median_elapsed_ms={statistics.median(r[2] for r in records):.2f} '
      f'p90_elapsed_ms={sorted(r[2] for r in records)[int(.9*len(records))]} '
      f'skips={log.count("previous scan still running, skip")} '
      f'files={len(files)} min_entries={min(sizes)//8 if sizes else 0} '
      f'max_entries={max(sizes)//8 if sizes else 0} '
      f'unique_addresses={len(set(addresses))} hot_entries={hot_entries}')
shutil.rmtree(Path('/tmp', tag))
