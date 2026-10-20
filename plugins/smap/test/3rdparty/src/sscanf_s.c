/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2024-2024. All rights reserved.
 * Description: sscanf_s stub function
 * Create: 2024-09-27
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include "securec.h"

/*
 * 委托给标准 vsscanf 实现真实解析。源码中 sscanf_s 仅使用数值转换
 * （%lu/%u/%llu/%lx），未使用 securec 特有的 %s/%c + size 参数语义，
 * 因此标准 vsscanf 行为与 securec 一致。需要特殊返回值的用例通过
 * MOCKER(sscanf_s) 覆盖，不受此实现影响。
 */
int sscanf_s(const char *buffer, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    int ret = vsscanf(buffer, format, args);
    va_end(args);
    return ret;
}
