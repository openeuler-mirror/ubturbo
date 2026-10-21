/* SPDX-License-Identifier: GPL-2.0-only */
#include <linux/completion.h>

void complete(struct completion *x)
{
}

void wait_for_completion(struct completion *x)
{
}

bool completion_done(struct completion *x)
{
    return true;
}

unsigned long wait_for_completion_timeout(struct completion *x,
					  unsigned long timeout)
{
    /* 测试环境无迁回 worker 线程，同步等待视为立即完成 */
    return 1UL;
}