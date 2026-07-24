#include "burning.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define NEWC_HEADER_SIZE 110U
#define NEWC_NAME_LIMIT 4096U
#define NEWC_ENTRY_LIMIT 1000000U
#define NEWC_MAGIC "070701"
#define NEWC_TRAILER "TRAILER!!!"

struct path_item {
    char *path;
    char *target;
    mode_t mode;
    uid_t uid;
    gid_t gid;
};

struct path_list {
    struct path_item *items;
    size_t count;
    size_t capacity;
};

struct newc_header {
    mode_t mode;
    uid_t uid;
    gid_t gid;
    uint32_t file_size;
    uint32_t name_size;
};

static void path_list_free(struct path_list *list)
{
    size_t index;
    for (index = 0U; index < list->count; ++index) {
        free(list->items[index].path);
        free(list->items[index].target);
    }
    free(list->items);
    memset(list, 0, sizeof(*list));
}

static int path_list_add(struct path_list *list, const char *path, mode_t mode,
                         uid_t uid, gid_t gid, char *error, size_t error_size)
{
    struct path_item *resized;

    if (list->count >= NEWC_ENTRY_LIMIT) {
        bp_error_set(error, error_size, "CPIO entry limit exceeded");
        return -1;
    }
    if (list->count == list->capacity) {
        size_t capacity = list->capacity == 0U ? 64U : list->capacity * 2U;
        resized = realloc(list->items, capacity * sizeof(*resized));
        if (resized == NULL) {
            bp_error_set(error, error_size, "out of memory collecting CPIO paths");
            return -1;
        }
        list->items = resized;
        list->capacity = capacity;
    }
    list->items[list->count].path = strdup(path);
    if (list->items[list->count].path == NULL) {
        bp_error_set(error, error_size, "out of memory copying CPIO path");
        return -1;
    }
    list->items[list->count].mode = mode;
    list->items[list->count].uid = uid;
    list->items[list->count].gid = gid;
    list->items[list->count].target = NULL;
    ++list->count;
    return 0;
}

static int compare_path_items(const void *left, const void *right)
{
    const struct path_item *left_item = left;
    const struct path_item *right_item = right;
    return strcmp(left_item->path, right_item->path);
}

static int allowed_type(mode_t mode)
{
    return S_ISREG(mode) || S_ISDIR(mode) || S_ISLNK(mode);
}

static int join_path(char *output, size_t output_size, const char *left,
                     const char *right, char *error, size_t error_size)
{
    int length = snprintf(output, output_size, "%s/%s", left, right);
    if (length < 0 || (size_t)length >= output_size) {
        bp_error_set(error, error_size, "path is too long: %s/%s", left, right);
        return -1;
    }
    return 0;
}

static int collect_directory(const char *root, const char *relative, dev_t root_device,
                             struct path_list *list, char *error, size_t error_size)
{
    char directory_path[BP_PATH_CAPACITY];
    DIR *directory;
    struct dirent *entry;

