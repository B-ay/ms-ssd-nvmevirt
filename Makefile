KERNELDIR := /lib/modules/$(shell uname -r)/build
PWD       := $(shell pwd)
INSTALL_MOD_PATH :=

include Makefile.local

default:
		$(MAKE) -C $(KERNELDIR) M=$(PWD) modules

install:
		$(MAKE) INSTALL_MOD_PATH="$(INSTALL_MOD_PATH)" -C $(KERNELDIR) modules_install

insert:
		sudo insmod ./nvmev.ko memmap_start=16G memmap_size=32G cpus=7,8

remove:
		sudo rmmod nvmev

dmesg:
		sudo dmesg | tail -n 20

# 编译用户空间程序
user_program: user_program.c
		gcc -o user_program user_program.c

# 编译测试程序
user_test: ssd_mmap_test.c
		gcc -o ssd_mmap_test ssd_mmap_test.c

# 运行用户空间程序
run_read:
		sudo ./user_program

run_write:
		sudo ./user_program "hello"

# 运行测试程序
run_test:
		sudo ./ssd_mmap_test

.PHONY: clean
clean:
	   $(MAKE) -C $(KERNELDIR) M=$(PWD) clean
	   rm -f cscope.out tags nvmev.S

.PHONY: cscope
cscope:
		cscope -b -R
		ctags *.[ch]

.PHONY: tags
tags: cscope

.PHONY: format
format:
	clang-format -i *.[ch]

.PHONY: dis
dis:
	objdump -d -S nvmev.ko > nvmev.S
