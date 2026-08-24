#include <linux/module.h>
#define INCLUDE_VERMAGIC
#include <linux/build-salt.h>
#include <linux/elfnote-lto.h>
#include <linux/export-internal.h>
#include <linux/vermagic.h>
#include <linux/compiler.h>

#ifdef CONFIG_UNWINDER_ORC
#include <asm/orc_header.h>
ORC_HEADER;
#endif

BUILD_SALT;
BUILD_LTO_INFO;

MODULE_INFO(vermagic, VERMAGIC_STRING);
MODULE_INFO(name, KBUILD_MODNAME);

__visible struct module __this_module
__section(".gnu.linkonce.this_module") = {
	.name = KBUILD_MODNAME,
	.init = init_module,
#ifdef CONFIG_MODULE_UNLOAD
	.exit = cleanup_module,
#endif
	.arch = MODULE_ARCH_INIT,
};

#ifdef CONFIG_RETPOLINE
MODULE_INFO(retpoline, "Y");
#endif

KSYMTAB_DATA(remote_ram_list, "", "");
KSYMTAB_DATA(rem_ram_list_lock, "", "");
KSYMTAB_FUNC(get_node_page_cnt_iomem, "", "");
KSYMTAB_FUNC(smap_is_remote_addr_valid, "", "");
KSYMTAB_FUNC(get_ham_pages_freqs, "", "");
KSYMTAB_FUNC(convert_pos_to_paddr_sorted, "", "");
KSYMTAB_FUNC(walk_pid_pagemap, "", "");
KSYMTAB_DATA(g_pagesize_huge, "", "");
KSYMTAB_FUNC(calc_time_us, "", "");

SYMBOL_CRC(remote_ram_list, 0x76a70797, "");
SYMBOL_CRC(rem_ram_list_lock, 0x3bded464, "");
SYMBOL_CRC(get_node_page_cnt_iomem, 0x3446f0d2, "");
SYMBOL_CRC(smap_is_remote_addr_valid, 0x151196ee, "");
SYMBOL_CRC(get_ham_pages_freqs, 0x5e8c379f, "");
SYMBOL_CRC(convert_pos_to_paddr_sorted, 0x282698cb, "");
SYMBOL_CRC(walk_pid_pagemap, 0x6a89dd25, "");
SYMBOL_CRC(g_pagesize_huge, 0xb06c69d1, "");
SYMBOL_CRC(calc_time_us, 0x73854ad6, "");

 const struct modversion_info ____versions[]
