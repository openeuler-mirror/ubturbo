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

KSYMTAB_DATA(smap_pgtype, "_gpl", "");
KSYMTAB_FUNC(is_smap_pg_huge, "", "");
KSYMTAB_FUNC(check_and_create_dir, "", "");
KSYMTAB_FUNC(check_filesize, "", "");
KSYMTAB_FUNC(rename_file, "", "");

SYMBOL_CRC(smap_pgtype, 0x7c168728, "_gpl");
SYMBOL_CRC(is_smap_pg_huge, 0x588f1397, "");
SYMBOL_CRC(check_and_create_dir, 0x9a2e36c0, "");
SYMBOL_CRC(check_filesize, 0xe2d6afae, "");
SYMBOL_CRC(rename_file, 0x0c536ab5, "");

 const struct modversion_info ____versions[]
__used __section("__versions") = {
	{ 0x3446f0d2, "get_node_page_cnt_iomem" },
	{ 0xc528a49a, "queued_write_lock_slowpath" },
	{ 0x216b6ae9, "filp_open" },
	{ 0x39ef0c6a, "set_linear_mapping_nc" },
	{ 0xe3ec2f2b, "alloc_chrdev_region" },
	{ 0x88db9f48, "__check_object_size" },
	{ 0x551bd071, "__rb_erase_color" },
	{ 0x7696f8c7, "__list_add_valid_or_report" },
	{ 0x49cd25ed, "alloc_workqueue" },
	{ 0xc60d0620, "__num_online_cpus" },
	{ 0x656e4a6e, "snprintf" },
	{ 0xa6257a2f, "complete" },
	{ 0x8132d93, "find_get_task_by_vpid" },
	{ 0x8810754a, "_find_first_bit" },
	{ 0xca9360b5, "rb_next" },
	{ 0x69a52ad3, "iterate_dir" },
	{ 0x608741b5, "__init_swait_queue_head" },
	{ 0x9440a840, "class_destroy" },
	{ 0x96848186, "scnprintf" },
	{ 0x2c64f08a, "__mmap_lock_do_trace_acquire_returned" },
	{ 0x55873d1d, "__put_task_struct" },
	{ 0xcf2a6966, "up" },
	{ 0x6a89dd25, "walk_pid_pagemap" },
	{ 0x37a0cba, "kfree" },
	{ 0x5e8c379f, "get_ham_pages_freqs" },
	{ 0x73854ad6, "calc_time_us" },
	{ 0xfa2c045e, "__tracepoint_mmap_lock_acquire_returned" },
	{ 0x327754c1, "__tracepoint_mmap_lock_released" },
	{ 0x32047ad5, "__per_cpu_offset" },
	{ 0x916758a3, "node_states" },
	{ 0x4f46cd36, "path_put" },
	{ 0xc3ff38c2, "down_read_trylock" },
	{ 0xd6ad165c, "cna_queued_spin_lock_slowpath" },
	{ 0xcbd4898c, "fortify_panic" },
	{ 0x8726ecd8, "vm_event_states" },
	{ 0x6917d878, "wake_up_process" },
	{ 0xab0b82f5, "get_hugetlb_folio_nodemask" },
	{ 0x91c64b67, "numa_is_remote_node" },
	{ 0x122c3a7e, "_printk" },
	{ 0x1d24c881, "___ratelimit" },
	{ 0x1000e51, "schedule" },
	{ 0xf0fdf6cb, "__stack_chk_fail" },
	{ 0x296695f, "refcount_warn_saturate" },
	{ 0xcc935375, "walk_iomem_res_desc" },
	{ 0xb8a6122e, "queue_delayed_work_on" },
	{ 0x6cbbfc54, "__arch_copy_to_user" },
	{ 0xa916b694, "strnlen" },
	{ 0x48394d1b, "isolate_and_migrate_folios" },
	{ 0x618911fc, "numa_node" },
	{ 0x40a9b349, "vzalloc" },
	{ 0x2128a81b, "system_cpucaps" },
	{ 0x3ca2ab1f, "find_vma" },
	{ 0x92b99a33, "acpi_put_table" },
	{ 0x800473f, "__cond_resched" },
	{ 0x14f1e46e, "cdev_add" },
	{ 0xe8e2b55b, "device_create_file" },
	{ 0x46c47fb6, "__node_distance" },
	{ 0xbcb36fe4, "hugetlb_optimize_vmemmap_key" },
	{ 0x2d35652, "debugfs_lookup" },
	{ 0x2cf5f947, "putback_hugetlb_folio" },
	{ 0x282698cb, "convert_pos_to_paddr_sorted" },
	{ 0x57bc19d2, "down_write" },
	{ 0xce807a25, "up_write" },
	{ 0xb7c0f443, "sort" },
	{ 0x1aabc170, "unlock_page" },
	{ 0x8c8569cb, "kstrtoint" },
	{ 0xa7eedcc4, "call_usermodehelper" },
	{ 0x9e7dc74, "device_create" },
	{ 0x1a45cb6c, "acpi_disabled" },
	{ 0x6626afca, "down" },
	{ 0x1a60a567, "class_create" },
	{ 0x8c03d20c, "destroy_workqueue" },
	{ 0x3199fbeb, "mem_section" },
	{ 0x4dfa8d4b, "mutex_lock" },
	{ 0x120b336a, "__rb_insert_augmented" },
	{ 0x8d05763a, "debugfs_remove" },
	{ 0x5a921311, "strncmp" },
	{ 0x9688de8b, "memstart_addr" },
	{ 0xb06c69d1, "g_pagesize_huge" },
	{ 0x151196ee, "smap_is_remote_addr_valid" },
	{ 0x950eb34e, "__list_del_entry_valid_or_report" },
	{ 0xe078abb1, "get_task_mm" },
	{ 0x53a1e8d9, "_find_next_bit" },
	{ 0xbcab6ee6, "sscanf" },
	{ 0x4b0a3f52, "gic_nonsecure_priorities" },
	{ 0x1e2e1b63, "default_llseek" },
	{ 0xd728a45d, "contpte_ptep_get" },
	{ 0xdcb764ad, "memset" },
	{ 0x793847e6, "kern_path" },
	{ 0x5c3c7387, "kstrtoull" },
	{ 0x7c4fd0a2, "hisi_soc_cache_maintain" },
	{ 0x21bbfe7b, "kernel_read" },
	{ 0x25974000, "wait_for_completion" },
	{ 0xfb73b6cd, "pfn_to_online_page" },
	{ 0x9166fc03, "__flush_workqueue" },
	{ 0x17de3d5, "nr_cpu_ids" },
	{ 0x668b19a1, "down_read" },
	{ 0x15ba50a6, "jiffies" },
	{ 0x49042fdf, "kthread_create_on_node" },
	{ 0xa3a86f5b, "__mmap_lock_do_trace_start_locking" },
	{ 0x3c3ff9fd, "sprintf" },
	{ 0xe48afdb7, "__folio_alloc" },
	{ 0x16cdc340, "acpi_get_table" },
	{ 0xa21ae614, "debugfs_create_file" },
	{ 0x999e8297, "vfree" },
	{ 0x6091b333, "unregister_chrdev_region" },
	{ 0x4d2d1bde, "mmput" },
	{ 0x3213f038, "mutex_unlock" },
	{ 0x6b1a4c91, "cancel_delayed_work_sync" },
	{ 0xd058781f, "init_timer_key" },
	{ 0x5c45acf1, "filp_close" },
	{ 0xcd9b03f7, "__tracepoint_mmap_lock_start_locking" },
	{ 0xede5e7f, "__folio_put" },
	{ 0x49e138ca, "device_destroy" },
	{ 0x2cf56265, "__dynamic_pr_debug" },
	{ 0xb43f9365, "ktime_get" },
	{ 0xbe8697bb, "__mmap_lock_do_trace_released" },
	{ 0xdd8f4aa3, "dput" },
	{ 0x5a88af97, "delayed_work_timer_fn" },
	{ 0x12a4e128, "__arch_copy_from_user" },
	{ 0x472cf3b, "register_kprobe" },
	{ 0xbebe692d, "folio_mapping" },
	{ 0x1cd8438b, "pxm_to_node" },
	{ 0xaebe0d88, "find_vpid" },
	{ 0x810a4a55, "node_data" },
	{ 0xa65c6def, "alt_cb_patch_nops" },
	{ 0xeb78b1ed, "unregister_kprobe" },
	{ 0x9f1a778, "kmalloc_trace" },
	{ 0xd4113dfc, "walk_page_range" },
	{ 0x5a7b3bb7, "set_linear_mapping_invalid" },
	{ 0x656a8149, "debugfs_create_dir" },
	{ 0x5d847854, "shake_page" },
	{ 0x619cb7dd, "simple_read_from_buffer" },
	{ 0x53b954a2, "up_read" },
	{ 0xf9a482f9, "msleep" },
	{ 0x71aa536a, "cdev_init" },
	{ 0xeb233a45, "__kmalloc" },
	{ 0x173ff7bd, "kmalloc_caches" },
	{ 0xa6e9cfac, "cdev_del" },
	{ 0xf1068469, "device_remove_file" },
	{ 0xd464115a, "kernel_write" },
	{ 0x73837fe1, "module_layout" },
};

MODULE_INFO(depends, "smap_access_tracking");


MODULE_INFO(srcversion, "8E02685D8CC0A7BB7299898");
