# SMAP性能测试

## SMAP内存分级

### 内存迁移

> [!IMPORTANT] 须知
>
> - SMAP内存迁移依赖将远端内存借用到本地。
> - 已安装SMAP。

### 概述

在虚拟化场景中，随着虚机内使用内存增加，NUMA内的内存使用率随之增加，当达到一定水线时，虚机的部分内存需要逃生到借用内存。此时SMAP提供能力，将虚机冷数据按照可控比例迁出至借用内存。在之后的时间内，SMAP周期性统计内存冷热信息，保证虚机内应用在使用远端内存时，性能可控。当NUMA内的内存使用率下降到一定水线时，SMAP支持按照借用的内存迁回虚机数据。

### 功能说明

- SMAP内存迁移功能目前只开放给rack\_manager组件使用，由rack\_manager调用“/usr/lib64/libsmap.so”提供的内存迁移功能。
- SMAP底层使用内核函数migrate\_pages\(\)进行NUMA内存迁移，migrate\_pages\(\)保留内核原有能力，SMAP未对其进行修改。

## 虚拟化场景2M模式SMAP性能约束

虚拟化场景下SMAP为达成2M模式下轻载虚机性能跌落不高于5%，重载虚机性能跌落不高于10%，混布场景平均性能跌落不高于5%，需要满足以下约束：

