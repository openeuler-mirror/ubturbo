// SPDX-License-Identifier: GPL-2.0
/*
 * HotScan — 内核态进程页面热度扫描模块
 *
 * 功能：
 *  - 通过 walk_page_range + ptep_test_and_clear_young 遍历目标进程的所有 PTE
 *  - 收集被访问过的页面信息，编码为 u64 数组
 *  - 利用高精度定时器 (hrtimer) 实现精确周期扫描
 *  - 并发扫描：大 VMA 按 VA 均分，小 VMA 整体分配，每个 work 任务只含一个 [start, end) 区间
 *  - 结果写入 /tmp/<test_tag>/scan_<round>.out
 *
 * 模块参数:
 *   target_pid      — 目标进程 PID (默认: 0)
 *   scan_period_ms  — 扫描周期，毫秒 (默认: 1000)
 *   test_tag        — 输出目录名 (默认: 加载时的秒级时间戳)
 *   num_workers     — 并行 worker 数 (默认: 20)
 *
 * 数据格式 (u64 entry):
 *   [63:48]  访问标记 (16b) — 本轮被访问过 = 1
 *   [47:12]  VA[47:12] (36b) — 虚拟地址（去高 16b + 低 12b）
 *   [11:8]   NUMA 本地标记 (4b) — 均为本地 NUMA = 1
 *   [7:0]    预留 (8b)
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/sched/mm.h>
#include <linux/pid.h>
#include <linux/workqueue.h>
#include <linux/hrtimer.h>
#include <linux/ktime.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/namei.h>
#include <linux/dcache.h>
/* 需要同时失效 TLB，否则 ARM64 上持续访问的页面可能一直显示为冷页。 */
#include <linux/pagewalk.h>
#include <linux/pgtable.h>
#include <linux/atomic.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/delay.h>
#include <linux/nodemask.h>
#include <asm/pgtable.h>
#include <asm/tlbflush.h>

/* ========================================================================
 * 模块参数
 * ======================================================================== */

static int target_pid;
module_param(target_pid, int, 0444);
MODULE_PARM_DESC(target_pid, "Target process PID to scan");

static int scan_period_ms = 1000;
module_param(scan_period_ms, int, 0444);
MODULE_PARM_DESC(scan_period_ms, "Scan period in milliseconds (default: 1000)");

static char *test_tag;
module_param(test_tag, charp, 0444);
MODULE_PARM_DESC(test_tag, "Output directory tag under /tmp/ (default: unix timestamp)");

static int num_workers = 20;
module_param(num_workers, int, 0444);
MODULE_PARM_DESC(num_workers, "Number of parallel scan workers (default: 20)");

/* ========================================================================
 * 常量
 * ======================================================================== */

#define HOTSCAN_WQ_NAME         "hotscan_wq"
#define HOTSCAN_OUTPUT_BASE     "/tmp"
#define HOTSCAN_FILE_PREFIX     "scan_"
#define HOTSCAN_FILE_SUFFIX     ".out"
#define HOTSCAN_MAX_CPUS        20
#define HOTSCAN_DEFAULT_TAG_LEN 32
#define HOTSCAN_BATCH_ENTRIES   128

/* u64 编码常量 */
#define HOTSCAN_ACCESSED_MASK   (1ULL << 48)
#define HOTSCAN_VA_MASK         0x0000FFFFFFFFF000ULL  /* VA[47:12] */
#define HOTSCAN_NUMA_LOCAL      (1ULL << 8)            /* NUMA = 1 at [11:8] */

/* ========================================================================
 * 数据结构
 * ======================================================================== */

/*
 * 每轮扫描的共享状态
 *
 * 生命周期: dispatch_work 分配 → scan works 并发写入 buffer
 *           → 最后一个完成的 scan work 触发 io_work
 *           → io_work 写文件后释放
 */
struct scan_state {
	struct mm_struct *mm;           /* 目标进程 mm（mmgrab 已固定） */
	atomic_t remaining;             /* 还在运行的 scan work 计数 */
	atomic_t buf_index;             /* buffer 下一个空闲槽位 */
	u64 *buffer;                    /* 结果缓冲区 */
	size_t buf_entries;             /* buffer 最大条目数 */
	unsigned long round;            /* 扫描轮次（用于文件命名） */
	ktime_t start_time;             /* 扫描开始时间 */
	struct work_struct io_work;     /* I/O 回写 work */
};

