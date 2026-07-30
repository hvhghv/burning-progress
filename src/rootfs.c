#include "burning.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define BP_GZIP_MAGIC_0 0x1fU
#define BP_GZIP_MAGIC_1 0x8bU

const char *bp_rootfs_format_name(enum bp_rootfs_format format)
{
    return format == BP_ROOTFS_TAR_GZIP ? "tar.gz" : "cpio";
}

int bp_rootfs_format_parse(const char *text, enum bp_rootfs_format *format)
{
    if (strcmp(text, "cpio") == 0) {
        *format = BP_ROOTFS_CPIO;
        return 0;
    }
    if (strcmp(text, "tar.gz") == 0 || strcmp(text, "tgz") == 0) {
        *format = BP_ROOTFS_TAR_GZIP;
        return 0;
    }
    return -1;
}

static int detect_format(const char *path, enum bp_rootfs_format *format,
                         char *error, size_t error_size)
{
    unsigned char magic[6];
    FILE *file = fopen(path, "rb");
    size_t count;

    if (file == NULL) {
        bp_error_set(error, error_size, "open %s: %s", path, strerror(errno));
        return -1;
    }
    count = fread(magic, 1U, sizeof(magic), file);
    fclose(file);
    if (count >= 2U && magic[0] == BP_GZIP_MAGIC_0 && magic[1] == BP_GZIP_MAGIC_1) {
        *format = BP_ROOTFS_TAR_GZIP;
        return 0;
    }
    if (count == sizeof(magic) && memcmp(magic, "070701", sizeof(magic)) == 0) {
        *format = BP_ROOTFS_CPIO;
        return 0;
    }
    bp_error_set(error, error_size, "unsupported rootfs archive format");
    return -1;
}

int bp_rootfs_pack(const char *source, const char *output,
                   enum bp_rootfs_format format, struct bp_rootfs_info *info,
                   char *error, size_t error_size)
{
    if (format == BP_ROOTFS_TAR_GZIP) {
        return bp_tar_gzip_pack(source, output, info, error, error_size);
    }
    return bp_cpio_pack(source, output, info, error, error_size);
}

int bp_rootfs_verify(const char *archive, struct bp_rootfs_info *info,
                     char *error, size_t error_size)
{
    enum bp_rootfs_format format;
    if (detect_format(archive, &format, error, error_size) != 0) {
        return -1;
    }
    if (format == BP_ROOTFS_TAR_GZIP) {
        return bp_tar_gzip_verify(archive, info, error, error_size);
    }
    return bp_cpio_verify(archive, info, error, error_size);
}

int bp_rootfs_extract(const char *archive, const char *destination,
                      struct bp_rootfs_info *info, char *error, size_t error_size)
{
    enum bp_rootfs_format format;
    if (detect_format(archive, &format, error, error_size) != 0) {
        return -1;
    }
    if (format == BP_ROOTFS_TAR_GZIP) {
        return bp_tar_gzip_extract(archive, destination, info, error, error_size);
    }
    return bp_cpio_extract(archive, destination, info, error, error_size);
}

int bp_rootfs_unpack(const char *archive, const char *destination,
                     struct bp_rootfs_info *info, char *error, size_t error_size)
{
    enum bp_rootfs_format format;
    if (detect_format(archive, &format, error, error_size) != 0) {
        return -1;
    }
    if (format == BP_ROOTFS_TAR_GZIP) {
        return bp_tar_gzip_unpack(archive, destination, info, error, error_size);
    }
    return bp_cpio_unpack(archive, destination, info, error, error_size);
}

static int checked_rootfs_path(const char *rootfs,
                               char checked[BP_PATH_CAPACITY],
                               char *error, size_t error_size)
{
    struct stat status;
    size_t length;

    if (rootfs == NULL || rootfs[0] == '\0' ||
        strlen(rootfs) >= BP_PATH_CAPACITY) {
        bp_error_set(error, error_size,
                     "rootfs directory is missing or is not a real directory: %s",
                     rootfs == NULL ? "(null)" : rootfs);
        return -1;
    }
    strcpy(checked, rootfs);
    length = strlen(checked);
    while (length > 1U && checked[length - 1U] == '/') {
        checked[--length] = '\0';
    }
    if (lstat(checked, &status) != 0 || !S_ISDIR(status.st_mode)) {
        bp_error_set(error, error_size,
                     "rootfs directory is missing or is not a real directory: %s",
                     rootfs);
        return -1;
    }
    return 0;
}

static int runtime_config_paths(const char *rootfs,
                                char etc_path[BP_PATH_CAPACITY],
                                char config_path[BP_PATH_CAPACITY],
                                char *error, size_t error_size)
{
    char checked_rootfs[BP_PATH_CAPACITY];
    struct stat status;

    if (checked_rootfs_path(rootfs, checked_rootfs,
                            error, error_size) != 0 ||
        bp_path(etc_path, BP_PATH_CAPACITY, checked_rootfs, "/etc",
                error, error_size) != 0 ||
        bp_path(config_path, BP_PATH_CAPACITY, checked_rootfs,
                BP_RUNTIME_CONFIG_PATH, error, error_size) != 0) {
        return -1;
    }
    if (lstat(etc_path, &status) == 0) {
        if (!S_ISDIR(status.st_mode)) {
            bp_error_set(error, error_size,
                         "rootfs etc path is not a real directory: %s", etc_path);
            return -1;
        }
    } else if (errno != ENOENT) {
        bp_error_set(error, error_size, "inspect %s: %s",
                     etc_path, strerror(errno));
        return -1;
    }
    return 0;
}

