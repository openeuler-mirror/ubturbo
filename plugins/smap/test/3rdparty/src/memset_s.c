/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2024-2024. All rights reserved.
 * Description: memset_s stub function
 * Create: 2024-09-18
 */

#include <stdlib.h>
#include <string.h>
#include "securec.h"

errno_t memset_s(void *dest, size_t destMax, int c, size_t count)
{
    memset(dest, c, count);
    return 0;
}
