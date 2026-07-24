#include "burning.h"

#include <archive.h>
#include <archive_entry.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define BP_TAR_ENTRY_LIMIT 1000000U
#define BP_TAR_CONFIG_LIMIT 65536U

struct tar_item {
    char *path;
    char *hardlink;
    mode_t mode;
    size_t order;
};

struct tar_list {
    struct tar_item *items;
    size_t count;
    size_t capacity;
};

static void tar_list_free(struct tar_list *list)
{
    size_t index;
    for (index = 0U; index < list->count; ++index) {
        free(list->items[index].path);
        free(list->items[index].hardlink);
    }
    free(list->items);
    memset(list, 0, sizeof(*list));
}

static int tar_list_add(struct tar_list *list, const char *path,
                        const char *hardlink, mode_t mode, size_t order,
                        char *error, size_t error_size)
{
    struct tar_item *resized;
    size_t capacity;

    if (list->count >= BP_TAR_ENTRY_LIMIT) {
        bp_error_set(error, error_size, "too many tar entries");
        return -1;
    }
    if (list->count == list->capacity) {
        capacity = list->capacity == 0U ? 64U : list->capacity * 2U;
        resized = realloc(list->items, capacity * sizeof(*resized));
        if (resized == NULL) {
            bp_error_set(error, error_size, "out of memory collecting tar entries");
            return -1;
        }
        list->items = resized;
        list->capacity = capacity;
    }
    list->items[list->count].path = strdup(path);
    if (list->items[list->count].path == NULL) {
        bp_error_set(error, error_size, "out of memory copying tar path");
        return -1;
    }
    list->items[list->count].mode = mode;
    list->items[list->count].hardlink = NULL;
    if (hardlink != NULL) {
        list->items[list->count].hardlink = strdup(hardlink);
        if (list->items[list->count].hardlink == NULL) {
            free(list->items[list->count].path);
            bp_error_set(error, error_size,
                         "out of memory copying tar hardlink target");
            return -1;
        }
    }
    list->items[list->count].order = order;
    ++list->count;
    return 0;
}

static int compare_tar_items(const void *left, const void *right)
{
    const struct tar_item *left_item = left;
    const struct tar_item *right_item = right;
    return strcmp(left_item->path, right_item->path);
}

static const struct tar_item *tar_list_find(const struct tar_list *list,
                                            const char *path)
{
    struct tar_item key;
    key.path = (char *)path;
    key.hardlink = NULL;
    key.mode = 0;
    key.order = 0U;
    return bsearch(&key, list->items, list->count, sizeof(list->items[0]),
                   compare_tar_items);
}

static void set_archive_error(char *error, size_t error_size,
                              struct archive *archive, const char *action)
{
    const char *detail = archive_error_string(archive);
    bp_error_set(error, error_size, "%s: %s", action,
                 detail == NULL ? "archive error" : detail);
}

static int normalize_tar_path(const char *input, char output[BP_PATH_CAPACITY],
                              int *is_root, char *error, size_t error_size)
{
    const char *path = input;
    size_t length;
    size_t start;
    size_t index;

    if (path == NULL) {
        bp_error_set(error, error_size, "tar entry has no path");
        return -1;
    }
    while (path[0] == '.' && path[1] == '/') {
        path += 2;
    }
    length = strlen(path);
    while (length > 0U && path[length - 1U] == '/') {
        --length;
    }
    if (length == 0U || (length == 1U && path[0] == '.')) {
        *is_root = 1;
        output[0] = '\0';
        return 0;
    }
    if (path[0] == '/' || length >= BP_PATH_CAPACITY) {
        bp_error_set(error, error_size, "unsafe tar path: %s", input);
        return -1;
    }
    start = 0U;
    for (index = 0U; index <= length; ++index) {
        if (index == length || path[index] == '/') {
            size_t component_length = index - start;
            if (component_length == 0U ||
                (component_length == 1U && path[start] == '.') ||
                (component_length == 2U && path[start] == '.' &&
                 path[start + 1U] == '.')) {
                bp_error_set(error, error_size, "unsafe tar path: %s", input);
                return -1;
            }
            start = index + 1U;
        }
    }
    memcpy(output, path, length);
    output[length] = '\0';
    *is_root = 0;
    return 0;
}

static int allowed_tar_type(mode_t mode)
{
    return S_ISREG(mode) || S_ISDIR(mode) || S_ISLNK(mode);
}

