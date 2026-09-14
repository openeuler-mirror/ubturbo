KERNELDIR ?= /lib/modules/$(shell uname -r)/build
PWD       := $(shell pwd)

obj-m := tracking-core.o
obj-m += access-tracking.o
obj-m += hist-tracking.o

tracking-core-objs := core.o
tracking-core-objs += bus.o
access-tracking-objs := access_iomem.o access_acpi_helper.o access_acpi_mem.o hist_ops.o hist_tracking.o accessed_bit.o access_ioctl.o access_pid.o access_mmu.o access_tracking.o kvm_pgtable.o access_tracking_wrapper.o
hist-tracking-objs := ub_hist.o

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
