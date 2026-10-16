/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
 *
 * smap is licensed under the Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *      http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */
#define _GNU_SOURCE
#include <unistd.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <errno.h>
#include <string.h>
#include "securec.h"
#include "manage.h"
#include "thread.h"
#include "thread_pool.h"
#include "strategy/migration.h"
#include "smap_log_core.h"
#include "smap_user_log.h"

void EventLoopResetEnqueued(struct PidSlot *slot)
{
    EnvAtomicSet(&slot->eventEnqueued, 0);
}

void EventLoopUnregisterSlotLocked(struct PidSlot *slot)
{
    EventLoop *l = &GetProcessManager()->eventLoop;
    if (slot->memFreqFd >= 0) {
        if (l->epfd >= 0) {
            (void)epoll_ctl(l->epfd, EPOLL_CTL_DEL, slot->memFreqFd, NULL);
        }
        close(slot->memFreqFd);
        slot->memFreqFd = -1;
    }
    EventLoopResetEnqueued(slot);
}

void EventLoopUnregisterSlot(struct PidSlot *slot)
{
    EnvMutexLock(&slot->attrLock);
    EventLoopUnregisterSlotLocked(slot);
    EnvMutexUnlock(&slot->attrLock);
}

/* 注册一个 NORMAL_SCAN pid 的 /proc/smap/<pid>/mem_freq 到 epoll（attrLock 内串行化注册与注销） */
int EventLoopRegisterPid(pid_t pid)
{
    EventLoop *l = &GetProcessManager()->eventLoop;
    struct PidSlot *slot;
    int ret = 0;

    if (l->epfd < 0 || EnvAtomicRead(&l->stop)) {
        return 0;
    }
    slot = PidSlotGetRef(pid);
    if (!slot) {
        return -ENOENT;
    }

    char path[64];
    EnvMutexLock(&slot->attrLock);
    if (EnvAtomicRead(&l->stop) || EnvAtomicRead(&slot->state) != PID_SLOT_INUSE || slot->attr == NULL ||
        slot->attr->scanType != NORMAL_SCAN || slot->memFreqFd >= 0) {
        goto out;
    }
    if (snprintf_s(path, sizeof(path), sizeof(path) - 1, "/proc/smap/%d/mem_freq", pid) < 0) {
        ret = -EINVAL;
        goto out;
    }
    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        ret = -errno;
        goto out;
    }
    /* 用槽位指针关联 mem_freq 的扫描完成及关闭事件。 */
    struct epoll_event ev = { .events = EPOLLIN | EPOLLERR | EPOLLHUP, .data.ptr = slot };
    if (epoll_ctl(l->epfd, EPOLL_CTL_ADD, fd, &ev)) {
        close(fd);
        ret = -errno;
        goto out;
    }
    slot->memFreqFd = fd;
    EventLoopResetEnqueued(slot);
out:
    EnvMutexUnlock(&slot->attrLock);
    PidSlotReleaseRefs(&slot, 1);
    return ret;
}

static int EventLoopRegisterExistingPids(struct ProcessManager *manager)
{
    struct PidSlot *slots[MAX_PID_SLOTS];
    size_t count = PidSlotCollectRefs(manager, slots, MAX_PID_SLOTS);
    int ret = 0;

    for (size_t i = 0; i < count; i++) {
        int registerRet = EventLoopRegisterPid(slots[i]->pid);

        if (registerRet && ret == 0) {
            ret = registerRet;
        }
    }
    PidSlotReleaseRefs(slots, count);
    return ret;
}

