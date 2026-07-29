#include "burning.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

enum backup_kind {
    BACKUP_SYMLINK,
    BACKUP_REGULAR
};

struct install_state {
    enum backup_kind backup_kind;
    char init_sha256[65];
    char progress_sha256[65];
};

static int path_for(char output[BP_PATH_CAPACITY], const char *root, const char *path,
                    char *error, size_t error_size)
{
    return bp_path(output, BP_PATH_CAPACITY, root, path, error, error_size);
}

static int current_init_is_dispatcher(const char *root)
{
    char path[BP_PATH_CAPACITY];
    char error[BP_ERROR_CAPACITY];
    char target[BP_PATH_CAPACITY];
    struct stat status;
    ssize_t length;

    if (path_for(path, root, BP_INIT_PATH, error, sizeof(error)) != 0 ||
        lstat(path, &status) != 0 || !S_ISLNK(status.st_mode)) {
        return 0;
    }
    length = readlink(path, target, sizeof(target) - 1U);
    if (length < 0) {
        return 0;
    }
    target[length] = '\0';
    return strcmp(target, BP_BURNING_INIT_PATH) == 0;
}

static int backup_original(const char *root, enum backup_kind *kind,
                           char *error, size_t error_size)
{
    char init[BP_PATH_CAPACITY];
    char backup[BP_PATH_CAPACITY];
    struct stat status;

    if (path_for(init, root, BP_INIT_PATH, error, error_size) != 0 ||
        path_for(backup, root, BP_ORIGINAL_INIT_PATH, error, error_size) != 0) {
        return -1;
    }
    if (lstat(backup, &status) == 0 || errno != ENOENT) {
        bp_error_set(error, error_size, "original init backup already exists: %s", backup);
        return -1;
    }
    if (lstat(init, &status) != 0) {
        bp_error_set(error, error_size, "inspect %s: %s", init, strerror(errno));
        return -1;
    }
    if (S_ISLNK(status.st_mode)) {
        char target[BP_PATH_CAPACITY];
        ssize_t length = readlink(init, target, sizeof(target) - 1U);
        if (length < 0) {
            bp_error_set(error, error_size, "readlink %s: %s", init, strerror(errno));
            return -1;
        }
        target[length] = '\0';
        if (bp_atomic_symlink(target, backup, error, error_size) != 0) {
            return -1;
        }
        *kind = BACKUP_SYMLINK;
    } else if (S_ISREG(status.st_mode)) {
        if (bp_atomic_hardlink(init, backup, error, error_size) != 0) {
            return -1;
        }
        *kind = BACKUP_REGULAR;
    } else {
        bp_error_set(error, error_size, "original init is not a file or symlink");
        return -1;
    }
    return 0;
}

static int format_state(const struct install_state *state, char *output, size_t size,
                        char *error, size_t error_size)
{
    int length = snprintf(output, size,
                          "version=1\nprogramVersion=%s\ninitTarget=%s\n"
                          "backupKind=%s\nburningInitSha256=%s\n"
                          "burningProgressSha256=%s\n",
                          BP_VERSION, BP_BURNING_INIT_PATH,
                          state->backup_kind == BACKUP_SYMLINK ? "symlink" : "regular",
                          state->init_sha256, state->progress_sha256);
    if (length < 0 || (size_t)length >= size) {
        bp_error_set(error, error_size, "install-state output is too large");
        return -1;
    }
    return 0;
}

static int extract_state_value(const char *text, const char *key,
                               char *output, size_t output_size)
{
    size_t key_length = strlen(key);
    const char *line = text;

    while (*line != '\0') {
        const char *end = strchr(line, '\n');
        size_t line_length = end == NULL ? strlen(line) : (size_t)(end - line);
        if (line_length > key_length + 1U && memcmp(line, key, key_length) == 0 &&
            line[key_length] == '=') {
            size_t value_length = line_length - key_length - 1U;
            if (value_length >= output_size) {
                return -1;
            }
            memcpy(output, line + key_length + 1U, value_length);
            output[value_length] = '\0';
            return 0;
        }
        if (end == NULL) {
            break;
        }
        line = end + 1;
    }
    return -1;
}

