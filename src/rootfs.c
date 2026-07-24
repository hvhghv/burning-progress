#include "burning.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

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
