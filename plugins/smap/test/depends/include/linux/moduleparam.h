/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_MODULE_PARAMS_H
#define _LINUX_MODULE_PARAMS_H

#define module_param(name, type, perm)
#define module_param_array(name, type, nump, perm)
#define MODULE_PARM_DESC(_parm, desc)
#define module_param_cb(name, ops, arg, perm)

struct kernel_param;

struct kernel_param_ops {
    int (*set)(const char *val, const struct kernel_param *kp);
    int (*get)(char *buffer, const struct kernel_param *kp);
};

struct kernel_param {
    const struct kernel_param_ops *ops;
};

extern int param_set_uint(const char *val, const struct kernel_param *kp);
extern int param_get_uint(char *buffer, const struct kernel_param *kp);
extern int param_set_ulong(const char *val, const struct kernel_param *kp);
extern int param_get_ulong(char *buffer, const struct kernel_param *kp);

#endif /* _LINUX_MODULE_PARAMS_H */