static int path_is_within_prefix(const char *path, const char *prefix)
{
    size_t length = strlen(prefix);
    return strcmp(path, prefix) == 0 ||
           (strncmp(path, prefix, length) == 0 && path[length] == '/');
}

static int strip_tar_prefix(const char *path, const char *prefix,
                            char output[BP_PATH_CAPACITY], int *is_root,
                            char *error, size_t error_size)
{
    size_t prefix_length = strlen(prefix);
    const char *stripped = path;

    if (prefix_length != 0U) {
        if (!path_is_within_prefix(path, prefix)) {
            bp_error_set(error, error_size,
                         "tar path is outside the archive root: %s", path);
            return -1;
        }
        if (path[prefix_length] == '\0') {
            output[0] = '\0';
            *is_root = 1;
            return 0;
        }
        stripped = path + prefix_length + 1U;
    }
    if (strlen(stripped) >= BP_PATH_CAPACITY) {
        bp_error_set(error, error_size, "tar path is too long: %s", path);
        return -1;
    }
    strcpy(output, stripped);
    *is_root = 0;
    return 0;
}

static int apply_single_root_prefix(struct tar_list *paths,
                                    char prefix[BP_PATH_CAPACITY],
                                    char *error, size_t error_size)
{
    size_t candidate;
    size_t selected = SIZE_MAX;
    size_t index;

    prefix[0] = '\0';
    for (candidate = 0U; candidate < paths->count; ++candidate) {
        int contains_all = 1;
        if (!S_ISDIR(paths->items[candidate].mode) ||
            strchr(paths->items[candidate].path, '/') != NULL) {
            continue;
        }
        for (index = 0U; index < paths->count; ++index) {
            if (!path_is_within_prefix(paths->items[index].path,
                                       paths->items[candidate].path)) {
                contains_all = 0;
                break;
            }
        }
        if (contains_all) {
            selected = candidate;
            break;
        }
    }
    if (selected == SIZE_MAX) {
        return 0;
    }
    if (strlen(paths->items[selected].path) >= BP_PATH_CAPACITY) {
        bp_error_set(error, error_size, "tar root prefix is too long");
        return -1;
    }
    strcpy(prefix, paths->items[selected].path);
    for (index = 0U; index < paths->count; ++index) {
        char stripped[BP_PATH_CAPACITY];
        int is_root;
        if (strip_tar_prefix(paths->items[index].path, prefix, stripped,
                             &is_root, error, error_size) != 0) {
            return -1;
        }
        if (!is_root) {
            strcpy(paths->items[index].path, stripped);
        }
        if (paths->items[index].hardlink != NULL) {
            if (strip_tar_prefix(paths->items[index].hardlink, prefix, stripped,
                                 &is_root, error, error_size) != 0 || is_root) {
                bp_error_set(error, error_size,
                             "unsafe tar hardlink target: %s",
                             paths->items[index].hardlink);
                return -1;
            }
            strcpy(paths->items[index].hardlink, stripped);
        }
    }

    free(paths->items[selected].path);
    free(paths->items[selected].hardlink);
    if (selected + 1U < paths->count) {
        memmove(&paths->items[selected], &paths->items[selected + 1U],
                (paths->count - selected - 1U) * sizeof(paths->items[0]));
    }
    --paths->count;
    return 0;
}

static int runtime_config_candidate(const char *path)
{
    static const char config[] = "etc/burning-progress.conf";
    size_t path_length = strlen(path);
    size_t config_length = sizeof(config) - 1U;

    return strcmp(path, config) == 0 ||
           (path_length > config_length &&
            path[path_length - config_length - 1U] == '/' &&
            strcmp(path + path_length - config_length, config) == 0);
}

static int open_tar_reader(const char *path, struct archive **reader,
                           char *error, size_t error_size)
{
    struct archive *archive = archive_read_new();
    if (archive == NULL) {
        bp_error_set(error, error_size, "cannot allocate tar reader");
        return -1;
    }
    archive_read_support_filter_gzip(archive);
    archive_read_support_format_tar(archive);
    if (archive_read_open_filename(archive, path, 65536U) != ARCHIVE_OK) {
        set_archive_error(error, error_size, archive, "open tar.gz");
        archive_read_free(archive);
        return -1;
    }
    *reader = archive;
    return 0;
}

