// SPDX-License-Identifier: GPL-2.0
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <dirent.h>
#include <time.h>

#include "../../include/simplefs_ioctl.h"

#define BATCH_SIZE 1024

static void cli_usage(const char *name)
{
    printf("Usage:\n");
    printf("  %s <mountpoint> test            - write & read random value in every file\n", name);
    printf("  %s <mountpoint> zero            - zero all files (IOCTL ZERO)\n", name);
    printf("  %s <mountpoint> erase           - erase FS (IOCTL ERASE)\n", name);
    printf("  %s <mountpoint> metadata        - dump all file metadata (IOCTL METADATA)\n", name);
    printf("  %s <mountpoint> map <filename>  - dump sector mapping of file (IOCTL MAP)\n", name);
    printf("  %s <mountpoint> info            - get file count (IOCTL INFO)\n", name);
}

static int open_mount(const char *path)
{
    int fd = open(path, O_RDONLY | O_DIRECTORY);
    if (fd < 0) {
        perror("cannot open mountpoint");
        exit(EXIT_FAILURE);
    }
    return fd;
}

static int cmd_zero(const char *path)
{
    int fd = open_mount(path);

    if (ioctl(fd, SIMPLEFS_IOCTL_ZERO) < 0) {
        perror("ioctl ZERO");
        close(fd);
        return 1;
    }
    printf("All files zeroed.\n");
    close(fd);
    return 0;
}

static int cmd_erase(const char *path)
{
    int fd = open_mount(path);

    if (ioctl(fd, SIMPLEFS_IOCTL_ERASE) < 0) {
        perror("ioctl ERASE");
        close(fd);
        return 1;
    }
    printf("Filesystem erased. Remount with -o format to reuse.\n");
    close(fd);
    return 0;
}

static int cmd_info(const char *path)
{
    int fd = open_mount(path);
    struct info_response info = {0};

    if (ioctl(fd, SIMPLEFS_IOCTL_INFO, &info) < 0) {
        perror("ioctl INFO");
        close(fd);
        return 1;
    }
    printf("File count: %u\n", info.file_count);
    close(fd);
    return 0;
}

static int cmd_metadata(const char *path)
{
    int fd = open_mount(path);
    struct info_response info = {0};

    if (ioctl(fd, SIMPLEFS_IOCTL_INFO, &info) < 0) {
        perror("ioctl INFO");
        close(fd);
        return 1;
    }

    printf("=== metadata: %u files ===\n", info.file_count);
    if (info.file_count == 0) {
        close(fd);
        return 0;
    }

    struct metadata_entry *entries =
        calloc(BATCH_SIZE, sizeof(*entries));
    if (!entries) {
        perror("calloc");
        close(fd);
        return 1;
    }

    printf("%-16s %12s %12s %10s\n", "NAME", "OFFSET", "SIZE", "CRC32");

    for (uint32_t off = 0; off < info.file_count; off += BATCH_SIZE) {
        struct metadata_query q = {
            .entries_ptr = (uint64_t)(uintptr_t)entries,
            .offset      = off,
            .capacity    = BATCH_SIZE,
            .count       = 0,
        };

        if (ioctl(fd, SIMPLEFS_IOCTL_METADATA, &q) < 0) {
            perror("ioctl METADATA");
            free(entries);
            close(fd);
            return 1;
        }

        for (uint32_t i = 0; i < q.count; i++) {
            printf("%-16s %12llu %12llu 0x%08x\n",
                   entries[i].name,
                   (unsigned long long)entries[i].offset,
                   (unsigned long long)entries[i].size,
                   entries[i].hash);
        }
    }

    free(entries);
    close(fd);
    return 0;
}

static int cmd_map(const char *path, const char *filename)
{
    int fd = open_mount(path);
    struct map_response resp = {0};
    struct map_query q = {0};
    uint64_t *sectors;
    uint32_t capacity = 256;

    sectors = calloc(capacity, sizeof(*sectors));
    if (!sectors) {
        perror("calloc");
        close(fd);
        return 1;
    }

    strncpy(q.name, filename, sizeof(q.name) - 1);
    q.sectors_capacity = capacity;
    q.sectors_ptr      = (uint64_t)(uintptr_t)sectors;
    q.response_ptr     = (uint64_t)(uintptr_t)&resp;

    if (ioctl(fd, SIMPLEFS_IOCTL_MAP, &q) < 0) {
        perror("ioctl MAP");
        free(sectors);
        close(fd);
        return 1;
    }

    printf("name:         %s\n", q.name);
    printf("start_sector: %llu\n", (unsigned long long)resp.start_sector);
    printf("sector_count: %llu\n", (unsigned long long)resp.sector_count);
    printf("size:         %llu bytes\n", (unsigned long long)resp.size);
    printf("sectors:\n");
    for (uint64_t i = 0; i < resp.length; i++)
        printf("  [%llu] %llu\n", (unsigned long long)i,
               (unsigned long long)sectors[i]);
    if (resp.length < resp.sector_count)
        printf("  ... and %llu more\n",
               (unsigned long long)(resp.sector_count - resp.length));

    free(sectors);
    close(fd);
    return 0;
}

static int cmd_test(const char *path)
{
    DIR *dir = opendir(path);
    if (!dir) {
        perror("opendir");
        return 1;
    }

    struct dirent *entry;
    int total = 0, passed = 0;

    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, "file", 4) != 0)
            continue;

        char filepath[512];
        snprintf(filepath, sizeof(filepath), "%s/%s", path, entry->d_name);

        int fd = open(filepath, O_RDWR);
        if (fd < 0) {
            printf("cannot open %s\n", filepath);
            continue;
        }
        total++;

        uint32_t w = (uint32_t)rand();
        uint32_t r = 0;

        if (pwrite(fd, &w, sizeof(w), 0) != sizeof(w)) {
            printf("write failed: %s\n", entry->d_name);
            close(fd);
            continue;
        }
        if (pread(fd, &r, sizeof(r), 0) != sizeof(r)) {
            printf("read failed: %s\n", entry->d_name);
            close(fd);
            continue;
        }
        close(fd);

        if (w == r) {
            passed++;
        } else {
            printf("%s: FAIL (wrote %u, read %u)\n",
                   entry->d_name, w, r);
        }
    }

    closedir(dir);
    printf("RESULT: %d/%d\n", passed, total);
    return (passed == total) ? 0 : 1;
}

int main(int argc, char *argv[])
{
    if (argc < 3) {
        cli_usage(argv[0]);
        return 1;
    }

    srand(time(NULL));

    const char *path = argv[1];
    const char *cmd  = argv[2];

    if (!strcmp(cmd, "test"))     return cmd_test(path);
    if (!strcmp(cmd, "zero"))     return cmd_zero(path);
    if (!strcmp(cmd, "erase"))    return cmd_erase(path);
    if (!strcmp(cmd, "metadata")) return cmd_metadata(path);
    if (!strcmp(cmd, "info"))     return cmd_info(path);
    if (!strcmp(cmd, "map")) {
        if (argc < 4) {
            cli_usage(argv[0]);
            return 1;
        }
        return cmd_map(path, argv[3]);
    }

    cli_usage(argv[0]);
    return 1;
}