#include "burning.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static unsigned long bp_temp_counter;

void bp_error_set(char *error, size_t error_size, const char *format, ...)
{
    va_list arguments;

    if (error == NULL || error_size == 0U) {
        return;
    }
    va_start(arguments, format);
    (void)vsnprintf(error, error_size, format, arguments);
    va_end(arguments);
}

int bp_path(char *output, size_t output_size, const char *root,
            const char *absolute_path, char *error, size_t error_size)
{
    int length;
    const char *relative;

    if (output == NULL || output_size == 0U || root == NULL || absolute_path == NULL) {
        bp_error_set(error, error_size, "invalid path arguments");
        return -1;
    }
    relative = absolute_path[0] == '/' ? absolute_path + 1 : absolute_path;
    if (strcmp(root, "/") == 0) {
        length = snprintf(output, output_size, "/%s", relative);
    } else {
        length = snprintf(output, output_size, "%s/%s", root, relative);
    }
    if (length < 0 || (size_t)length >= output_size) {
        bp_error_set(error, error_size, "path is too long: %s + %s", root, absolute_path);
        return -1;
    }
    return 0;
}

int bp_mkdir_p(const char *path, mode_t mode, char *error, size_t error_size)
{
    char buffer[BP_PATH_CAPACITY];
    size_t index;
    size_t length;

    if (path == NULL) {
        bp_error_set(error, error_size, "mkdir path is null");
        return -1;
    }
    length = strlen(path);
    if (length == 0U || length >= sizeof(buffer)) {
        bp_error_set(error, error_size, "mkdir path is invalid");
        return -1;
    }
    memcpy(buffer, path, length + 1U);
    for (index = 1U; index < length; ++index) {
        if (buffer[index] != '/') {
            continue;
        }
        buffer[index] = '\0';
        if (mkdir(buffer, mode) != 0 && errno != EEXIST) {
            bp_error_set(error, error_size, "mkdir %s: %s", buffer, strerror(errno));
            return -1;
        }
        buffer[index] = '/';
    }
    if (mkdir(buffer, mode) != 0 && errno != EEXIST) {
        bp_error_set(error, error_size, "mkdir %s: %s", buffer, strerror(errno));
        return -1;
    }
    return 0;
}

static int bp_parent_path(const char *path, char *parent, size_t parent_size,
                          char *error, size_t error_size)
{
    char *separator;
    size_t length;

    length = strlen(path);
    if (length == 0U || length >= parent_size) {
        bp_error_set(error, error_size, "invalid path: %s", path);
        return -1;
    }
    memcpy(parent, path, length + 1U);
    separator = strrchr(parent, '/');
    if (separator == NULL) {
        if (parent_size < 2U) {
            return -1;
        }
        strcpy(parent, ".");
    } else if (separator == parent) {
        separator[1] = '\0';
    } else {
        *separator = '\0';
    }
    return 0;
}

int bp_sync_parent(const char *path, char *error, size_t error_size)
{
    char parent[BP_PATH_CAPACITY];
    int descriptor;

    if (bp_parent_path(path, parent, sizeof(parent), error, error_size) != 0) {
        return -1;
    }
    descriptor = open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (descriptor < 0) {
        bp_error_set(error, error_size, "open directory %s: %s", parent, strerror(errno));
        return -1;
    }
    if (fsync(descriptor) != 0) {
        bp_error_set(error, error_size, "fsync directory %s: %s", parent, strerror(errno));
        close(descriptor);
        return -1;
    }
    close(descriptor);
    return 0;
}

static int bp_temp_path(const char *path, char *temporary, size_t temporary_size,
                        char *error, size_t error_size)
{
    char parent[BP_PATH_CAPACITY];
    const char *name;
    int length;

    if (bp_parent_path(path, parent, sizeof(parent), error, error_size) != 0) {
        return -1;
    }
    name = strrchr(path, '/');
    name = name == NULL ? path : name + 1;
    length = snprintf(temporary, temporary_size, "%s/.%s.tmp.%ld.%lu",
                      parent, name, (long)getpid(), bp_temp_counter++);
    if (length < 0 || (size_t)length >= temporary_size) {
        bp_error_set(error, error_size, "temporary path is too long");
        return -1;
    }
    return 0;
}

