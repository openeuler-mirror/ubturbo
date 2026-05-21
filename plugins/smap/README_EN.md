# Project Introduction

SMAP migrates page data based on the local spatiotemporal characteristics of application access patterns, combined with the hardware-assisted hot and cold data identification capabilities provided by the chip (the chip must support hot and cold data identification for remote memory). Hot data is stored in local memory while cold data is placed in remote memory, thereby delivering end-to-end performance acceleration for applications.

- Hot and cold data identification: The Linux kernel page table and the 1650 chip are used to identify hot and cold memory pages.
- Memory page migration: Pages to be migrated are determined based on the number of page access times, and then migration is performed.
- Process resource allocation: The proportion of borrowed memory used by each process is adjusted based on the process-level memory usage and CPU load.

## Project Architecture

![Project architecture](./doc/images/SMAP_ARCHITECTURE_EN.png)

Key technologies and solutions:

1. Hot and cold data identification

   By identifying the hot and cold information of memory pages and migrating hot pages to the local memory, the memory subsystem can be improved in terms of capacity, latency, and bandwidth at the system level. This project uses software-hardware collaborative design to improve the feasibility, reliability, and flexibility of the hot and cold data identification solution. The SMAP project developed hardware-based hot and cold data identification technologies. The hardware-based hot data identification technology has been incorporated into the UB union die of the 1650 chip; the cold data identification technology is currently implemented using software based on the Linux kernel page table and is planned to be incorporated into the subsequent 1650 version. Hardware is used to identify the hottest pages in the remote memory, which is more challenging, while software is used to identify the coldest pages in the local memory, which is less challenging.

   - Software-based cold data identification: It is known that the ARM CPU can automatically set the Access Flag (AF) for page table entries (PTEs) that have been accessed by processes. Therefore, the access status of a memory page can be obtained by reading the AF of the PTE.
   - Hardware-based hot data identification: When the CPU delivers a memory access request, the hot data identification module uses a statistical array to measure the number of times the address of the accessed page is accessed, thereby obtaining the page's popularity information. As the scanning range of the hot data identification module is limited in a single scan, the result of remote memory scanning using software is referenced before the hot data identification module performs scanning to improve its scanning efficiency.

   ![ARCH](./doc/images/ARCH_EN.png)

   As shown in the preceding figure, the device driver creation and working processes related to hot and cold data identification are as follows:

   1. In the initialization phase, to decouple the core service layer from the device layer, the control character device and the underlying software and hardware tracking device are created.
   2. The upper-layer management module delivers a scanning start command to the corresponding node device.
   3. The node device delivers the command to the software and hardware control device, which allows user-mode programs to operate without sensing specific software and hardware devices.
   4. The software and hardware control device enables software scanning and hardware scanning based on the configuration.
   5. After software scanning and hardware scanning are complete, the control device sorts the results by node and mounts them to the node character device.
   6. The upper-layer management module reads the scanning result from the character device.

2. Data migration

   Data migration is mainly implemented by the data migration policy module and page migration module. Two key points are involved: how to determine the pages to be migrated and how to design the migration interface. The data migration policy module selects cold pages in the local memory and hot pages in the remote memory based on the number of page accesses and the sizes of the local and remote memory allocated to a single process. Then, the module sends the physical addresses of these pages and the destination node for migration to the page migration module, which then performs the migration.

   ![ARCH2](./doc/images/ARCH2_EN.png)

   As shown in the preceding figure, data migration involves the kernel-mode scanning and migration subsystem and the user-mode management and policy subsystem. The working process is as follows:

   1. SMAP automatically identifies VMs or external VMs whose memory is managed by SMAP.
   2. The sampling module measures the number of access times of local and remote memory pages.
   3. The memory access information module sorts the collected memory access information and provides an interface for the user space.
   4. When the migration period arrives, the policy module instructs the management module to collect memory access data.
   5. The hot and cold information management module calls an interface to obtain the memory page access data of each VM.
   6. The management module returns the memory access and resource usage data to the policy module.
   7. The resource allocation policy module determines the allocation of local and remote memory resources for each VM based on the resource usage of each VM.
   8. The data migration policy module identifies the cold pages in the local memory and hot pages in the remote memory based on the resource allocation and memory access data of each VM, and sends the specific page migration data to the kernel management module to perform hot and cold page exchange.
   9. If the memory to be migrated back to the remote memory is specified, the specific address segment is sent to the kernel management module.
   10. The migration management module calls the kernel interface to complete page migration.

3. Process resource allocation

   The following figure shows the allocation of borrowed memory resources in a single server. This function is implemented by the following three modules:

   1. Resource management module: Monitors VM-level information such as the footprint and the size of the hot and cold pages on a node, and provides an interface for obtaining the information for the user space.
   2. Resource allocation policy module: Allocates the borrowed memory among VMs on the local server and provides a VM-level local-remote memory ratio configuration interface to obtain the VM-level SLA Flavour.
   3. Page migration module: Uses the page migration technology to achieve the expected ratio based on the VM-level local-remote memory ratio target.

   Optionally, the resource management module periodically obtains information such as whether the CPU of the current VM is idle and whether an I/O bottleneck occurs at the OS layer and pipeline layer. A hierarchical policy design is used to decouple borrowed memory allocation within a single server and hot and cold page exchange within a VM. In addition, per-VM memory ratio configuration is allowed to maintain system flexibility and scalability, supporting the adjustment of VM-level local-remote memory resource ratios for external interconnection.

   ![Process resource allocation](./doc/images/RESOURCE_ALLOCATE_EN.png)

## Application Adaptation Method

- Enable ACPI.
- Run the following commands to disable NUMA balancing, transparent huge pages, and other functions:

  ```shell
  echo 0 > /proc/sys/kernel/numa_balancing
  echo 0 > /proc/sys/vm/compaction_proactiveness
  echo never > /sys/kernel/mm/transparent_hugepage/defrag
  echo never > /sys/kernel/mm/transparent_hugepage/enabled
  ```

- Install the libvirt and numactl libraries.
- Create the ubturbo user and user group, and install URMA, OBMM, and UBTurbo.

## How to Contribute Code

### Code Submission

- Fork the main repository to your personal repository.
- You can modify, add, and submit code normally.
- Create a merge request to the master branch of the main repository.

## Contact Us

If you have any comments, suggestions, or questions during the development and use of our project, you can:

- Create an issue.
- Contact us: To be added.