    if (*relative == '\0') {
        if (strlen(root) >= sizeof(directory_path)) {
            bp_error_set(error, error_size, "source path is too long");
            return -1;
        }
        strcpy(directory_path, root);
    } else if (join_path(directory_path, sizeof(directory_path), root, relative,
                         error, error_size) != 0) {
        return -1;
    }
    directory = opendir(directory_path);
    if (directory == NULL) {
        bp_error_set(error, error_size, "open directory %s: %s",
                     directory_path, strerror(errno));
        return -1;
    }
    errno = 0;
    while ((entry = readdir(directory)) != NULL) {
        char child_relative[BP_PATH_CAPACITY];
        char child_path[BP_PATH_CAPACITY];
        struct stat status;
        int length;

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        length = *relative == '\0'
                     ? snprintf(child_relative, sizeof(child_relative), "%s", entry->d_name)
                     : snprintf(child_relative, sizeof(child_relative), "%s/%s",
                                relative, entry->d_name);
        if (length < 0 || (size_t)length >= sizeof(child_relative) ||
            join_path(child_path, sizeof(child_path), root, child_relative,
                      error, error_size) != 0) {
            closedir(directory);
            return -1;
        }
        if (lstat(child_path, &status) != 0) {
            bp_error_set(error, error_size, "inspect %s: %s", child_path, strerror(errno));
            closedir(directory);
            return -1;
        }
        if (S_ISDIR(status.st_mode) && status.st_dev != root_device) {
            continue;
        }
        if (!allowed_type(status.st_mode)) {
            bp_error_set(error, error_size, "unsupported CPIO source type: %s", child_path);
            closedir(directory);
            return -1;
        }
        if (strlen(child_relative) > NEWC_NAME_LIMIT ||
            path_list_add(list, child_relative, status.st_mode, status.st_uid,
                          status.st_gid, error, error_size) != 0) {
            closedir(directory);
            return -1;
        }
        if (S_ISDIR(status.st_mode) &&
            collect_directory(root, child_relative, root_device, list,
                              error, error_size) != 0) {
            closedir(directory);
            return -1;
        }
        errno = 0;
    }
    if (errno != 0) {
        bp_error_set(error, error_size, "read directory %s: %s",
                     directory_path, strerror(errno));
        closedir(directory);
        return -1;
    }
    closedir(directory);
    return 0;
}

static size_t alignment_padding(size_t size)
{
    return (4U - (size % 4U)) % 4U;
}

static int write_all(FILE *file, const void *data, size_t size)
{
    return fwrite(data, 1U, size, file) == size ? 0 : -1;
}

static int write_padding(FILE *file, size_t size)
{
    static const unsigned char zeros[3] = {0U, 0U, 0U};
    size_t count = alignment_padding(size);
    return count == 0U || write_all(file, zeros, count) == 0 ? 0 : -1;
}

static int write_header(FILE *file, uint32_t inode, mode_t mode, uid_t uid, gid_t gid,
                        uint32_t mtime, uint32_t file_size, uint32_t name_size)
{
    char header[NEWC_HEADER_SIZE + 1U];
    int length = snprintf(header, sizeof(header),
                          NEWC_MAGIC "%08x%08x%08x%08x%08x%08x%08x"
                          "%08x%08x%08x%08x%08x%08x",
                          inode, (uint32_t)mode, (uint32_t)uid, (uint32_t)gid,
                          1U, mtime, file_size, 0U, 0U, 0U, 0U, name_size, 0U);
    if (length != (int)NEWC_HEADER_SIZE) {
        errno = EOVERFLOW;
        return -1;
    }
    return write_all(file, header, NEWC_HEADER_SIZE);
}

static int copy_file_data(FILE *output, const char *path, uint64_t expected_size)
{
    unsigned char buffer[65536];
    FILE *input = fopen(path, "rb");
    uint64_t copied = 0U;

    if (input == NULL) {
        return -1;
    }
    while (copied < expected_size) {
        size_t wanted = expected_size - copied > sizeof(buffer)
                            ? sizeof(buffer)
                            : (size_t)(expected_size - copied);
        size_t count = fread(buffer, 1U, wanted, input);
        if (count == 0U || write_all(output, buffer, count) != 0) {
            fclose(input);
            return -1;
        }
        copied += count;
    }
    if (fgetc(input) != EOF) {
        fclose(input);
        errno = EBUSY;
        return -1;
    }
    return fclose(input);
}

static int output_inside_source(const char *source, const char *output)
{
    char canonical_source[BP_PATH_CAPACITY];
    char output_copy[BP_PATH_CAPACITY];
    char canonical_parent[BP_PATH_CAPACITY];
    char candidate[BP_PATH_CAPACITY];
    char *separator;
    size_t source_length;

    if (realpath(source, canonical_source) == NULL || strlen(output) >= sizeof(output_copy)) {
        return -1;
    }
    strcpy(output_copy, output);
    separator = strrchr(output_copy, '/');
    if (separator == NULL) {
        strcpy(output_copy, ".");
        separator = (char *)output;
    } else {
        *separator = '\0';
        ++separator;
    }
    if (realpath(output_copy, canonical_parent) == NULL ||
        snprintf(candidate, sizeof(candidate), "%s/%s", canonical_parent, separator) < 0) {
        return -1;
    }
    source_length = strlen(canonical_source);
    return strncmp(candidate, canonical_source, source_length) == 0 &&
           (candidate[source_length] == '\0' || candidate[source_length] == '/');
}

