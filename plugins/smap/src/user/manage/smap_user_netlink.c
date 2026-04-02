#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>
#include <linux/netlink.h>
#include <sys/types.h>
#include <time.h>
#include <pthread.h>
#include <stdint.h>
#include <sys/sysinfo.h>

#include "smap_user_log.h"
#include "securec.h"
#include "smap_user_netlink.h"

#define NETLINK_USER_PROTOCOL 31
#define MAX_PAYLOAD_SIZE 1024

enum nl_message_type {
    NL_MSG_REGISTER = 1,
    NL_MSG_UNREGISTER,
    NL_MSG_USER_TO_KERNEL,
    NL_MSG_KERNEL_TO_USER,
    NL_MSG_ACK,
};

struct NlMsgHeader {
    uint16_t type;
    uint16_t dataLen;
};

static int nlSocket = -1;
static volatile int g_running = 1;

static int SendNlMsg(int sockFd, uint32_t type, const void *data, size_t datalen)
{
    struct sockaddr_nl dstAddr;
    struct nlmsghdr *nlh;
    struct NlMsgHeader *header;
    struct iovec iov;
    struct msghdr msg;
    size_t totalSize;
    char *buffer;
    
    int ret = memset_s(&dstAddr, sizeof(dstAddr), 0, sizeof(dstAddr));
    if (ret) {
        SMAP_LOGGER_ERROR("Failed to memset destination address: %d", ret);
        return -ENOMEM;
    }
    dstAddr.nl_family = AF_NETLINK;
    dstAddr.nl_pid = 0;
    dstAddr.nl_groups = 0;
    
    totalSize = NLMSG_SPACE(sizeof(struct NlMsgHeader) + datalen);
    
    buffer = malloc(totalSize);
    if (!buffer) {
        SMAP_LOGGER_ERROR("Failed to allocate buffer for netlink message.");
        return -ENOMEM;
    }
    
    nlh = (struct nlmsghdr *)buffer;
    nlh->nlmsg_len = totalSize;
    nlh->nlmsg_pid = getpid();
    nlh->nlmsg_flags = 0;
    
    header = NLMSG_DATA(nlh);
    header->type = type;
    header->dataLen = datalen;
    
    if (data && datalen > 0) {
        ret = memcpy_s((void *)(header + 1), datalen, data, datalen);
        if (ret) {
            free(buffer);
            return -ENOMEM;
        }
    }
    
    iov.iov_base = (void *)nlh;
    iov.iov_len = nlh->nlmsg_len;

    ret = memset_s(&msg, sizeof(msg), 0, sizeof(msg));
    if (ret) {
        SMAP_LOGGER_ERROR("Failed to memset message header: %d", ret);
        free(buffer);
        return -ENOMEM;
    }
    msg.msg_name = (void *)&dstAddr;
    msg.msg_namelen = sizeof(dstAddr);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    
    ret = sendmsg(sockFd, &msg, 0);
    free(buffer);
    
    if (ret < 0) {
        SMAP_LOGGER_ERROR("Failed to send netlink message: %d", ret);
    }
    
    return ret;
}

int RecvNlMsg(int sockFd, void *buffer, size_t bufferSize)
{
    struct sockaddr_nl src_addr;
    struct nlmsghdr *nlh;
    struct iovec iov;
    struct msghdr msg;
    int ret;
    
    nlh = (struct nlmsghdr *)buffer;
    
    iov.iov_base = (void *)nlh;
    iov.iov_len = bufferSize;
    
    memset(&msg, 0, sizeof(msg));
    msg.msg_name = (void *)&src_addr;
    msg.msg_namelen = sizeof(src_addr);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    
    ret = recvmsg(sockFd, &msg, 0);
    
    if (src_addr.nl_pid != 0) {
        SMAP_LOGGER_WARNING("[Listener] Received message from unknown source: %u", src_addr.nl_pid);
        return -EINVAL;
    }
    
    return ret;
}

void ProcessNlMsg(struct nlmsghdr *nlh) {
    struct NlMsgHeader *header;
    void *payload;
    
    if (!nlh || !NLMSG_OK(nlh, nlh->nlmsg_len)) {
        return;
    }
    
    header = NLMSG_DATA(nlh);
    payload = (void *)(header + 1);
    
    switch (header->type) {
        case NL_MSG_KERNEL_TO_USER:
            if (header->dataLen > 0) {
                ;// TODO
            }
            break;
            
        case NL_MSG_ACK:
            break;
            
        default:
            SMAP_LOGGER_ERROR("[UNKNOWN] Received unknown Netlink message type: %u", header->type);
    }
}

void *NlListenerThread(void *arg)
{
    char buffer[MAX_PAYLOAD_SIZE + sizeof(struct nlmsghdr) + 
                 sizeof(struct NlMsgHeader)];
    int ret;
    
    while (g_running) {
        ret = RecvNlMsg(nlSocket, buffer, sizeof(buffer));
        if (ret > 0) {
            ProcessNlMsg((struct nlmsghdr *)buffer);
        } else {
            SMAP_LOGGER_ERROR("[Listener] Failed to receive Netlink message: %d", ret);
        }
        usleep(10000);  // 10ms
    }

    return NULL;
}

int SendNumaInfoToKernel(const NumaStatusList *numaList) 
{
    if (!numaList) {
        SMAP_LOGGER_ERROR("NumaStatusList is NULL.");
        return -EINVAL;
    }

    size_t dataSize = sizeof(uint16_t) + sizeof(NumaEntry) * numaList->cnt;
    int ret = SendNlMsg(nlSocket, NL_MSG_USER_TO_KERNEL, (void *)numaList, dataSize);
    if (ret) {
        SMAP_LOGGER_ERROR("Failed to send NUMA info to kernel.");
    }

    return ret;
}

int UserNetlinkInit(void) 
{
    struct sockaddr_nl src_addr;
    pthread_t listenerTid;
    int ret;

    nlSocket = socket(AF_NETLINK, SOCK_RAW, NETLINK_USER_PROTOCOL);
    if (nlSocket < 0) {
        SMAP_LOGGER_ERROR("Failed to create Netlink socket.");
        return -ENOMEM;
    }

    memset(&src_addr, 0, sizeof(src_addr));
    src_addr.nl_family = AF_NETLINK;
    src_addr.nl_pid = getpid();
    src_addr.nl_groups = 0;

    if (bind(nlSocket, (struct sockaddr *)&src_addr, sizeof(src_addr)) < 0) {
        SMAP_LOGGER_ERROR("Failed to bind Netlink socket.");
        close(nlSocket);
        return -ENOMEM;
    }
    ret = SendNlMsg(nlSocket, NL_MSG_REGISTER, NULL, 0);
    if (ret) {
        SMAP_LOGGER_ERROR("Failed to register with kernel.");
        close(nlSocket);
        return -ENOMEM;
    }

    if (pthread_create(&listenerTid, NULL, NlListenerThread, NULL) != 0) {
        SMAP_LOGGER_ERROR("Failed to create listener thread.");
        SendNlMsg(nlSocket, NL_MSG_UNREGISTER, NULL, 0);
        close(nlSocket);
        return -ENOMEM;
    }
    pthread_detach(listenerTid);

    return 0;
}

void UserNetlinkCleanup(void) 
{
    g_running = 0;
    SendNlMsg(nlSocket, NL_MSG_UNREGISTER, NULL, 0);
    close(nlSocket);
}