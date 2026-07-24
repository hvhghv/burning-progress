#include "burning.h"

#include <archive.h>
#include <archive_entry.h>
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

static void write_unsafe_tar(const char *path)
{
    struct archive *writer = archive_write_new();
    struct archive_entry *entry = archive_entry_new();

    assert(writer != NULL);
    assert(entry != NULL);
    assert(archive_write_add_filter_gzip(writer) == ARCHIVE_OK);
    assert(archive_write_set_format_pax_restricted(writer) == ARCHIVE_OK);
    assert(archive_write_open_filename(writer, path) == ARCHIVE_OK);
    archive_entry_set_pathname(entry, "../escape");
    archive_entry_set_filetype(entry, AE_IFREG);
    archive_entry_set_perm(entry, 0644);
    archive_entry_set_size(entry, 1);
    assert(archive_write_header(writer, entry) == ARCHIVE_OK);
    assert(archive_write_data(writer, "x", 1U) == 1);
    assert(archive_write_close(writer) == ARCHIVE_OK);
    assert(archive_write_free(writer) == ARCHIVE_OK);
    archive_entry_free(entry);
}

static void add_tar_entry(struct archive *writer, const char *path,
                          mode_t type, const char *data,
                          const char *symlink, const char *hardlink)
{
    struct archive_entry *entry = archive_entry_new();
    size_t size = data == NULL ? 0U : strlen(data);

    assert(entry != NULL);
    archive_entry_set_pathname(entry, path);
    if (type != 0) {
        archive_entry_set_filetype(entry, type);
    }
    archive_entry_set_perm(entry, 0755);
    archive_entry_set_size(entry, (la_int64_t)size);
    if (symlink != NULL) {
        archive_entry_set_symlink(entry, symlink);
    }
    if (hardlink != NULL) {
        archive_entry_set_hardlink(entry, hardlink);
    }
    assert(archive_write_header(writer, entry) == ARCHIVE_OK);
    if (size != 0U) {
        assert(archive_write_data(writer, data, size) == (la_ssize_t)size);
    }
    archive_entry_free(entry);
}

static void write_prefixed_hardlink_tar(const char *path)
{
    struct archive *writer = archive_write_new();

    assert(writer != NULL);
    assert(archive_write_add_filter_gzip(writer) == ARCHIVE_OK);
    assert(archive_write_set_format_pax_restricted(writer) == ARCHIVE_OK);
    assert(archive_write_open_filename(writer, path) == ARCHIVE_OK);
    add_tar_entry(writer, "template-root/", AE_IFDIR, NULL, NULL, NULL);
    add_tar_entry(writer, "template-root/bin/", AE_IFDIR, NULL, NULL, NULL);
    add_tar_entry(writer, "template-root/bin/busybox", AE_IFREG,
                  "busybox", NULL, NULL);
    add_tar_entry(writer, "template-root/bin/sh", AE_IFLNK,
                  NULL, "busybox", NULL);
    add_tar_entry(writer, "template-root/usr/", AE_IFDIR, NULL, NULL, NULL);
    add_tar_entry(writer, "template-root/usr/sbin/", AE_IFDIR,
                  NULL, NULL, NULL);
    add_tar_entry(writer, "template-root/usr/sbin/busybox", 0,
                  NULL, NULL, "template-root/bin/busybox");
    assert(archive_write_close(writer) == ARCHIVE_OK);
    assert(archive_write_free(writer) == ARCHIVE_OK);
}

static void write_unsafe_hardlink_tar(const char *path)
{
    struct archive *writer = archive_write_new();

    assert(writer != NULL);
    assert(archive_write_add_filter_gzip(writer) == ARCHIVE_OK);
    assert(archive_write_set_format_pax_restricted(writer) == ARCHIVE_OK);
    assert(archive_write_open_filename(writer, path) == ARCHIVE_OK);
    add_tar_entry(writer, "template-root/", AE_IFDIR, NULL, NULL, NULL);
    add_tar_entry(writer, "template-root/escape", 0,
                  NULL, NULL, "../escape");
    assert(archive_write_close(writer) == ARCHIVE_OK);
    assert(archive_write_free(writer) == ARCHIVE_OK);
}