/* 为尚未注册 mem_freq 的普通扫描 PID 重试事件监听。 */
void EventLoopEnsureRegistered(struct ProcessManager *manager)
{
    EventLoop *l = &manager->eventLoop;
    struct PidSlot *slots[MAX_PID_SLOTS];
    size_t count;

    if (l->epfd < 0 || EnvAtomicRead(&l->stop)) {
        return;
    }
    count = PidSlotCollectRefs(manager, slots, MAX_PID_SLOTS);
    for (size_t i = 0; i < count; i++) {
        struct PidSlot *slot = slots[i];
        if (EnvAtomicRead(&slot->eventEnqueued) != 0) {
            continue;
        }
        EnvMutexLock(&slot->attrLock);
        /* 迁移中的 PID 暂不补挂监听。 */
        if (EnvAtomicRead(&slot->state) == PID_SLOT_INUSE && slot->memFreqFd < 0 && slot->attr != NULL &&
            slot->attr->scanType == NORMAL_SCAN && slot->attr->state != PROC_MIGRATE) {
            pid_t pid = slot->pid;
            EnvMutexUnlock(&slot->attrLock);
            (void)EventLoopRegisterPid(pid);
        } else {
            EnvMutexUnlock(&slot->attrLock);
        }
    }
    PidSlotReleaseRefs(slots, count);
}

/* 处理 wakeFd 唤醒事件：eventfd 计数 > 0 时 epoll 会一直视为可读（水平触发），
 * 必须 read 清空计数，否则事件循环将持续空转；EAGAIN 表示计数已为 0，属正常 */
static void EventLoopHandleWakeFd(EventLoop *l)
{
    uint64_t value;
    ssize_t readRet = read(l->wakeFd, &value, sizeof(value));
    if (readRet < 0 && errno != EAGAIN) {
        SMAP_LOGGER_WARNING("Read event loop wake fd failed: %d.", errno);
    }
}

/* 唤醒通知：向 eventfd 写计数使阻塞中的 epoll_wait 立即返回；
 * EAGAIN 表示计数器已满（唤醒本就生效），无需重试 */
static void EventLoopNotifyWakeFd(EventLoop *l)
{
    uint64_t value = 1;
    if (l->wakeFd >= 0 && write(l->wakeFd, &value, sizeof(value)) < 0 && errno != EAGAIN) {
        SMAP_LOGGER_WARNING("Wake event loop failed: %d.", errno);
    }
}

static void *EventLoopMain(void *arg)
{
    struct ProcessManager *m = arg;
    EventLoop *l = &m->eventLoop;
    struct epoll_event ev[MAX_POOL_WORKERS];
    while (!EnvAtomicRead(&l->stop)) {
        int n = epoll_wait(l->epfd, ev, MAX_POOL_WORKERS, -1);
        if (n < 0) {
            /* epoll_wait 阻塞期间可被信号打断返回 EINTR：不是错误，应继续等待；
             * 其他 errno（如 EBADF）说明 epoll 已损坏，继续等没有意义 */
            if (errno != EINTR) {
                break;
            }
            continue;
        }
        for (int i = 0; i < n; i++) {
            if (EnvAtomicRead(&l->stop)) {
                break;
            }
            struct PidSlot *slot = ev[i].data.ptr;
            /* data.ptr == NULL 约定为该事件来自 wakeFd（门铃），只用于主动唤醒，不产生任务 */
            if (slot == NULL) {
                EventLoopHandleWakeFd(l);
                continue;
            }
            if (!PidSlotTryGetRef(slot)) {
                continue;
            }
            EnvMutexLock(&slot->attrLock);
            if (EnvAtomicRead(&slot->state) == PID_SLOT_INUSE && EnvAtomicCmpAndSwap(0, 1, &slot->eventEnqueued) == 0) {
                pid_t pid = slot->pid;

                if (ThreadPoolSubmit(m, slot, pid) == 0) {
                    /* 入队成功：引用随任务转交给 worker（由 PidMigrationWork 归还），此处不释放 */
                    EnvMutexUnlock(&slot->attrLock);
                    continue;
                }
                /* 入队失败（队列满）：复位去重标志；epoll 水平触发下次仍可读，事件不丢 */
                EventLoopResetEnqueued(slot);
            }
            PidSlotReleaseRefs(&slot, 1);
            EnvMutexUnlock(&slot->attrLock);
        }
    }
    return NULL;
}

static void *ThreadMainEvent(void *args)
{
    struct ProcessManager *manager = args;

    SMAP_LOGGER_INFO("ScanMigrate daemon thread %lu created.", gettid());
    while (!EnvAtomicRead(&manager->scanMigrateStop)) {
        EnvMsleep(manager->migPeriod);
        ManagerDaemonWork(manager);
    }
    return NULL;
}

