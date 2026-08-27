// SPDX-License-Identifier: GPL-2.0-only
/*
 * Description: jiffies header stub
 */

#ifndef _LINUX_JIFFIES_H
#define _LINUX_JIFFIES_H
extern unsigned long jiffies;
unsigned long msecs_to_jiffies(const unsigned int m);
unsigned int jiffies_to_usecs(const unsigned long j);

#endif