static int bp_write_all(int descriptor, const void *data, size_t size)
{
    const unsigned char *cursor = data;
    size_t remaining = size;

    while (remaining > 0U) {
        ssize_t count = write(descriptor, cursor, remaining);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            return -1;
        }
        cursor += (size_t)count;
        remaining -= (size_t)count;
    }
    return 0;
}

int bp_atomic_write(const char *path, const void *data, size_t size, mode_t mode,
                    char *error, size_t error_size)
{
    char parent[BP_PATH_CAPACITY];
    char temporary[BP_PATH_CAPACITY];
    int descriptor = -1;
    int result = -1;

    if (bp_parent_path(path, parent, sizeof(parent), error, error_size) != 0 ||
        bp_mkdir_p(parent, 0755, error, error_size) != 0 ||
        bp_temp_path(path, temporary, sizeof(temporary), error, error_size) != 0) {
        return -1;
    }
    descriptor = open(temporary, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, mode);
    if (descriptor < 0) {
        bp_error_set(error, error_size, "create %s: %s", temporary, strerror(errno));
        goto cleanup;
    }
    if (bp_write_all(descriptor, data, size) != 0 || fchmod(descriptor, mode) != 0 ||
        fsync(descriptor) != 0) {
        bp_error_set(error, error_size, "write %s: %s", temporary, strerror(errno));
        goto cleanup;
    }
    if (close(descriptor) != 0) {
        descriptor = -1;
        bp_error_set(error, error_size, "close %s: %s", temporary, strerror(errno));
        goto cleanup;
    }
    descriptor = -1;
    if (rename(temporary, path) != 0) {
        bp_error_set(error, error_size, "rename %s to %s: %s",
                     temporary, path, strerror(errno));
        goto cleanup;
    }
    if (bp_sync_parent(path, error, error_size) != 0) {
        goto cleanup;
    }
    result = 0;

cleanup:
    if (descriptor >= 0) {
        close(descriptor);
    }
    if (result != 0) {
        unlink(temporary);
    }
    return result;
}

int bp_atomic_copy(const char *source, const char *destination, mode_t mode,
                   char *error, size_t error_size)
{
    int source_fd = -1;
    int destination_fd = -1;
    char parent[BP_PATH_CAPACITY];
    char temporary[BP_PATH_CAPACITY];
    unsigned char buffer[65536];
    int result = -1;

    if (bp_parent_path(destination, parent, sizeof(parent), error, error_size) != 0 ||
        bp_mkdir_p(parent, 0755, error, error_size) != 0 ||
        bp_temp_path(destination, temporary, sizeof(temporary), error, error_size) != 0) {
        return -1;
    }
    source_fd = open(source, O_RDONLY | O_CLOEXEC);
    if (source_fd < 0) {
        bp_error_set(error, error_size, "open %s: %s", source, strerror(errno));
        goto cleanup;
    }
    destination_fd = open(temporary, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, mode);
    if (destination_fd < 0) {
        bp_error_set(error, error_size, "create %s: %s", temporary, strerror(errno));
        goto cleanup;
    }
    for (;;) {
        ssize_t count = read(source_fd, buffer, sizeof(buffer));
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count < 0 || (count > 0 && bp_write_all(destination_fd, buffer, (size_t)count) != 0)) {
            bp_error_set(error, error_size, "copy %s: %s", source, strerror(errno));
            goto cleanup;
        }
        if (count == 0) {
            break;
        }
    }
    if (fchmod(destination_fd, mode) != 0 || fsync(destination_fd) != 0) {
        bp_error_set(error, error_size, "sync %s: %s", temporary, strerror(errno));
        goto cleanup;
    }
    close(destination_fd);
    destination_fd = -1;
    if (rename(temporary, destination) != 0) {
        bp_error_set(error, error_size, "install %s: %s", destination, strerror(errno));
        goto cleanup;
    }
    if (bp_sync_parent(destination, error, error_size) != 0) {
        goto cleanup;
    }
    result = 0;

