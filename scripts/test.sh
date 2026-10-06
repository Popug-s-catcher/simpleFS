#!/bin/bash

set -e

MODULE_PATH="./simplefs.ko"
CLI_PATH="./simplefs_cli"
MOUNT_POINT="/mnt/simplefs"
DISK="./disk.img"
DEVICE=""

cleanup() {
    echo "Cleaning up..."
    sudo umount $MOUNT_POINT 2>/dev/null || true
    sudo rmmod simplefs 2>/dev/null || true
    sudo losetup -d $DEVICE 2>/dev/null || true
    rm -f $DISK
    make clean
}

trap cleanup EXIT

if [ ! -f "$DISK" ]; then
    echo "Creating disk"
    sudo dd if=/dev/zero of="$DISK" bs=512 count=2050 2>/dev/null
fi

DEVICE=$(sudo losetup -f --show "$DISK")
echo "Loop DEVICE: $DEVICE"

echo "Running build..."
make
echo "Build successfully"

if [ ! -d "$MOUNT_POINT" ]; then
    echo "mount point: $MOUNT_POINT"
    sudo mkdir -p "$MOUNT_POINT"
fi

echo "Loading KERNEL module"
sudo insmod "$MODULE_PATH" device_name="$DEVICE" sb_main_sector=0 sb_backup_sector=1024 max_filename_len=64 max_file_sectors=16

echo "Mounting module"
sudo mount -t simplefs "$DEVICE" "$MOUNT_POINT" -o format

echo "Files:"
ls -la "$MOUNT_POINT"

echo "Test write & read:"
"$CLI_PATH" "$MOUNT_POINT" test

echo "Files metadata:"
"$CLI_PATH" "$MOUNT_POINT" metadata

echo "Mapping of file: file0000"
"$CLI_PATH" "$MOUNT_POINT" map file0000

echo "Zero all files:"
"$CLI_PATH" "$MOUNT_POINT" zero

echo "Files meta after zero:"
"$CLI_PATH" "$MOUNT_POINT" metadata

echo "Erasing FS:"
"$CLI_PATH" "$MOUNT_POINT" erase
