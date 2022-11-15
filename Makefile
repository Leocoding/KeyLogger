ifneq ($(KERNELRELEASE),)
obj-m := psa_mod.o

else
K_DIR ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(K_DIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(K_DIR) M=$(PWD) clean
	$(RM) *~

endif
