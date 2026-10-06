#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/crc32.h>
#include <linux/fs.h>
#include <linux/buffer_head.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include "simplefs.h"
#include "../../include/simplefs_ioctl.h"
#include "simplefs_ioctl.h"

static int compute_file_crc32(struct super_block *sb, u32 file_idx, u32 *crc_out)
{
    struct simplefs_info *fsi = get_simplefs_info(sb);
    struct buffer_head *bh;
    u32 crc = 0;

    for (u32 i = 0; i < fsi->file_sectors; i++) {
        bh = sb_bread(sb, get_file_sector(fsi, file_idx, i));
        if (!bh)
            return -EIO;
        crc = crc32_le(crc, bh->b_data, sb->s_blocksize);
        brelse(bh);
    }
    *crc_out = crc;
    return 0;
}

static int erase_ioctl(struct super_block *sb)
{
    struct simplefs_info *fsi = sb->s_fs_info;
    struct buffer_head *bh;

    bh = sb_bread(sb, fsi->main_sb);
    if (bh) {
        memset(bh->b_data, 0, SIMPLEFS_SECTOR_SIZE);
        mark_buffer_dirty(bh);
        sync_dirty_buffer(bh);
        brelse(bh);
    }
    bh = sb_bread(sb, fsi->backup_sb);
    if (bh) {
        memset(bh->b_data, 0, SIMPLEFS_SECTOR_SIZE);
        mark_buffer_dirty(bh);
        sync_dirty_buffer(bh);
        brelse(bh);
    }

    sb->s_flags |= SB_RDONLY;
    fsi->erased = true;
    printk(KERN_INFO "SimpleFS: erased; remount required\n");
    return 0;
}

static int metadata_ioctl(struct super_block *sb, void __user *argp)
{
    struct simplefs_info *fsi = get_simplefs_info(sb);
    struct metadata_query query;
    struct metadata_entry *entries = NULL;
    u32 count;
    int ret = 0;

    if (copy_from_user(&query, argp, sizeof(query)))
        return -EFAULT;

    if (query.offset >= fsi->file_count) {
        query.count = 0;
        if (copy_to_user(argp, &query, sizeof(query)))
            return -EFAULT;
        return 0;
    }

    count = min(query.capacity, fsi->file_count - query.offset);

    if (count > 0) {
        entries = kmalloc_array(count, sizeof(*entries), GFP_KERNEL);
        if (!entries)
            return -ENOMEM;
    }

    for (u32 i = 0; i < count; i++) {
        u32 file_idx = query.offset + i;
        u32 crc;

        ret = compute_file_crc32(sb, file_idx, &crc);
        if (ret) {
            kfree(entries);
            return ret;
        }
        format_filename(fsi, file_idx, entries[i].name,
                        sizeof(entries[i].name));
        entries[i].offset = get_file_sector(fsi, file_idx, 0);
        entries[i].size   = (u64)fsi->file_sectors * sb->s_blocksize;
        entries[i].hash   = crc;
    }

    if (count > 0 &&
        copy_to_user((void __user *)(unsigned long)query.entries_ptr,
                     entries, count * sizeof(*entries))) {
        kfree(entries);
        return -EFAULT;
    }

    query.count = count;
    if (copy_to_user(argp, &query, sizeof(query))) {
        kfree(entries);
        return -EFAULT;
    }
    kfree(entries);
    return 0;
}

static int map_ioctl(struct super_block *sb, void __user *argp)
{
    struct simplefs_info *fsi = get_simplefs_info(sb);
    struct map_query query;
    struct map_response response = {0};
    u64 *sectors = NULL;
    int file_idx;

    if (copy_from_user(&query, argp, sizeof(query)))
        return -EFAULT;
    query.name[sizeof(query.name) - 1] = '\0';

    file_idx = lookup_filename(fsi, query.name, strlen(query.name));
    if (file_idx < 0)
        return file_idx;

    response.start_sector = get_file_sector(fsi, file_idx, 0);
    response.sector_count = fsi->file_sectors;
    response.size         = (u64)fsi->file_sectors * sb->s_blocksize;
    response.length       = min(fsi->file_sectors, query.sectors_capacity);

    if (copy_to_user((void __user *)(unsigned long)query.response_ptr,
                     &response, sizeof(response)))
        return -EFAULT;

    sectors = kmalloc_array(response.length, sizeof(u64), GFP_KERNEL);
    if (!sectors)
        return -ENOMEM;

    for (u32 i = 0; i < response.length; i++)
        sectors[i] = get_file_sector(fsi, file_idx, i);

    if (copy_to_user((void __user *)(unsigned long)query.sectors_ptr,
                     sectors, response.length * sizeof(u64))) {
        kfree(sectors);
        return -EFAULT;
    }
    kfree(sectors);
    return 0;
}

static int info_ioctl(struct super_block *sb, void __user *argp)
{
    struct simplefs_info *fsi = get_simplefs_info(sb);
    struct info_response response = {
        .file_count = fsi->file_count,
    };
    if (copy_to_user(argp, &response, sizeof(response)))
        return -EFAULT;
    return 0;
}

long ioctl_handler(struct file *file, unsigned int cmd, unsigned long arg)
{
    struct inode *inode = file_inode(file);
    struct super_block *sb = inode->i_sb;
    struct simplefs_info *fsi = get_simplefs_info(sb);
    void __user *argp = (void __user *)arg;

    if (fsi->erased) {
        printk(KERN_INFO "SimpleFS: ioctl rejected (erased)\n");
        return -EIO;
    }

    switch (cmd) {
    case SIMPLEFS_IOCTL_ZERO:
        return zero_all_files(sb);
    case SIMPLEFS_IOCTL_ERASE:
        return erase_ioctl(sb);
    case SIMPLEFS_IOCTL_METADATA:
        return metadata_ioctl(sb, argp);
    case SIMPLEFS_IOCTL_MAP:
        return map_ioctl(sb, argp);
    case SIMPLEFS_IOCTL_INFO:
        return info_ioctl(sb, argp);
    default:
        return -ENOTTY;
    }
}