int InitScanMigrateThread(struct ProcessManager *manager, uint32_t period)
{
    int ret;
    manager->migPeriod = period;
    EnvAtomicSet(&manager->scanMigrateStop, 0);
    /* 先清零 pthread_t，确保 pthread_create 失败时不残留未定义值，
     * DestroyScanMigrateThread 通过判断 pthread_t 是否为零来跳过无效 join */
    (void)memset_s(&manager->scanMigrateThread, sizeof(manager->scanMigrateThread), 0,
                   sizeof(manager->scanMigrateThread));
    ret = pthread_create(&manager->scanMigrateThread, NULL, ThreadMainEvent, manager);
    if (ret) {
        SMAP_LOGGER_ERROR("Create scan migrate thread failed: %d.", ret);
        (void)memset_s(&manager->scanMigrateThread, sizeof(manager->scanMigrateThread), 0,
                       sizeof(manager->scanMigrateThread));
        return -ret;
    }
    return 0;
}

int DestroyScanMigrateThread(struct ProcessManager *manager)
{
    pthread_t zeroThread;
    (void)memset_s(&zeroThread, sizeof(zeroThread), 0, sizeof(zeroThread));

    EnvAtomicSet(&manager->scanMigrateStop, 1);
    /* 仅在 pthread_t 非零（即线程已成功创建）时才 join，避免对未初始化的 pthread_t 执行未定义行为 */
    if (memcmp(&manager->scanMigrateThread, &zeroThread, sizeof(pthread_t)) != 0) {
        pthread_join(manager->scanMigrateThread, NULL);
    }
    SMAP_LOGGER_INFO("ScanMigrate thread destroyed.");
    return 0;
}

/* 关闭 epoll/wake fd 并复位，供初始化失败回滚与正常销毁共用 */
static void EventLoopCloseFds(EventLoop *l)
{
    if (l->wakeFd >= 0) {
        close(l->wakeFd);
        l->wakeFd = -1;
    }
    if (l->epfd >= 0) {
        close(l->epfd);
        l->epfd = -1;
    }
}

/* 置停止标志并唤醒事件循环线程，等待其退出 */
static void EventLoopStopAndJoin(EventLoop *l)
{
    EnvAtomicSet(&l->stop, 1);
    EventLoopNotifyWakeFd(l);
    if (l->inited) {
        pthread_join(l->thread, NULL);
    }
}

int InitEventLoop(struct ProcessManager *m)
{
    EventLoop *l = &m->eventLoop;
    (void)memset_s(l, sizeof(*l), 0, sizeof(*l));
    l->epfd = -1;
    l->wakeFd = -1;
    l->inited = false;
    l->epfd = epoll_create1(EPOLL_CLOEXEC);
    if (l->epfd < 0) {
        return -errno;
    }
    l->wakeFd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (l->wakeFd < 0) {
        int ret = -errno;
        EventLoopCloseFds(l);
        return ret;
    }
    struct epoll_event wakeEvent = { .events = EPOLLIN, .data.ptr = NULL };
    if (epoll_ctl(l->epfd, EPOLL_CTL_ADD, l->wakeFd, &wakeEvent)) {
        int ret = -errno;
        EventLoopCloseFds(l);
        return ret;
    }
    int ret = pthread_create(&l->thread, NULL, EventLoopMain, m);
    if (ret) {
        EventLoopCloseFds(l);
        return -ret;
    }
    l->inited = true;
    ret = EventLoopRegisterExistingPids(m);
    if (ret) {
        EventLoopStopAndJoin(l);
        EventLoopCloseFds(l);
        return ret;
    }
    return 0;
}

int DestroyEventLoop(struct ProcessManager *m)
{
    EventLoop *l = &m->eventLoop;
    if (l->epfd < 0) {
        return 0;
    }
    l->inited = false;
    EventLoopStopAndJoin(l);
    for (int i = 0; i < MAX_PID_SLOTS; i++) {
        EventLoopUnregisterSlot(&m->slots[i]);
    }
    EventLoopCloseFds(l);
    return 0;
}