/*
 * 每个 scan work 只负责一个 [start, end) 区间
 *
 * 大 VMA 被均分为多个子区间，每个子区间成为一个独立的 scan work；
 * 小 VMA 整体成为一个 scan work。不再使用链表聚合多个区间。
 */
struct scan_work {
	struct work_struct work;
	unsigned long start;            /* VA 起始地址 */
	unsigned long end;              /* VA 结束地址（不包含） */
	struct scan_state *state;       /* 共享 scan_state */
	u64 batch[HOTSCAN_BATCH_ENTRIES];
	unsigned int batch_count;
};

/* ========================================================================
 * 全局状态
 * ======================================================================== */

static struct workqueue_struct *hotscan_wq;
static struct workqueue_struct *hotscan_io_wq;
static struct hrtimer hotscan_timer;
static struct work_struct dispatch_work;

/* 保护周期性触发的并发控制 */
static atomic_t scan_running = ATOMIC_INIT(0);

/* 扫描轮次计数器 */
static atomic64_t scan_round_counter = ATOMIC64_INIT(0);

/* 输出目录路径 */
static char output_dir[256];

/* ========================================================================
 * u64 编码 / 解码
 * ======================================================================== */

/*
 * 将页面信息编码为一个 u64 条目
 * @addr:     页面虚拟地址（页对齐）
 * @accessed: 是否在本轮被访问过
 */
static inline u64 hotscan_encode_entry(unsigned long addr, bool accessed)
{
	u64 entry = 0;

	/* [63:48] 访问标记 */
	if (accessed)
		entry |= HOTSCAN_ACCESSED_MASK;

	/* [47:12] VA[47:12] */
	entry |= (addr & HOTSCAN_VA_MASK);

	/* [11:8] NUMA 本地 = 1 */
	entry |= HOTSCAN_NUMA_LOCAL;

	/* [7:0] 预留 (保持为 0) */

	return entry;
}

/* ========================================================================
 * Buffer 操作
 * ======================================================================== */

/*
 * 每个 work 先在自己的 batch 内收集结果，再一次性预留共享 buffer 的空间。
 * 避免所有 worker 每扫描一个页面都争用同一个原子计数器。
 */
static void hotscan_flush_batch(struct scan_work *sw)
{
	struct scan_state *state = sw->state;
	unsigned int count = sw->batch_count;
	int idx;

	if (!count)
		return;
	idx = atomic_fetch_add(count, &state->buf_index);
	if (likely(idx >= 0 && (size_t)idx + count <= state->buf_entries))
		memcpy(state->buffer + idx, sw->batch, count * sizeof(u64));
	else
		pr_warn_ratelimited("hotscan: round %lu: result buffer full, dropped %u entries\n",
				state->round, count);
	sw->batch_count = 0;
}

static void hotscan_buffer_append(struct scan_work *sw, u64 entry)
{
	sw->batch[sw->batch_count++] = entry;
	if (sw->batch_count == HOTSCAN_BATCH_ENTRIES)
		hotscan_flush_batch(sw);
}

/* ========================================================================
 * 页表遍历回调
 * ======================================================================== */

/* PTE 层回调：检查并清除 Access Flag，记录被访问的页面 */
static int hotscan_pte_entry(pte_t *pte, unsigned long addr,
			      unsigned long next, struct mm_walk *walk)
{
	struct scan_work *sw = walk->private;
	int young;

	/* 跳过文件映射页（代码段、共享库、mmap 文件等），只扫描匿名页 */
	if (walk->vma->vm_file)
		return 0;

	/* 跳过不存在的 PTE */
	if (pte_none(*pte))
		return 0;

	/* 跳过非 present 的页面（已换出等） */
	if (!pte_present(*pte))
		return 0;

	/* TLB 在本轮全部 worker 完成后统一失效。 */
	young = ptep_test_and_clear_young(walk->vma, addr, pte);

	/* 记录全部 present 页面，[63:48] 编码本轮访问标记 */
	hotscan_buffer_append(sw, hotscan_encode_entry(addr, young));

	return 0;
}

