/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_POLL_H
#define _LINUX_POLL_H

struct file;

struct poll_table_struct {
	int dummy;
};

typedef struct poll_table_struct poll_table;

#define poll_wait(filp, wait_address, p) ((void)(filp), (void)(wait_address), (void)(p))

#define POLLIN 0x0001
#define POLLRDNORM 0x0040
#define POLLOUT 0x0004
#define POLLWRNORM 0x0100
#define POLLERR 0x0008
#define POLLHUP 0x0010

#endif /* _LINUX_POLL_H */
