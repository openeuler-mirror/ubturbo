/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * Description: sprintf_s stub function
 * Create: 2025-06-24
 */

#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include "securec.h"

int sprintf_s(char *strDest, size_t destMax, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    int ret = vsnprintf(strDest, destMax, format, args);
    va_end(args);
    return ret > 0 ? ret : -1;
}
