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

KSYMTAB_FUNC(set_reinit_pending_flag, "", "");
KSYMTAB_FUNC(tracking_dev_add, "_gpl", "");
KSYMTAB_FUNC(tracking_dev_remove, "_gpl", "");

SYMBOL_CRC(set_reinit_pending_flag, 0xedbdc533, "");
SYMBOL_CRC(tracking_dev_add, 0xca8fc078, "_gpl");
SYMBOL_CRC(tracking_dev_remove, 0x9b98405a, "_gpl");

 const struct modversion_info ____versions[]
__used __section("__versions") = {
	{ 0xe7a02573, "ida_alloc_range" },
	{ 0xe3ec2f2b, "alloc_chrdev_region" },
	{ 0x7696f8c7, "__list_add_valid_or_report" },
	{ 0xa7d5f92e, "ida_destroy" },
	{ 0x67c0d5a6, "dev_set_name" },
	{ 0x69c9dc25, "cdev_device_del" },
	{ 0x22a9d9ec, "device_unregister" },
	{ 0x9440a840, "class_destroy" },
	{ 0x1d529f1f, "device_initialize" },
	{ 0x37a0cba, "kfree" },
	{ 0xcbd4898c, "fortify_panic" },
	{ 0x122c3a7e, "_printk" },
	{ 0xadf604b7, "cdev_device_add" },
	{ 0xf0fdf6cb, "__stack_chk_fail" },
	{ 0x6cbbfc54, "__arch_copy_to_user" },
	{ 0x46c6b16f, "put_device" },
	{ 0x2a975b0d, "sysfs_create_link" },
	{ 0x94b24091, "bus_unregister" },
	{ 0x44ec1f1a, "device_add" },
	{ 0xf7bce20a, "sysfs_remove_link" },
	{ 0x1a60a567, "class_create" },
	{ 0x4dfa8d4b, "mutex_lock" },
	{ 0xffb7c514, "ida_free" },
	{ 0x87348ca0, "driver_unregister" },
	{ 0x950eb34e, "__list_del_entry_valid_or_report" },
	{ 0x61748fb2, "device_del" },
	{ 0xdcb764ad, "memset" },
	{ 0x6091b333, "unregister_chrdev_region" },
	{ 0x3213f038, "mutex_unlock" },
	{ 0x2cf56265, "__dynamic_pr_debug" },
	{ 0x12a4e128, "__arch_copy_from_user" },
	{ 0xa65c6def, "alt_cb_patch_nops" },
	{ 0x9f1a778, "kmalloc_trace" },
	{ 0x6e1f3b2e, "driver_register" },
	{ 0x71aa536a, "cdev_init" },
	{ 0x173ff7bd, "kmalloc_caches" },
	{ 0x9b3cb79d, "bus_register" },
	{ 0x73837fe1, "module_layout" },
};

MODULE_INFO(depends, "");


MODULE_INFO(srcversion, "7F9E5C49648040FE1EAA88D");
