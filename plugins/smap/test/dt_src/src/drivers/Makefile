KERNELDIR ?= /lib/modules/$(shell uname -r)/build
PWD       := $(shell pwd)

obj-m := smap_tracking_core.o
obj-m += smap_access_tracking.o
obj-m += smap_histogram_tracking.o

smap_tracking_core-objs := core.o
smap_tracking_core-objs += bus.o
smap_access_tracking-objs := access_iomem.o access_acpi_helper.o access_acpi_mem.o hist_ops.o hist_tracking.o accessed_bit.o access_ioctl.o access_pid.o access_mmu.o access_tracking.o kvm_pgtable.o access_tracking_wrapper.o
smap_histogram_tracking-objs := ub_hist.o

ccflags-y += -DACPI_NOT_READY

ifeq ($(KERNEL_VERSION), ocos)
    ccflags-y += -DKERNEL_OCOS
else ifeq ($(KERNEL_VERSION), velinux)
    ccflags-y += -DKERNEL_VELINUX
else
    ccflags-y += -DKERNEL_OPENEULER
endif

.PHONY: modules clean
.default: modules

modules:
	$(MAKE) -C  $(KERNELDIR) M=$(PWD) modules

clean:
	rm -rf *.o *~ core .depend .*.cmd *.mod.c .tmp_versions module* Module* *.mod *.d *.ko .*.d
