import argparse
from glob import glob
import os
import sys
from os import path

import numpy as np

# Ensure the script directory is on sys.path so hotplot can be imported
# regardless of the current working directory.
_script_dir = path.dirname(path.abspath(__file__))
if _script_dir not in sys.path:
    sys.path.insert(0, _script_dir)

from hotplot import ActcPlot


def parse_args():
    parser = argparse.ArgumentParser(description="Parse HotScan rounds and draw hot regions")
    parser.add_argument("file_path", help="quoted scan_*.out glob")
    parser.add_argument("policy", choices=["zic"])
    parser.add_argument("--verbose", action="store_true")
    parser.add_argument("--plot", action="store_true")
    parser.add_argument("--sim-scan-period", type=int, default=1,
                        help="combine this many raw rounds into one binary access round")
    parser.add_argument("--assemble-period", type=int, default=1,
                        help="combine this many access rounds into one migration cycle")
    args = parser.parse_args()
    if args.sim_scan_period < 1 or args.assemble_period < 1:
        parser.error("periods must be positive")
    file_path = args.file_path
    # Derive img_tag from the parent directory name of the input path.
    # e.g. "/tmp/actc/*.out" → "actc"
    dir_name = path.basename(path.dirname(file_path))
    img_tag = dir_name if dir_name else "default"
    return (file_path, img_tag, args.policy, args.verbose, args.plot,
            args.sim_scan_period, args.assemble_period)


def parse_one_file(file_path: str):
    raw_actc = np.fromfile(file_path, dtype=np.uint64)
    raw_actc = raw_actc[raw_actc > 0]
    actc = raw_actc >> 48
    local_mask = (raw_actc & 0xf00) >> 8
    local_mask = local_mask.astype(bool)
    # if sum(local_mask) == len(local_mask):
    #     return None, None, None
    vaddr = raw_actc & 0x0000fffffffff000
    indice = np.argsort(vaddr)
    actc = actc[indice]
    vaddr = vaddr[indice]
    local_mask = local_mask[indice]
    return actc, vaddr, local_mask


def load_interest_vas(filepath="/tmp/interest_va.bin", verbose=False):
    """Parse the interest VA binary file if it exists.

    The file stores raw u64 virtual addresses, one per entry.
    Only bits [47:12] (page-aligned) are retained, matching the scan
    entry format in the kernel module.

    Returns None if the file does not exist.
    """
    if not path.exists(filepath):
        return None
    interest_vas = np.fromfile(filepath, dtype=np.uint64)
    # Mask to page-aligned VA [47:12], matching scan entry parsing
    interest_vas = interest_vas & 0x0000fffffffff000
    if verbose:
        print(f"Loaded {len(interest_vas)} interest VAs from {filepath}")
    return interest_vas


def build_interest_mask(unique_va, interest_vas, verbose=False):
    """Build a boolean mask aligned to the unified VA index space.

    unique_va : list[int] — sorted, de-duplicated VAs across all rounds
    interest_vas : np.ndarray[uint64] — raw interest VAs from the binary file

    Returns np.ndarray[bool] of the same length as unique_va, or None.
    """
    if interest_vas is None:
        return None
    interest_set = set(int(va) for va in interest_vas)
    mask = np.array([va in interest_set for va in unique_va], dtype=bool)
    if verbose:
        print(f"Interest mask: {mask.sum()} pages of {len(unique_va)} marked")
    return mask