cleanup:
    if (source_fd >= 0) {
        close(source_fd);
    }
    if (destination_fd >= 0) {
        close(destination_fd);
    }
    if (result != 0) {
        unlink(temporary);
    }
    return result;
}

int bp_atomic_symlink(const char *target, const char *destination,
                      char *error, size_t error_size)
{
    char parent[BP_PATH_CAPACITY];
    char temporary[BP_PATH_CAPACITY];

    if (bp_parent_path(destination, parent, sizeof(parent), error, error_size) != 0 ||
        bp_mkdir_p(parent, 0755, error, error_size) != 0 ||
        bp_temp_path(destination, temporary, sizeof(temporary), error, error_size) != 0) {
        return -1;
    }
    if (symlink(target, temporary) != 0) {
        bp_error_set(error, error_size, "symlink %s: %s", temporary, strerror(errno));
        return -1;
    }
    if (rename(temporary, destination) != 0) {
        bp_error_set(error, error_size, "install symlink %s: %s", destination, strerror(errno));
        unlink(temporary);
        return -1;
    }
    return bp_sync_parent(destination, error, error_size);
}

int bp_atomic_hardlink(const char *source, const char *destination,
                       char *error, size_t error_size)
{
    char parent[BP_PATH_CAPACITY];
    char temporary[BP_PATH_CAPACITY];

    if (bp_parent_path(destination, parent, sizeof(parent), error, error_size) != 0 ||
        bp_temp_path(destination, temporary, sizeof(temporary), error, error_size) != 0) {
        return -1;
    }
    if (link(source, temporary) != 0) {
        bp_error_set(error, error_size, "hard link %s: %s", temporary, strerror(errno));
        return -1;
    }
    if (rename(temporary, destination) != 0) {
        bp_error_set(error, error_size, "install hard link %s: %s",
                     destination, strerror(errno));
        unlink(temporary);
        return -1;
    }
    return bp_sync_parent(destination, error, error_size);
}

int bp_remove_synced(const char *path, int *removed,
                     char *error, size_t error_size)
{
    if (removed != NULL) {
        *removed = 0;
    }
    if (unlink(path) != 0) {
        if (errno == ENOENT) {
            return 0;
        }
        bp_error_set(error, error_size, "remove %s: %s", path, strerror(errno));
        return -1;
    }
    if (removed != NULL) {
        *removed = 1;
    }
    return bp_sync_parent(path, error, error_size);
}

int bp_read_text_file(const char *path, char **contents, size_t max_size,
                      char *error, size_t error_size)
{
    struct stat status;
    int descriptor;
    char *buffer;
    size_t used = 0U;

    *contents = NULL;
    descriptor = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0) {
        bp_error_set(error, error_size, "open %s: %s", path, strerror(errno));
        return -1;
    }
    if (fstat(descriptor, &status) != 0 || !S_ISREG(status.st_mode) ||
        status.st_size < 0 || (uintmax_t)status.st_size > max_size) {
        bp_error_set(error, error_size, "invalid text file %s", path);
        close(descriptor);
        return -1;
    }
    buffer = malloc((size_t)status.st_size + 1U);
    if (buffer == NULL) {
        bp_error_set(error, error_size, "out of memory reading %s", path);
        close(descriptor);
        return -1;
    }
    while (used < (size_t)status.st_size) {
        ssize_t count = read(descriptor, buffer + used, (size_t)status.st_size - used);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            bp_error_set(error, error_size, "read %s: %s", path, strerror(errno));
            free(buffer);
            close(descriptor);
            return -1;
        }
        used += (size_t)count;
    }
    buffer[used] = '\0';
    close(descriptor);
    *contents = buffer;
    return 0;
}
