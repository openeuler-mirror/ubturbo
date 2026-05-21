# UBTurbo

## Project Overview

UBTurbo is an open source intra-node resource management framework. It provides configuration reading, plugin loading, log printing, and IPC communication capabilities, and integrates the SMAP capability to provide basic multi-level memory scheduling services.

For example, in virtualization scenarios, the RMRS memory migration tool is developed based on the UBTurbo framework and runs in the UBTurbo process. It provides memory migration decision-making and execution services through the IPC and SMAP capabilities. External processes use the UBTurbo client to send instructions and message flows to the RMRS. The configuration items of the RMRS are stored in the configuration file and can be obtained through the configuration reading function of the UBTurbo. In addition, logs can be printed using the UBTurbo framework.

## Directory Structure

```sh
UBTURBO/
├── 3rdparty                    // Third-party source code library
├── build                       // Project script
├── conf                        // Configuration file
├── doc                         // Documentation
├── include                     // Global header file
├── plugins                     // UBTurbo plugin library
├── src                         
│   ├── include                 // Header file
│   ├── config                  // Configuration module
│   ├── ipc                     // Communication module
│   ├── log                     // Log module
│   ├── main                    // UBTurbo main
│   ├── plugin                  // Plugin module
│   ├── smap                    // SMAP encoding/decoding
│   ├── utils                   // Tool
└── test
    ├── 3rdparty                // Third-party test library
    └── testcase                // Test case
```

## Constraints

- When using UBTurbo to borrow memory, ensure that the security of the source memory address is the same as that of the destination memory address.
- The user permissions of the VMs or containers to be migrated must be the same as those of the remote memory.
- To use UBTurbo, you need to add the user to the UBTurbo owner group. The added user must have the permissions of the node memory resource administrator to use the UBTurbo memory migration functionality. During memory migration, PIDs are managed and delivered by the cluster resource management center. The UBTurbo component cannot verify the validity of PIDs. Therefore, developers need to consider the security of transmission and storage of parameters such as PID, srcNid, and destNid in the overall solution.

## Project Architecture

![UBTURBO_ARCHITECTURE.png](./doc/images/UBTURBO_ARCHITECTURE.png "UBTURBO_ARCHITECTURE")

The **UBTurbo** component provides the following services:

- **UBTurboSDK**: SDK provided by the UBTurbo service. As an independent SDK, it provides the UBTurbo capability for external modules and components through interfaces.
- **Common**: Common component that provides some common capabilities.
  - **Log**: Log module.
  - **Config**: Configuration module that parses the configuration information of the UBTurbo service.
  - **Daemon**: UBTurbo process that provides process services.
- **MessageServer**: Receives requests from the UBTurboSDK to the UBTurboServer through the UDS to enable the acceleration capability.
- **RMRS**: Resource scheduling module that schedules memory resources of VMs and containers.
- **SMAP**: Hierarchical memory enabling module that enables hierarchical memory capabilities through page scanning and migration.

Key technologies and solutions:

1. **Configuration loading**: Reads **ubturbo.conf**, **ubturbo_plugin_admission.conf**, and the configuration file of each plugin from the **/opt/ubturbo/conf** directory.
2. **Plugin loading**: Searches for the **.so** file in the specified directory; uses dlopen to load the plugin and, during plugin uninstallation, uses dlclose to close the dynamic library.
3. **Process communication**: Uses the Unix domain socket mechanism to enable communication between processes on a node and provides connection-oriented reliable data transmission. Using the Reactor pattern, the server starts a thread to listen on a specified socket file. After accepting a client connection, it creates a new thread, calls a specified callback function, and sends the result back to the client.
4. **Log management**:

  - (1) **Asynchronous ring buffer**: An asynchronous ring buffer is used to implement asynchronous logging, preventing the main thread from being blocked.
  - (2) **Lock mechanism**: An appropriate lock mechanism is used to ensure thread security in a multi-thread environment.
  - (3) **Timestamp processing**: The system time function is used to obtain timestamp information.
  - (4) **File operations**: APIs related to file operations are used to write and manage log files. Logs of the UBTurbo framework and each plugin are independent, with the maximum size of each log file being 200 MB. Log files are wrapped and a maximum of 10 files can be stored for each plugin.

# Quick Start

## Prerequisites

- UBTurbo integrates the SMAP functionality. To use such functionality, you need to install SMAP in advance.

- The **/dev/shm/smap_config** file stores Information such as NUMA and process configurations. If the UBTurbo process needs to switch to another user, delete this file first.

- The **/dev/shm/ubturbo_page_type.dat** file stores the SMAP initialization type information. If the UBTurbo process needs to switch to another user or scenario (for example, from a virtualization scenario to a big data scenario), delete this file first.

- By default, no plug-in is enabled for UBTurbo. You can enable the plug-ins by uncommenting them in the **ubturbo_plugin_admission.conf** file based on the service scenario.

- Before enabling a plug-in in the **ubturbo_plugin_admission.conf** file, ensure that it has been installed and configured. Otherwise, UBTurbo and the corresponding plug-in will fail to start.

## UBTurbo Compilation

Run the following commands in the root directory:

```bash
git submodule update --init --recursive
dos2unix build.sh
sh build.sh
```

Compilation products:

- `ub_turbo_exec`, a binary file in the **dist/release/bin** directory

- `libubturbo_client.so`, a library file in the **dist/release/lib** directory

- `ubturbo_plugin_admission.conf` and `ubturbo.conf`, configuration files in the **dist/release/conf** directory

## UBTurbo Running

- Configure the **ubturbo.conf** file for log level control.

| No.| Parameter| Description| Value| Configuration Node| Application Scenario|
|-----|-----|-----|-----|-----|-----|
| 1 | log.level| Log level| Default value: **INFO**. Value range: **DEBUG**, **INFO**, **WARN**, **ERROR**, and **CRIT**| All nodes| Used for determining the log output level of the main process and plug-ins.|

- Run the following commands while maintaining the relative positions of the preceding compilation products:

```bash
chmod +x ub_turbo_exec
./ub_turbo_exec
```
