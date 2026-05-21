# RMRS

## Project Introduction

RMRS is an open-source memory migration tool in the form of a plugin of the UBTurbo framework. Working with OBMM and leveraging the underlying SMAP, RMRS provides decision-making and execution capabilities for migrating virtual machines to remote memory and migrating them back.

For example, in virtualization scenarios, if the local NUMA memory is insufficient during virtual machine creation, the local memory of virtual machines can be migrated to remote memory.
In such cases, RMRS determines how many local virtual machines need to be migrated to remote memory and invokes SMAP to perform the migration. RMRS also supports rollback of memory migration when virtual machine creation fails or other rollback scenarios occur. In addition, in scenarios such as when a virtual machine is destroyed, the service side may invoke the memory return function. The RMRS can determine whether all virtual machines on remote NUMA nodes can be migrated back to local memory and perform the migration if needed.

Migration operations are executed by invoking SMAP, which periodically collects statistics on cold and hot memory to ensure controllable performance of applications on virtual machines that access remote memory.

## Directory Structure

```sh
├── doc                             # Documentation
├── src
│   ├── include                     # Global header files
│   └── ubturbo_plugin
│       ├── common                  # Common functions
│       ├── conf                    # Configuration file
│       ├── export                  # Collection module
│       ├── include                 # Header files
│       ├── intranode_strategy      # Migration module
│       ├── message                 # Message module
│       ├── serialization           # Serialization module
│       └── ucache                  # ucache module
```

## Project Architecture

**RMRS layered architecture:**

The **UBS RMRS** is deployed in the UBS Engine. The agent on each compute node is only responsible for data collection and message forwarding. The active node serves as the functional entry and provides interfaces for borrowing, migrating out, returning, and rolling back fragmented memory. It is responsible for managing fragmented memory between nodes.
The **RMRS** is deployed in the UBTurbo of each compute node. Based on its own resource collection, it provides virtual machine migration-out/migration-back policies in fragmentation scenarios and uses the SMAP migration capability to migrate virtual machines out and back.

# Quick Start

## Prerequisites

- Install and start the libvirt service.

  ```bash
  yum install -y qemu*
  yum install -y libvirt*
  systemctl start libvirtd
  ```

- Install and start the UBTurbo service.

- Configure obmm before installing RMRS because RMRS depends on obmm to generate remote NUMA.

- Install SMAP because RMRS requires SMAP's capabilities of memory migration and hot data identification.

## RMRS Compilation

The RMRS plugin is integrated in UBTurbo by default. For details about how to install, deploy, and use the plugin, see [UBTurbo Doc](../../../doc/).

## RMRS Activation

Check whether the following option in the `/opt/ubturbo/conf/ubturbo_plugin_admission.conf` file is enabled:

```bash
# The code value must be greater than 200.
rmrs=777
```
