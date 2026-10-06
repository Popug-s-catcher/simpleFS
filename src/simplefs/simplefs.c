#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/crc32.h>
#include <linux/fs.h>
#include <linux/fs_context.h>
#include <linux/blkdev.h>
#include <linux/buffer_head.h>
#include <linux/slab.h>
#include <linux/minmax.h>

#include "simplefs.h"
#include "simplefs_ioctl.h"

static char *device_name;
static ulong sb_main_sector = 0;
static ulong sb_backup_sector = 0;
static uint max_filename_len = 64;
static uint max_file_sectors = 8;

module_param(device_name, charp, 0444);
module_param(sb_main_sector, ulong, 0444);
module_param(sb_backup_sector, ulong, 0444);
module_param(max_filename_len, uint, 0444);
module_param(max_file_sectors, uint, 0444);


static struct inode *get_inode(struct super_block *sb, unsigned long ino);

struct simplefs_info *get_simplefs_info(const struct super_block *sb)
{
    return sb->s_fs_info;
}

void format_filename(const struct simplefs_info *fsi, u32 idx,
                     char *buf, size_t len)
{
    snprintf(buf, len, FILE_PREFIX "%0*u", fsi->filename_width, idx);
}

static u32 digits_count(u32 num)
{
    u32 digits = 1;
    while (num >= 10) {
        num /= 10;
        digits++;
    }
    return digits;
}

sector_t get_file_sector(const struct simplefs_info *fsi,
                         u32 file_idx, u32 sector_idx)
{
    sector_t sector = (sector_t)file_idx * fsi->file_sectors + sector_idx;
    sector_t first_sb  = min(fsi->main_sb, fsi->backup_sb);
    sector_t second_sb = max(fsi->main_sb, fsi->backup_sb);

    if (sector >= first_sb)
        sector++;
    if (sector >= second_sb)
        sector++;

    return sector;
}

int lookup_filename(const struct simplefs_info *fsi,
                    const char *name, size_t len)
{
    char generated[FILE_MAX_NAME];

    if (fsi->erased)
        return -ENOENT;

    for (u32 i = 0; i < fsi->file_count; i++) {
        format_filename(fsi, i, generated, sizeof(generated));
        if (strlen(generated) == len && !strncmp(name, generated, len))
            return i;
    }
    return -ENOENT;
}


int clear_sector(struct super_block *sb, sector_t sector)
{
    struct buffer_head *bh = sb_getblk(sb, sector);
    if (!bh)
        return -EIO;

    lock_buffer(bh);
    memset(bh->b_data, 0, sb->s_blocksize);
    set_buffer_uptodate(bh);
    unlock_buffer(bh);

    mark_buffer_dirty(bh);
    sync_dirty_buffer(bh);

    if (buffer_write_io_error(bh)) {
        brelse(bh);
        return -EIO;
    }
    brelse(bh);
    return 0;
}

int zero_all_files(struct super_block *sb)
{
    struct simplefs_info *fsi = get_simplefs_info(sb);

    for (u32 i = 0; i < fsi->file_count; i++) {
        for (u32 j = 0; j < fsi->file_sectors; j++) {
            int ret = clear_sector(sb, get_file_sector(fsi, i, j));
            if (ret)
                return ret;
        }
    }
    return 0;
}


static u32 calc_superblock_crc(struct simplefs_superblock *sb)
{
    __le32 saved = sb->checksum;
    u32 crc;

    sb->checksum = 0;
    crc = crc32_le(~0, (unsigned char *)sb, sizeof(*sb));
    sb->checksum = saved;

    return crc;
}

static int validate_superblock(const struct simplefs_info *fsi,
                               struct simplefs_superblock *ondisk)
{
    if (le32_to_cpu(ondisk->magic) != SIMPLEFS_MAGIC)
        return -EINVAL;
    if (le64_to_cpu(ondisk->main_sb) != fsi->main_sb ||
        le64_to_cpu(ondisk->backup_sb) != fsi->backup_sb)
        return -EINVAL;
    if (calc_superblock_crc(ondisk) != le32_to_cpu(ondisk->checksum))
        return -EUCLEAN;
    return 0;
}

