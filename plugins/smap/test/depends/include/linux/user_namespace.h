#ifndef _LINUX_USER_NAMESPACE_H
#define _LINUX_USER_NAMESPACE_H

#include <linux/uidgid.h>

#ifdef __cplusplus
extern "C" {
#endif

struct user_namespace {
	kuid_t owner;
	kgid_t group;
};

kuid_t make_kuid(struct user_namespace *ns, uid_t uid);
kgid_t make_kgid(struct user_namespace *ns, gid_t gid);

#ifdef __cplusplus
}
#endif

#endif
