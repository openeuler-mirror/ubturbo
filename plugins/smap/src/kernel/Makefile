KERNELDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

obj-m := smap.o

smap-y := main.o
smap-y += comm/acpi_helper.o comm/acpi_mem.o comm/iomem.o
smap-y += scan/hist_ops.o scan/hist_tracking.o \
	scan/accessed_bit.o scan/scan_ioctl.o scan/access_pid.o \
	scan/access_mmu.o scan/scan_main.o scan/kvm_pgtable.o \
	scan/access_tracking_wrapper.o scan/ub_hist.o
smap-y += migrate/rmap.o migrate/ksm.o migrate/smap_migrate_wrapper.o \
	migrate/smap_migrate_pages.o migrate/migrate_task.o migrate/migrate_back_ioctl.o \
	migrate/migrate_back_debugfs.o migrate/work.o migrate/numa.o \
	migrate/migrate_dev.o migrate/migrate_main.o migrate/smap_debugfs.o \
	migrate/dump_info.o migrate/obmm_monitor.o

ifeq ($(filter $(KERNEL_VERSION),ocos velinux),)
smap-y += migrate/ham_migration.o migrate/basic.o \
	migrate/coherence_maintain.o migrate/ham_tasks_mgr.o
endif

ccflags-y += -I$(src)/comm -I$(src)/..
ifeq ($(KERNEL_VERSION),ocos)
	ccflags-y += -DKERNEL_OCOS
else ifeq ($(KERNEL_VERSION),velinux)
	ccflags-y += -DKERNEL_VELINUX
else
	ccflags-y += -DHAM_ENABLED -DKERNEL_OPENEULER
endif

.PHONY: modules clean
default: modules

modules:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) clean