static void build_superblock(struct simplefs_info *fsi,
                             struct simplefs_superblock *ondisk)
{
    memset(ondisk, 0, sizeof(*ondisk));
    ondisk->magic            = cpu_to_le32(SIMPLEFS_MAGIC);
    ondisk->version          = cpu_to_le32(SIMPLEFS_VERSION);
    ondisk->total_sectors    = cpu_to_le64(fsi->total_sectors);
    ondisk->main_sb          = cpu_to_le64(fsi->main_sb);
    ondisk->backup_sb        = cpu_to_le64(fsi->backup_sb);
    ondisk->file_sectors     = cpu_to_le32(fsi->file_sectors);
    ondisk->file_count       = cpu_to_le32(fsi->file_count);
    ondisk->max_filename_len = cpu_to_le32(fsi->max_filename_len);
    ondisk->filename_width   = cpu_to_le32(fsi->filename_width);
    ondisk->checksum         = cpu_to_le32(calc_superblock_crc(ondisk));
}

static void fill_info_from_disk(struct simplefs_info *fsi,
                                struct simplefs_superblock *ondisk)
{
    fsi->total_sectors    = le64_to_cpu(ondisk->total_sectors);
    fsi->main_sb          = le64_to_cpu(ondisk->main_sb);
    fsi->backup_sb        = le64_to_cpu(ondisk->backup_sb);
    fsi->max_filename_len = le32_to_cpu(ondisk->max_filename_len);
    fsi->file_sectors     = le32_to_cpu(ondisk->file_sectors);
    fsi->file_count       = le32_to_cpu(ondisk->file_count);
    fsi->filename_width   = digits_count(max(1, fsi->file_count - 1));
}

static int write_superblock_copy(struct super_block *sb, sector_t sector,
                                 struct simplefs_superblock *ondisk)
{
    struct buffer_head *bh = sb_getblk(sb, sector);
    if (!bh)
        return -EIO;

    lock_buffer(bh);
    memset(bh->b_data, 0, sb->s_blocksize);
    memcpy(bh->b_data, ondisk, sizeof(*ondisk));
    set_buffer_uptodate(bh);
    unlock_buffer(bh);

    mark_buffer_dirty(bh);
    sync_dirty_buffer(bh);

    int ret = buffer_write_io_error(bh) ? -EIO : 0;
    brelse(bh);
    return ret;
}

static int write_superblocks(struct super_block *sb, struct simplefs_info *fsi)
{
    struct simplefs_superblock ondisk;
    int ret;

    build_superblock(fsi, &ondisk);
    ret = write_superblock_copy(sb, fsi->main_sb, &ondisk);
    if (ret)
        return ret;
    return write_superblock_copy(sb, fsi->backup_sb, &ondisk);
}

static int init_layout(struct super_block *sb, struct simplefs_info *fsi)
{
    sector_t total     = fsi->total_sectors;
    sector_t reserved  = 2;
    sector_t usable    = total - reserved;
    u32 file_sectors   = fsi->file_sectors;

    while (file_sectors > 1 && usable % file_sectors)
        file_sectors--;

    if (file_sectors < 1)
        return -ENOSPC;

    fsi->file_sectors  = file_sectors;
    fsi->file_count    = usable / file_sectors;
    fsi->filename_width = digits_count(max(1, fsi->file_count - 1));
    return 0;
}

static int check_device_name(struct super_block *sb)
{
    const char *actual   = sb->s_bdev->bd_disk->disk_name;
    const char *expected = device_name;

    if (!expected || !*expected)
        return -EINVAL;
    if (!strcmp(actual, expected))
        return 0;
    if (!strncmp(expected, "/dev/", 5) && !strcmp(expected + 5, actual))
        return 0;
    return -EINVAL;
}

