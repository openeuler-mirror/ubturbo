# Tutorial

## Best Practices

在灵衢超节点架构中，通过内存池化技术，单个节点可以利用其它节点的内存资源。其它节点的内存相对于当前节点而言是远端内存，访问远端内存比访问本节点内存时延高，当节点上的进程使用这些远端内存时，性能可能会有较大的劣化。为了解决这些性能劣化的问题，SMAP通过对进程内存进行冷热识别和迁移，保障进程在使用远端内存时性能不会出现较大的劣化。

## Demo

使用SMAP的一个典型场景是：虚拟化场景下，某节点借用到了其它节点的内存，借用内存形成了一个新的NUMA，管理程序设置新的NUMA的可用量，迁出虚机内存到新的NUMA上，过一段时间后管理程序将虚机内存全部迁回到本地，最后移除SMAP对虚机的管理。下面代码调用接口依次完成了以下动作：

1. 初始化SMAP为2M模式
2. 设置NUMA0可以使用NUMA4内存的量为1GB
3. 配置虚机迁出25%内存到NUMA4（虚机绑定使用了NUMA0的内存）
4. 配置虚机迁出0%内存到NUMA4
5. 移除SMAP对虚机的管理

```C
#include <stdio.h>
#include <stdlib.h>

#include "smap_interface.h"

int main()
{
    int ret;
    struct SetRemoteNumaInfoMsg nmsg = {
        .srcNid = 0,
        .destNid = 4,
        .size = 1024, // 1024MB
    };
    struct MigrateOutMsg mmsg = {
        .count = 1, // payload的有效长度为1
        .payload = {
            {
                .destNid = 4, // 迁出到NUMA4
                .pid = 16384, // 虚机PID为16384
                .ratio = 25,  // 迁出比例为25%
            }
        },
    };
    struct RemoveMsg rmsg = {
        .count = 1,
        .payload = {
            {
                .pid = 16384,
            }
        },
    };

    // 初始化SMAP为2M模式
    ret = ubturbo_smap_start(1);
    if (ret != 0) {
        printf("ubturbo_smap_start failed: %d\n", ret);
        exit(EXIT_FAILURE);
    }
    printf("ubturbo_smap_start done\n");

    // 设置NUMA0可以使用NUMA4内存的量
    ret = ubturbo_smap_remote_numa_info_set(&nmsg);
    if (ret != 0) {
        printf("Set available numa failed: %d\n", ret);
        exit(EXIT_FAILURE);
    }
    printf("Set available numa done\n");

    // 配置虚机迁出25%内存到NUMA4
    ret = ubturbo_smap_migrate_out(&mmsg, 1);
    if (ret != 0) {
        printf("Set migrate ratio to 25%% failed: %d\n", ret);
        exit(EXIT_FAILURE);
    }
    printf("Set migrate ratio to 25%% done\n");

    // 等待5s
    sleep(5);

    // 配置虚机迁出0%内存到NUMA4
    mmsg.ratio = 0;
    ret = ubturbo_smap_migrate_out(&mmsg, 1);
    if (ret != 0) {
        printf("Set migrate ratio to 0%% failed: %d\n", ret);
        exit(EXIT_FAILURE);
    }
    printf("Set migrate ratio to 0%% done\n");

    // 移除SMAP对虚机的管理
    ret = ubturbo_smap_remove(&rmsg, 1);
    if (ret != 0) {
        printf("Remove management failed: %d\n", ret);
        exit(EXIT_FAILURE);
    }
    printf("Remove management done\n");

    return 0;
}

```

## Getting Started: Compilation Guide

进入项目根目录执行下列命令来编译内核模块：

```shell
make -C src/kernel KERNEL_VERSION=openeuler -j$(nproc)
```

编译成功后会生成以下文件:

- src/kernel/smap.ko

在项目根目录下执行下列命令来编译动态库:

```shell
dos2unix build.sh
sh build.sh
```

编译成功后会生成以下文件:

- output/smap/lib/libsmap.so

## Getting Started: Installation Guide

SMAP的运行模式根据进程页面大小分为4K和2M模式，由`ubturbo_smap_start(pageType)`入参控制（0=4K，1=2M），无需重插模块切换。SMAP仅加载一个内核模块；硬件判热场景在加载时设置`enable_hist=1`（仅鲲鹏950机型支持）。安装命令如下：

    ```shell
    insmod src/kernel/smap.ko
    ```

加载后提供`/dev/smap_scan_dev`和`/dev/smap_migrate_dev`两个SMAP主设备节点，权限为`ubturbo:ubturbo`、0600。

## Getting Started: Test Guide

测试代码在项目根目录的test目录下，如果你想针对某个函数进行测试开发，只需要在对应目录下编写新的UT代码即可，该目录下主要文件如下所示：

```plaintext
test/
├── depends/                # 内核态打桩代码目录
├── scan/                   # 内核态扫描模块测试代码目录
├── run_dt.sh               # 单元测试准备代码
├── migrate/                # 内核态迁移模块测试代码目录
├── user/                   # 用户态测试代码目录
│   ├── advanced-strategy/  # 高阶策略测试代码目录
│   ├── manage/             # 用户态管理模块测试代码目录
│   └── strategy/           # 用户态策略模块测试代码目录
└── user_depends/           # 用户态打桩代码目录
```

## Getting Started: Contribution Guide

我们非常欢迎新贡献者加入到项目中来，也非常高兴能为新加入贡献者提供指导和帮助，如果您有任何的意见、建议、问题，可以创建issue。