int main(void)
{
    char template[] = "/tmp/burning-cpio-test.XXXXXX";
    char *base = mkdtemp(template);
    char source[BP_PATH_CAPACITY];
    char path[BP_PATH_CAPACITY];
    char archive[BP_PATH_CAPACITY];
    char extracted[BP_PATH_CAPACITY];
    char tar_archive[BP_PATH_CAPACITY];
    char tar_extracted[BP_PATH_CAPACITY];
    char unsafe_tar[BP_PATH_CAPACITY];
    char truncated_tar[BP_PATH_CAPACITY];
    char template_tar[BP_PATH_CAPACITY];
    char template_extracted[BP_PATH_CAPACITY];
    char unsafe_hardlink_tar[BP_PATH_CAPACITY];
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
    assert(info.format == BP_ROOTFS_CPIO);
    assert(bp_rootfs_verify(archive, &info, error, sizeof(error)) == 0);
    assert(info.format == BP_ROOTFS_CPIO);

    make_path(extracted, base, "/extracted");
    assert(bp_cpio_extract(archive, extracted, &info, error, sizeof(error)) == 0);
    make_path(path, extracted, "/bin/ash");
    length = readlink(path, target, sizeof(target) - 1U);
    assert(length > 0);
    target[length] = '\0';
    assert(strcmp(target, "sh") == 0);
    make_path(path, extracted, "/entry.sh");
    assert(access(path, X_OK) == 0);

    make_path(tar_archive, base, "/rootfs.tar.gz");
    assert(bp_rootfs_pack(source, tar_archive, BP_ROOTFS_TAR_GZIP,
                          &info, error, sizeof(error)) == 0);
    assert(info.format == BP_ROOTFS_TAR_GZIP);
    assert(bp_rootfs_verify(tar_archive, &info, error, sizeof(error)) == 0);
    assert(info.runtime.entry_mode == BP_ENTRY_HANDOFF);
    make_path(tar_extracted, base, "/tar-extracted");
    assert(bp_rootfs_extract(tar_archive, tar_extracted,
                             &info, error, sizeof(error)) == 0);
    make_path(path, tar_extracted, "/bin/ash");
    length = readlink(path, target, sizeof(target) - 1U);
    assert(length > 0);
    target[length] = '\0';
    assert(strcmp(target, "sh") == 0);
    make_path(path, tar_extracted, "/entry.sh");
    assert(access(path, X_OK) == 0);

    make_path(unsafe_tar, base, "/unsafe.tar.gz");
    write_unsafe_tar(unsafe_tar);
    error[0] = '\0';
    assert(bp_rootfs_verify(unsafe_tar, &info, error, sizeof(error)) != 0);
    assert(strstr(error, "unsafe tar path") != NULL);

    make_path(truncated_tar, base, "/truncated.tar.gz");
    error[0] = '\0';
    assert(bp_atomic_copy(tar_archive, truncated_tar, 0600,
                          error, sizeof(error)) == 0);
    {
        struct stat status;
        assert(stat(truncated_tar, &status) == 0);
        assert(status.st_size > 8);
        assert(truncate(truncated_tar, status.st_size - 8) == 0);
    }
    error[0] = '\0';
    assert(bp_rootfs_verify(truncated_tar, &info, error, sizeof(error)) != 0);

    make_path(template_tar, base, "/template.tar.gz");
    write_prefixed_hardlink_tar(template_tar);
    error[0] = '\0';
    assert(bp_rootfs_verify(template_tar, &info, error, sizeof(error)) != 0);
    assert(strstr(error, "missing bin/sh or sbin/burning-progress") != NULL);
    make_path(template_extracted, base, "/template-extracted");
    error[0] = '\0';
    assert(bp_rootfs_unpack(template_tar, template_extracted,
                            &info, error, sizeof(error)) == 0);
    make_path(path, template_extracted, "/template-root");
    assert(access(path, F_OK) != 0);
    {
        struct stat busybox_status;
        struct stat hardlink_status;
        make_path(path, template_extracted, "/bin/busybox");
        assert(stat(path, &busybox_status) == 0);
        make_path(path, template_extracted, "/usr/sbin/busybox");
        assert(stat(path, &hardlink_status) == 0);
        assert(busybox_status.st_dev == hardlink_status.st_dev);
        assert(busybox_status.st_ino == hardlink_status.st_ino);
    }

    make_path(unsafe_hardlink_tar, base, "/unsafe-hardlink.tar.gz");
    write_unsafe_hardlink_tar(unsafe_hardlink_tar);
    error[0] = '\0';
    assert(bp_rootfs_unpack(unsafe_hardlink_tar, template_extracted,
                            &info, error, sizeof(error)) != 0);
    assert(strstr(error, "unsafe tar hardlink target") != NULL);

    puts("test_cpio: OK");
    return 0;
}
