import os
import numpy as np
from copy import deepcopy
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import inspect

class ActcPlot():
    """
    传入的频次数据为每个迁移周期下每个页的累计频次
    """

    def __init__(self, pte_actc: np.array, img_dir: str, local_ratio: float = 0, page_masks: np.array = None,
                 hist_actc: np.array = None, local_mask_preset: np.array = None,
                 interest_mask: np.array = None,
                 mem_limited: bool = True, max_plot_cycle: int = 400, plot: bool = True,
                 real_actc: np.array = None, policy: str = 'zic', verbose: bool = False):
        if local_ratio and local_mask_preset is not None:
            raise ValueError("Cannot set both local_ratio and local_mask_preset")
        if not local_ratio and local_mask_preset is None:
            raise ValueError("One of local_ratio and local_mask_preset should be set")
        self.pte_actc = pte_actc
        self.real_actc = real_actc if real_actc is not None else pte_actc.copy()
        if self.pte_actc.ndim != 2 or self.pte_actc.size == 0:
            raise ValueError("Expected a nonempty (cycles, pages) access array")
        self.cycles = len(pte_actc)
        if page_masks is None:
            self.page_masks = np.ones_like(self.pte_actc, dtype=bool)
        else:
            self.page_masks = page_masks
        self.page_cnt = self.page_masks.sum(axis=1)
        self.page_use_max = len(self.pte_actc[0])
        self.pte_max = self.pte_actc.max()
        self.real_pte_max = self.real_actc.max()
        if hist_actc is None:
            self.enable_hist = False
            self.hist_actc = None
        else:
            self.enable_hist = True
            self.hist_actc = hist_actc
        if local_mask_preset is not None:
            self.local_pages = local_mask_preset.sum(axis=1)
        else:
            self.local_pages = (self.page_cnt * local_ratio).astype(np.int64)
        self.local_mask_preset = local_mask_preset
        self.interest_mask = interest_mask
        self.remote_pages = self.page_cnt - self.local_pages
        self.img_dir = img_dir
        os.makedirs(self.img_dir, exist_ok=True)
        self.mem_limited = mem_limited
        self.plot_enabled = plot
        self.verbose = verbose
        self.policy = policy
        self.max_plot_cycle = min(max_plot_cycle, self.cycles)
        if self.cycles > self.max_plot_cycle:
            print(f"{self.cycles} is too large than {self.max_plot_cycle}, {self.img_dir} will be cut")

    def gen_initial_localmask(self):
        if self.local_mask_preset is None:
            local_mask = np.zeros(self.page_use_max, dtype=bool)
            # 使用 real_actc 第一轮数据降序排序，选择访问频次最高的页作为初始 local 页。
            # 生产环境下第一个迁移周期能保证进行高频扫描，因此 real_actc[0] 能准确反映页面冷热。
            actc = self.real_actc[0].copy()
            actc[~self.page_masks[0]] = -1  # 不存在的页面排到最后，确保不会被选中
            indices = self.custom_argsort(actc)
            local_mask[indices[:self.local_pages[0]]] = True
        else:
            local_mask = self.local_mask_preset[0]
        remote_mask = self.page_masks[0] ^ local_mask
        return local_mask, remote_mask

    def custom_argsort(self, arr):
        # 降序排序，数值相同按照位置先后排序
        auxiliary = np.array([(val, -idx) for idx, val in enumerate(arr)])
        indice = np.lexsort((auxiliary[:, 1], auxiliary[:, 0]))
        return indice[::-1]

    def custom_migrate_mm(self, actc, i):
        indices = self.custom_argsort(actc)
        indices = indices[:self.local_pages[i]]
        local_mask = np.zeros_like(actc, dtype=bool)
        local_mask[indices] = True
        return local_mask

    def custom_migrate(self, actc, i):
        if i > self.cycles - 2:
            i = self.cycles - 2
        if self.local_mask_preset is None:
            local_mask = self.custom_migrate_mm(actc, i)
            return local_mask, self.page_masks[i + 1] ^ local_mask
        else:
            return self.local_mask_preset[i + 1], self.page_masks[i + 1] ^ self.local_mask_preset[i + 1]

    def sort_data(self, data):
        if len(data) == 0:
            return [], []
        sorted_indices = self.custom_argsort(data)
        return data[sorted_indices], sorted_indices

    def __func_name(self):
        return inspect.currentframe().f_back.f_code.co_name

    def create_figures(self, grid_cols=4):
        grid_rows = self.max_plot_cycle // grid_cols + 1
        grid = plt.GridSpec(grid_rows, grid_cols, wspace=0.2, hspace=0.3)
        f = plt.figure(figsize=(35, grid_rows * 5))
        return f, grid

    def get_subfigure(self, figure, grid, index):
        row = index // grid.ncols
        col = index % grid.ncols
        ax = figure.add_subplot(grid[row, col])
        return ax

    def save_img(self, figure, filename: str):
        img_save_path = os.path.join(self.img_dir, filename)
        figure.savefig(img_save_path)
        plt.close(figure)
        if self.verbose:
            print(img_save_path)
        return img_save_path

    def plot(self, ax, x_list, y_list, label='', color='orangered', line: bool = False):
        if isinstance(x_list, list) and isinstance(y_list, list):
            for i, x in enumerate(x_list):
                if len(x_list) == 0:
                    continue
                if i == 0:
                    if line:
                        ax.plot(x_list[i], y_list[i], label=label, color=color)
                    else:
                        ax.scatter(x_list[i], y_list[i], label=label, color=color, marker='X', s=2)
                else:
                    if line:
                        ax.plot(x_list[i], y_list[i], color=color)
                    else:
                        ax.scatter(x_list[i], y_list[i], color=color, marker='X', s=2)
        else:
            if line:
                ax.plot(x_list, y_list, label=label, color=color)
            else:
                ax.scatter(x_list, y_list, label=label, color=color, marker='X', s=2)

    def plot_hot_region(self, sort=True):
        tag = "_sorted" if sort else ""
        f, grid = self.create_figures()
        for i in range(self.max_plot_cycle):
            pte_data = self.real_actc[i][self.page_masks[i]]
            if sort:
                pte_data, sorted_indices = self.sort_data(pte_data)
                hist_data = (self.hist_actc[i][self.page_masks[i]])[sorted_indices] if self.enable_hist else None
            else:
                hist_data = (self.hist_actc[i][self.page_masks[i]]) if self.enable_hist else None
            ax = self.get_subfigure(f, grid, i)
            self.plot(ax, range(self.page_cnt[i]), pte_data,
                      f'accessed pages: {np.count_nonzero(pte_data)}/{self.page_cnt[i]}')
            if self.interest_mask is not None:
                interest_data = self.interest_mask[self.page_masks[i]]
                if sort:
                    interest_data = interest_data[sorted_indices]
                self.plot(ax, range(self.page_cnt[i]), pte_data * interest_data,
                          f'interested pages {np.count_nonzero(pte_data * interest_data)}/{interest_data.sum(dtype="int")}',
                          color='black')

            ax.set(xlim=[0, self.page_cnt[i]], xlabel=f'Page_Index{tag}',
                   ylim=[0, self.frequency_limit(pte_data)], ylabel='Freq')
            ax.legend(loc='upper left')
            if self.enable_hist:
                ax2 = ax.twinx()
                ax2.set(ylim=[0, self.hist_actc.max()])
                self.plot(ax2, range(self.page_cnt[i]), hist_data,
                          f'hist accessed pages: {np.count_nonzero(hist_data)}/{self.page_cnt[i]}', color='green')
                ax2.legend(loc='upper right')
        self.save_img(f, f'{self.__func_name()}{tag}.png')

    def find_p99_value(self, vector):
        return np.percentile(vector, 99, method='lower')

    def frequency_limit(self, vector):
        p99 = self.find_p99_value(vector)
        return max(1, int((p99 if p99 else np.max(vector)) * 1.1) + 1)

    def plot_lifetime_hot_zone(self, sort=True):
        def calc_local_hit_ratio(sorted_data, ratio=0.75, local_page=0):
            freq_sum = sorted_data.sum()
            if local_page:
                freq_hit = sorted_data[:local_page].sum()
            else:
                freq_hit = sorted_data[:int(len(sorted_data) * ratio)].sum()
            hit_ratio = freq_hit / freq_sum if freq_sum else 0
            hit_ratio = round(hit_ratio, 2)
            return hit_ratio

        tag = "_sorted" if sort else ""
        f, ax = plt.subplots(figsize=(10, 8))
        pte_actc_total = self.real_actc.sum(axis=0)
        hist_actc_total = self.hist_actc.sum(axis=0, dtype=np.int64) if self.enable_hist else None
        if sort:
            pte_actc_total, sorted_indices = self.sort_data(pte_actc_total)
            if self.enable_hist:
                hist_actc_total = hist_actc_total[sorted_indices]
        title_hit = f'\n75% hit ratio: {calc_local_hit_ratio(pte_actc_total, ratio=0.75):.2%}' if sort else ''
        self.plot(ax, range(len(pte_actc_total)), pte_actc_total,
                  f'accessed pages: {np.count_nonzero(pte_actc_total)}{title_hit}')
        if self.interest_mask is not None:
            interest_mask = self.interest_mask[sorted_indices] if sort else self.interest_mask
            self.plot(ax, range(len(pte_actc_total)), pte_actc_total * interest_mask,
                      f'interested pages {np.count_nonzero(pte_actc_total * interest_mask)}/'
                      f'{self.interest_mask.sum(dtype="int")}', color='black')

        ax.set_xlabel(f'Page_Index{tag}')
        ax.set_ylabel("Access count")
        ax.set_xlim(0, self.page_use_max)
        ax.set_ylim(0, self.frequency_limit(pte_actc_total))
        ax.ticklabel_format(style='plain', axis='x')
        ax.set_title(f'Lifetime Hot Zone{tag} Plot')
        ax.legend(loc='upper left')
        if self.enable_hist:
            ax2 = ax.twinx()
            ax2.set_ylim(0, hist_actc_total.max())
            title_hit = f'\nhist 75% hit ratio: {calc_local_hit_ratio(hist_actc_total, ratio=0.75):.2%}' if sort else ''
            self.plot(ax2, range(len(pte_actc_total)), hist_actc_total,
                      f'hist accessed pages: {np.count_nonzero(hist_actc_total)}{title_hit}', color='green')
            ax2.ticklabel_format(style='plain', axis='y')
            ax2.legend(loc='upper right')
        self.save_img(f, f'{self.__func_name()}{tag}.png')

    def calc_migrate_pages(self, mask_local: np.array, mask_remote: np.array,
                           mask_local_next: np.array, mask_remote_next: np.array, i):
        if i >= self.cycles - 1:
            return np.zeros_like(self.page_masks[i]), np.zeros_like(self.page_masks[i])
        vaddr_inter = self.page_masks[i] & self.page_masks[i + 1]
        cur_vaddr_local = vaddr_inter & mask_local
        cur_vaddr_remote = vaddr_inter & mask_remote
        next_vaddr_local = vaddr_inter & mask_local_next
        next_vaddr_remote = vaddr_inter & mask_remote_next

        demote_page_mask = cur_vaddr_local & next_vaddr_remote
        promote_page_mask = cur_vaddr_remote & next_vaddr_local
        return demote_page_mask, promote_page_mask

    def plot_actc_sorted_partition(self, thre: float = 0, policy=None):
        if policy is None:
            policy = self.policy
        tag = f"_thre_{thre:.2}" if thre else ""
        f, grid = (None, None)
        if self.plot_enabled:
            f, grid = self.create_figures()
        total_migrate = 0
        total_local_hit = total_access = 0
        hist_total_local_hit = hist_total_access = 0
        cycle_stats = []
        mask_local, mask_remote = self.gen_initial_localmask()
        for i in range(self.cycles):
            # 清理已释放页面的残留标记：mask_local/mask_remote 可能包含
            # 在上一轮存在但本轮已被释放的页，约束到当前 cycle 的实际页集合。
            mask_local = mask_local & self.page_masks[i]
            mask_remote = mask_remote & self.page_masks[i]

            if policy == 'zic':
                data_tmp = deepcopy(self.pte_actc[i])
                data_tmp[mask_local] *= int(self.pte_max + 1)
                mask_local_next, mask_remote_next = self.custom_migrate(data_tmp, i)
            elif thre > 0:
                data_tmp = deepcopy(self.pte_actc[i])
                data_tmp[mask_local] += int(self.pte_max * thre)
                mask_local_next, mask_remote_next = self.custom_migrate(data_tmp, i)
            else:
                mask_local_next, mask_remote_next = self.custom_migrate(self.pte_actc[i], i)

            # 新分配的页面始终从本地内存分配，将其视为 local
            if i < self.cycles - 1:
                new_pages = self.page_masks[i + 1] & ~self.page_masks[i]
                if new_pages.any():
                    mask_local_next[new_pages] = True
                    mask_remote_next[new_pages] = False

            demotion_mask, promotion_mask = self.calc_migrate_pages(mask_local, mask_remote,
                                                                    mask_local_next, mask_remote_next, i)
            nr_migrate = sum(demotion_mask) + sum(promotion_mask)
            total_migrate += nr_migrate

            pte_data = self.real_actc[i]
            pte_data_local, pte_data_remote = pte_data[mask_local], pte_data[mask_remote]
            pte_hit_ratio = pte_data_local.sum() / pte_data.sum() if pte_data.sum() else 0
            total_local_hit += pte_data_local.sum()
            total_access += pte_data.sum()

            cycle_stat = {
                'cycle': i,
                'accessed': int(np.count_nonzero(pte_data)),
                'total': int(self.page_cnt[i]),
                'local_hit_ratio': pte_hit_ratio,
                'demote': int(sum(demotion_mask)),
                'promote': int(sum(promotion_mask)),
                'nr_migrate': int(nr_migrate),
                'local_pages': int(mask_local.sum()),
                'remote_pages': int(mask_remote.sum()),
                'local_access': int(pte_data_local.sum()),
                'total_access': int(pte_data.sum()),
            }
            if self.enable_hist:
                hist_data = self.hist_actc[i]
                hist_data_local = hist_data[mask_local]
                hist_hit_ratio = hist_data_local.sum() / hist_data.sum() if hist_data.sum() else 0
                hist_total_local_hit += hist_data_local.sum()
                hist_total_access += hist_data.sum()
                cycle_stat['hist_hit_ratio'] = hist_hit_ratio

            if self.interest_mask is not None:
                interest_remote = self.interest_mask & self.page_masks[i] & mask_remote
                cycle_stat['interest_remote'] = int(interest_remote.sum())

            cycle_stats.append(cycle_stat)

            if i < self.max_plot_cycle and self.plot_enabled:
                # Constrain masks to current cycle's page_masks[i] to ensure
                # x/y sizes match when page_masks vary across cycles.
                plot_mask_local = mask_local & self.page_masks[i]
                plot_mask_remote = mask_remote & self.page_masks[i]
                n_plot_local = int(plot_mask_local.sum())

                ax = self.get_subfigure(f, grid, i)
                ax.axvline(n_plot_local, color='b', linestyle='--')
                pte_data_local, sorted_indices_local = self.sort_data(pte_data[plot_mask_local])
                pte_data_remote, sorted_indices_remote = self.sort_data(pte_data[plot_mask_remote])
                migration_mask = np.concatenate((demotion_mask[plot_mask_local][sorted_indices_local],
                                                 promotion_mask[plot_mask_remote][sorted_indices_remote]))
                self.plot(ax, range(self.page_cnt[i]), migration_mask * self.real_pte_max,
                          f'pages to do migration\nPromote:{sum(promotion_mask)} Demote:{sum(demotion_mask)}',
                          color='blue')
                self.plot(ax, [range(n_plot_local), range(n_plot_local, self.page_cnt[i])],
                          [pte_data_local, pte_data_remote],
                          f'accessed pages: {np.count_nonzero(pte_data)}/{self.page_cnt[i]}\n'
                          f'local hit ratio: {pte_hit_ratio:.2%}')
                ax.set(xlim=[0, self.page_cnt[i]], ylim=[0, self.frequency_limit(pte_data)],
                       xlabel=f'Sorted_Page_index{i}_Mig{nr_migrate}',
                       ylabel='PageFrequency')
                ax.legend(loc='upper left')
                if self.enable_hist:
                    ax2 = ax.twinx()
                    ax2.set_ylim(0, self.hist_actc.max())
                    hist_data_local = hist_data[plot_mask_local][sorted_indices_local]
                    hist_data_remote = hist_data[plot_mask_remote][sorted_indices_remote]
                    self.plot(ax2, [range(n_plot_local), range(n_plot_local, self.page_cnt[i])],
                              [hist_data_local, hist_data_remote],
                              f'hist local hit ratio: {hist_hit_ratio:.2%}', color='green')
                    ax2.legend(loc='upper right')

            mask_local, mask_remote = deepcopy(mask_local_next), deepcopy(mask_remote_next)

        if self.enable_hist:
            avg_hit = round(hist_total_local_hit * 100.0 / hist_total_access, 2)
        else:
            avg_hit = round(total_local_hit * 100.0 / total_access, 2) if total_access else 0

        if self.plot_enabled:
            f.suptitle(f'Average local hit: {avg_hit:.2f}% | Total migrations: {total_migrate}')
            file_path = f'{self.__func_name()}{tag}.png'
            file_path = self.save_img(f, file_path)
        else:
            file_path = f'{self.__func_name()}{tag}'
            self._print_cycle_stats(cycle_stats, avg_hit, total_migrate)
        return file_path, avg_hit

    def _print_cycle_stats(self, stats: list, avg_hit: float, total_migrate: int):
        """打印每个周期的统计数据（格式化的表格），用于非绘图模式。

        verbose 模式下打印完整表格；默认只打印 Total cycles 汇总行。
        """
        # 汇总时去掉前2个和后1个cycle，避免冷启动和收尾阶段的数据干扰
        if len(stats) > 3:
            summary_stats = stats[2:-1]
        else:
            summary_stats = stats

        cum_local_access = sum(s['local_access'] for s in summary_stats)
        cum_total_access = sum(s['total_access'] for s in summary_stats)
        summary_avg_hit = round(cum_local_access * 100.0 / cum_total_access, 2) if cum_total_access else 0
        summary_migrate = sum(s['nr_migrate'] for s in summary_stats)

        has_hist = any('hist_hit_ratio' in s for s in stats)
        has_interest = any('interest_remote' in s for s in stats)
        if has_interest:
            cum_interest_remote = sum(s['interest_remote'] for s in summary_stats)

        if self.verbose:
            if has_hist:
                header = (f"{'Cyc':>5s} {'Hit%':>8s} {'HistHit%':>9s} {'Mig':>6s} "
                          f"{'Prom':>7s} {'Dem':>6s} {'Pages':>7s} {'Local':>7s} {'Remote':>7s} ")
            else:
                header = (f"{'Cyc':>5s} {'Hit%':>8s} {'Mig':>6s} "
                          f"{'Prom':>7s} {'Dem':>6s} {'Pages':>7s} {'Local':>7s} {'Remote':>7s} ")
            if has_interest:
                header += f"{'IntRmt':>7s} "
            header += f"{'AccLocal/AccTotal':>18s}"

            sep = "-" * len(header)
            print(f"\n=== {self.__func_name()} ===")
            print(sep)
            print(header)
            print(sep)

            for s in stats:
                if has_hist:
                    line = (f"{s['cycle']:>5d} {s['local_hit_ratio']:>7.2%} {s['hist_hit_ratio']:>8.2%} "
                            f"{s['nr_migrate']:>6d} "
                            f"{s['promote']:>7d} {s['demote']:>6d} "
                            f"{s['total']:>7d} {s['local_pages']:>7d} {s['remote_pages']:>7d} ")
                else:
                    line = (f"{s['cycle']:>5d} {s['local_hit_ratio']:>7.2%} "
                            f"{s['nr_migrate']:>6d} "
                            f"{s['promote']:>7d} {s['demote']:>6d} "
                            f"{s['total']:>7d} {s['local_pages']:>7d} {s['remote_pages']:>7d} ")
                if has_interest:
                    line += f"{s['interest_remote']:>7d} "
                line += f"{s['local_access']:>8d}/{s['total_access']:<8d}"
                print(line)

            print(sep)

        if len(stats) > 3:
            print(f"Total cycles: {len(stats)}  |  Filtered: {len(summary_stats)} (drop first 2 + last 1)  |  "
                  f"Avg Hit: {summary_avg_hit:.2f}%  |  "
                  f"Total Migrate: {summary_migrate}  |  Cum Acc: {cum_local_access}/{cum_total_access}")
        else:
            print(f"Total cycles: {len(stats)}  |  Avg Hit: {summary_avg_hit:.2f}%  |  "
                  f"Total Migrate: {summary_migrate}  |  Cum Acc: {cum_local_access}/{cum_total_access}")

        if self.verbose and has_interest:
            print(f"  Interest Remote: {cum_interest_remote}")

        if self.verbose:
            print()

    def sim_funcs(self):
        funcs = [
            [self.plot_hot_region, False],  # 绘制实时热区
            [self.plot_hot_region, True],
            [self.plot_lifetime_hot_zone, False],  # 绘制累计频次热区
            [self.plot_lifetime_hot_zone, True],
            self.plot_actc_sorted_partition,  # 分区迁移分析（--no-plot 时仅打印）
        ]
        if not self.plot_enabled:
            # 不画图时只跑分区迁移分析（会打印计算数据），跳过绘图函数
            funcs = [self.plot_actc_sorted_partition]
        return funcs

    def plot_imgs(self, funcs: list, force_sequence=False):
        # pyplot maintains process-wide figure state; draw in order.
        for func in funcs:
            if isinstance(func, list):
                func[0](*func[1:])
            else:
                func()