static int write_archive(const char *source, FILE *output, struct path_list *paths,
                         char *error, size_t error_size)
{
    size_t index;

    for (index = 0U; index < paths->count; ++index) {
        char full_path[BP_PATH_CAPACITY];
        struct stat status;
        unsigned char *symlink_data = NULL;
        uint64_t file_size;
        uint32_t mtime;

        if (join_path(full_path, sizeof(full_path), source, paths->items[index].path,
                      error, error_size) != 0 || lstat(full_path, &status) != 0) {
            bp_error_set(error, error_size, "inspect %s: %s", full_path, strerror(errno));
            return -1;
        }
        if (S_ISREG(status.st_mode)) {
            if (status.st_size < 0 || (uint64_t)status.st_size > UINT32_MAX) {
                bp_error_set(error, error_size, "file is too large for newc: %s", full_path);
                return -1;
            }
            file_size = (uint64_t)status.st_size;
        } else if (S_ISLNK(status.st_mode)) {
            ssize_t length;
            symlink_data = malloc(NEWC_NAME_LIMIT + 1U);
            if (symlink_data == NULL) {
                bp_error_set(error, error_size, "out of memory reading symlink");
                return -1;
            }
            length = readlink(full_path, (char *)symlink_data, NEWC_NAME_LIMIT + 1U);
            if (length < 0 || length > (ssize_t)NEWC_NAME_LIMIT) {
                bp_error_set(error, error_size, "read symlink %s: %s",
                             full_path, strerror(errno));
                free(symlink_data);
                return -1;
            }
            file_size = (uint64_t)length;
        } else {
            file_size = 0U;
        }
        mtime = status.st_mtime < 0 ? 0U : (uint32_t)status.st_mtime;
        if (write_header(output, (uint32_t)(index + 1U), status.st_mode,
                         status.st_uid, status.st_gid, mtime, (uint32_t)file_size,
                         (uint32_t)strlen(paths->items[index].path) + 1U) != 0 ||
            write_all(output, paths->items[index].path,
                      strlen(paths->items[index].path) + 1U) != 0 ||
            write_padding(output, NEWC_HEADER_SIZE +
                                  strlen(paths->items[index].path) + 1U) != 0) {
            bp_error_set(error, error_size, "write CPIO header: %s", strerror(errno));
            free(symlink_data);
            return -1;
        }
        if ((S_ISREG(status.st_mode) && copy_file_data(output, full_path, file_size) != 0) ||
            (S_ISLNK(status.st_mode) && write_all(output, symlink_data, (size_t)file_size) != 0) ||
            write_padding(output, (size_t)file_size) != 0) {
            bp_error_set(error, error_size, "write CPIO data for %s: %s",
                         full_path, strerror(errno));
            free(symlink_data);
            return -1;
        }
        free(symlink_data);
    }
    if (write_header(output, 0U, 0U, 0U, 0U, 0U, 0U,
                     (uint32_t)strlen(NEWC_TRAILER) + 1U) != 0 ||
        write_all(output, NEWC_TRAILER, strlen(NEWC_TRAILER) + 1U) != 0 ||
        write_padding(output, NEWC_HEADER_SIZE + strlen(NEWC_TRAILER) + 1U) != 0) {
        bp_error_set(error, error_size, "write CPIO trailer: %s", strerror(errno));
        return -1;
    }
    return 0;
}

static int parse_hex_field(const char *field, uint32_t *value)
{
    char buffer[9];
    char *end;
    unsigned long parsed;

    memcpy(buffer, field, 8U);
    buffer[8] = '\0';
    errno = 0;
    parsed = strtoul(buffer, &end, 16);
    if (errno != 0 || *end != '\0' || parsed > UINT32_MAX) {
        return -1;
    }
    *value = (uint32_t)parsed;
    return 0;
}