static int read_runtime_config(struct archive *reader, la_int64_t size,
                               char **text, char *error, size_t error_size)
{
    size_t offset = 0U;
    char *buffer;

    if (size < 0 || size > (la_int64_t)BP_TAR_CONFIG_LIMIT || *text != NULL) {
        bp_error_set(error, error_size, "invalid runtime configuration entry");
        return -1;
    }
    buffer = malloc((size_t)size + 1U);
    if (buffer == NULL) {
        bp_error_set(error, error_size, "out of memory reading runtime configuration");
        return -1;
    }
    while (offset < (size_t)size) {
        la_ssize_t count = archive_read_data(reader, buffer + offset,
                                             (size_t)size - offset);
        if (count <= 0) {
            free(buffer);
            set_archive_error(error, error_size, reader,
                              "read runtime configuration");
            return -1;
        }
        offset += (size_t)count;
    }
    if (memchr(buffer, '\0', (size_t)size) != NULL) {
        free(buffer);
        bp_error_set(error, error_size, "runtime configuration contains NUL");
        return -1;
    }
    buffer[size] = '\0';
    *text = buffer;
    return 0;
}

static int verify_tar_hardlinks(const struct tar_list *paths,
                                char *error, size_t error_size)
{
    size_t index;
    for (index = 0U; index < paths->count; ++index) {
        const struct tar_item *item = &paths->items[index];
        const struct tar_item *target;
        if (item->hardlink == NULL) {
            continue;
        }
        target = tar_list_find(paths, item->hardlink);
        if (target == NULL || !S_ISREG(target->mode) ||
            target->hardlink != NULL || target->order >= item->order) {
            bp_error_set(error, error_size,
                         "unsafe tar hardlink: %s -> %s",
                         item->path, item->hardlink);
            return -1;
        }
    }
    return 0;
}

