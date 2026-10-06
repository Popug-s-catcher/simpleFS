#ifndef SIMPLEFS_H
#define SIMPLEFS_H

#include <linux/fs.h>
#include <linux/types.h>
#include <linux/seq_file.h>

#define MODULE_NAME         "simplefs"
#define SIMPLEFS_MAGIC      0xABCD1234
#define SIMPLEFS_VERSION    1
#define SIMPLEFS_SECTOR_SIZE 512
#define SIMPLEFS_ROOT_INO   1
#define SIMPLEFS_FIRST_FILE_INO 2

#define FILE_MAX_NAME       255
#define FILE_PREFIX         "file"

struct simplefs_mount_opts {
    bool format;
};

struct simplefs_superblock {
    __le32 magic;
    __le32 version;
    __le32 checksum;

    __le64 total_sectors;
    __le64 main_sb;
    __le64 backup_sb;

    __le32 file_sectors;
    __le32 file_count;
    __le32 max_filename_len;
    __le32 filename_width;
} __attribute__((packed));

struct simplefs_info {
    sector_t total_sectors;
    sector_t main_sb;
    sector_t backup_sb;

    u32 max_filename_len;
    u32 file_sectors;
    u32 file_count;
    u32 filename_width;

    bool erased;
    struct simplefs_mount_opts mops;
};

struct simplefs_info *get_simplefs_info(const struct super_block *);
sector_t get_file_sector(const struct simplefs_info *, u32, u32);
void format_filename(const struct simplefs_info *, u32, char *, size_t);
int lookup_filename(const struct simplefs_info *, const char *, size_t);
int clear_sector(struct super_block *, sector_t);
int zero_all_files(struct super_block *);

int parse_mount_ops(char *, struct simplefs_mount_opts *);
void show_ops(struct seq_file *, struct simplefs_mount_opts);

#endif