/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Description: SMAP ACPI helper header
 */

#ifndef _TIERING_ACPI_HELPER_H
#define _TIERING_ACPI_HELPER_H

#include <linux/acpi.h>

#define ACPI_SIG_LENGTH 4

enum acpi_sub_type {
	SUBTABLE_COMMON,
	SUBTABLE_HMAT,
};

struct acpi_subtable_entry {
	union acpi_subtable_headers *hdr;
	enum acpi_sub_type type;
};

int acpi_parse_entries_array(char *id, unsigned long table_size,
			     struct acpi_table_header *table_header,
			     struct acpi_subtable_proc *proc, int proc_num,
			     unsigned int max_entries);

#endif /* _TIERING_ACPI_HELPER_H */
