/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2024-2024. All rights reserved.
 * Description: snprintf_s stub function
 * Create: 2024-09-18
 */

#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include "securec.h"

int snprintf_s(char *strDest, size_t destMax, size_t count, const char *format,
                           ...)
{
    va_list args;
    va_start(args, format);
    int ret = vsnprintf(strDest, destMax, format, args);
    va_end(args);
    return ret > 0 ? ret : -1;
}
