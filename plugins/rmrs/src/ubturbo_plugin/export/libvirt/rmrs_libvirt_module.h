/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.

 * rmrs is licensed under the Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *      http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */
#ifndef RMRS_LIBVIRT_MODULE_H
#define RMRS_LIBVIRT_MODULE_H

#include <libvirt/libvirt.h>
#include <cstdint>
#include "rmrs_error.h"

namespace rmrs::libvirt {
using VirConnectPtr = void *;

using VirConnectOpenFunc = void *(*)(const char *);
using VirConnectCloseFunc = int (*)(void *);
using VirConnectListAllDomainsFunc = int (*)(void *, void ***, virConnectListAllDomainsFlags);
using VirDomainGetNameFunc = const char *(*)(void *);
using VirDomainGetIDFunc = unsigned int (*)(void *);
using VirDomainGetUUIDStringFunc = int (*)(void *, char *);
using VirDomainGetInfoFunc = int (*)(void *, void *);
using VirDomainGetVcpusFunc = int (*)(void *, void *, int, unsigned char *, int);
using VirConnectGetHostnameFunc = char *(*)(void *);
using VirDomainFreeFunc = int (*)(void *);
using VirConnectIsAliveFunc = int (*)(void *);
using VirConnectDomainEventCallbackFunc = int (*)(void *, void *, int, int, void *);
using VirEventRegisterDefaultImplFunc = int (*)();
using VirEventRunDefaultImplFunc = int (*)();
using VirConnectDomainEventRegisterFunc = int (*)(void *, VirConnectDomainEventCallbackFunc, void *, void *);
using VirConnectDomainEventDeRegisterFunc = int (*)(void *, VirConnectDomainEventCallbackFunc);

class LibvirtModule {
public:
    static RmrsResult Init();

    static VirConnectOpenFunc VirConnectOpen();

    static VirConnectCloseFunc VirConnectClose();

    static VirConnectListAllDomainsFunc VirConnectListAllDomains();

    static VirDomainGetNameFunc VirDomainGetName();

    static VirDomainGetIDFunc VirDomainGetID();

    static VirDomainGetUUIDStringFunc VirDomainGetUUIDString();

    static VirDomainGetInfoFunc VirDomainGetInfo();

    static VirDomainGetVcpusFunc VirDomainGetVcpus();

    static VirConnectGetHostnameFunc VirConnectGetHostname();

    static VirDomainFreeFunc VirDomainFree();

    static VirConnectIsAliveFunc VirConnectIsAlive();

    static VirEventRegisterDefaultImplFunc VirEventRegisterDefaultImpl();

    static VirEventRunDefaultImplFunc VirEventRunDefaultImpl();

    static VirConnectDomainEventRegisterFunc VirConnectDomainEventRegister();

    static VirConnectDomainEventDeRegisterFunc VirConnectDomainEventDeRegister();

private:
    static void *libvirtHandle;
    static VirConnectOpenFunc virConnectOpenFunc;
    static VirConnectCloseFunc virConnectCloseFunc;
    static VirConnectListAllDomainsFunc virConnectListAllDomainsFunc;
    static VirDomainGetNameFunc virDomainGetNameFunc;
    static VirDomainGetIDFunc virDomainGetIDFunc;
    static VirDomainGetUUIDStringFunc virDomainGetUUIDStringFunc;
    static VirDomainGetInfoFunc virDomainGetInfoFunc;
    static VirDomainGetVcpusFunc virDomainGetVcpusFunc;
    static VirConnectGetHostnameFunc virConnectGetHostnameFunc;
    static VirDomainFreeFunc virDomainFreeFunc;
    static VirConnectIsAliveFunc virConnectIsAliveFunc;
    static VirEventRegisterDefaultImplFunc virEventRegisterDefaultImplFunc;
    static VirEventRunDefaultImplFunc virEventRunDefaultImplFunc;
    static VirConnectDomainEventRegisterFunc virConnectDomainEventRegisterFunc;
    static VirConnectDomainEventDeRegisterFunc virConnectDomainEventDeRegisterFunc;
};
} // namespace rmrs::libvirt

#endif // RMRS_LIBVIRT_MODULE_H