static int read_exact(FILE *file, void *data, size_t size)
{
    return fread(data, 1U, size, file) == size ? 0 : -1;
}

static int read_header(FILE *file, struct newc_header *header)
{
    char raw[NEWC_HEADER_SIZE];
    uint32_t fields[13];
    size_t index;

    if (read_exact(file, raw, sizeof(raw)) != 0 || memcmp(raw, NEWC_MAGIC, 6U) != 0) {
        return -1;
    }
    for (index = 0U; index < 13U; ++index) {
        if (parse_hex_field(raw + 6U + index * 8U, &fields[index]) != 0) {
            return -1;
        }
    }
    header->mode = (mode_t)fields[1];
    header->uid = (uid_t)fields[2];
    header->gid = (gid_t)fields[3];
    header->file_size = fields[6];
    header->name_size = fields[11];
    if (header->name_size == 0U || header->name_size > NEWC_NAME_LIMIT + 1U) {
        return -1;
    }
    return 0;
}

static int read_padding(FILE *file, size_t size)
{
    unsigned char ignored[3];
    size_t count = alignment_padding(size);
    return count == 0U || read_exact(file, ignored, count) == 0 ? 0 : -1;
}

static int valid_archive_name(const char *name)
{
    const char *component;

    if (*name == '\0' || *name == '/' || strlen(name) > NEWC_NAME_LIMIT) {
        return 0;
    }
    component = name;
    while (*component != '\0') {
        const char *end = strchr(component, '/');
        size_t length = end == NULL ? strlen(component) : (size_t)(end - component);
        if (length == 0U || (length == 1U && component[0] == '.') ||
            (length == 2U && component[0] == '.' && component[1] == '.')) {
            return 0;
        }
        if (end == NULL) {
            break;
        }
        component = end + 1;
    }
    return 1;
}

static int skip_data(FILE *file, uint32_t size)
{
    unsigned char buffer[65536];
    uint32_t remaining = size;
    while (remaining > 0U) {
        size_t count = remaining > sizeof(buffer) ? sizeof(buffer) : remaining;
        if (read_exact(file, buffer, count) != 0) {
            return -1;
        }
        remaining -= (uint32_t)count;
    }
    return read_padding(file, size);
}

static const struct path_item *find_path(const struct path_list *paths, const char *path)
{
    size_t index;
    for (index = 0U; index < paths->count; ++index) {
        if (strcmp(paths->items[index].path, path) == 0) {
            return &paths->items[index];
        }
    }
    return NULL;
}

