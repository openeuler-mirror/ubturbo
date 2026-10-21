# HotScan Kernel Module Makefile
# Target: Linux 6.6, ARM64 (aarch64)

obj-m := hotscan.o
hotscan-objs := hotscan_main.o

# Kernel build directory
KERNELDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

.PHONY: all clean

all: hotscan.ko

hotscan.ko: hotscan_main.c Makefile
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) clean
	rm -f *.o *.ko *.mod.c *.mod *.order *.symvers