static int source_entry_status(const char *rootfs, const char *relative,
                               struct stat *status)
{
    char checked_rootfs[BP_PATH_CAPACITY];
    char path[BP_PATH_CAPACITY];
    char *component;
    char *separator;
    char ignored[BP_ERROR_CAPACITY];
    int directory = -1;
    int next_directory;
    int result = -1;

    if (checked_rootfs_path(rootfs, checked_rootfs,
                            ignored, sizeof(ignored)) != 0 ||
        relative == NULL || relative[0] == '\0' ||
        strlen(relative) >= sizeof(path)) {
        errno = EINVAL;
        return -1;
    }
    strcpy(path, relative);
    directory = open(checked_rootfs,
                     O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (directory < 0) {
        return -1;
    }
    component = path;
    while ((separator = strchr(component, '/')) != NULL) {
        *separator = '\0';
        if (component[0] == '\0') {
            errno = EINVAL;
            goto cleanup;
        }
        next_directory = openat(directory, component,
                                O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (next_directory < 0) {
            goto cleanup;
        }
        close(directory);
        directory = next_directory;
        component = separator + 1;
    }
    if (component[0] != '\0' &&
        fstatat(directory, component, status, AT_SYMLINK_NOFOLLOW) == 0) {
        result = 0;
    }

cleanup:
    close(directory);
    return result;
}

int bp_rootfs_source_verify(const char *rootfs,
                            struct bp_runtime_config *runtime,
                            char *error, size_t error_size)
{
    struct stat status;

    if (bp_rootfs_runtime_config_load(rootfs, runtime,
                                      error, error_size) != 0) {
        return -1;
    }
    if (source_entry_status(rootfs, "bin/sh", &status) != 0) {
        bp_error_set(error, error_size, "rootfs is missing bin/sh");
        return -1;
    }
    if (!S_ISREG(status.st_mode) && !S_ISLNK(status.st_mode)) {
        bp_error_set(error, error_size,
                     "rootfs bin/sh is not a regular file or symlink");
        return -1;
    }
    if (source_entry_status(rootfs, "sbin/burning-progress", &status) != 0) {
        bp_error_set(error, error_size,
                     "rootfs is missing sbin/burning-progress");
        return -1;
    }
    if (!S_ISREG(status.st_mode) && !S_ISLNK(status.st_mode)) {
        bp_error_set(error, error_size,
                     "rootfs sbin/burning-progress is not a regular file or symlink");
        return -1;
    }
    if (runtime->entry_mode == BP_ENTRY_HANDOFF &&
        (source_entry_status(rootfs, runtime->entry + 1, &status) != 0 ||
         !S_ISREG(status.st_mode))) {
        bp_error_set(error, error_size,
                     "handoff entry is missing or not a regular file: %s",
                     runtime->entry);
        return -1;
    }
    return 0;
}

int bp_rootfs_runtime_config_load(const char *rootfs,
                                  struct bp_runtime_config *config,
                                  char *error, size_t error_size)
{
    char etc_path[BP_PATH_CAPACITY];
    char config_path[BP_PATH_CAPACITY];
    char *contents = NULL;
    int result;

    if (runtime_config_paths(rootfs, etc_path, config_path,
                             error, error_size) != 0) {
        return -1;
    }
    errno = 0;
    if (bp_read_text_file(config_path, &contents, 65536U,
                          error, error_size) != 0) {
        if (errno == ENOENT) {
            bp_runtime_config_default(config);
            if (error != NULL && error_size != 0U) {
                error[0] = '\0';
            }
            return 0;
        }
        return -1;
    }
    result = bp_runtime_config_parse(contents, config, error, error_size);
    free(contents);
    return result;
}

int bp_rootfs_runtime_config_save(const char *rootfs,
                                  const struct bp_runtime_config *config,
                                  char *error, size_t error_size)
{
    char etc_path[BP_PATH_CAPACITY];
    char config_path[BP_PATH_CAPACITY];
    char text[BP_PATH_CAPACITY + 64U];

    if (runtime_config_paths(rootfs, etc_path, config_path,
                             error, error_size) != 0 ||
        bp_runtime_config_format(config, text, sizeof(text),
                                 error, error_size) != 0) {
        return -1;
    }
    return bp_atomic_write(config_path, text, strlen(text), 0644,
                           error, error_size);
}

int bp_rootfs_default_entry_ensure(const char *rootfs,
                                   const struct bp_runtime_config *config,
                                   int *created, char *error, size_t error_size)
{
    static const char default_entry[] = "#!/bin/sh\nexec /bin/sh\n";
    char etc_path[BP_PATH_CAPACITY];
    char config_path[BP_PATH_CAPACITY];
    char entry_path[BP_PATH_CAPACITY];
    struct stat status;

    if (created != NULL) {
        *created = 0;
    }
    if (strcmp(config->entry, BP_DEFAULT_ENTRY_PATH) != 0) {
        return 0;
    }
    if (runtime_config_paths(rootfs, etc_path, config_path,
                             error, error_size) != 0 ||
        bp_path(entry_path, sizeof(entry_path), rootfs,
                BP_DEFAULT_ENTRY_PATH, error, error_size) != 0) {
        return -1;
    }
    if (lstat(entry_path, &status) == 0) {
        if (!S_ISREG(status.st_mode)) {
            bp_error_set(error, error_size,
                         "default entry is not a regular file: %s", entry_path);
            return -1;
        }
        return 0;
    }
    if (errno != ENOENT) {
        bp_error_set(error, error_size, "inspect %s: %s",
                     entry_path, strerror(errno));
        return -1;
    }
    if (bp_atomic_write(entry_path, default_entry,
                        sizeof(default_entry) - 1U, 0755,
                        error, error_size) != 0) {
        return -1;
    }
    if (created != NULL) {
        *created = 1;
    }
    return 0;
}