static int init_super(struct super_block *sb, struct simplefs_info *fsi)
{
    int ret = 0;
    struct buffer_head *bh1 = sb_bread(sb, fsi->main_sb);
    struct buffer_head *bh2 = sb_bread(sb, fsi->backup_sb);

    if (!bh1 || !bh2) {
        ret = -EIO;
        goto out;
    }

    if (fsi->mops.format) {
        printk(KERN_INFO "SimpleFS: format new FS\n");
        init_layout(sb, fsi);
        ret = write_superblocks(sb, fsi);
        if (ret)
            goto out;
        zero_all_files(sb);
        goto out;
    }

    struct simplefs_superblock *primary = (void *)bh1->b_data;
    struct simplefs_superblock *backup  = (void *)bh2->b_data;

    int valid1 = !validate_superblock(fsi, primary);
    int valid2 = !validate_superblock(fsi, backup);

    if (valid1) {
        printk(KERN_INFO "SimpleFS: using primary SB\n");
        fill_info_from_disk(fsi, primary);
        if (!valid2 || memcmp(primary, backup, sizeof(*primary)) != 0) {
            printk(KERN_INFO "SimpleFS: restoring backup\n");
            ret = write_superblock_copy(sb, fsi->backup_sb, primary);
        }
    } else if (valid2) {
        printk(KERN_INFO "SimpleFS: using backup SB\n");
        fill_info_from_disk(fsi, backup);
        ret = write_superblock_copy(sb, fsi->main_sb, backup);
    } else {
        printk(KERN_ERR "SimpleFS: both SB invalid; use -o format\n");
        ret = -EUCLEAN;
    }

out:
    brelse(bh1);
    brelse(bh2);
    return ret;
}


static int dir_iterate(struct file *filep, struct dir_context *ctx)
{
    struct inode *root = file_inode(filep);
    struct simplefs_info *fsi = get_simplefs_info(root->i_sb);
    char name[FILE_MAX_NAME];

    if (!dir_emit_dots(filep, ctx))
        return 0;
    if (fsi->erased)
        return 0;

    u32 idx = ctx->pos - 2;
    while (idx < fsi->file_count) {
        memset(name, 0, sizeof(name));
        format_filename(fsi, idx, name, sizeof(name));
        if (!dir_emit(ctx, name, strlen(name),
                      SIMPLEFS_FIRST_FILE_INO + idx, DT_REG))
            return 0;
        ctx->pos++;
        idx++;
    }
    return 0;
}

static const struct file_operations dir_ops = {
    .owner          = THIS_MODULE,
    .read           = generic_read_dir,
    .iterate_shared = dir_iterate,
    .unlocked_ioctl = ioctl_handler,
};

static struct dentry *lookup(struct inode *dir, struct dentry *dentry,
                             unsigned int flags)
{
    struct simplefs_info *fsi = get_simplefs_info(dir->i_sb);
    struct inode *inode = NULL;

    if (dentry->d_name.len > fsi->max_filename_len)
        return ERR_PTR(-ENAMETOOLONG);

    int file_idx = lookup_filename(fsi, dentry->d_name.name,
                                   dentry->d_name.len);
    if (file_idx >= 0) {
        inode = get_inode(dir->i_sb, SIMPLEFS_FIRST_FILE_INO + file_idx);
        if (IS_ERR(inode))
            return ERR_CAST(inode);
    }
    return d_splice_alias(inode, dentry);
}

static const struct inode_operations dir_iops = {
    .lookup = lookup,
};


static ssize_t file_read(struct file *filp, char __user *buf,
                         size_t len, loff_t *offset)
{
    struct inode *inode = file_inode(filp);
    struct super_block *sb = inode->i_sb;
    struct simplefs_info *fsi = get_simplefs_info(sb);
    loff_t pos = *offset;
    size_t copied = 0;

    if (fsi->erased)
        return -EIO;

    u32 file_idx = inode->i_ino - SIMPLEFS_FIRST_FILE_INO;
    loff_t file_size = (loff_t)fsi->file_sectors * sb->s_blocksize;

    if (pos < 0)
        return -EINVAL;
    if (pos >= file_size)
        return 0;
    if (pos + len > file_size)
        len = file_size - pos;

    while (len > 0) {
        u32 sector_idx = pos / sb->s_blocksize;
        size_t off_in_sec = pos % sb->s_blocksize;
        size_t chunk = min_t(size_t, len, sb->s_blocksize - off_in_sec);
        sector_t physical = get_file_sector(fsi, file_idx, sector_idx);
        struct buffer_head *bh = sb_bread(sb, physical);

        if (!bh)
            return copied ? copied : -EIO;
        if (copy_to_user(buf + copied, bh->b_data + off_in_sec, chunk)) {
            brelse(bh);
            return copied ? copied : -EFAULT;
        }
        brelse(bh);
        copied += chunk;
        pos    += chunk;
        len    -= chunk;
    }
    *offset = pos;
    return copied;
}