static int read_state(const char *root, struct install_state *state,
                      char *error, size_t error_size)
{
    char path[BP_PATH_CAPACITY];
    char *text = NULL;
    char value[128];

    if (path_for(path, root, BP_INSTALL_STATE_PATH, error, error_size) != 0 ||
        bp_read_text_file(path, &text, 65536U, error, error_size) != 0) {
        return -1;
    }
    if (extract_state_value(text, "version", value, sizeof(value)) != 0 ||
        strcmp(value, "1") != 0 ||
        extract_state_value(text, "initTarget", value, sizeof(value)) != 0 ||
        strcmp(value, BP_BURNING_INIT_PATH) != 0 ||
        extract_state_value(text, "backupKind", value, sizeof(value)) != 0) {
        bp_error_set(error, error_size, "invalid install-state");
        free(text);
        return -1;
    }
    if (strcmp(value, "symlink") == 0) {
        state->backup_kind = BACKUP_SYMLINK;
    } else if (strcmp(value, "regular") == 0) {
        state->backup_kind = BACKUP_REGULAR;
    } else {
        bp_error_set(error, error_size, "invalid install-state backup kind");
        free(text);
        return -1;
    }
    if (extract_state_value(text, "burningInitSha256", state->init_sha256,
                            sizeof(state->init_sha256)) != 0 ||
        strlen(state->init_sha256) != 64U ||
        extract_state_value(text, "burningProgressSha256", state->progress_sha256,
                            sizeof(state->progress_sha256)) != 0 ||
        strlen(state->progress_sha256) != 64U) {
        bp_error_set(error, error_size, "invalid install-state digest");
        free(text);
        return -1;
    }
    free(text);
    return 0;
}

int bp_install(const char *root, const char *init_source, const char *progress_source,
               char *error, size_t error_size)
{
    char init_destination[BP_PATH_CAPACITY];
    char progress_destination[BP_PATH_CAPACITY];
    char init_path[BP_PATH_CAPACITY];
    char state_path[BP_PATH_CAPACITY];
    char state_text[512];
    struct install_state state;
    struct stat status;
    int backup_created = 0;
    int config_created = 0;
    int removed;
    int result = -1;

    if (current_init_is_dispatcher(root)) {
        bp_error_set(error, error_size, "burning-progress is already installed");
        return -1;
    }
    if (stat(init_source, &status) != 0 || !S_ISREG(status.st_mode) || status.st_size == 0 ||
        stat(progress_source, &status) != 0 || !S_ISREG(status.st_mode) || status.st_size == 0) {
        bp_error_set(error, error_size, "install sources must be non-empty regular files");
        return -1;
    }
    if (path_for(init_destination, root, BP_BURNING_INIT_PATH, error, error_size) != 0 ||
        path_for(progress_destination, root, BP_PROGRESS_PATH, error, error_size) != 0 ||
        path_for(init_path, root, BP_INIT_PATH, error, error_size) != 0 ||
        path_for(state_path, root, BP_INSTALL_STATE_PATH, error, error_size) != 0) {
        return -1;
    }
    if (lstat(init_destination, &status) == 0 || errno != ENOENT ||
        lstat(progress_destination, &status) == 0 || errno != ENOENT ||
        lstat(state_path, &status) == 0 || errno != ENOENT) {
        bp_error_set(error, error_size, "installation destination already exists");
        return -1;
    }
    if (bp_atomic_copy(init_source, init_destination, 0755, error, error_size) != 0 ||
        bp_atomic_copy(progress_source, progress_destination, 0755, error, error_size) != 0) {
        goto rollback;
    }
    if (backup_original(root, &state.backup_kind, error, error_size) != 0) {
        goto rollback;
    }
    backup_created = 1;
    if (bp_sha256_file(init_destination, state.init_sha256, error, error_size) != 0 ||
        bp_sha256_file(progress_destination, state.progress_sha256, error, error_size) != 0 ||
        format_state(&state, state_text, sizeof(state_text), error, error_size) != 0 ||
        bp_atomic_write(state_path, state_text, strlen(state_text), 0600,
                        error, error_size) != 0) {
        goto rollback;
    }
    if (bp_host_config_ensure(root, &config_created, error, error_size) != 0) {
        goto rollback;
    }
    if (bp_atomic_symlink(BP_BURNING_INIT_PATH, init_path, error, error_size) != 0) {
        goto rollback;
    }
    result = 0;

rollback:
    if (result != 0) {
        char backup_path[BP_PATH_CAPACITY];
        char config_path[BP_PATH_CAPACITY];
        if (backup_created &&
            path_for(backup_path, root, BP_ORIGINAL_INIT_PATH, error, error_size) == 0) {
            (void)bp_remove_synced(backup_path, &removed, error, error_size);
        }
        (void)bp_remove_synced(state_path, &removed, error, error_size);
        (void)bp_remove_synced(progress_destination, &removed, error, error_size);
        (void)bp_remove_synced(init_destination, &removed, error, error_size);
        if (config_created &&
            path_for(config_path, root, BP_HOST_CONFIG_PATH, error, error_size) == 0) {
            (void)bp_remove_synced(config_path, &removed, error, error_size);
        }
    }
    return result;
}