- 虚机内存模式为立即分配模式。
- 虚拟化场景下，内存迁移及自适应场景需保证远端内存和近端内存的大页内存有余量（大于总的虚机规格的10%）。
- 虚拟机使用的热页总数不超过分配给虚拟机的本地页的80%。
- 虚机混合部署时，对于近端NUMA节点与远端NUMA节点相同的一组虚拟机，需满足轻载虚机总内存规格大于等于重载虚机总内存规格。
- 轻载场景包括：Redis典型用例（参见[Redis典型参数](#redis典型参数)）。
- 重载场景包括：MySQL典型用例、SPECint典型用例（参见[MySQL典型参数](#mysql典型参数)和[SPECint典型参数](#specint典型参数)）。
- 多虚机场景，虚机总数量不高于4个，且测试性能时一个虚机中只能跑一种典型用例，其他场景性能不保证。
- 虚机规格核存比为1:2或1:4，典型虚机规格为4U8G（鲲鹏虚拟化场景）或8U32G。如需使用4U8G以外的规格进行测试，请遵循其他约束缩放推荐测试参数，其他规格场景性能不保证。
- 所有性能数据，至少测试3次，然后按照虚机内存规格计算算术平均值（SUM（虚拟机性能跌落百分比 \* 虚拟机内存规格）/SUM（虚拟机内存规格））。
- 部署SMAP的节点需要保证SMAP进程所在逻辑核与虚拟机所在逻辑核不相同。

在多虚机场景下，当不满足上述约束时，性能有不达标风险。当使用SMAP提供的CLI工具测试SMAP性能时，SMAP支持灵活可配置的方式来达成性能目标，首先通过以下命令关闭SMAP的多虚机自适应配比功能，设置正确的借用内存信息后，通过迁出命令调整每个虚机到合适的配比（总的本地内存使用与总的借用内存使用的比例与灵活配置前保持一致）：

```shell
smap smap_enable_adapt_mem  0
smap set_smap_remote_numa_info [src_nid] [dest_nid] [size]
smap smap_mig_out [dest_nid] [pid] [ratio] [pidType]
```

### Redis典型参数

其中Redis典型用例参数为，4U8G虚机场景下：

- Server

    ```shell
    taskset -c 1 redis-server --port [redis-server port] --protected-mode no
    ```

- Client

    ```shell
    redis-benchmark -t set -n 20000000 -c 128 -r 1640000 -d 2048 --threads 64 -h [redis-server ip] -p [redis-server port]
    ```

### MySQL典型参数

MySQL典型用例测试参数：

- Server侧启动MySQL的配置文件my.cnf。

    ```shell
    [mysqld_safe]
    log-error=/data/mysql/log/mysql.log
    pid-file=/data/mysql/run/mysqld.pid
    
    [client]
    socket=/data/mysql/run/mysql.sock
    default-character-set=utf8
    
    [mysqld]
    basedir=/usr/local/mysql
    tmpdir=/data/mysql/tmp
    datadir=/data/mysql/data
    socket=/data/mysql/run/mysql.sock
    port=3306
    user=root
    default_authentication_plugin=mysql_native_password
    ssl=0 # 关闭ssl
    max_connections=2500  # 设置最大连接数
    back_log=1000  # 设置会话请求缓存个数
    performance_schema=OFF # 关闭性能模式
    max_prepared_stmt_count=1048576
    
    # file
    innodb_file_per_table #设置每个表一个文件
    innodb_log_file_size=1000M #设置logfile大小
    innodb_log_files_in_group=4 #设置logfile组个数
    innodb_open_files=3000 #设置最大打开表个数
    
    # buffers
    innodb_buffer_pool_size=4G # 适用于8G内存规格的虚拟机，设置buffer pool size,一般为服务器内存60%
    # innodb_buffer_pool_size=16G 适用于32G内存规格的虚拟机
    innodb_buffer_pool_instances=2 # 设置buffer pool instance个数，提高并发能力
    innodb_log_buffer_size=16M # 设置log buffer size大小
    
    # tune
    default_time_zone=+8:00
    thread_cache_size=100
    sync_binlog=1 # 设置每次sync_binlog事务提交刷盘
    innodb_flush_log_at_trx_commit=1 # 每次事务提交时MySQL都会把log buffer的数据写入log file，并且flush(刷到磁盘)中去
    innodb_use_native_aio=1 # 开启异步IO
    innodb_spin_wait_delay=20 # 设置spin_wait_delay 参数，防止进入系统自旋
    # innodb_sync_spin_loops=25  # 设置spin_loops 循环次数，防止进入系统自旋
    innodb_spin_wait_pause_multiplier=5 # 设置spin lock循环随机数
    innodb_flush_method=O_DIRECT # 设置innodb数据文件及redo log的打开、刷写模式
    innodb_io_capacity=12000 # 设置innodb后台线程每秒最大iops上限
    innodb_io_capacity_max=24000 # 设置压力下innodb后台线程每秒最大iops上限
    innodb_lru_scan_depth=9000 # 设置page cleaner线程每次刷脏页的数量
    innodb_page_cleaners=2  # 设置将脏数据写入到磁盘的线程数
    table_open_cache_instances=2 # 设置打开句柄分区数
    table_open_cache=18000 # 设置打开表的数量
    
    # perf special
    innodb_flush_neighbors=0 # 检测该页所在区(extent)的所有页，如果是脏页，那么一起进行刷新，SSD关闭该功能
    # innodb_write_io_threads=24 # 设置写线程数
    # innodb_read_io_threads=16 # 设置读线程数
    # innodb_purge_threads=32  # 设置回收已经使用并分配的undo页线程数
    # innodb_adaptive_hash_index=0
    sql_mode=STRICT_TRANS_TABLES,NO_ENGINE_SUBSTITUTION,NO_AUTO_VALUE_ON_ZERO,STRICT_ALL_TABLES
    # log-bin=mysql-bin  # 开binlog
    skip_log_bin    # 关binlog
    ```

- client侧的激励脚本read\_write.sh。

    ```shell
    #
    #!/bin/bash
    host=$1
    vm_name=$2
    max_threads=512
    threads_arr=(8 16 32 64 128 256 512)
    time=120
    run_test()
    {
            local threads=$1
            local time=$2
            echo "use ${threads} threads, run ${time}s"
            sysbench --db-driver=mysql --mysql-host=${host} --mysql-port=3306 --mysql-user=root --mysql-password={your_mysql_password} --mysql-db=sbtest \
            --table_size=10000000 --tables=64 --time=${time} --threads=${threads} --percentile=95 --report-interval=1 oltp_read_write run > /home/mysql_test_results/${vm_name}/${threads}_run.log
    }
    run_loop()
    {
            for threads in ${threads_arr[@]}
            do
                    date
                    run_test ${threads} ${time}
                    sleep 3
            done
    }
    run_loop
    ```

### SPECint典型参数

SPECint典型用例测试参数，4U8G虚机场景：

获取[speccpu-gcc-best.cfg](./resource/speccpu-gcc-best.cfg)

```shell
HUGETLB_ELFMAP=RW runcpu --config=speccpu-gcc-best.cfg --iteration=1 --copies=4 intrate
```

### Stream典型参数

此SMAP性能保证需要满足以下条件：

1. 计算虚机内部内存使用

    执行**free -h**命令查看剩余内存量，那么虚机内部内存使用比例计算如下：

    虚机内部内存使用比例 = 1 - 剩余内存量 / 虚机规格

2. 外部计算本地内存占比

    本地内存占比 = 本地内存 / 虚机规格

3. SMAP保证性能需满足以下条件：

    虚机内部内存使用比例  <= 本地内存占比

## 容器场景SMAP性能约束

在容器测试场景下，例如Redis、MySQL等测试场景，在性能测试时应保证对CPU绑核和对内存绑NUMA，对于跨NUMA内存访问的进程，SMAP的冷热交换策略暂未适配该场景（跨NUMA的文件页不在考虑范围内），不保证性能表现。

## Redis场景额外约束

针对Redis场景有memcpy优化，性能可相比基线提升2%，由于性能提升比例受基线波动率影响较大，因此约束测试环境基线性能波动率要小于5%。

### 测试前置条件

服务端虚机与客户端虚机均需要下电，确认服务端虚机与客户端虚机的配置文件XML中，已将网卡的MTU调至9000，调优网络收发包的基础性能。

- 虚机网卡MTU配置示例如下：

    ```xml
        <interface type='bridge'>
          <mac address='ff:ff:ff:ff:ff:ff'/>
          <source bridge='virbr0'/>
          <target dev='vnet7'/>
          <model type='virtio'/>
          <driver name='vhost' queues='4'/>
          <mtu size='9000'/>
          <alias name='net0'/>
          <address type='pci' domain='0x0000' bus='0x01' slot='0x00' function='0x0'/>
        </interface>
    ```

- 按照单虚机Redis内存池化场景，配置SMAP参数。

### 基础优化

启动服务端虚机与客户端虚机后，需要进行基础调优，保证基线性能稳定。

- 虚机vhost线程绑核

    查看服务端虚机和客户端虚机的vhost线程。

    ```shell
    ps -ef | grep vhost
    ```

    通过taskset -cp命令对服务端虚机和客户端虚机的vhost线程手动绑核，注意需要规避虚机占用的核。

- 服务端虚机网卡基础性能优化

    以虚机使用网卡为eth0示例，查看中断号。

    ```shell
    cat /proc/interrupts| grep virtio0
    ```

    将默认在0号核上的中断绑定在1号核（此处假设0号核涉及的中断号为62、63）。

    ```shell
    echo 1 > /proc/irq/62/smp_affinity_list
    echo 1 > /proc/irq/63/smp_affinity_list
    ```

    网卡队列不绑核。

    ```shell
    echo 0 > /sys/class/net/eth0/queues/rx-0/xps_cpus
    echo 0 > /sys/class/net/eth0/queues/rx-1/xps_cpus
    echo 0 > /sys/class/net/eth0/queues/rx-2/xps_cpus
    echo 0 > /sys/class/net/eth0/queues/rx-3/xps_cpus
    echo 0 > /sys/class/net/eth0/queues/tx-0/xps_cpus
    echo 0 > /sys/class/net/eth0/queues/tx-1/xps_cpus
    echo 0 > /sys/class/net/eth0/queues/tx-2/xps_cpus
    echo 0 > /sys/class/net/eth0/queues/tx-3/xps_cpus
    ```

- 客户端虚机网卡基础性能优化

    以虚机使用网卡为eth0示例，网卡队列不绑核。

    ```shell
    echo 0 > /sys/class/net/eth0/queues/rx-0/xps_cpus
    echo 0 > /sys/class/net/eth0/queues/rx-1/xps_cpus
    echo 0 > /sys/class/net/eth0/queues/rx-2/xps_cpus
    echo 0 > /sys/class/net/eth0/queues/rx-3/xps_cpus
    echo 0 > /sys/class/net/eth0/queues/tx-0/xps_cpus
    echo 0 > /sys/class/net/eth0/queues/tx-1/xps_cpus
    echo 0 > /sys/class/net/eth0/queues/tx-2/xps_cpus
    echo 0 > /sys/class/net/eth0/queues/tx-3/xps_cpus
    ```

### 测试步骤

- 基线测试：服务端虚机绑定0号核拉起Redis-server，客户端连续测试5次set，再连续测试5次get，统计均值。

    ```shell
    taskset -c 0 /home/redis-6.0.5/src/redis-server /home/redis-6.0.5/redis.conf --port 6379
    ```

- memcpy优化测试：服务端虚机加载算法优化库，绑0号核拉起Redis-server，客户端连续测试5次set，再连续测试5次get，统计均值。

    ```shell
    LD_PRELOAD=/usr/lib64/libksal_libc.so taskset -c 0 /home/redis-6.0.5/src/redis-server /home/redis-6.0.5/redis.conf --port 6379
    ```
    
    >[!IMPORTANT] 须知
    >
    > libksal_libc.so文件需要额外获取。

## MySQL场景额外约束

以K8S容器测试为例，基于K8S启动一个8U40G的容器，容器配置文件参考如下，启动mysqld使通过numactl绑定NUMA的CPU和内存：

```shell
apiVersion: v1
kind: Pod
metadata:
  name: mysql-pod
  namespace: kube-system
  labels:
    remote-mem-allocation-ratio: "0"
spec:
  nodeName: master
  #  hostNetwork: true
  containers:
  - name: mysql
    image: docker.io/library/mysql:8.0.27
    imagePullPolicy: IfNotPresent
    ports:
    - containerPort: 3306
      hostPort: 3307
    command: ["/bin/sh", "-c", "ulimit -l unlimited && chown -R mysql:mysql /var/lib/mysql && exec numactl --cpunodebind=0 --membind=0 /usr/local/mysql/bin/mysqld --user=mysql"]
    resources:
      requests:
        cpu: "8"
        memory: "40Gi"
      limits:
        cpu: "8"
        memory: "40Gi"
    securityContext:
      privileged: true
      capabilities:
        add: ["IPC_LOCK", "SYS_PTRACE"]
    volumeMounts:
    - name: mysql-data
      mountPath: /var/lib/mysql
    - name: config-volume
      mountPath: /etc/my.cnf
      subPath: my.cnf
  volumes:
  - name: mysql-data
    hostPath:
      path: /mnt/mysql_data_100
      type: DirectoryOrCreate
  - name: config-volume
    configMap:
      name: mysql-config
      items:
      - key: my.cnf
        path: my.cnf
```

正式测试时，执行以下命令：

```shell
# 将服务端和客户端的直连高性能网卡的mtu提升到9000
ifconfig enp38s0f0np0 mtu 9000
# 启动容器之前清除原有pagecache
echo 3 > /proc/sys/vm/drop_caches
# 服务端容器启动在本地，提前用大页占住NUMA 0的内存，使本地内存余量为43GiB左右，迁移测试时，本地内存余量为50GiB左右
echo 39495 > /sys/devices/system/node/node0/hugepages/hugepages-2048kB/nr_hugepages
```

插SMAP ko时指定enable\_hist参数为0：

```shell
insmod smap_access_tracking.ko enable_hist=0 smap_scene=2
```

MySQL的配置文件my.cnf的内容如下：

```shell
[mysqld_safe]
log-error=/var/log/mysql/error.log
pid-file=/var/lib/mysql/mysqld.pid
[client]
socket=/var/lib/mysql/mysql.sock
default-character-set=utf8
[mysqld]
server-id=1
basedir=/usr/local/mysql
datadir=/var/lib/mysql
tmpdir=/tmp
socket=/var/lib/mysql/mysql.sock
pid-file=/var/lib/mysql/mysqld.pid
log-error=/var/log/mysql/error.log
#skip-grant-tables
net_read_timeout=600
net_write_timeout=600
max_allowed_packet=500M
skip_networking=off
bind-address = 0.0.0.0
default_authentication_plugin=mysql_native_password
port=3306
user=root
#innodb_page_size=4k
max_connections=2000
back_log=4000
performance_schema=OFF
max_prepared_stmt_count=1280000
#transaction_isolation=READ-COMMITTED
#file
innodb_file_per_table
innodb_log_file_size=2048M
innodb_log_files_in_group=32
innodb_open_files=10000
table_open_cache_instances=64
#buffers
innodb_buffer_pool_size=33G
innodb_buffer_pool_instances=8
#innodb_log_buffer_size=2048M
#tune
default_time_zone=+8:00
#innodb_numa_interleave=1
thread_cache_size=2000
sync_binlog=1
innodb_flush_log_at_trx_commit=1
innodb_use_native_aio=1
innodb_spin_wait_delay=3
innodb_sync_spin_loops=10
innodb_flush_method=O_DIRECT
innodb_io_capacity=30000
innodb_io_capacity_max=40000
innodb_lru_scan_depth=9000
innodb_page_cleaners=8
innodb_spin_wait_pause_multiplier=5
#perf special
innodb_flush_neighbors=0
innodb_write_io_threads=24
innodb_read_io_threads=16
innodb_purge_threads=32
sql_mode=STRICT_TRANS_TABLES,NO_ENGINE_SUBSTITUTION,NO_AUTO_VALUE_ON_ZERO,STRICT_ALL_TABLES
log-bin=mysql-bin
#skip_log_bin
ssl=0
table_open_cache=30000
max_connect_errors=2000
innodb_adaptive_hash_index=0
#large_pages=1
mysqlx=0
```

通过sysbench下发激励命令如下，以64个线程进行压测，新启动容器后，连续测试两次，取第2次的稳定数据作为测试结果；执行过write\_only和read\_write场景后，需要重新拷贝数据库：

```shell
# read_only场景
numactl -C 70-101 -m 1 sysbench --db-driver=mysql --mysql-host=[mysql_host] --mysql-port=3307 --mysql-user=root --mysql-password=[mysql_password] --mysql-db=sbtest --table_size=10000000 --tables=64 --time=600  --threads=64 --percentile=95 --report-interval=20 oltp_read_only run
# write_only场景
numactl -C 70-101 -m 1 sysbench --db-driver=mysql --mysql-host=[mysql_host] --mysql-port=3307 --mysql-user=root --mysql-password=[mysql_password] --mysql-db=sbtest --table_size=10000000 --tables=64 --time=600  --threads=64 --percentile=95 --report-interval=20 oltp_write_only run
# read_write场景
numactl -C 70-101 -m 1 sysbench --db-driver=mysql --mysql-host=[mysql_host] --mysql-port=3307 --mysql-user=root --mysql-password=[mysql_password] --mysql-db=sbtest --table_size=10000000 --tables=64 --time=600  --threads=64 --percentile=95 --report-interval=20 oltp_read_write run
```

## 规格说明

**表 1**  规格说明

|规格项|规格（虚拟化2M场景）|-|
|:----|:----|:----|
|单虚机最小规格|2U2G|同解决方案对齐最小规格|
|单节点最大虚机数量|100（容器场景为300个进程）|按照2U2G最小虚机计算，内存最大为2TB，CPU型号1650，CPU核心数量192。|
|最大借用内存节点|7|RackServer最大邻居节点7个。|
|最大借用远端NUMA数|18|每个socket呈现1个远端NUMA。|
|本地最大NUMA数|4|-|
|迁出最大比例|100%|-|
|性能跌落|5%|同解决方案对齐性能指标：<ol><li>虚机使用远端内存3:1场景VS虚机全部使用本地内存，使用membench测试性能劣化<5%。</li><li>虚机使用远端内存3:1场景下虚机应用（Redis，MySQL，UnixBench、Nginx，Tomcat）性能劣化<5%。</li><li>容器MySQL场景（详见需求描述）。</li></ol>|
|可管理的最大进程数量|100（容器场景为300个进程）|-|

## 迁移周期和扫描周期配置

在SMAP用户态初始化完成后，会在/etc/smap/目录下生成period.config配置文件。

>[!NOTE] 说明 
>若之前已生成或手动生成period.config配置文件，则初始化完成后不会覆盖，将继续沿用之前生成的配置文件。

### 配置文件说明

初始配置文件内容如下：

```shell
smap.scan.period = 200
smap.migrate.period = 12000
smap.remote.freq.percentile = 99
smap.slow.threshold = 2
smap.freq.wt = 0
smap.remote.hot.threshold = 65535
smap.group.swap.ratio = 1
smap.group.swap.min.remote.freq = 0
smap.group.swap.min.freq.gain = 0
smap.zero.freq.migrate.enable = true
smap.adaptive.ratio.enable = true
smap.period.file.config.switch = false
```

配置文件说明如[表1](#table1)所示。

**表 1**  配置参数说明<a id="table1"></a>

|序号|参数|取值|说明|
|--|--|--|--|
|1|smap.scan.period|默认值：200<br>单位：ms<br>取值范围：[50,60000]<br>参数配置必须是50的倍数。|扫描周期。|
|2|smap.migrate.period|默认值：2000<br>单位：ms<br>取值范围：[500,60000]<br>迁移周期不能小于扫描周期。|迁移周期。|
|3|smap.remote.freq.percentile|百分比。<br>默认值：99<br>取值范围：[1,100]|远端热页最大频次取值百分比。|
|4|smap.slow.threshold|默认值：2<br>取值范围：[0,40]|冷热页面判定阈值。|
|5|smap.freq.wt|默认值：0<br>取值范围：[0,65535]|冷热比较时本地页面缩放因子。|
|6|smap.remote.hot.threshold|默认值：65535<br> 取值范围：[1,65535]|远端热页频次阈值，达到阈值的页一定会交换。|
|7|smap.group.swap.ratio|默认值：1<br>取值范围：[0,10]|大虚机场景，一次迁移的量的比例最大值。|
|8|smap.group.swap.min.remote.freq|默认值：0<br>取值范围：[0,65535]|大虚机场景，远端参与冷热交换的页面最小频次|
|9|smap.group.swap.min.freq.gain|默认值：0<br>取值范围：[0,65535]|大虚机场景，远端频次 > 本地页面频次 + gain，满足条件的页面才能进行交换。|
|10|smap.zero.freq.migrate.enable|默认值：true<br>取值范围：<br>- false<br>- true|虚机场景，是否交换本地0页与远端有频次的页面。|
|11|smap.adaptive.ratio.enable|默认值：true<br>取值范围：<br>- false<br>- true|虚机场景自适应调整虚机内存比例的开关。|
|12|smap.period.file.config.switch|默认值：false<br>取值范围：<br>- false：系统采用算法配置周期。<br>- true：使用配置文件配置的周期。|配置周期开关。|

> [!NOTE] 说明 
>
> - 如果配置文件中缺任何一项，或者有一项出现配置错误，此处配置将不会生效，系统将会沿用上一次有效值（如果配置过，未配置将会用上述中的默认配置）。
> - 在不同场景下推荐调整配置以达到最佳效果：对于页面数量少的进程如果想要提升扫描精度，可以适当增大每次迁移的扫描次数，通过减小扫描周期并增大迁移周期来实现；对于页面数量多的进程，为了减少每次迁移的扫描开销，减少每次迁移周期的扫描次数，推荐增大扫描周期并减小迁移周期，例如可以进行如下配置：
> 
>    smap.scan.period = 200<br>
>    smap.migrate.period = 2000

### 参数约束

迁移周期和扫描周期可配置值范围如下：

```configure
50 <= smap.scan.period <= 60000
500 <= smap.migrate.period <= 60000
1 <= smap.remote.freq.percentile <= 100
0 <= smap.slow.threshold <= 40
0 <= smap.freq.wt <= 65535
```

扫描周期配置必须是50的倍数，迁移周期不能小于扫描周期。
