obj-m := simplefs.o
simplefs-y := src/simplefs/simplefs.o \
              src/simplefs/simplefs_ioctl.o \
              src/simplefs/mount_ops.o

PWD := $(shell pwd)
KERNEL_DIR ?= /lib/modules/$(shell uname -r)/build

CFLAGS = -I$(PWD)/include
CLIENT = simplefs_cli
CLIENT_SRC = src/cli/simplefs_cli.c

all: modules client

modules:
	$(MAKE) -C $(KERNEL_DIR) M=$(PWD) modules

client: $(CLIENT_SRC)
	gcc $(CFLAGS) -o $(CLIENT) $(CLIENT_SRC)

clean:
	$(MAKE) -C $(KERNEL_DIR) M=$(PWD) clean
	rm -f $(CLIENT)

load:
	sudo insmod simplefs.ko device_name=/dev/loop0 \
		sb_main_sector=0 sb_backup_sector=1024 \
		max_filename_len=64 max_file_sectors=16

unload:
	sudo rmmod simplefs

mount:
	sudo mkdir -p /mnt/simplefs
	sudo mount -t simplefs /dev/loop0 /mnt/simplefs -o format

umount:
	sudo umount /mnt/simplefs

.PHONY: all modules client clean load unload mount umount