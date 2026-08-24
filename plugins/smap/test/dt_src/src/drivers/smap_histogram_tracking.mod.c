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

KSYMTAB_FUNC(ub_hist_query_ba_count, "", "");
KSYMTAB_FUNC(ub_hist_query_ba_tags, "", "");
KSYMTAB_FUNC(ub_hist_query_ba_info, "", "");
KSYMTAB_FUNC(ub_hist_set_state, "", "");
KSYMTAB_FUNC(ub_hist_get_state, "", "");
KSYMTAB_FUNC(ub_hist_get_statistic_result, "", "");
KSYMTAB_FUNC(ub_hist_mar_perf_en, "", "");
KSYMTAB_FUNC(ub_hist_mar_perf_check, "", "");
KSYMTAB_FUNC(ub_hist_get_access_count, "", "");
KSYMTAB_FUNC(get_path_idx_by_addr, "", "");
KSYMTAB_FUNC(ub_hist_get_hw_type, "", "");
KSYMTAB_FUNC(ub_hist_init, "", "");
KSYMTAB_FUNC(ub_hist_exit, "", "");

SYMBOL_CRC(ub_hist_query_ba_count, 0xa3b8aaf9, "");
SYMBOL_CRC(ub_hist_query_ba_tags, 0x44e91100, "");
SYMBOL_CRC(ub_hist_query_ba_info, 0x42ef1b24, "");
SYMBOL_CRC(ub_hist_set_state, 0x8c9a2030, "");
SYMBOL_CRC(ub_hist_get_state, 0x73dcff17, "");
SYMBOL_CRC(ub_hist_get_statistic_result, 0x3ca3c09a, "");
SYMBOL_CRC(ub_hist_mar_perf_en, 0xd02fe946, "");
SYMBOL_CRC(ub_hist_mar_perf_check, 0x5e31fde4, "");
SYMBOL_CRC(ub_hist_get_access_count, 0x86842e5f, "");
SYMBOL_CRC(get_path_idx_by_addr, 0x2b9c5ffa, "");
SYMBOL_CRC(ub_hist_get_hw_type, 0x7dd406d7, "");
SYMBOL_CRC(ub_hist_init, 0x706d61ec, "");
SYMBOL_CRC(ub_hist_exit, 0x896e304e, "");

 const struct modversion_info ____versions[]
__used __section("__versions") = {
	{ 0x3ca6ed90, "is_acpi_device_node" },
	{ 0x7696f8c7, "__list_add_valid_or_report" },
	{ 0x3c19b766, "devm_kmalloc" },
	{ 0xdaca65ae, "platform_driver_unregister" },
	{ 0xedc03953, "iounmap" },
	{ 0xaf56600a, "arm64_use_ng_mappings" },
	{ 0xd6ad165c, "cna_queued_spin_lock_slowpath" },
	{ 0x122c3a7e, "_printk" },
	{ 0xf0fdf6cb, "__stack_chk_fail" },
	{ 0x11d14b77, "_dev_err" },
	{ 0xa439bd29, "platform_get_resource" },
	{ 0x950eb34e, "__list_del_entry_valid_or_report" },
	{ 0xa009ddc1, "device_property_read_u64_array" },
	{ 0xeed92013, "__platform_driver_register" },
	{ 0x2cf56265, "__dynamic_pr_debug" },
	{ 0xa65c6def, "alt_cb_patch_nops" },
	{ 0x40863ba1, "ioremap_prot" },
	{ 0x73837fe1, "module_layout" },
};

MODULE_INFO(depends, "");

MODULE_ALIAS("acpi*:HISI0531:*");

MODULE_INFO(srcversion, "C6B036367810BE76F360633");
