#ifndef SIMPLEFS_IOCTL_H
#define SIMPLEFS_IOCTL_H

#include <linux/ioctl.h>
#include <linux/types.h>

struct metadata_query {
    __u64 entries_ptr;
    __u32 offset;
    __u32 capacity;
    __u32 count;
};

struct map_query {
    char name[255];
    __u32 sectors_capacity;
    __u64 sectors_ptr;
    __u64 response_ptr;
};

struct info_response {
    __u32 file_count;
};

struct map_response {
    __u64 start_sector;
    __u64 sector_count;
    __u64 size;
    __u64 length;
};

struct metadata_entry {
    char name[255];
    __u64 offset;
    __u64 size;
    __u32 hash;
};

#define SIMPLEFS_IOCTL_MAGIC 'i'

#define SIMPLEFS_IOCTL_ZERO     _IO(SIMPLEFS_IOCTL_MAGIC, 1)
#define SIMPLEFS_IOCTL_ERASE    _IO(SIMPLEFS_IOCTL_MAGIC, 2)
#define SIMPLEFS_IOCTL_METADATA _IOWR(SIMPLEFS_IOCTL_MAGIC, 3, struct metadata_query)
#define SIMPLEFS_IOCTL_MAP      _IOWR(SIMPLEFS_IOCTL_MAGIC, 4, struct map_query)
#define SIMPLEFS_IOCTL_INFO     _IOR(SIMPLEFS_IOCTL_MAGIC, 5, struct info_response)

#endif