int bp_cpio_verify(const char *archive, struct bp_rootfs_info *info,
                   char *error, size_t error_size)
{
    FILE *file = NULL;
    struct path_list paths = {0};
    char *runtime_text = NULL;
    uint64_t data_bytes = 0U;
    int result = -1;
    size_t index;

    file = fopen(archive, "rb");
    if (file == NULL) {
        bp_error_set(error, error_size, "open %s: %s", archive, strerror(errno));
        return -1;
    }
    for (;;) {
        struct newc_header header;
        char name[NEWC_NAME_LIMIT + 1U];
        int capture_config;

        if (read_header(file, &header) != 0 ||
            read_exact(file, name, header.name_size) != 0 ||
            name[header.name_size - 1U] != '\0' ||
            memchr(name, '\0', header.name_size - 1U) != NULL ||
            read_padding(file, NEWC_HEADER_SIZE + header.name_size) != 0) {
            bp_error_set(error, error_size, "invalid or truncated newc archive");
            goto cleanup;
        }
        if (strcmp(name, NEWC_TRAILER) == 0) {
            if (header.file_size != 0U || fgetc(file) != EOF) {
                bp_error_set(error, error_size, "invalid CPIO trailer");
                goto cleanup;
            }
            break;
        }
        if (!valid_archive_name(name) || !allowed_type(header.mode) ||
            find_path(&paths, name) != NULL ||
            (S_ISDIR(header.mode) && header.file_size != 0U) ||
            (S_ISLNK(header.mode) && header.file_size > NEWC_NAME_LIMIT)) {
            bp_error_set(error, error_size, "invalid CPIO entry: %s", name);
            goto cleanup;
        }
        if (path_list_add(&paths, name, header.mode, header.uid, header.gid,
                          error, error_size) != 0) {
            goto cleanup;
        }
        data_bytes += header.file_size;
        capture_config = strcmp(name, "etc/burning-progress.conf") == 0;
        if (capture_config) {
            if (!S_ISREG(header.mode) || header.file_size > 65536U) {
                bp_error_set(error, error_size, "invalid runtime configuration entry");
                goto cleanup;
            }
            runtime_text = malloc((size_t)header.file_size + 1U);
            if (runtime_text == NULL ||
                read_exact(file, runtime_text, header.file_size) != 0 ||
                memchr(runtime_text, '\0', header.file_size) != NULL ||
                read_padding(file, header.file_size) != 0) {
                bp_error_set(error, error_size, "cannot read runtime configuration");
                goto cleanup;
            }
            runtime_text[header.file_size] = '\0';
        } else if (S_ISLNK(header.mode)) {
            char target[NEWC_NAME_LIMIT + 1U];
            if (read_exact(file, target, header.file_size) != 0 ||
                memchr(target, '\0', header.file_size) != NULL ||
                read_padding(file, header.file_size) != 0) {
                bp_error_set(error, error_size, "invalid symlink data for %s", name);
                goto cleanup;
            }
        } else if (skip_data(file, header.file_size) != 0) {
            bp_error_set(error, error_size, "truncated CPIO data for %s", name);
            goto cleanup;
        }
    }
    qsort(paths.items, paths.count, sizeof(paths.items[0]), compare_path_items);
    for (index = 1U; index < paths.count; ++index) {
        if (strcmp(paths.items[index - 1U].path, paths.items[index].path) == 0) {
            bp_error_set(error, error_size, "duplicate CPIO path");
            goto cleanup;
        }
    }
    {
        const struct path_item *shell = find_path(&paths, "bin/sh");
        const struct path_item *progress = find_path(&paths, "sbin/burning-progress");
        if (shell == NULL || progress == NULL ||
            !(S_ISREG(shell->mode) || S_ISLNK(shell->mode)) ||
            !(S_ISREG(progress->mode) || S_ISLNK(progress->mode))) {
            bp_error_set(error, error_size,
                         "rootfs is missing bin/sh or sbin/burning-progress");
            goto cleanup;
        }
    }
    if (runtime_text == NULL) {
        bp_runtime_config_default(&info->runtime);
    } else if (bp_runtime_config_parse(runtime_text, &info->runtime,
                                       error, error_size) != 0) {
        goto cleanup;
    }
    if (info->runtime.entry_mode == BP_ENTRY_HANDOFF) {
        const struct path_item *entry = find_path(&paths, info->runtime.entry + 1);
        if (entry == NULL || !S_ISREG(entry->mode)) {
            bp_error_set(error, error_size, "handoff entry is missing or not a regular file");
            goto cleanup;
        }
    }
    info->entries = paths.count;
    info->data_bytes = data_bytes;
    info->format = BP_ROOTFS_CPIO;
    result = 0;

cleanup:
    free(runtime_text);
    path_list_free(&paths);
    if (file != NULL) {
        fclose(file);
    }
    return result;
}

int bp_cpio_pack(const char *source, const char *output,
                 struct bp_rootfs_info *info, char *error, size_t error_size)
{
    struct stat status;
    struct path_list paths = {0};
    char parent[BP_PATH_CAPACITY];
    char temporary[BP_PATH_CAPACITY] = {0};
    char output_copy[BP_PATH_CAPACITY];
    char *separator;
    FILE *file = NULL;
    int descriptor;
    int result = -1;