def data_normalize(actc_list: list, vaddr_list: list, local_mask_list: list):
    unique_va = set()
    for vaddr in vaddr_list:
        unique_va.update(vaddr)
    unique_va = sorted(unique_va)
    va_to_idx = {address: idx for idx, address in enumerate(unique_va)}

    actc_list_expand = []
    page_masks_expand = []
    local_masks_expand = []
    for cycle_i, vaddr in enumerate(vaddr_list):
        actc_data = np.zeros(len(unique_va))
        va_mask = np.zeros(len(unique_va), dtype=bool)
        local_mask = np.zeros(len(unique_va), dtype=bool)
        for i, va in enumerate(vaddr):
            idx = va_to_idx[va]
            actc_data[idx] = actc_list[cycle_i][i]
            va_mask[idx] = True
            local_mask[idx] = local_mask_list[cycle_i][i]
        actc_list_expand.append(actc_data)
        page_masks_expand.append(va_mask)
        local_masks_expand.append(local_mask)

    actc_list_expand = np.array(actc_list_expand, dtype=np.int64)
    page_masks_expand = np.array(page_masks_expand, dtype=bool)
    local_masks_expand = np.array(local_masks_expand, dtype=bool)

    return actc_list_expand, page_masks_expand, local_masks_expand, unique_va


def extract_number(file_name):
    """Extract the round number from a scan filename.

    Expected format: .../scan_<N>.out  → returns N
    """
    # Remove directory and extension, then parse the trailing number.
    base = path.basename(file_name)           # "scan_0.out"
    stem = base.rsplit('.', 1)[0]             # "scan_0"
    return int(stem.split('_')[-1])           # 0


def parse_raw_smap_pte_file(path_prefix: str, interest_vas=None, verbose=False):
    actc_list = []
    vaddr_list = []
    local_mask_list = []
    files = sorted(glob(path_prefix), key=extract_number)
    if not files:
        raise FileNotFoundError(f"No scan files match {path_prefix!r}")

    for file in files:
        actc, vaddr, local_mask = parse_one_file(file)
        if actc is None:
            continue
        actc_list.append(actc)
        vaddr_list.append(vaddr)
        local_mask_list.append(local_mask)

    actc_list, page_masks, local_masks, unique_va = data_normalize(actc_list, vaddr_list, local_mask_list)
    interest_mask = build_interest_mask(unique_va, interest_vas, verbose=verbose)
    return actc_list, page_masks, local_masks, interest_mask, unique_va


def parse_raw_smap_files(file_path: str, make_cache=True, verbose=False):
    """
    SMAP数据源u64编码规则：
    最高16位64-48为频次
    次高36位48-12为vaddr
    中间4位12-8为是否本地NUMA
    低8位为预留
    """
    interest_vas = load_interest_vas(verbose=verbose)
    files = sorted(glob(file_path), key=extract_number)
    if not files:
        raise FileNotFoundError(f"No scan files match {file_path!r}")
    file_names = np.array([path.abspath(file) for file in files])
    stats = [os.stat(file) for file in files]
    file_sizes = np.array([stat.st_size for stat in stats], dtype=np.int64)
    file_mtimes = np.array([stat.st_mtime_ns for stat in stats], dtype=np.int64)
    cache_path = (file_path.split('*')[0] + 'cache.npz' if '*' in file_path
                  else path.join(path.dirname(file_path), 'cache.npz'))
    cache_valid = False
    if path.exists(cache_path) and make_cache:
        with np.load(cache_path, allow_pickle=False) as cached:
            cache_valid = (all(key in cached for key in ('file_names', 'file_sizes', 'file_mtimes'))
                           and np.array_equal(cached['file_names'], file_names)
                           and np.array_equal(cached['file_sizes'], file_sizes)
                           and np.array_equal(cached['file_mtimes'], file_mtimes))
            if cache_valid:
                if verbose:
                    print(f"Reading cache from {cache_path}")
                actc_list, page_masks, local_masks = \
                    cached['actc_list'], cached['page_masks'], cached['local_masks']
                unique_va = cached['unique_va']
                interest_mask = build_interest_mask(unique_va, interest_vas, verbose=verbose)
    if not cache_valid:
        actc_list, page_masks, local_masks, interest_mask, unique_va = \
            parse_raw_smap_pte_file(file_path, interest_vas, verbose=verbose)
        if make_cache:
            np.savez(cache_path, actc_list=actc_list, page_masks=page_masks,
                     local_masks=local_masks, unique_va=unique_va,
                     file_names=file_names, file_sizes=file_sizes,
                     file_mtimes=file_mtimes)
            if verbose:
                print(f"Cache is saved into {cache_path}")

    return actc_list, page_masks, local_masks, interest_mask


