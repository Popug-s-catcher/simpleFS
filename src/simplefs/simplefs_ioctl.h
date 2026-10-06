#ifndef SIMPLEFS_IOCTL_INTERNAL_H
#define SIMPLEFS_IOCTL_INTERNAL_H

#include <linux/types.h>
#include <linux/fs.h>

long ioctl_handler(struct file *, unsigned int, unsigned long);

#endif