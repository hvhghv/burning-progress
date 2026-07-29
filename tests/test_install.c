#include "burning.h"

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void make_file(const char *path, const char *contents, mode_t mode)
{
    int descriptor = open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
    assert(descriptor >= 0);
    assert(write(descriptor, contents, strlen(contents)) == (ssize_t)strlen(contents));
    assert(close(descriptor) == 0);
}

static void make_dir(const char *path)
{
    assert(mkdir(path, 0755) == 0);
}

int main(void)
{
    char template[] = "/tmp/burning-progress-test.XXXXXX";
    char *root = mkdtemp(template);
    char sbin[BP_PATH_CAPACITY];
    char init[BP_PATH_CAPACITY];
    char backup[BP_PATH_CAPACITY];
    char source_init[BP_PATH_CAPACITY];
    char source_progress[BP_PATH_CAPACITY];
    char target[BP_PATH_CAPACITY];
    char error[BP_ERROR_CAPACITY] = {0};
    ssize_t length;

    assert(root != NULL);
    snprintf(sbin, sizeof(sbin), "%s/sbin", root);
    assert(mkdir(sbin, 0755) == 0);
    snprintf(init, sizeof(init), "%s/sbin/init", root);
    assert(symlink("../lib/systemd/systemd", init) == 0);
    snprintf(source_init, sizeof(source_init), "%s/source-init", root);
    snprintf(source_progress, sizeof(source_progress), "%s/source-progress", root);
    make_file(source_init, "dispatcher", 0755);
    make_file(source_progress, "progress", 0755);

    assert(bp_install(root, source_init, source_progress, error, sizeof(error)) == 0);
    assert(bp_is_installed(root));
    length = readlink(init, target, sizeof(target) - 1U);
    assert(length > 0);
    target[length] = '\0';
    assert(strcmp(target, BP_BURNING_INIT_PATH) == 0);
    snprintf(backup, sizeof(backup), "%s/sbin/init.burning-original", root);
    length = readlink(backup, target, sizeof(target) - 1U);
    assert(length > 0);
    target[length] = '\0';
    assert(strcmp(target, "../lib/systemd/systemd") == 0);

    assert(bp_uninstall(root, error, sizeof(error)) == 0);
    length = readlink(init, target, sizeof(target) - 1U);
    assert(length > 0);
    target[length] = '\0';
    assert(strcmp(target, "../lib/systemd/systemd") == 0);

    {
        char source_template[] = "/tmp/burning-progress-rootfs-source.XXXXXX";
        char target_template[] = "/tmp/burning-progress-rootfs-target.XXXXXX";
        char archive[BP_PATH_CAPACITY];
        char source_bin[BP_PATH_CAPACITY];
        char source_sbin[BP_PATH_CAPACITY];
        char source_sh[BP_PATH_CAPACITY];
        char source_progress[BP_PATH_CAPACITY];
        char config_path[BP_PATH_CAPACITY];
        char *source_root = mkdtemp(source_template);
        char *target_root = mkdtemp(target_template);
        char *contents = NULL;
        struct bp_rootfs_info rootfs_info;

        assert(source_root != NULL);
        assert(target_root != NULL);
        snprintf(source_bin, sizeof(source_bin), "%s/bin", source_root);
        snprintf(source_sbin, sizeof(source_sbin), "%s/sbin", source_root);
        make_dir(source_bin);
        make_dir(source_sbin);
        snprintf(source_sh, sizeof(source_sh), "%s/bin/sh", source_root);
        snprintf(source_progress, sizeof(source_progress), "%s/sbin/burning-progress",
                 source_root);
        make_file(source_sh, "sh", 0755);
        make_file(source_progress, "progress", 0755);
        snprintf(archive, sizeof(archive), "%s/rootfs.cpio", target_root);
        assert(bp_cpio_pack(source_root, archive, &rootfs_info,
                            error, sizeof(error)) == 0);
        assert(bp_rootfs_install(target_root, archive, &rootfs_info,
                                 error, sizeof(error)) == 0);
        snprintf(config_path, sizeof(config_path),
                 "%s/etc/BurningProcess/config", target_root);
        assert(bp_read_text_file(config_path, &contents, 1024U,
                                 error, sizeof(error)) == 0);
        assert(strstr(contents, "timeout=5") != NULL);
        assert(strstr(contents, "default=normal") != NULL);
        free(contents);
    }

    printf("test_install: OK\n");
    return 0;
}
