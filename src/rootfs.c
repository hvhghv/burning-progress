#include "burning.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

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
    return bp_cpio_extract(archive, destination, info, error, error_size);
}

static int runtime_config_paths(const char *rootfs,
                                char etc_path[BP_PATH_CAPACITY],
                                char config_path[BP_PATH_CAPACITY],
                                char *error, size_t error_size)
{
    char checked_rootfs[BP_PATH_CAPACITY];
    struct stat status;
    size_t length;

    if (rootfs == NULL || rootfs[0] == '\0' ||
        strlen(rootfs) >= sizeof(checked_rootfs)) {
        bp_error_set(error, error_size,
                     "rootfs directory is missing or is not a real directory: %s",
                     rootfs == NULL ? "(null)" : rootfs);
        return -1;
    }
    strcpy(checked_rootfs, rootfs);
    length = strlen(checked_rootfs);
    while (length > 1U && checked_rootfs[length - 1U] == '/') {
        checked_rootfs[--length] = '\0';
    }
    if (lstat(checked_rootfs, &status) != 0 || !S_ISDIR(status.st_mode)) {
        bp_error_set(error, error_size,
                     "rootfs directory is missing or is not a real directory: %s",
                     rootfs);
        return -1;
    }
    if (bp_path(etc_path, BP_PATH_CAPACITY, checked_rootfs, "/etc",
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
