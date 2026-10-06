#include <linux/module.h>
#include <linux/fs.h>
#include <linux/parser.h>
#include <linux/seq_file.h>

#include "simplefs.h"

enum {
    OPT_FORMAT,
    OPT_ERR,
};

static const match_table_t tokens = {
    { OPT_FORMAT, "format" },
    { OPT_ERR, NULL }
};

int parse_mount_ops(char *options, struct simplefs_mount_opts *opts)
{
    char *p;
    substring_t args[MAX_OPT_ARGS];
    int token;

    if (!options)
        return 0;

    while ((p = strsep(&options, ",")) != NULL) {
        if (!*p)
            continue;

        token = match_token(p, tokens, args);
        switch (token) {
        case OPT_FORMAT:
            opts->format = true;
            break;
        default:
            printk(KERN_WARNING "SimpleFS: unknown option: %s\n", p);
            return -EINVAL;
        }
    }
    return 0;
}

void show_ops(struct seq_file *seq, struct simplefs_mount_opts opts)
{
    if (opts.format)
        seq_puts(seq, ",format");
}