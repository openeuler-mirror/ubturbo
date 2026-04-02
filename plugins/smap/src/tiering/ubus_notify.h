#ifndef _UBUS_NOTIFY_H
#define _UBUS_NOTIFY_H

int hisi_ubus_notify_error(struct notifier_block *nb, unsigned long event, void *data);

int is_link_down(void);

void remove_link_down(void);

#endif