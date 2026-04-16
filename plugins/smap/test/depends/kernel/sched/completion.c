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