/* mm_walk_ops 定义 */
static const struct mm_walk_ops hotscan_walk_ops = {
	.pte_entry  = hotscan_pte_entry,
	.walk_lock  = PGWALK_RDLOCK,
};

/* ARM64 的 ptep_clear_flush_young() 引用了模块不可用的 MMU notifier 符号。
 * 普通用户进程的扫描在全部 worker 完成后失效目标 mm 的 TLB，下一轮才
 * 能重新观察硬件 Access Flag。此路径不支持二级 MMU 映射的目标进程。
 */
static void hotscan_flush_mm_tlb(struct mm_struct *mm)
{
	unsigned long asid;

	dsb(ishst);
	asid = __TLBI_VADDR(0, ASID(mm));
	__tlbi(aside1is, asid);
	__tlbi_user(aside1is, asid);
	dsb(ish);
}

/* ========================================================================
 * 文件 I/O
 * ======================================================================== */

/*
 * 将 buffer 内容写入输出文件
 * 文件路径: /tmp/<test_tag>/scan_<round>.out
 */
static int hotscan_write_buffer(struct scan_state *state)
{
	struct file *filp;
	loff_t pos = 0;
	char filepath[512];
	size_t count;
	int ret;

	count = (size_t)atomic_read(&state->buf_index);
	if (count > state->buf_entries)
		count = state->buf_entries;

	if (count == 0) {
		pr_debug("hotscan: round %lu: no pages accessed, skipping file write\n",
			state->round);
		return 0;
	}

	snprintf(filepath, sizeof(filepath), "%s/%s%lu%s",
		 output_dir, HOTSCAN_FILE_PREFIX,
		 state->round, HOTSCAN_FILE_SUFFIX);

	pr_debug("hotscan: round %lu: writing %zu entries to %s\n",
		state->round, count, filepath);

	filp = filp_open(filepath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (IS_ERR(filp)) {
		ret = PTR_ERR(filp);
		pr_err("hotscan: failed to open %s: %d\n", filepath, ret);
		return ret;
	}

	ret = kernel_write(filp, state->buffer, count * sizeof(u64), &pos);
	if (ret < 0) {
		pr_err("hotscan: failed to write %s: %d\n", filepath, ret);
	} else if ((size_t)ret != count * sizeof(u64)) {
		pr_warn("hotscan: partial write to %s: %d/%zu bytes\n",
			filepath, ret, count * sizeof(u64));
		ret = -ENOSPC;
	} else {
		pr_debug("hotscan: round %lu: wrote %zu entries (%zu bytes) OK\n",
			state->round, count, count * sizeof(u64));
		ret = 0;
	}

	filp_close(filp, NULL);
	return ret;
}

/* ========================================================================
 * 目录管理
 * ======================================================================== */

/*
 * 创建输出目录 /tmp/<test_tag>/
 * 使用 VFS 接口（vfs_mkdir），从内核态创建目录
 */
static int hotscan_create_output_dir(void)
{
	struct path parent;
	struct dentry *dentry;
	int err;

	/* 获取父目录 /tmp */
	err = kern_path(HOTSCAN_OUTPUT_BASE, LOOKUP_DIRECTORY, &parent);
	if (err) {
		pr_err("hotscan: parent directory %s not found: %d\n",
		       HOTSCAN_OUTPUT_BASE, err);
		return err;
	}

	inode_lock(d_inode(parent.dentry));

	/* 查找或创建目标目录 */
	dentry = lookup_one_len(output_dir + strlen(HOTSCAN_OUTPUT_BASE) + 1,
				parent.dentry,
				strlen(output_dir + strlen(HOTSCAN_OUTPUT_BASE) + 1));
	if (IS_ERR(dentry)) {
		err = PTR_ERR(dentry);
		pr_err("hotscan: lookup_one_len failed: %d\n", err);
		inode_unlock(d_inode(parent.dentry));
		path_put(&parent);
		return err;
	}

	if (d_is_negative(dentry)) {
		/* 目录不存在，创建 */
		err = vfs_mkdir(&nop_mnt_idmap, d_inode(parent.dentry),
				dentry, 0755);
		if (err) {
			pr_err("hotscan: vfs_mkdir %s failed: %d\n",
			       output_dir, err);
		} else {
			pr_info("hotscan: created output directory %s\n",
				output_dir);
		}
	} else {
		pr_info("hotscan: output directory %s already exists\n",
			output_dir);
		err = 0;
	}

	dput(dentry);
	inode_unlock(d_inode(parent.dentry));
	path_put(&parent);
	return err;
}

/*
 * 生成 output_dir 路径: /tmp/<test_tag>
 */
static void hotscan_setup_output_dir(void)
{
	if (test_tag && strlen(test_tag) > 0) {
		snprintf(output_dir, sizeof(output_dir), "%s/%s",
			 HOTSCAN_OUTPUT_BASE, test_tag);
	} else {
		/* 默认使用秒级时间戳 */
		snprintf(output_dir, sizeof(output_dir), "%s/%ld",
			 HOTSCAN_OUTPUT_BASE,
			 (long)ktime_get_real_seconds());
	}
}

/* ========================================================================
 * 地址空间分割辅助函数
 * ======================================================================== */

/* 前向声明：scan work 回调，queue_one_work 需要引用 */
static void hotscan_scan_work_func(struct work_struct *work);

/*
 * 判断 VMA 是否足够大，应触发并发拆分
 *
 * 阈值通常为 total_pages / num_workers，即每个 worker 平均分配到的页数。
 * VMA 页面数达到该阈值时，拆分为 num_workers 个子区间以实现并行扫描。
 */
static bool hotscan_vma_is_large(struct vm_area_struct *vma,
				 unsigned long threshold_pages)
{
	return ((vma->vm_end - vma->vm_start) >> PAGE_SHIFT) >= threshold_pages;
}

/*
 * 创建一个 scan work 并排队到 hotscan_wq。
 * remaining 包含一个 dispatch 引用，因此 worker 不会在排队结束前释放 state。
 */
static void hotscan_queue_one_work(unsigned long start, unsigned long end,
				   struct scan_state *state)
{
	struct scan_work *sw;

	sw = kzalloc(sizeof(*sw), GFP_KERNEL);
	if (!sw) {
		pr_err("hotscan: failed to allocate scan_work, "
		       "skipping range [0x%lx, 0x%lx)\n", start, end);
		return;
	}

	sw->start = start;
	sw->end = end;
	sw->state = state;
	INIT_WORK(&sw->work, hotscan_scan_work_func);

	atomic_inc(&state->remaining);
	if (!queue_work(hotscan_wq, &sw->work)) {
		atomic_dec(&state->remaining);
		kfree(sw);
	}
}

/* ========================================================================
 * Work 函数
 * ======================================================================== */

static void hotscan_finish_scan(struct scan_state *state)
{
	ktime_t end_time;

	hotscan_flush_mm_tlb(state->mm);
	end_time = ktime_get();
	pr_info("hotscan: round %lu scan completed, elapsed %lld ms (%lld us)\n",
		state->round, ktime_ms_delta(end_time, state->start_time),
		ktime_us_delta(end_time, state->start_time));
	atomic_set(&scan_running, 0);
	queue_work(hotscan_io_wq, &state->io_work);
}

/* Scan work: 遍历指定的 [start, end) 区间，收集 Access Flag */
static void hotscan_scan_work_func(struct work_struct *work)
{
	struct scan_work *sw = container_of(work, struct scan_work, work);
	struct scan_state *state = sw->state;

	mmap_read_lock(state->mm);
	walk_page_range(state->mm, sw->start, sw->end,
			&hotscan_walk_ops, sw);
	mmap_read_unlock(state->mm);
	hotscan_flush_batch(sw);

	/* 自己是最后一个？触发 I/O work */
	if (atomic_dec_and_test(&state->remaining))
		hotscan_finish_scan(state);

	kfree(sw);
}

/* I/O work: 将 buffer 写入文件，释放本轮扫描资源 */
static void hotscan_io_work_func(struct work_struct *work)
{
	struct scan_state *state = container_of(work, struct scan_state, io_work);

	pr_debug("hotscan: I/O work starting for round %lu, %d entries collected\n",
		state->round, atomic_read(&state->buf_index));

	hotscan_write_buffer(state);

	/* 释放 mm 引用 */
	if (state->mm) {
		mmput(state->mm);
		state->mm = NULL;
	}

	/* 释放 buffer */
	if (state->buffer) {
		kvfree(state->buffer);
		state->buffer = NULL;
	}

	/* 释放 scan_state 本身 */
	kfree(state);

	pr_debug("hotscan: I/O work done for round\n");
}

/* Dispatch work: 准备本轮扫描，分配资源，按 VMA 大小决定并发策略 */
static void hotscan_dispatch_work_func(struct work_struct *work)
{
	struct scan_state *state;
	struct task_struct *task;
	struct pid *pid_struct;
	struct mm_struct *mm;
	unsigned long total_pages = 0;
	unsigned long threshold_pages;
	size_t buf_size;
	int total_works = 0;
	int workers = num_workers;

	pr_debug("hotscan: dispatch work starting\n");

	/* ---- 1. 获取目标进程的 mm ---- */
	pid_struct = find_get_pid(target_pid);
	if (!pid_struct) {
		pr_err("hotscan: process with PID %d not found\n", target_pid);
		atomic_set(&scan_running, 0);
		return;
	}

	task = get_pid_task(pid_struct, PIDTYPE_PID);
	put_pid(pid_struct);
	if (!task) {
		pr_err("hotscan: process with PID %d has no task_struct\n", target_pid);
		atomic_set(&scan_running, 0);
		return;
	}

	mm = get_task_mm(task);
	put_task_struct(task);

	if (!mm) {
		pr_err("hotscan: process %d has no mm (probably a kernel thread or zombie)\n",
		       target_pid);
		atomic_set(&scan_running, 0);
		return;
	}

	/*
	 * ---- 2. 第一遍：统计匿名页总数 + 大 VMA 阈值 + work 总数 ----
	 *
	 * 同一把 mmap_read_lock 内完成统计和计数，保证阈值与计数一致。
	 * 大 VMA（pages >= threshold）：拆成 workers 份 work；
	 * 小 VMA：整个 VMA 作为一个 work。
	 */
	mmap_read_lock(mm);
	{
		struct vm_area_struct *vma;

		/* 2a. 统计匿名页总数 */
		VMA_ITERATOR(vmi, mm, 0);
		for_each_vma(vmi, vma) {
			if (vma->vm_file)
				continue;
			total_pages += (vma->vm_end - vma->vm_start) >> PAGE_SHIFT;
		}

		if (total_pages == 0) {
			mmap_read_unlock(mm);
			pr_warn("hotscan: process %d has no anonymous VMAs, skipping\n",
				target_pid);
			mmput(mm);
			atomic_set(&scan_running, 0);
			return;
		}

		/* 2b. 计算大 VMA 阈值 */
		threshold_pages = total_pages / workers;
		if (threshold_pages == 0)
			threshold_pages = 1;

		/* 2c. 预计算 work 总数 */
		{
			struct vm_area_struct *vma2;

			VMA_ITERATOR(vmi2, mm, 0);
			for_each_vma(vmi2, vma2) {
				if (vma2->vm_file)
					continue;
				if (hotscan_vma_is_large(vma2, threshold_pages) &&
				    workers > 1)
					total_works += min_t(unsigned long, workers,
						(vma2->vm_end - vma2->vm_start) >> PAGE_SHIFT);
				else
					total_works += 1;
			}
		}
	}
	pr_debug("hotscan: PID %d: %lu pages, %d workers, %d works\n",
		 target_pid, total_pages, workers, total_works);

	/* ---- 3. 分配 scan_state + buffer ---- */
	state = kzalloc(sizeof(*state), GFP_KERNEL);
	if (!state) {
		pr_err("hotscan: failed to allocate scan_state\n");
		mmap_read_unlock(mm);
		mmput(mm);
		atomic_set(&scan_running, 0);
		return;
	}

	buf_size = (size_t)total_pages * sizeof(u64);
	state->buffer = kvmalloc(buf_size, GFP_KERNEL);
	if (!state->buffer) {
		pr_err("hotscan: failed to allocate buffer of %zu bytes\n", buf_size);
		kfree(state);
		mmap_read_unlock(mm);
		mmput(mm);
		atomic_set(&scan_running, 0);
		return;
	}

	/*
	 * ---- 4. 初始化 state ----
	 *
	 * remaining 必须在排队任何 work 之前设置，否则已完成
	 * 的 work 可能看到错误的计数。
	 */
	state->mm = mm;
	atomic_set(&state->buf_index, 0);
	state->buf_entries = total_pages;
	state->round = (unsigned long)atomic64_fetch_inc(&scan_round_counter);
	state->start_time = ktime_get();
	INIT_WORK(&state->io_work, hotscan_io_work_func);
	atomic_set(&state->remaining, 1);

	/*
	 * ---- 5. 第二遍：遍历 VMA，拆分并排队 work ----
	 *
	 * 每个 work 只包含一个 [start, end) 区间，不使用链表。
	 * 每个成功排队的 work 增加 remaining，失败的区间不计入。
	 */
	{
		struct vm_area_struct *vma;

		VMA_ITERATOR(vmi, mm, 0);
		for_each_vma(vmi, vma) {
			if (vma->vm_file)
				continue;

			if (hotscan_vma_is_large(vma, threshold_pages) &&
			    workers > 1) {
				/*
				 * 大 VMA：按 workers 数均分 VA 范围，
				 * 每个子区间成为一个独立的 scan work。
				 * chunk_size 按 4K 对齐。
				 */
				unsigned long chunk_size, pos;
				int parts = min_t(unsigned long, workers,
					(vma->vm_end - vma->vm_start) >> PAGE_SHIFT);
				int i;

				chunk_size = ((vma->vm_end - vma->vm_start)
					      / parts) & PAGE_MASK;

				pos = vma->vm_start;
				for (i = 0; i < parts - 1; i++) {
					unsigned long next;

					next = (pos + chunk_size) & PAGE_MASK;
					hotscan_queue_one_work(pos, next, state);
					pos = next;
				}
				/* 最后一段覆盖剩余部分 */
				hotscan_queue_one_work(pos, vma->vm_end, state);
			} else {
				/* 小 VMA：整体作为一个 work */
				hotscan_queue_one_work(vma->vm_start,
						       vma->vm_end, state);
			}
		}
	}
	mmap_read_unlock(mm);

	pr_debug("hotscan: dispatch work done for round %lu (%d works)\n",
		 state->round, total_works);
	if (atomic_dec_and_test(&state->remaining))
		hotscan_finish_scan(state);
}

/* ========================================================================
 * 高精度定时器回调
 * ======================================================================== */

/*
 * hrtimer 回调（硬中断上下文）
 *
 * 职责：检查是否已有扫描在进行，若无则触发 dispatch work。
 * 使用 atomic_xchg 作为 test-and-set 避免竞争。
 * 使用 hrtimer_forward_now 确保精确周期，防止漂移。
 */
static enum hrtimer_restart hotscan_timer_callback(struct hrtimer *timer)
{
	/*
	 * 如果 scan_running 为 0（空闲），原子地设置为 1 并触发本次扫描
	 * 如果 scan_running 已经是 1（上一轮还未结束），跳过本次触发
	 */
	if (!atomic_xchg(&scan_running, 1)) {
		pr_debug("hotscan: timer tick, queuing dispatch work\n");
		queue_work(hotscan_wq, &dispatch_work);
	} else {
		pr_debug("hotscan: timer tick, previous scan still running, skip\n");
	}

	/* 使用 forward 而非 restart，保持精确周期不漂移 */
	hrtimer_forward_now(timer, ms_to_ktime(scan_period_ms));
	return HRTIMER_RESTART;
}

/* ========================================================================
 * 模块初始化 / 退出
 * ======================================================================== */

static int __init hotscan_init(void)
{
	struct workqueue_attrs *attrs;
	int ret;

	pr_info("hotscan: initializing (target_pid=%d, period=%dms, workers=%d)\n",
		target_pid, scan_period_ms, num_workers);

	if (target_pid <= 0) {
		pr_err("hotscan: invalid target_pid=%d, must be > 0\n", target_pid);
		return -EINVAL;
	}

	if (scan_period_ms <= 0) {
		pr_err("hotscan: invalid scan_period_ms=%d, must be > 0\n",
		       scan_period_ms);
		return -EINVAL;
	}

	if (num_workers <= 0 || num_workers > 64) {
		pr_err("hotscan: invalid num_workers=%d, must be 1..64\n",
		       num_workers);
		return -EINVAL;
	}

	/* 1. 创建输出目录 */
	hotscan_setup_output_dir();
	ret = hotscan_create_output_dir();
	if (ret) {
		pr_warn("hotscan: failed to create output dir %s (err=%d), "
		        "file writes will fail\n", output_dir, ret);
		/* 不阻止模块加载，允许用户手动创建目录 */
	}

	/* 2. 创建 workqueue（非绑定 + CPU 密集），设置 CPU 亲和性 0-19 */
	hotscan_wq = alloc_workqueue(HOTSCAN_WQ_NAME,
				     WQ_UNBOUND | WQ_CPU_INTENSIVE | WQ_FREEZABLE,
				     num_workers);
	if (!hotscan_wq) {
		pr_err("hotscan: failed to allocate workqueue\n");
		return -ENOMEM;
	}

	/* 2b. 创建 IO workqueue（有序，确保写入顺序） */
	hotscan_io_wq = alloc_ordered_workqueue("hotscan_io",
						WQ_UNBOUND | WQ_FREEZABLE);
	if (!hotscan_io_wq) {
		pr_err("hotscan: failed to allocate IO workqueue\n");
		destroy_workqueue(hotscan_wq);
		return -ENOMEM;
	}

	attrs = alloc_workqueue_attrs();
	if (!attrs) {
		pr_err("hotscan: failed to alloc workqueue attrs\n");
		destroy_workqueue(hotscan_io_wq);
		destroy_workqueue(hotscan_wq);
		return -ENOMEM;
	}

	/* 绑定到 CPU 0..min(19, nr_cpu_ids-1) */
	int max_cpu = min(HOTSCAN_MAX_CPUS, (int)nr_cpu_ids);
	cpumask_clear(attrs->cpumask);
	for (int i = 0; i < max_cpu; i++)
		cpumask_set_cpu(i, attrs->cpumask);

	ret = apply_workqueue_attrs(hotscan_wq, attrs);
	free_workqueue_attrs(attrs);
	if (ret) {
		pr_warn("hotscan: apply_workqueue_attrs failed (err=%d), "
		        "proceeding without CPU affinity\n", ret);
		/* 不致命，继续使用默认亲和性 */
	}

	/* 3. 初始化 dispatch work */
	INIT_WORK(&dispatch_work, hotscan_dispatch_work_func);

	/* 4. 初始化并启动高精度定时器 */
	hrtimer_init(&hotscan_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	hotscan_timer.function = hotscan_timer_callback;
	hrtimer_start(&hotscan_timer, ms_to_ktime(scan_period_ms),
		      HRTIMER_MODE_REL_PINNED);

	pr_info("hotscan: initialized successfully\n");
	pr_info("hotscan: scanning PID %d every %d ms, output to %s/\n",
		target_pid, scan_period_ms, output_dir);

	return 0;
}

static void __exit hotscan_exit(void)
{
	pr_info("hotscan: shutting down...\n");

	/* 1. 取消定时器 */
	hrtimer_cancel(&hotscan_timer);
	pr_info("hotscan: timer cancelled\n");

	/* 2. 等待所有扫描 work 完成（可能排队 IO work） */
	flush_workqueue(hotscan_wq);
	pr_info("hotscan: scan workqueue flushed\n");

	/* 3. 等待所有 IO work 完成 */
	flush_workqueue(hotscan_io_wq);
	pr_info("hotscan: IO workqueue flushed\n");

	/* 4. 销毁 workqueues */
	destroy_workqueue(hotscan_wq);
	hotscan_wq = NULL;
	destroy_workqueue(hotscan_io_wq);
	hotscan_io_wq = NULL;

	/* 5. 确保 scan_running 复位（防止竞态导致的永久置位） */
	atomic_set(&scan_running, 0);

	pr_info("hotscan: module unloaded\n");
}

module_init(hotscan_init);
module_exit(hotscan_exit);

/* ========================================================================
 * 模块元信息
 * ======================================================================== */

MODULE_LICENSE("GPL");
MODULE_AUTHOR("HotScan");
MODULE_DESCRIPTION("Kernel module for tracking process page access hotness");
MODULE_VERSION("1.0");