def calc_or(actc):
    return actc.max(axis=1)


def calc_sum(actc):
    return actc.sum(axis=1)


def assemble_actc(actc, assemble_steps, func):
    cycles, page_cnt = actc.shape
    merged_cycles = cycles // assemble_steps
    keep = merged_cycles * assemble_steps
    actc = actc[:keep]  # discard trailing rows that don't fit a full group
    actc = np.reshape(actc, (merged_cycles, assemble_steps, page_cnt))
    return func(actc)


def _apply_assemble(actc, page_masks, local_masks, period, actc_func):
    """Assemble all three arrays with the given period. Masks always use calc_or."""
    return (
        assemble_actc(actc, period, actc_func),
        assemble_actc(page_masks, period, calc_or),
        assemble_actc(local_masks, period, calc_or),
    )


def run_smap_results(file_path: str, img_tag: str,
                     assemble_period: int = 1, plot: bool = False,
                     sim_scan_period: int = 1, policy: str = 'zic',
                     verbose: bool = False):
    if assemble_period < 1 or sim_scan_period < 1:
        raise ValueError("periods must be positive")
    if policy != 'zic':
        raise ValueError(f"Unsupported policy: {policy}")
    img_save_dir = f"img/{img_tag}"
    pte_actc, page_masks, local_masks, interest_mask = parse_raw_smap_files(file_path, make_cache=True, verbose=verbose)

    # sim_mig = assemble_period * sim_scan_period — 模拟迁移周期对应的原始扫描周期总数
    sim_mig = assemble_period * sim_scan_period
    if len(pte_actc) < sim_mig:
        raise ValueError(f"Need at least {sim_mig} scan rounds, found {len(pte_actc)}")

    # Pre-trim to a consistent boundary so multiple assemble_actc calls
    # operate on the same underlying raw data without drift.
    if sim_mig > 1:
        mig_cycles = len(pte_actc) // sim_mig
        pte_actc = pte_actc[:mig_cycles * sim_mig]
        page_masks = page_masks[:mig_cycles * sim_mig]
        local_masks = local_masks[:mig_cycles * sim_mig]

    # Save raw data before binary simulation for real_actc
    raw_actc = pte_actc.copy() if sim_scan_period > 1 else None

    # Stage 1: simulate low-frequency scanning — aggregate N scan cycles to binary (0/1)
    if sim_scan_period > 1:
        pte_actc, page_masks, local_masks = _apply_assemble(
            pte_actc, page_masks, local_masks, sim_scan_period, calc_or)

    # Stage 2: accumulate into migration cycles
    if assemble_period > 1:
        pte_actc, page_masks, local_masks = _apply_assemble(
            pte_actc, page_masks, local_masks, assemble_period, calc_sum)

    # real_actc: direct calc_sum over the same total window to preserve frequency info
    real_actc = assemble_actc(raw_actc, sim_mig, calc_sum) if raw_actc is not None else pte_actc.copy()

    p = ActcPlot(pte_actc, img_save_dir, page_masks=page_masks,
                 local_ratio=0.75, interest_mask=interest_mask, plot=plot,
                 real_actc=real_actc, policy=policy, verbose=verbose)
    p.plot_imgs(p.sim_funcs())
    return p


if __name__ == "__main__":
    g_pte_file, img_tag, g_policy, g_verbose, g_plot, g_sim_period, g_assemble_period = parse_args()
    run_smap_results(g_pte_file, img_tag, policy=g_policy, verbose=g_verbose, plot=g_plot,
                     sim_scan_period=g_sim_period, assemble_period=g_assemble_period)
