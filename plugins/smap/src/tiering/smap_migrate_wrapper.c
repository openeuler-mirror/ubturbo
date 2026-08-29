// SPDX-License-Identifier: GPL-2.0-only
/*
 * Description: SMAP Tiering Memory Solution: wrapper
 */

#include <linux/nodemask.h>
#include <linux/kprobes.h>

#include "smap_migrate_wrapper.h"

DEFINE_STATIC_KEY_FALSE(cpusets_pre_enable_key);
DEFINE_STATIC_KEY_FALSE(cpusets_enabled_key);

unsigned long (*fp_kallsyms_lookup_name)(const char *) = NULL;
int (*fp_migrate_pages)(struct list_head *from, new_folio_t get_new_folio,
			free_folio_t put_new_folio, unsigned long priv,
			enum migrate_mode mode, int reason,
			unsigned int *ret_succeeded) = NULL;
void (*fp_putback_movable_pages)(struct list_head *l) = NULL;
bool (*fp_isolate_folio_to_list)(struct folio *folio,
				 struct list_head *list) = NULL;

void lookup_kallsyms_lookup_name(void)
{
	struct kprobe kp;
	memset(&kp, 0, sizeof(struct kprobe));
	kp.symbol_name = "kallsyms_lookup_name";
	if (register_kprobe(&kp) < 0) {
		return;
	}
	fp_kallsyms_lookup_name = (unsigned long (*)(const char *))kp.addr;
	unregister_kprobe(&kp);
}

int smap_process_symbols(void)
{
	lookup_kallsyms_lookup_name();
	if (!fp_kallsyms_lookup_name)
		return -EFAULT;

	// clang-format off
	fp_migrate_pages =
		(int (*)(struct list_head *from, new_folio_t get_new_folio,
			 free_folio_t put_new_folio, unsigned long priv,
			 enum migrate_mode mode, int reason,
			 unsigned int *ret_succeeded))
			fp_kallsyms_lookup_name("migrate_pages");
	fp_putback_movable_pages =
		(void (*)(struct list_head *l))
			fp_kallsyms_lookup_name("putback_movable_pages");
	fp_isolate_folio_to_list =
		(bool (*)(struct folio *folio, struct list_head *list))
			fp_kallsyms_lookup_name("isolate_folio_to_list");
	// clang-format on
	if (!(fp_migrate_pages && fp_putback_movable_pages &&
	      fp_isolate_folio_to_list))
		return -EFAULT;

	return 0;
}

/* get_pfnblock_flags_mask and its helpers now imported from drivers */