    if (stat(source, &status) != 0 || !S_ISDIR(status.st_mode)) {
        bp_error_set(error, error_size, "CPIO source is not a directory");
        return -1;
    }
    if (strlen(output) >= sizeof(output_copy)) {
        bp_error_set(error, error_size, "CPIO output path is too long");
        return -1;
    }
    strcpy(output_copy, output);
    separator = strrchr(output_copy, '/');
    if (separator == NULL) {
        strcpy(parent, ".");
    } else if (separator == output_copy) {
        strcpy(parent, "/");
    } else {
        *separator = '\0';
        strcpy(parent, output_copy);
    }
    if (bp_mkdir_p(parent, 0755, error, error_size) != 0) {
        return -1;
    }
    if (output_inside_source(source, output) != 0) {
        bp_error_set(error, error_size, "CPIO output cannot be inside its source directory");
        return -1;
    }
    if (collect_directory(source, "", status.st_dev, &paths, error, error_size) != 0) {
        goto cleanup;
    }
    qsort(paths.items, paths.count, sizeof(paths.items[0]), compare_path_items);
    if (snprintf(temporary, sizeof(temporary), "%s.tmp.%ld", output, (long)getpid()) < 0) {
        bp_error_set(error, error_size, "cannot build temporary CPIO path");
        goto cleanup;
    }
    descriptor = open(temporary, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (descriptor < 0 || (file = fdopen(descriptor, "wb")) == NULL) {
        bp_error_set(error, error_size, "create %s: %s", temporary, strerror(errno));
        if (descriptor >= 0) {
            close(descriptor);
        }
        goto cleanup;
    }
    if (write_archive(source, file, &paths, error, error_size) != 0 ||
        fflush(file) != 0 || fsync(fileno(file)) != 0 || fclose(file) != 0) {
        file = NULL;
        bp_error_set(error, error_size, "finish CPIO archive: %s", strerror(errno));
        goto cleanup;
    }
    file = NULL;
    if (bp_cpio_verify(temporary, info, error, error_size) != 0 ||
        rename(temporary, output) != 0 || bp_sync_parent(output, error, error_size) != 0) {
        if (error[0] == '\0') {
            bp_error_set(error, error_size, "install CPIO archive: %s", strerror(errno));
        }
        goto cleanup;
    }
    result = 0;

cleanup:
    if (file != NULL) {
        fclose(file);
    }
    if (result != 0 && temporary[0] != '\0') {
        unlink(temporary);
    }
    path_list_free(&paths);
    return result;
}

static int ensure_safe_parent(const char *destination, const char *relative,
                              char *output, size_t output_size,
                              char *error, size_t error_size)
{
    char copy[BP_PATH_CAPACITY];
    char current[BP_PATH_CAPACITY];
    char *component;
    char *save = NULL;
    char *last;

    if (strlen(relative) >= sizeof(copy) || strlen(destination) >= sizeof(current)) {
        bp_error_set(error, error_size, "extraction path is too long");
        return -1;
    }
    strcpy(copy, relative);
    last = strrchr(copy, '/');
    if (last != NULL) {
        *last = '\0';
    } else {
        copy[0] = '\0';
    }
    strcpy(current, destination);
    for (component = strtok_r(copy, "/", &save); component != NULL;
         component = strtok_r(NULL, "/", &save)) {
        struct stat status;
        size_t length = strlen(current);
        int written = snprintf(current + length, sizeof(current) - length, "/%s", component);
        if (written < 0 || (size_t)written >= sizeof(current) - length) {
            bp_error_set(error, error_size, "extraction path is too long");
            return -1;
        }
        if (lstat(current, &status) == 0) {
            if (!S_ISDIR(status.st_mode)) {
                bp_error_set(error, error_size, "extraction parent is not a directory: %s", current);
                return -1;
            }
        } else if (errno == ENOENT) {
            if (mkdir(current, 0755) != 0) {
                bp_error_set(error, error_size, "mkdir %s: %s", current, strerror(errno));
                return -1;
            }
        } else {
            bp_error_set(error, error_size, "inspect %s: %s", current, strerror(errno));
            return -1;
        }
    }
    return join_path(output, output_size, destination, relative, error, error_size);
}

static int directory_is_empty(const char *path)
{
    DIR *directory = opendir(path);
    struct dirent *entry;
    if (directory == NULL) {
        return 0;
    }
    while ((entry = readdir(directory)) != NULL) {
        if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0) {
            closedir(directory);
            return 0;
        }
    }
    closedir(directory);
    return 1;
}

