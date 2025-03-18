
ifneq ($(KERNELRELEASE),)

obj-m  := test_mem.o

else

KDIR ?=/lib/modules/`uname -r`/build
PWD  := $(shell pwd)

default:
	make -C $(KDIR) M=$(PWD) modules

endif
