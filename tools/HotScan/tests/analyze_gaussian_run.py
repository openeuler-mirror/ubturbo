#!/usr/bin/env python3
"""Validate archived VM scans against the Gaussian stimulus and draw evidence."""
import argparse
import csv
import json
import re
import tarfile
import tempfile
from pathlib import Path

import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import numpy as np

import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from hotplot import ActcPlot
from parse_smap import parse_raw_smap_pte_file

VA_MASK = 0x0000fffffffff000
PAGES = 256


def analyze(scan_dir, image_dir):
    run_log = (scan_dir / 'run.log').read_text()
    base = int(re.search(r'base=(0x[0-9a-f]+)', run_log).group(1), 16)
    page_size = int(re.search(r'page_size=(\d+)', run_log).group(1))
    start = int(re.search(r'start_monotonic_ns=(\d+)', run_log).group(1)) / 1e9
    files = sorted(scan_dir.glob('scan_*.out'), key=lambda p: int(p.stem[5:]))
    rounds = [int(p.stem[5:]) for p in files]
    assert rounds == list(range(len(files))) and len(files) >= 200, 'incomplete scan series'
    scans = np.zeros((len(files), PAGES), dtype=np.int64)
    target_vas = base + np.arange(PAGES, dtype=np.uint64) * page_size
    for row, filename in enumerate(files):
        raw = np.fromfile(filename, dtype='<u8')
        assert filename.stat().st_size == raw.size * 8 and raw.size > PAGES
        vas = raw & VA_MASK
        assert np.unique(vas).size == raw.size, f'duplicate VA: {filename}'
        positions = np.searchsorted(np.sort(vas), target_vas)
        assert np.array_equal(np.sort(vas)[positions], target_vas), f'missing target page: {filename}'
        mask = (vas >= base) & (vas < base + PAGES * page_size)
        indices = ((vas[mask] - base) // page_size).astype(int)
        assert len(indices) == PAGES
        assert np.all((raw[mask] & 0xfff) == 0x100), f'bad NUMA metadata: {filename}'
        assert np.all((raw[mask] >> 48) <= 1), f'bad access flag: {filename}'
        scans[row, indices] = (raw[mask] >> 48).astype(np.int64)

    with (scan_dir / 'truth.csv').open() as stream:
        truth_rows = list(csv.DictReader(stream))
    assert [int(row['page']) for row in truth_rows] == list(range(PAGES))
    touches = np.array([int(row['touches']) for row in truth_rows])
    probability = np.array([float(row['probability']) for row in truth_rows])
    with (scan_dir / 'ticks.csv').open() as stream:
        ticks = list(csv.DictReader(stream))
    assert len(ticks) == 1200 and [int(row['tick']) for row in ticks] == list(range(1200))
    assert sum(int(row['touches']) for row in ticks) == int(touches.sum())
    assert all(int(row['active']) == int((int(row['tick']) // 100) % 2 == 0)
               for row in ticks)
    assert all(int(row['touches']) == 0 for row in ticks if int(row['active']) == 0)

    log = (scan_dir / 'kernel.log').read_text()
    timestamps = {int(number): float(stamp) for stamp, number in re.findall(
        r'\[\s*([\d.]+)\] hotscan: round (\d+) scan completed', log)}
    assert sorted(timestamps) == rounds, 'missing kernel timestamp'
    times = np.array([timestamps[n] - start for n in rounds])
    assert np.all(np.diff(times) > 0)

    # Verify that the repository parser passes the same per-page access data to hotplot.
    actc, page_masks, local_masks, _, unique_va = parse_raw_smap_pte_file(
        str(scan_dir / 'scan_*.out'))
    target_indices = np.searchsorted(unique_va, target_vas)
    assert np.array_equal(np.asarray(unique_va)[target_indices], target_vas)
    assert np.array_equal(actc[:, target_indices], scans), 'parser disagrees with raw u64 data'
    assert np.all(page_masks[:, target_indices]) and np.all(local_masks[:, target_indices])

    # Exclude initial PTE warmup and 250 ms around each phase edge.
    on, off = [], []
    for row, elapsed in enumerate(times):
        if not 0.25 < elapsed < 23.75 or elapsed % 2 < 0.25 or elapsed % 2 > 1.75:
            continue
        (on if int(elapsed // 2) % 2 == 0 else off).append(int(scans[row, 96:160].sum()))
    assert len(on) >= 70 and len(off) >= 70
    totals = scans[5:].sum(axis=0)
    correlation = float(np.corrcoef(totals, touches)[0, 1])
    mean_on, mean_off = float(np.mean(on)), float(np.mean(off))
    assert correlation > 0.95, f'Gaussian shape correlation {correlation:.3f}'
    assert mean_on > 20 and mean_off < 0.5, f'phase response {mean_on:.2f}/{mean_off:.2f}'
    assert abs(int(np.argmax(totals)) - 128) <= 12

    image_dir.mkdir(parents=True, exist_ok=True)
    plot = ActcPlot(actc, str(image_dir), page_masks=page_masks,
                    local_ratio=0.75, plot=True)
    plot.plot_lifetime_hot_zone(False)
    plot.plot_lifetime_hot_zone(True)
    target_plot = ActcPlot(actc[:, target_indices], str(image_dir / 'target_mapping'),
                           page_masks=page_masks[:, target_indices],
                           local_ratio=0.75, plot=True)
    target_plot.plot_lifetime_hot_zone(False)
    target_plot.plot_lifetime_hot_zone(True)

    fig, ax = plt.subplots(figsize=(12, 5))
    x = np.arange(PAGES)
    ax.plot(x, totals / totals.max(), color='#df4d27', lw=2,
            label='HotScan: lifetime accesses (normalized)')
    ax.plot(x, touches / touches.max(), color='#175ca3', lw=1.5,
            label='Stimulus: actual page touches (normalized)')
    ax.plot(x, probability / probability.max(), color='#333333', ls='--', lw=1.2,
            label='Gaussian access probability')
    ax.set(xlabel='Page index within the 256-page VA mapping', ylabel='Relative intensity',
           xlim=(0, 255), ylim=(0, 1.08),
           title=f'Gaussian VA hotspot: HotScan versus stimulus (r = {correlation:.4f})')
    ax.grid(alpha=.2)
    ax.legend()
    fig.tight_layout()
    fig.savefig(image_dir / 'gaussian_truth_overlay.png', dpi=160)
    plt.close(fig)

    fig, (top, bottom) = plt.subplots(2, 1, figsize=(12, 7), sharex=True,
                                      gridspec_kw={'height_ratios': [1, 2]})
    active = np.array([int(row['active']) for row in ticks])
    tick_times = np.array([int(row['monotonic_ns']) / 1e9 - start for row in ticks])
    top.step(tick_times, active, where='post', color='#175ca3', label='Stimulus active')
    top.set(ylabel='Active', ylim=(-.1, 1.2), title='Periodic stimulus and observed VA accesses')
    top.grid(alpha=.2)
    bottom.plot(times, scans[:, 96:160].sum(axis=1), color='#df4d27', lw=1,
                label='HotScan hot-core pages accessed per round')
    bottom.set(xlabel='Seconds since stimulus start', ylabel='Accessed pages / scan',
               xlim=(-.5, 24), ylim=(0, 70))
    bottom.grid(alpha=.2)
    top.legend(loc='upper right')
    bottom.legend(loc='upper right')
    fig.tight_layout()
    fig.savefig(image_dir / 'gaussian_phase_response.png', dpi=160)
    plt.close(fig)

    metrics = {
        'rounds': len(files), 'target_pages_per_round': PAGES,
        'all_process_pages': int(len(unique_va)),
        'target_index_start_in_global_plot': int(target_indices[0]),
        'target_index_end_in_global_plot': int(target_indices[-1]),
        'base_va': hex(base), 'page_size': page_size,
        'first_scan_relative_s': round(float(times[0]), 4),
        'last_scan_relative_s': round(float(times[-1]), 4),
        'gaussian_center_page': 127.5, 'gaussian_sigma_pages': 35,
        'stimulus_total_touches': int(touches.sum()),
        'scan_total_accesses_after_warmup': int(totals.sum()),
        'scan_peak_page': int(np.argmax(totals)),
        'scan_peak_count': int(totals.max()),
        'shape_pearson_r': round(correlation, 6),
        'stable_active_rounds': len(on), 'stable_idle_rounds': len(off),
        'active_core_accesses_per_round': round(mean_on, 4),
        'idle_core_accesses_per_round': round(mean_off, 4),
        'parser_matches_raw': True,
    }
    return metrics


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('archive', type=Path)
    parser.add_argument('--image-dir', type=Path, default=Path('img/hotscan-gaussian-vm'))
    parser.add_argument('--metrics', type=Path,
                        default=Path('tests/artifacts/hotscan-gaussian-20260929.json'))
    args = parser.parse_args()
    with tempfile.TemporaryDirectory() as temporary:
        with tarfile.open(args.archive) as archive:
            archive.extractall(temporary, filter='data')
        roots = list(Path(temporary).iterdir())
        assert len(roots) == 1 and roots[0].is_dir()
        metrics = analyze(roots[0], args.image_dir)
    args.metrics.parent.mkdir(parents=True, exist_ok=True)
    args.metrics.write_text(json.dumps(metrics, indent=2) + '\n')
    print(json.dumps(metrics, indent=2))


if __name__ == '__main__':
    main()