__used __section("__versions") = {
	{ 0xc528a49a, "queued_write_lock_slowpath" },
	{ 0x216b6ae9, "filp_open" },
	{ 0xddf6ad7a, "completion_done" },
	{ 0xe3ec2f2b, "alloc_chrdev_region" },
	{ 0x88db9f48, "__check_object_size" },
	{ 0x7b4da6ff, "__init_rwsem" },
	{ 0x7696f8c7, "__list_add_valid_or_report" },
	{ 0x49cd25ed, "alloc_workqueue" },
	{ 0xcf878a87, "param_ops_uint" },
	{ 0x7dd406d7, "ub_hist_get_hw_type" },
	{ 0x73dcff17, "ub_hist_get_state" },
	{ 0x7f02188f, "__msecs_to_jiffies" },
	{ 0x67c0d5a6, "dev_set_name" },
	{ 0x8de5481f, "proc_create" },
	{ 0x656e4a6e, "snprintf" },
	{ 0xa6257a2f, "complete" },
	{ 0x205c0dad, "queue_work_on" },
	{ 0x22a9d9ec, "device_unregister" },
	{ 0x21ea5251, "__bitmap_weight" },
	{ 0x608741b5, "__init_swait_queue_head" },
	{ 0x2a436e2, "get_pid_task" },
	{ 0x9440a840, "class_destroy" },
	{ 0x96848186, "scnprintf" },
	{ 0x896e304e, "ub_hist_exit" },
	{ 0x2c64f08a, "__mmap_lock_do_trace_acquire_returned" },
	{ 0x55873d1d, "__put_task_struct" },
	{ 0x1d529f1f, "device_initialize" },
	{ 0xca8fc078, "tracking_dev_add" },
	{ 0x706d61ec, "ub_hist_init" },
	{ 0x4829a47e, "memcpy" },
	{ 0x3b6c41ea, "kstrtouint" },
	{ 0x37a0cba, "kfree" },
	{ 0x3ca3c09a, "ub_hist_get_statistic_result" },
	{ 0xb079d592, "find_get_pid" },
	{ 0x142ea082, "proc_create_data" },
	{ 0xc3055d20, "usleep_range_state" },
	{ 0xb3f7646e, "kthread_should_stop" },
	{ 0xfa2c045e, "__tracepoint_mmap_lock_acquire_returned" },
	{ 0x327754c1, "__tracepoint_mmap_lock_released" },
	{ 0x916758a3, "node_states" },
	{ 0x635bca3d, "param_get_ulong" },
	{ 0x44e91100, "ub_hist_query_ba_tags" },
	{ 0x4f46cd36, "path_put" },
	{ 0xd6ad165c, "cna_queued_spin_lock_slowpath" },
	{ 0xcbd4898c, "fortify_panic" },
	{ 0x6917d878, "wake_up_process" },
	{ 0x122c3a7e, "_printk" },
	{ 0x5e31fde4, "ub_hist_mar_perf_check" },
	{ 0xd082b11a, "param_get_uint" },
	{ 0x617c452b, "queued_read_lock_slowpath" },
	{ 0xf0fdf6cb, "__stack_chk_fail" },
	{ 0x296695f, "refcount_warn_saturate" },
	{ 0xb8a6122e, "queue_delayed_work_on" },
	{ 0xcc935375, "walk_iomem_res_desc" },
	{ 0x6cbbfc54, "__arch_copy_to_user" },
	{ 0x46c6b16f, "put_device" },
	{ 0x4c345b5b, "make_kuid" },
	{ 0xa916b694, "strnlen" },
	{ 0x87b30034, "mas_find" },
	{ 0x40a9b349, "vzalloc" },
	{ 0x2128a81b, "system_cpucaps" },
	{ 0x8c9a2030, "ub_hist_set_state" },
	{ 0x3ca2ab1f, "find_vma" },
	{ 0x599fb41c, "kvmalloc_node" },
	{ 0x92b99a33, "acpi_put_table" },
	{ 0x800473f, "__cond_resched" },
	{ 0x14f1e46e, "cdev_add" },
	{ 0xbcb36fe4, "hugetlb_optimize_vmemmap_key" },
	{ 0x9b98405a, "tracking_dev_remove" },
	{ 0x95b90f76, "fput" },
	{ 0x57bc19d2, "down_write" },
	{ 0xce807a25, "up_write" },
	{ 0xb7c0f443, "sort" },
	{ 0xb59bd0fd, "kvm_flush_remote_tlbs" },
	{ 0x44ec1f1a, "device_add" },
	{ 0x8c8569cb, "kstrtoint" },
	{ 0x6814bc38, "__srcu_read_lock" },
	{ 0xbb7e9690, "gfn_to_hva_memslot" },
	{ 0x9e7dc74, "device_create" },
	{ 0x1a45cb6c, "acpi_disabled" },
	{ 0x1a60a567, "class_create" },
	{ 0x8c03d20c, "destroy_workqueue" },
	{ 0x3199fbeb, "mem_section" },
	{ 0x5a921311, "strncmp" },
	{ 0x9688de8b, "memstart_addr" },
	{ 0x9166fada, "strncpy" },
	{ 0x950eb34e, "__list_del_entry_valid_or_report" },
	{ 0xe078abb1, "get_task_mm" },
	{ 0x53a1e8d9, "_find_next_bit" },
	{ 0x5ffd9032, "__cpu_online_mask" },
	{ 0x9ed12e20, "kmalloc_large" },
	{ 0x192296c9, "kthread_stop" },
	{ 0x61748fb2, "device_del" },
	{ 0xa88d8963, "proc_mkdir" },
	{ 0x6053f4b4, "__cpu_possible_mask" },
	{ 0xd728a45d, "contpte_ptep_get" },
	{ 0xdcb764ad, "memset" },
	{ 0x793847e6, "kern_path" },
	{ 0x5c3c7387, "kstrtoull" },
	{ 0x2b9c5ffa, "get_path_idx_by_addr" },
	{ 0x25974000, "wait_for_completion" },
	{ 0xfb73b6cd, "pfn_to_online_page" },
	{ 0x9166fc03, "__flush_workqueue" },
	{ 0x17de3d5, "nr_cpu_ids" },
	{ 0x84365b9d, "make_kgid" },
	{ 0xe009bacb, "proc_remove" },
	{ 0x62f7e207, "down_read_killable" },
	{ 0xe2d5255a, "strcmp" },
	{ 0x668b19a1, "down_read" },
	{ 0x49042fdf, "kthread_create_on_node" },
	{ 0x42ef1b24, "ub_hist_query_ba_info" },
	{ 0xa3a86f5b, "__mmap_lock_do_trace_start_locking" },
	{ 0x16cdc340, "acpi_get_table" },
	{ 0x7a2af7b4, "cpu_number" },
	{ 0x999e8297, "vfree" },
	{ 0x6091b333, "unregister_chrdev_region" },
	{ 0x4d2d1bde, "mmput" },
	{ 0x6b1a4c91, "cancel_delayed_work_sync" },
	{ 0xd196e860, "proc_set_user" },
	{ 0xd058781f, "init_timer_key" },
	{ 0x93984c1b, "init_user_ns" },
	{ 0x5c45acf1, "filp_close" },
	{ 0xcd9b03f7, "__tracepoint_mmap_lock_start_locking" },
	{ 0x49e138ca, "device_destroy" },
	{ 0x2cf56265, "__dynamic_pr_debug" },
	{ 0xd24e181e, "remove_proc_entry" },
	{ 0xb43f9365, "ktime_get" },
	{ 0xbe8697bb, "__mmap_lock_do_trace_released" },
	{ 0x5a88af97, "delayed_work_timer_fn" },
	{ 0x12a4e128, "__arch_copy_from_user" },
	{ 0xfa52792c, "__srcu_read_unlock" },
	{ 0x1cd8438b, "pxm_to_node" },
	{ 0xaebe0d88, "find_vpid" },
	{ 0xd4c14632, "system_unbound_wq" },
	{ 0xa65c6def, "alt_cb_patch_nops" },
	{ 0xd02fe946, "ub_hist_mar_perf_en" },
	{ 0x9f1a778, "kmalloc_trace" },
	{ 0x98cf60b3, "strlen" },
	{ 0xd4113dfc, "walk_page_range" },
	{ 0x7aa1756e, "kvfree" },
	{ 0xd6ee688f, "vmalloc" },
	{ 0x53b954a2, "up_read" },
	{ 0x86842e5f, "ub_hist_get_access_count" },
	{ 0x8937ca8f, "put_pid" },
	{ 0xf9a482f9, "msleep" },
	{ 0x71aa536a, "cdev_init" },
	{ 0xeb233a45, "__kmalloc" },
	{ 0xa3b8aaf9, "ub_hist_query_ba_count" },
	{ 0x173ff7bd, "kmalloc_caches" },
	{ 0xa6e9cfac, "cdev_del" },
	{ 0xd464115a, "kernel_write" },
	{ 0x73837fe1, "module_layout" },
};

MODULE_INFO(depends, "smap_histogram_tracking,smap_tracking_core");


MODULE_INFO(srcversion, "1D5BBCD7AB74AB1387409C3");
