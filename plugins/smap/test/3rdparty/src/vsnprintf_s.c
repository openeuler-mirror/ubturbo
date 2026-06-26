/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2024-2024. All rights reserved.
 * Description: vsnprintf_s stub function
 * Create: 2024-09-18
 */

#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include "securec.h"

int vsnprintf_s(char *strDest, size_t destMax, size_t count, const char *format, va_list argList)
{
    int ret = vsnprintf(strDest, destMax, format, argList);
    return ret > 0 ? ret : -1;
}