static ssize_t file_write(struct file *filp, const char __user *buf,
                          size_t len, loff_t *offset)
{
    struct inode *inode = file_inode(filp);
    struct super_block *sb = inode->i_sb;
    struct simplefs_info *fsi = get_simplefs_info(sb);
    loff_t pos = *offset;
    size_t written = 0;

    if (fsi->erased)
        return -EIO;

    u32 file_idx = inode->i_ino - SIMPLEFS_FIRST_FILE_INO;
    loff_t file_max = (loff_t)fsi->file_sectors * sb->s_blocksize;

    if (pos < 0)
        return -EINVAL;
    if (pos >= file_max)
        return -ENOSPC;
    if (pos + len > file_max)
        len = file_max - pos;

    while (len > 0) {
        u32 sector_idx = pos / sb->s_blocksize;
        size_t off_in_sec = pos % sb->s_blocksize;
        size_t chunk = min_t(size_t, len, sb->s_blocksize - off_in_sec);
        sector_t physical = get_file_sector(fsi, file_idx, sector_idx);
        struct buffer_head *bh = sb_bread(sb, physical);

        if (!bh)
            return written ? written : -EIO;
        if (copy_from_user(bh->b_data + off_in_sec, buf + written, chunk)) {
            brelse(bh);
            return written ? written : -EFAULT;
        }
        mark_buffer_dirty(bh);
        sync_dirty_buffer(bh);
        if (buffer_write_io_error(bh)) {
            brelse(bh);
            return written ? written : -EIO;
        }
        brelse(bh);
        written += chunk;
        pos     += chunk;
        len     -= chunk;
    }

    *offset = pos;
    if (pos > inode->i_size) {
        inode->i_size = pos;
        mark_inode_dirty(inode);
    }
    inode_set_mtime_to_ts(inode, inode_set_ctime_current(inode));
    return written;
}

static loff_t file_llseek(struct file *filep, loff_t off, int whence)
{
    struct inode *inode = file_inode(filep);
    return generic_file_llseek_size(filep, off, whence,
                                    MAX_LFS_FILESIZE, inode->i_size);
}

static const struct file_operations fops = {
    .owner  = THIS_MODULE,
    .llseek = file_llseek,
    .read   = file_read,
    .write  = file_write,
};

static int file_setattr(struct mnt_idmap *idmap, struct dentry *dentry,
                        struct iattr *iattr)
{
    int ret;
    struct inode *inode = d_inode(dentry);

    iattr->ia_valid &= ~ATTR_SIZE;
    ret = setattr_prepare(idmap, dentry, iattr);
    if (ret)
        return ret;

    setattr_copy(idmap, inode, iattr);
    mark_inode_dirty(inode);
    return 0;
}

static const struct inode_operations file_iops = {
    .setattr = file_setattr,
};


static struct inode *get_inode(struct super_block *sb, unsigned long ino)
{
    struct simplefs_info *fsi = get_simplefs_info(sb);
    struct inode *inode = iget_locked(sb, ino);

    if (!inode)
        return ERR_PTR(-ENOMEM);
    if (!(inode->i_state & I_NEW))
        return inode;

    if (ino == SIMPLEFS_ROOT_INO) {
        inode_init_owner(&nop_mnt_idmap, inode, NULL, S_IFDIR | 0555);
        inode->i_op  = &dir_iops;
        inode->i_fop = &dir_ops;
    } else if (ino >= SIMPLEFS_FIRST_FILE_INO &&
               ino < SIMPLEFS_FIRST_FILE_INO + fsi->file_count) {
        inode_init_owner(&nop_mnt_idmap, inode, NULL, S_IFREG | 0666);
        inode->i_size = (loff_t)fsi->file_sectors * sb->s_blocksize;
        inode->i_op  = &file_iops;
        inode->i_fop = &fops;
    } else {
        iget_failed(inode);
        return ERR_PTR(-ESTALE);
    }

