MODULE_NAME = smap_tiering
$(MODULE_NAME)-objs := rmap.o ksm.o smap_migrate_wrapper.o smap_migrate_pages.o migrate_task.o pid_ioctl.o migrate_back_debugfs.o work.o numa.o acpi_helper.o acpi_mem.o mig_init.o tracking_manage.o smap_debugfs.o  dump_info.o iomem.o

ifeq ($(filter $(KERNEL_VERSION), ocos velinux),)
$(MODULE_NAME)-objs += ham_migration.o basic.o coherence_maintain.o ham_tasks_mgr.o
endif

obj-m   := $(MODULE_NAME).o

KERNELDIR ?= /lib/modules/$(shell uname -r)/build
PWD       := $(shell pwd)
KBUILD_EXTRA_SYMBOLS = $(src)/depends/Module.symvers

ifeq ($(CRITICAL), 0)
	ccflags-y := -DCRITICAL_OFF
endif
ifeq ($(KERNEL_VERSION), ocos)
    ccflags-y += -DKERNEL_OCOS
else ifeq ($(KERNEL_VERSION), velinux)
    ccflags-y += -DKERNEL_VELINUX
else
    ccflags-y += -DHAM_ENABLED
    ccflags-y += -DKERNEL_OPENEULER
endif

.PHONY: modules
default: modules

modules:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) clean
