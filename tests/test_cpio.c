#include "burning.h"

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void write_file(const char *path, const char *contents, mode_t mode)
{
    int descriptor = open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
    assert(descriptor >= 0);
    assert(write(descriptor, contents, strlen(contents)) == (ssize_t)strlen(contents));
    assert(close(descriptor) == 0);
}

static void make_path(char output[BP_PATH_CAPACITY], const char *base, const char *suffix)
{
    assert(strlen(base) + strlen(suffix) < BP_PATH_CAPACITY);
    strcpy(output, base);
    strcat(output, suffix);
}

int main(void)
{
    char template[] = "/tmp/burning-cpio-test.XXXXXX";
    char *base = mkdtemp(template);
    char source[BP_PATH_CAPACITY];
    char path[BP_PATH_CAPACITY];
    char archive[BP_PATH_CAPACITY];
    char extracted[BP_PATH_CAPACITY];
    char target[BP_PATH_CAPACITY];
    char error[BP_ERROR_CAPACITY] = {0};
    struct bp_rootfs_info info;
    ssize_t length;

    assert(base != NULL);
    make_path(source, base, "/source");
    make_path(path, source, "/bin");
    assert(bp_mkdir_p(path, 0755, error, sizeof(error)) == 0);
    make_path(path, source, "/sbin");
    assert(bp_mkdir_p(path, 0755, error, sizeof(error)) == 0);
    make_path(path, source, "/etc");
    assert(bp_mkdir_p(path, 0755, error, sizeof(error)) == 0);
    make_path(path, source, "/bin/sh");
    write_file(path, "shell", 0755);
    make_path(path, source, "/sbin/burning-progress");
    write_file(path, "progress", 0755);
    make_path(path, source, "/entry.sh");
    write_file(path, "exec /sbin/init\n", 0755);
    make_path(path, source, "/etc/burning-progress.conf");
    write_file(path, "entryMode=handoff\nentry=/entry.sh\n", 0644);
    make_path(path, source, "/bin/ash");
    assert(symlink("sh", path) == 0);

    make_path(archive, base, "/rootfs.cpio");
    assert(bp_cpio_pack(source, archive, &info, error, sizeof(error)) == 0);
    assert(info.runtime.entry_mode == BP_ENTRY_HANDOFF);
    assert(info.entries >= 7U);
    assert(bp_cpio_verify(archive, &info, error, sizeof(error)) == 0);

    make_path(extracted, base, "/extracted");
    assert(bp_cpio_extract(archive, extracted, &info, error, sizeof(error)) == 0);
    make_path(path, extracted, "/bin/ash");
    length = readlink(path, target, sizeof(target) - 1U);
    assert(length > 0);
    target[length] = '\0';
    assert(strcmp(target, "sh") == 0);
    make_path(path, extracted, "/entry.sh");
    assert(access(path, X_OK) == 0);

    puts("test_cpio: OK");
    return 0;
}