int bp_uninstall(const char *root, char *error, size_t error_size)
{
    struct install_state state;
    char init[BP_PATH_CAPACITY];
    char backup[BP_PATH_CAPACITY];
    char burning_init[BP_PATH_CAPACITY];
    char progress[BP_PATH_CAPACITY];
    char state_path[BP_PATH_CAPACITY];
    char enable[BP_PATH_CAPACITY];
    char digest[65];
    struct stat status;
    int removed;

    if (read_state(root, &state, error, error_size) != 0 ||
        !current_init_is_dispatcher(root)) {
        if (error[0] == '\0') {
            bp_error_set(error, error_size, "/sbin/init is not the installed dispatcher");
        }
        return -1;
    }
    if (path_for(init, root, BP_INIT_PATH, error, error_size) != 0 ||
        path_for(backup, root, BP_ORIGINAL_INIT_PATH, error, error_size) != 0 ||
        path_for(burning_init, root, BP_BURNING_INIT_PATH, error, error_size) != 0 ||
        path_for(progress, root, BP_PROGRESS_PATH, error, error_size) != 0 ||
        path_for(state_path, root, BP_INSTALL_STATE_PATH, error, error_size) != 0 ||
        path_for(enable, root, BP_ENABLE_PATH, error, error_size) != 0) {
        return -1;
    }
    if (bp_sha256_file(burning_init, digest, error, error_size) != 0 ||
        strcmp(digest, state.init_sha256) != 0) {
        bp_error_set(error, error_size, "installed burning-init changed after installation");
        return -1;
    }
    if (lstat(backup, &status) != 0 ||
        (state.backup_kind == BACKUP_SYMLINK && !S_ISLNK(status.st_mode)) ||
        (state.backup_kind == BACKUP_REGULAR && !S_ISREG(status.st_mode))) {
        bp_error_set(error, error_size, "original init backup is missing or changed");
        return -1;
    }
    if (rename(backup, init) != 0) {
        bp_error_set(error, error_size, "restore original init: %s", strerror(errno));
        return -1;
    }
    if (bp_sync_parent(init, error, error_size) != 0) {
        return -1;
    }
    (void)bp_remove_synced(enable, &removed, error, error_size);
    (void)bp_remove_synced(burning_init, &removed, error, error_size);
    if (bp_sha256_file(progress, digest, error, error_size) == 0 &&
        strcmp(digest, state.progress_sha256) == 0) {
        (void)bp_remove_synced(progress, &removed, error, error_size);
    }
    (void)bp_remove_synced(state_path, &removed, error, error_size);
    return 0;
}

int bp_is_installed(const char *root)
{
    char state[BP_PATH_CAPACITY];
    char error[BP_ERROR_CAPACITY];
    return current_init_is_dispatcher(root) &&
           path_for(state, root, BP_INSTALL_STATE_PATH, error, sizeof(error)) == 0 &&
           access(state, F_OK) == 0;
}
