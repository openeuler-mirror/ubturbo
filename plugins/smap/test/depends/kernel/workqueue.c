/* SPDX-License-Identifier: GPL-2.0-only */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <linux/workqueue.h>

struct workqueue_struct {
};

struct workqueue_struct *alloc_workqueue(const char *fmt,
					 unsigned int flags,
					 int max_active, ...)
{
    return NULL;
}

static bool __cancel_work_timer(struct work_struct *work, bool is_dwork)
{
	return true;
}

bool cancel_delayed_work_sync(struct delayed_work *dwork)
{
	return __cancel_work_timer(&dwork->work, true);
}

void destroy_workqueue(struct workqueue_struct *wq)
{}

bool queue_delayed_work(struct workqueue_struct *wq,
				      struct delayed_work *dwork,
				      unsigned long delay)
{
	return true;
}

struct workqueue_attrs *alloc_workqueue_attrs(void)
{
    struct workqueue_attrs *attrs = (struct workqueue_attrs *)calloc(1, sizeof(struct workqueue_attrs));
    if (attrs)
        cpumask_clear(attrs->cpumask);
    return attrs;
}

void free_workqueue_attrs(struct workqueue_attrs *attrs)
{
    free(attrs);
}

int apply_workqueue_attrs(struct workqueue_struct *wq, const struct workqueue_attrs *attrs)
{
    return 0;
}