static int copy_exact_to_fd(FILE *input, int output, uint32_t size)
{
    unsigned char buffer[65536];
    uint32_t remaining = size;
    while (remaining > 0U) {
        size_t wanted = remaining > sizeof(buffer) ? sizeof(buffer) : remaining;
        size_t count = fread(buffer, 1U, wanted, input);
        size_t offset = 0U;
        if (count != wanted) {
            return -1;
        }
        while (offset < count) {
            ssize_t written = write(output, buffer + offset, count - offset);
            if (written < 0 && errno == EINTR) {
                continue;
            }
            if (written <= 0) {
                return -1;
            }
            offset += (size_t)written;
        }
        remaining -= (uint32_t)count;
    }
    return read_padding(input, size);
}

int bp_cpio_extract(const char *archive, const char *destination,
                    struct bp_rootfs_info *info, char *error, size_t error_size)
{
    FILE *file = NULL;
    struct path_list directories = {0};
    struct path_list symlinks = {0};
    int result = -1;
    size_t index;

    if (bp_cpio_verify(archive, info, error, error_size) != 0 ||
        bp_mkdir_p(destination, 0755, error, error_size) != 0 ||
        !directory_is_empty(destination)) {
        if (error[0] == '\0') {
            bp_error_set(error, error_size, "extraction destination must be empty");
        }
        return -1;
    }
    file = fopen(archive, "rb");
    if (file == NULL) {
        bp_error_set(error, error_size, "open %s: %s", archive, strerror(errno));
        return -1;
    }
    for (;;) {
        struct newc_header header;
        char name[NEWC_NAME_LIMIT + 1U];
        char path[BP_PATH_CAPACITY];

        if (read_header(file, &header) != 0 || read_exact(file, name, header.name_size) != 0 ||
            name[header.name_size - 1U] != '\0' ||
            memchr(name, '\0', header.name_size - 1U) != NULL ||
            read_padding(file, NEWC_HEADER_SIZE + header.name_size) != 0) {
            bp_error_set(error, error_size, "truncated CPIO during extraction");
            goto cleanup;
        }
        if (strcmp(name, NEWC_TRAILER) == 0) {
            break;
        }
        if (!valid_archive_name(name) || !allowed_type(header.mode)) {
            bp_error_set(error, error_size, "unsafe CPIO entry during extraction");
            goto cleanup;
        }
        if (ensure_safe_parent(destination, name, path, sizeof(path),
                               error, error_size) != 0) {
            goto cleanup;
        }
        if (S_ISDIR(header.mode)) {
            struct stat status;
            if (mkdir(path, 0755) != 0 && errno != EEXIST) {
                bp_error_set(error, error_size, "mkdir %s: %s", path, strerror(errno));
                goto cleanup;
            }
            if (lstat(path, &status) != 0 || !S_ISDIR(status.st_mode) ||
                path_list_add(&directories, path, header.mode, header.uid, header.gid,
                              error, error_size) != 0 || skip_data(file, header.file_size) != 0) {
                bp_error_set(error, error_size, "extract directory %s", path);
                goto cleanup;
            }
        } else if (S_ISREG(header.mode)) {
            int descriptor = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
                                  header.mode & 07777U);
            if (descriptor < 0 || copy_exact_to_fd(file, descriptor, header.file_size) != 0 ||
                (geteuid() == 0 && fchown(descriptor, header.uid, header.gid) != 0) ||
                fchmod(descriptor, header.mode & 07777U) != 0 || fsync(descriptor) != 0) {
                bp_error_set(error, error_size, "extract file %s: %s", path, strerror(errno));
                if (descriptor >= 0) {
                    close(descriptor);
                }
                goto cleanup;
            }
            close(descriptor);
        } else if (S_ISLNK(header.mode)) {
            char *target = malloc((size_t)header.file_size + 1U);
            if (target == NULL || read_exact(file, target, header.file_size) != 0 ||
                read_padding(file, header.file_size) != 0) {
                free(target);
                bp_error_set(error, error_size, "extract symlink %s", path);
                goto cleanup;
            }
            target[header.file_size] = '\0';
            if (path_list_add(&symlinks, path, header.mode, 0U, 0U,
                              error, error_size) != 0) {
                free(target);
                goto cleanup;
            }
            symlinks.items[symlinks.count - 1U].target = target;
            if (symlinks.items[symlinks.count - 1U].target == NULL) {
                free(target);
                bp_error_set(error, error_size, "out of memory deferring symlink");
                goto cleanup;
            }
        }
    }
    for (index = 0U; index < symlinks.count; ++index) {
        if (symlink(symlinks.items[index].target, symlinks.items[index].path) != 0) {
            bp_error_set(error, error_size, "create symlink %s: %s",
                         symlinks.items[index].path, strerror(errno));
            goto cleanup;
        }
    }
    for (index = directories.count; index > 0U; --index) {
        struct path_item *directory = &directories.items[index - 1U];
        if ((geteuid() == 0 && chown(directory->path, directory->uid, directory->gid) != 0) ||
            chmod(directory->path, directory->mode & 07777U) != 0) {
            bp_error_set(error, error_size, "set directory metadata %s: %s",
                         directory->path, strerror(errno));
            goto cleanup;
        }
    }
    result = 0;

