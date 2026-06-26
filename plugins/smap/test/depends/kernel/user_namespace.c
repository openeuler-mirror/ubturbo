#include <linux/user_namespace.h>

kuid_t make_kuid(struct user_namespace *ns, uid_t uid)
{
	kuid_t kuid = { 0 };
	return kuid;
}

kgid_t make_kgid(struct user_namespace *ns, gid_t gid)
{
	kgid_t kgid = { 0 };
	return kgid;
}