static int bp_tar_gzip_verify_internal(const char *path,
                                       struct bp_rootfs_info *info,
                                       int require_recovery_rootfs,
                                       char prefix[BP_PATH_CAPACITY],
                                       char *error, size_t error_size)
{
    struct archive *reader = NULL;
    struct archive_entry *entry;
    struct tar_list paths = {0};
    char *runtime_text = NULL;
    char runtime_path[BP_PATH_CAPACITY] = {0};
    uint64_t data_bytes = 0U;
    int result = -1;
    int status;
    size_t index;

    if (open_tar_reader(path, &reader, error, error_size) != 0) {
        return -1;
    }
    while ((status = archive_read_next_header(reader, &entry)) == ARCHIVE_OK) {
        char normalized[BP_PATH_CAPACITY];
        char normalized_hardlink[BP_PATH_CAPACITY];
        const char *hardlink = archive_entry_hardlink(entry);
        const char *stored_hardlink = NULL;
        const char *symlink = archive_entry_symlink(entry);
        mode_t mode = archive_entry_mode(entry);
        mode_t stored_mode = mode;
        la_int64_t size = archive_entry_size(entry);
        uint64_t stored_size = 0U;
        int is_root;
        int hardlink_is_root;

        if (normalize_tar_path(archive_entry_pathname(entry), normalized,
                               &is_root, error, error_size) != 0) {
            goto cleanup;
        }
        if (is_root) {
            if (archive_read_data_skip(reader) != ARCHIVE_OK) {
                set_archive_error(error, error_size, reader, "skip tar root entry");
                goto cleanup;
            }
            continue;
        }
        if (hardlink != NULL) {
            if (normalize_tar_path(hardlink, normalized_hardlink,
                                   &hardlink_is_root, error, error_size) != 0 ||
                hardlink_is_root) {
                bp_error_set(error, error_size,
                             "unsafe tar hardlink target: %s", hardlink);
                goto cleanup;
            }
            stored_hardlink = normalized_hardlink;
            if ((mode & S_IFMT) == 0) {
                stored_mode |= S_IFREG;
            }
        }
        if (!allowed_tar_type(stored_mode) ||
            (hardlink != NULL && (!S_ISREG(stored_mode) || size > 0)) ||
            (hardlink == NULL && S_ISREG(mode) && size < 0) ||
            (S_ISDIR(mode) && size != 0) ||
            (S_ISLNK(mode) && (symlink == NULL || symlink[0] == '\0'))) {
            bp_error_set(error, error_size,
                         "unsupported tar entry: %s (mode=%o size=%lld hardlink=%s)",
                         normalized, (unsigned int)mode, (long long)size,
                         hardlink == NULL ? "none" : hardlink);
            goto cleanup;
        }
        if (tar_list_add(&paths, normalized, stored_hardlink, stored_mode,
                         paths.count, error, error_size) != 0) {
            goto cleanup;
        }
        if (S_ISREG(mode) && hardlink == NULL) {
            stored_size = (uint64_t)size;
        } else if (S_ISLNK(mode)) {
            stored_size = strlen(symlink);
        }
        if (UINT64_MAX - data_bytes < stored_size) {
            bp_error_set(error, error_size, "tar data size overflow");
            goto cleanup;
        }
        data_bytes += stored_size;
        if (require_recovery_rootfs && runtime_config_candidate(normalized)) {
            if (!S_ISREG(mode) || hardlink != NULL ||
                read_runtime_config(reader, size, &runtime_text,
                                    error, error_size) != 0) {
                goto cleanup;
            }
            strcpy(runtime_path, normalized);
        } else if (archive_read_data_skip(reader) != ARCHIVE_OK) {
            set_archive_error(error, error_size, reader, "read tar entry");
            goto cleanup;
        }
    }
    if (status != ARCHIVE_EOF) {
        set_archive_error(error, error_size, reader, "read tar.gz");
        goto cleanup;
    }
    if (archive_filter_code(reader, 0) != ARCHIVE_FILTER_GZIP ||
        (archive_format(reader) & ARCHIVE_FORMAT_BASE_MASK) != ARCHIVE_FORMAT_TAR) {
        bp_error_set(error, error_size, "archive is not gzip-compressed tar");
        goto cleanup;
    }
    if (apply_single_root_prefix(&paths, prefix, error, error_size) != 0) {
        goto cleanup;
    }
    if (runtime_text != NULL) {
        char stripped_runtime[BP_PATH_CAPACITY];
        int runtime_is_root;
        if (strip_tar_prefix(runtime_path, prefix, stripped_runtime,
                             &runtime_is_root, error, error_size) != 0) {
            goto cleanup;
        }
        if (runtime_is_root ||
            strcmp(stripped_runtime, "etc/burning-progress.conf") != 0) {
            free(runtime_text);
            runtime_text = NULL;
        }
    }
    qsort(paths.items, paths.count, sizeof(paths.items[0]), compare_tar_items);
    for (index = 1U; index < paths.count; ++index) {
        if (strcmp(paths.items[index - 1U].path, paths.items[index].path) == 0) {
            bp_error_set(error, error_size, "duplicate tar path: %s",
                         paths.items[index].path);
            goto cleanup;
        }
    }
    if (verify_tar_hardlinks(&paths, error, error_size) != 0) {
        goto cleanup;
    }
    if (require_recovery_rootfs) {
        const struct tar_item *shell = tar_list_find(&paths, "bin/sh");
        const struct tar_item *progress = tar_list_find(&paths, "sbin/burning-progress");
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
    if (require_recovery_rootfs && info->runtime.entry_mode == BP_ENTRY_HANDOFF) {
        const struct tar_item *entry_item =
            tar_list_find(&paths, info->runtime.entry + 1);
        if (entry_item == NULL || !S_ISREG(entry_item->mode)) {
            bp_error_set(error, error_size,
                         "handoff entry is missing or not a regular file");
            goto cleanup;
        }
    }
    info->format = BP_ROOTFS_TAR_GZIP;
    info->entries = paths.count;
    info->data_bytes = data_bytes;
    result = 0;

cleanup:
    free(runtime_text);
    tar_list_free(&paths);
    if (reader != NULL) {
        archive_read_free(reader);
    }
    return result;
}

int bp_tar_gzip_verify(const char *path, struct bp_rootfs_info *info,
                       char *error, size_t error_size)
{
    char prefix[BP_PATH_CAPACITY];
    return bp_tar_gzip_verify_internal(path, info, 1, prefix,
                                       error, error_size);
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

static int bp_tar_gzip_extract_internal(const char *path, const char *destination,
                                        struct bp_rootfs_info *info,
                                        int require_recovery_rootfs,
                                        char *error, size_t error_size)
{
    struct archive *reader = NULL;
    struct archive *disk = NULL;
    struct archive_entry *entry;
    char prefix[BP_PATH_CAPACITY];
    int original_directory = -1;
    int destination_directory = -1;
    int changed_directory = 0;
    int status;
    int result = -1;

    if (bp_tar_gzip_verify_internal(path, info, require_recovery_rootfs,
                                    prefix, error, error_size) != 0 ||
        bp_mkdir_p(destination, 0755, error, error_size) != 0 ||
        !directory_is_empty(destination)) {
        if (error[0] == '\0') {
            bp_error_set(error, error_size, "extraction destination must be empty");
        }
        return -1;
    }
    if (open_tar_reader(path, &reader, error, error_size) != 0) {
        return -1;
    }
    disk = archive_write_disk_new();
    if (disk == NULL) {
        bp_error_set(error, error_size, "cannot allocate tar extractor");
        goto cleanup;
    }
    status = ARCHIVE_EXTRACT_PERM | ARCHIVE_EXTRACT_TIME |
             ARCHIVE_EXTRACT_SECURE_SYMLINKS |
             ARCHIVE_EXTRACT_SECURE_NODOTDOT |
             ARCHIVE_EXTRACT_SECURE_NOABSOLUTEPATHS;
    if (geteuid() == 0) {
        status |= ARCHIVE_EXTRACT_OWNER;
    }
    if (archive_write_disk_set_options(disk, status) != ARCHIVE_OK) {
        set_archive_error(error, error_size, disk, "configure tar extractor");
        goto cleanup;
    }
    archive_write_disk_set_standard_lookup(disk);
    original_directory = open(".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    destination_directory = open(destination, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (original_directory < 0 || destination_directory < 0 ||
        fchdir(destination_directory) != 0) {
        bp_error_set(error, error_size, "enter extraction destination: %s",
                     strerror(errno));
        goto cleanup;
    }
    changed_directory = 1;
    while ((status = archive_read_next_header(reader, &entry)) == ARCHIVE_OK) {
        char archive_path[BP_PATH_CAPACITY];
        char normalized[BP_PATH_CAPACITY];
        const char *hardlink = archive_entry_hardlink(entry);
        int is_root;
        if (normalize_tar_path(archive_entry_pathname(entry), archive_path,
                               &is_root, error, error_size) != 0) {
            goto cleanup;
        }
        if (is_root) {
            if (archive_read_data_skip(reader) != ARCHIVE_OK) {
                set_archive_error(error, error_size, reader, "skip tar root entry");
                goto cleanup;
            }
            continue;
        }
        if (strip_tar_prefix(archive_path, prefix, normalized, &is_root,
                             error, error_size) != 0) {
            goto cleanup;
        }
        if (is_root) {
            if (archive_read_data_skip(reader) != ARCHIVE_OK) {
                set_archive_error(error, error_size, reader,
                                  "skip tar root prefix");
                goto cleanup;
            }
            continue;
        }
        if (hardlink != NULL) {
            char archive_target[BP_PATH_CAPACITY];
            char normalized_target[BP_PATH_CAPACITY];
            int target_is_root;
            if (normalize_tar_path(hardlink, archive_target, &target_is_root,
                                   error, error_size) != 0 || target_is_root ||
                strip_tar_prefix(archive_target, prefix, normalized_target,
                                 &target_is_root, error, error_size) != 0 ||
                target_is_root) {
                bp_error_set(error, error_size,
                             "unsafe tar hardlink target: %s", hardlink);
                goto cleanup;
            }
            archive_entry_set_hardlink(entry, normalized_target);
        }
        archive_entry_set_pathname(entry, normalized);
        if (archive_read_extract2(reader, entry, disk) != ARCHIVE_OK) {
            set_archive_error(error, error_size, disk, "extract tar entry");
            goto cleanup;
        }
    }
    if (status != ARCHIVE_EOF) {
        set_archive_error(error, error_size, reader, "extract tar.gz");
        goto cleanup;
    }
    if (archive_write_close(disk) != ARCHIVE_OK) {
        set_archive_error(error, error_size, disk, "finish tar extraction");
        goto cleanup;
    }
    result = 0;

cleanup:
    if (changed_directory && fchdir(original_directory) != 0) {
        bp_error_set(error, error_size, "restore working directory: %s",
                     strerror(errno));
        result = -1;
    }
    if (destination_directory >= 0) {
        close(destination_directory);
    }
    if (original_directory >= 0) {
        close(original_directory);
    }
    if (disk != NULL) {
        archive_write_free(disk);
    }
    if (reader != NULL) {
        archive_read_free(reader);
    }
    return result;
}

int bp_tar_gzip_extract(const char *path, const char *destination,
                        struct bp_rootfs_info *info, char *error, size_t error_size)
{
    return bp_tar_gzip_extract_internal(path, destination, info, 1,
                                        error, error_size);
}

int bp_tar_gzip_unpack(const char *path, const char *destination,
                       struct bp_rootfs_info *info, char *error, size_t error_size)
{
    return bp_tar_gzip_extract_internal(path, destination, info, 0,
                                        error, error_size);
}

static int prepare_output_path(const char *source, const char *output,
                               char source_path[BP_PATH_CAPACITY],
                               char output_path[BP_PATH_CAPACITY],
                               char *error, size_t error_size)
{
    char output_copy[BP_PATH_CAPACITY];
    char parent[BP_PATH_CAPACITY];
    char canonical_parent[BP_PATH_CAPACITY];
    char *separator;
    const char *name;
    int written;
    size_t source_length;

    if (realpath(source, source_path) == NULL || strlen(output) >= sizeof(output_copy)) {
        bp_error_set(error, error_size, "invalid tar source or output path");
        return -1;
    }
    strcpy(output_copy, output);
    separator = strrchr(output_copy, '/');
    if (separator == NULL) {
        strcpy(parent, ".");
        name = output_copy;
    } else if (separator == output_copy) {
        strcpy(parent, "/");
        name = separator + 1;
    } else {
        *separator = '\0';
        strcpy(parent, output_copy);
        name = separator + 1;
    }
    if (name[0] == '\0' || bp_mkdir_p(parent, 0755, error, error_size) != 0 ||
        realpath(parent, canonical_parent) == NULL) {
        bp_error_set(error, error_size, "invalid tar output path");
        return -1;
    }
    written = snprintf(output_path, BP_PATH_CAPACITY, "%s/%s",
                       canonical_parent, name);
    if (written < 0 || (size_t)written >= BP_PATH_CAPACITY) {
        bp_error_set(error, error_size, "tar output path is too long");
        return -1;
    }
    source_length = strlen(source_path);
    if (strncmp(output_path, source_path, source_length) == 0 &&
        (output_path[source_length] == '\0' || output_path[source_length] == '/')) {
        bp_error_set(error, error_size, "tar output cannot be inside its source directory");
        return -1;
    }
    return 0;
}

int bp_tar_gzip_pack(const char *source, const char *output_path,
                     struct bp_rootfs_info *info, char *error, size_t error_size)
{
    struct archive *disk = NULL;
    struct archive *writer = NULL;
    struct archive_entry *entry;
    char source_path[BP_PATH_CAPACITY];
    char output[BP_PATH_CAPACITY];
    char temporary[BP_PATH_CAPACITY] = {0};
    char normalized[BP_PATH_CAPACITY];
    int original_directory = -1;
    int changed_directory = 0;
    int output_descriptor = -1;
    int status;
    int result = -1;
    int written;

    if (prepare_output_path(source, output_path, source_path, output,
                            error, error_size) != 0) {
        return -1;
    }
    written = snprintf(temporary, sizeof(temporary), "%s.tmp.%ld", output,
                       (long)getpid());
    if (written < 0 || (size_t)written >= sizeof(temporary)) {
        bp_error_set(error, error_size, "temporary tar output path is too long");
        return -1;
    }
    output_descriptor = open(temporary, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (output_descriptor < 0) {
        bp_error_set(error, error_size, "create %s: %s", temporary, strerror(errno));
        return -1;
    }
    disk = archive_read_disk_new();
    writer = archive_write_new();
    if (disk == NULL || writer == NULL) {
        bp_error_set(error, error_size, "cannot allocate tar packer");
        goto cleanup;
    }
    archive_read_disk_set_symlink_physical(disk);
    archive_read_disk_set_behavior(disk,
        ARCHIVE_READDISK_NO_TRAVERSE_MOUNTS | ARCHIVE_READDISK_NO_XATTR |
        ARCHIVE_READDISK_NO_ACL | ARCHIVE_READDISK_NO_FFLAGS |
        ARCHIVE_READDISK_NO_SPARSE);
    archive_read_disk_set_standard_lookup(disk);
    if (archive_write_add_filter_gzip(writer) != ARCHIVE_OK ||
        archive_write_set_format_pax_restricted(writer) != ARCHIVE_OK ||
        archive_write_open_fd(writer, output_descriptor) != ARCHIVE_OK) {
        set_archive_error(error, error_size, writer, "configure tar.gz writer");
        goto cleanup;
    }
    original_directory = open(".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (original_directory < 0 || chdir(source_path) != 0) {
        bp_error_set(error, error_size, "enter tar source: %s", strerror(errno));
        goto cleanup;
    }
    changed_directory = 1;
    if (archive_read_disk_open(disk, ".") != ARCHIVE_OK) {
        set_archive_error(error, error_size, disk, "open tar source");
        goto cleanup;
    }
    while ((status = archive_read_next_header(disk, &entry)) == ARCHIVE_OK) {
        int can_descend = archive_read_disk_can_descend(disk);
        int is_root;
        mode_t mode = archive_entry_mode(entry);
        const char *hardlink = archive_entry_hardlink(entry);

        if (can_descend) {
            archive_read_disk_descend(disk);
        }
        if (normalize_tar_path(archive_entry_pathname(entry), normalized,
                               &is_root, error, error_size) != 0) {
            goto cleanup;
        }
        if (is_root) {
            continue;
        }
        if (!allowed_tar_type(mode) || hardlink != NULL) {
            bp_error_set(error, error_size, "unsupported source entry: %s", normalized);
            goto cleanup;
        }
        archive_entry_set_pathname(entry, normalized);
        archive_entry_set_uname(entry, NULL);
        archive_entry_set_gname(entry, NULL);
        archive_entry_acl_clear(entry);
        archive_entry_xattr_clear(entry);
        archive_entry_set_fflags(entry, 0UL, 0UL);
        if (archive_write_header(writer, entry) != ARCHIVE_OK) {
            set_archive_error(error, error_size, writer, "write tar header");
            goto cleanup;
        }
        if (S_ISREG(mode)) {
            const void *buffer;
            size_t size;
            la_int64_t offset;
            while ((status = archive_read_data_block(disk, &buffer, &size, &offset)) ==
                   ARCHIVE_OK) {
                (void)offset;
                if (archive_write_data(writer, buffer, size) != (la_ssize_t)size) {
                    set_archive_error(error, error_size, writer, "write tar data");
                    goto cleanup;
                }
            }
            if (status != ARCHIVE_EOF) {
                set_archive_error(error, error_size, disk, "read source file");
                goto cleanup;
            }
        }
        if (archive_write_finish_entry(writer) != ARCHIVE_OK) {
            set_archive_error(error, error_size, writer, "finish tar entry");
            goto cleanup;
        }
    }
    if (status != ARCHIVE_EOF || archive_write_close(writer) != ARCHIVE_OK ||
        fsync(output_descriptor) != 0) {
        set_archive_error(error, error_size, writer, "finish tar.gz archive");
        goto cleanup;
    }
    if (changed_directory && fchdir(original_directory) != 0) {
        bp_error_set(error, error_size, "restore working directory: %s", strerror(errno));
        goto cleanup;
    }
    changed_directory = 0;
    if (close(output_descriptor) != 0) {
        output_descriptor = -1;
        bp_error_set(error, error_size, "close tar.gz archive: %s", strerror(errno));
        goto cleanup;
    }
    output_descriptor = -1;
    if (bp_tar_gzip_verify(temporary, info, error, error_size) != 0 ||
        rename(temporary, output) != 0 ||
        bp_sync_parent(output, error, error_size) != 0) {
        if (error[0] == '\0') {
            bp_error_set(error, error_size, "install tar.gz archive: %s",
                         strerror(errno));
        }
        goto cleanup;
    }
    result = 0;

cleanup:
    if (changed_directory && original_directory >= 0) {
        if (fchdir(original_directory) != 0 && error[0] == '\0') {
            bp_error_set(error, error_size, "restore working directory: %s",
                         strerror(errno));
        }
    }
    if (original_directory >= 0) {
        close(original_directory);
    }
    if (writer != NULL) {
        archive_write_free(writer);
    }
    if (disk != NULL) {
        archive_read_free(disk);
    }
    if (output_descriptor >= 0) {
        close(output_descriptor);
    }
    if (result != 0 && temporary[0] != '\0') {
        unlink(temporary);
    }
    return result;
}