    unlock_new_inode(inode);
    return inode;
}


static void put_simplefs_super(struct super_block *sb)
{
    kfree(sb->s_fs_info);
    sb->s_fs_info = NULL;
}

static int simplefs_show_options(struct seq_file *seq, struct dentry *dir)
{
    struct simplefs_info *fsi = get_simplefs_info(dir->d_sb);
    show_ops(seq, fsi->mops);
    return 0;
}

static const struct super_operations super_ops = {
    .put_super     = put_simplefs_super,
    .show_options  = simplefs_show_options,
};


static int simplefs_fill_super(struct super_block *sb, void *data, int silent)
{
    int ret;
    struct simplefs_mount_opts ops = {0};

    ret = parse_mount_ops((char *)data, &ops);
    if (ret)
        return ret;

    if (sb_main_sector == sb_backup_sector ||
        max_file_sectors < 1 ||
        max_filename_len > FILE_MAX_NAME)
        return -EINVAL;

    if (!sb_set_blocksize(sb, SIMPLEFS_SECTOR_SIZE))
        return -EINVAL;

    ret = check_device_name(sb);
    if (ret)
        return ret;

    struct simplefs_info *fsi = kzalloc(sizeof(*fsi), GFP_KERNEL);
    if (!fsi)
        return -ENOMEM;

    fsi->total_sectors    = sb->s_bdev->bd_nr_sectors;
    fsi->main_sb          = sb_main_sector;
    fsi->backup_sb        = sb_backup_sector;
    fsi->max_filename_len = max_filename_len;
    fsi->file_sectors     = max_file_sectors;
    fsi->mops             = ops;

    sb->s_fs_info  = fsi;
    sb->s_op       = &super_ops;
    sb->s_maxbytes = (loff_t)max_file_sectors * sb->s_blocksize;
    sb->s_time_gran = 1;

    ret = init_super(sb, fsi);
    if (ret)
        goto err;

    sb->s_maxbytes = (loff_t)fsi->file_sectors * sb->s_blocksize;

    if (fsi->max_filename_len < fsi->filename_width + strlen(FILE_PREFIX)) {
        printk(KERN_ERR "SimpleFS: max_filename_len too small\n");
        ret = -EINVAL;
        goto err;
    }

    struct inode *root = get_inode(sb, SIMPLEFS_ROOT_INO);
    if (IS_ERR(root)) {
        put_simplefs_super(sb);
        return PTR_ERR(root);
    }

    sb->s_root = d_make_root(root);
    if (!sb->s_root) {
        iput(root);
        ret = -ENOMEM;
        goto err;
    }

    printk(KERN_INFO "SimpleFS: mounted successfully\n");
    return 0;

err:
    put_simplefs_super(sb);
    return ret;
}

static struct dentry *mount_simplefs(struct file_system_type *fs_type,
                                     int flags, const char *dev_name,
                                     void *data)
{
    return mount_bdev(fs_type, flags, dev_name, data, simplefs_fill_super);
}

static struct file_system_type simplefs_fs_type = {
    .owner    = THIS_MODULE,
    .name     = MODULE_NAME,
    .mount    = mount_simplefs,
    .kill_sb  = kill_block_super,
    .fs_flags = FS_REQUIRES_DEV,
};


static int __init simplefs_init(void)
{
    int ret;

    printk(KERN_INFO "SimpleFS: init\n");
    ret = register_filesystem(&simplefs_fs_type);
    if (ret)
        printk(KERN_ERR "SimpleFS: register failed\n");
    return ret;
}

static void __exit simplefs_exit(void)
{
    unregister_filesystem(&simplefs_fs_type);
    printk(KERN_INFO "SimpleFS: exit\n");
}

module_init(simplefs_init);
module_exit(simplefs_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Egory");
MODULE_DESCRIPTION("Homework");