cleanup:
    if (file != NULL) {
        fclose(file);
    }
    path_list_free(&directories);
    path_list_free(&symlinks);
    return result;
}

int bp_rootfs_install(const char *root, const char *archive,
                      struct bp_rootfs_info *info, char *error, size_t error_size)
{
    char destination[BP_PATH_CAPACITY];
    char checksum_path[BP_PATH_CAPACITY];
    char digest[65];
    char checksum[128];
    int length;

    if (bp_rootfs_verify(archive, info, error, error_size) != 0 ||
        bp_path(destination, sizeof(destination), root, BP_ROOTFS_PATH,
                error, error_size) != 0 ||
        bp_path(checksum_path, sizeof(checksum_path), root, BP_ROOTFS_SHA256_PATH,
                error, error_size) != 0 ||
        bp_atomic_copy(archive, destination, 0600, error, error_size) != 0 ||
        bp_sha256_file(destination, digest, error, error_size) != 0) {
        return -1;
    }
    length = snprintf(checksum, sizeof(checksum), "%s  rootfs.cpio\n", digest);
    if (length < 0 || (size_t)length >= sizeof(checksum)) {
        bp_error_set(error, error_size, "cannot format rootfs checksum");
        return -1;
    }
    return bp_atomic_write(checksum_path, checksum, (size_t)length, 0600,
                           error, error_size);
}

int bp_rootfs_verify_installed(const char *root, struct bp_rootfs_info *info,
                               char *error, size_t error_size)
{
    char archive[BP_PATH_CAPACITY];
    char checksum_path[BP_PATH_CAPACITY];
    char actual[65];
    char *checksum = NULL;
    char expected[65];
    size_t index = 0U;

    if (bp_path(archive, sizeof(archive), root, BP_ROOTFS_PATH,
                error, error_size) != 0 ||
        bp_path(checksum_path, sizeof(checksum_path), root, BP_ROOTFS_SHA256_PATH,
                error, error_size) != 0 ||
        bp_read_text_file(checksum_path, &checksum, 1024U, error, error_size) != 0 ||
        bp_sha256_file(archive, actual, error, error_size) != 0) {
        free(checksum);
        return -1;
    }
    while (index < 64U && checksum[index] != '\0' && checksum[index] != ' ' &&
           checksum[index] != '\t' && checksum[index] != '\n') {
        expected[index] = checksum[index];
        ++index;
    }
    expected[index] = '\0';
    free(checksum);
    if (index != 64U || strcmp(expected, actual) != 0) {
        bp_error_set(error, error_size, "installed rootfs SHA-256 mismatch");
        return -1;
    }
    return bp_rootfs_verify(archive, info, error, error_size);
}
