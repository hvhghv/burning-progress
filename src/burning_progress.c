#include "burning.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void usage(FILE *stream)
{
    fprintf(stream,
            "Usage: burning-progress [--root PATH] COMMAND\n"
            "Commands:\n"
            "  install [--init-binary PATH] [--progress-binary PATH]\n"
            "  uninstall\n"
            "  status\n"
            "  enable [--once|--persistent]\n"
            "  disable\n"
            "  config [--timeout SECONDS] [--default normal|shell|poweroff]\n");
    fprintf(stream,
            "  rootfs pack DIRECTORY [--output FILE] [--format cpio|tar.gz]\n"
            "  rootfs unpack FILE --output DIRECTORY\n"
            "  rootfs configure DIRECTORY\n"
            "  rootfs verify FILE\n"
            "  rootfs install SOURCE [--entry-file FILE]\n");
}

static int full_path(char *output, size_t output_size, const char *root,
                     const char *path)
{
    char error[BP_ERROR_CAPACITY];
    if (bp_path(output, output_size, root, path, error, sizeof(error)) != 0) {
        fprintf(stderr, "burning-progress: %s\n", error);
        return -1;
    }
    return 0;
}

static int load_host_config(const char *root, struct bp_host_config *config,
                            int allow_default)
{
    char path[BP_PATH_CAPACITY];
    char error[BP_ERROR_CAPACITY];
    char *contents = NULL;

    if (full_path(path, sizeof(path), root, BP_HOST_CONFIG_PATH) != 0) {
        return -1;
    }
    if (bp_read_text_file(path, &contents, 65536U, error, sizeof(error)) != 0) {
        if (allow_default && errno == ENOENT) {
            bp_host_config_default(config);
            return 0;
        }
        fprintf(stderr, "burning-progress: %s\n", error);
        return -1;
    }
    if (bp_host_config_parse(contents, config, error, sizeof(error)) != 0) {
        fprintf(stderr, "burning-progress: %s\n", error);
        free(contents);
        return -1;
    }
    free(contents);
    return 0;
}

static int save_host_config(const char *root, const struct bp_host_config *config)
{
    char path[BP_PATH_CAPACITY];
    char text[256];
    char error[BP_ERROR_CAPACITY];

    if (full_path(path, sizeof(path), root, BP_HOST_CONFIG_PATH) != 0 ||
        bp_host_config_format(config, text, sizeof(text), error, sizeof(error)) != 0 ||
        bp_atomic_write(path, text, strlen(text), 0600, error, sizeof(error)) != 0) {
        fprintf(stderr, "burning-progress: %s\n", error);
        return -1;
    }
    return 0;
}

static int command_status(const char *root)
{
    char path[BP_PATH_CAPACITY];
    struct bp_host_config config;

    if (load_host_config(root, &config, 1) != 0) {
        bp_host_config_default(&config);
        printf("configWarning=true\n");
    }
    printf("installed=%s\n", bp_is_installed(root) ? "true" : "false");
    full_path(path, sizeof(path), root, BP_ENABLE_PATH);
    printf("enabled=%s\n", access(path, F_OK) == 0 ? "true" : "false");
    full_path(path, sizeof(path), root, BP_ONLY_ONCE_PATH);
    printf("onlyOnce=%s\n", access(path, F_OK) == 0 ? "true" : "false");
    full_path(path, sizeof(path), root, BP_ROOTFS_PATH);
    printf("rootfsInstalled=%s\n", access(path, F_OK) == 0 ? "true" : "false");
    printf("timeout=%u\n", config.timeout);
    printf("default=%s\n", bp_boot_choice_name(config.default_choice));
    return 0;
}

static int executable_directory(char *directory, size_t directory_size)
{
    char path[BP_PATH_CAPACITY];
    char *separator;
    ssize_t length = readlink("/proc/self/exe", path, sizeof(path) - 1U);

    if (length < 0 || (size_t)length >= sizeof(path)) {
        return -1;
    }
    path[length] = '\0';
    separator = strrchr(path, '/');
    if (separator == NULL) {
        return -1;
    }
    *separator = '\0';
    if (strlen(path) >= directory_size) {
        return -1;
    }
    strcpy(directory, path);
    return 0;
}

static int checked_directory(const char *path, char *checked,
                             size_t checked_size, char *error,
                             size_t error_size)
{
    struct stat status;
    size_t length;

    if (path == NULL || path[0] == '\0' || strlen(path) >= checked_size) {
        bp_error_set(error, error_size, "invalid directory: %s",
                     path == NULL ? "(null)" : path);
        return -1;
    }
    strcpy(checked, path);
    length = strlen(checked);
    while (length > 1U && checked[length - 1U] == '/') {
        checked[--length] = '\0';
    }
    if (lstat(checked, &status) != 0 || !S_ISDIR(status.st_mode)) {
        bp_error_set(error, error_size, "not a real directory: %s", path);
        return -1;
    }
    return 0;
}

static int rootfs_config_exists(const char *directory, int *exists,
                                char *error, size_t error_size)
{
    char path[BP_PATH_CAPACITY];
    struct stat status;

    if (exists != NULL) {
        *exists = 0;
    }
    if (bp_path(path, sizeof(path), directory, BP_RUNTIME_CONFIG_PATH,
                error, error_size) != 0) {
        return -1;
    }
    if (lstat(path, &status) == 0) {
        if (exists != NULL) {
            *exists = 1;
        }
        return 0;
    }
    if (errno == ENOENT) {
        return 0;
    }
    bp_error_set(error, error_size, "inspect %s: %s", path, strerror(errno));
    return -1;
}

static int create_temporary_directory(char *path, size_t path_size,
                                      const char *template_text,
                                      char *error, size_t error_size)
{
    if (strlen(template_text) >= path_size) {
        bp_error_set(error, error_size, "temporary path template is too long");
        return -1;
    }
    strcpy(path, template_text);
    if (mkdtemp(path) == NULL) {
        bp_error_set(error, error_size, "create temporary directory: %s", strerror(errno));
        return -1;
    }
    return 0;
}

static int remove_tree(const char *path, char *error, size_t error_size)
{
    struct stat status;
    DIR *directory;
    struct dirent *entry;
    char child[BP_PATH_CAPACITY];

    if (lstat(path, &status) != 0) {
        if (errno == ENOENT) {
            return 0;
        }
        bp_error_set(error, error_size, "inspect %s: %s", path, strerror(errno));
        return -1;
    }
    if (!S_ISDIR(status.st_mode)) {
        if (unlink(path) != 0 && errno != ENOENT) {
            bp_error_set(error, error_size, "remove %s: %s", path, strerror(errno));
            return -1;
        }
        return 0;
    }
    directory = opendir(path);
    if (directory == NULL) {
        bp_error_set(error, error_size, "open directory %s: %s", path, strerror(errno));
        return -1;
    }
    errno = 0;
    while ((entry = readdir(directory)) != NULL) {
        int written;
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        written = snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
        if (written < 0 || (size_t)written >= sizeof(child)) {
            closedir(directory);
            bp_error_set(error, error_size, "child path is too long: %s/%s",
                         path, entry->d_name);
            return -1;
        }
        if (remove_tree(child, error, error_size) != 0) {
            closedir(directory);
            return -1;
        }
    }
    if (errno != 0) {
        bp_error_set(error, error_size, "read directory %s: %s", path, strerror(errno));
        closedir(directory);
        return -1;
    }
    closedir(directory);
    if (rmdir(path) != 0) {
        bp_error_set(error, error_size, "remove directory %s: %s", path, strerror(errno));
        return -1;
    }
    return 0;
}

static int install_rootfs_entry(const char *directory,
                                const char *entry_source,
                                const struct bp_runtime_config *config,
                                char *error, size_t error_size)
{
    char path[BP_PATH_CAPACITY];
    struct stat status;

    if (entry_source == NULL) {
        return bp_rootfs_default_entry_ensure(directory, config, NULL,
                                              error, error_size);
    }
    if (stat(entry_source, &status) != 0 || !S_ISREG(status.st_mode) ||
        status.st_size == 0) {
        bp_error_set(error, error_size,
                     "entry file must be a non-empty regular file: %s",
                     entry_source);
        return -1;
    }
    if (bp_path(path, sizeof(path), directory, config->entry,
                error, error_size) != 0) {
        return -1;
    }
    if (bp_atomic_copy(entry_source, path, 0755, error, error_size) != 0) {
        return -1;
    }
    return 0;
}

static int ensure_pack_burning_progress(const char *source, char *error,
                                        size_t error_size)
{
    char checked_source[BP_PATH_CAPACITY];
    char sbin_path[BP_PATH_CAPACITY];
    char progress_path[BP_PATH_CAPACITY];
    struct stat status;

    if (checked_directory(source, checked_source, sizeof(checked_source),
                          error, error_size) != 0 ||
        bp_path(sbin_path, sizeof(sbin_path), checked_source, "/sbin",
                error, error_size) != 0 ||
        bp_path(progress_path, sizeof(progress_path), checked_source,
                BP_PROGRESS_PATH, error, error_size) != 0) {
        return -1;
    }
    if (lstat(sbin_path, &status) == 0 && !S_ISDIR(status.st_mode)) {
        bp_error_set(error, error_size, "rootfs sbin path is not a real directory: %s",
                     sbin_path);
        return -1;
    }
    if (lstat(progress_path, &status) == 0 && !S_ISREG(status.st_mode) &&
        !S_ISLNK(status.st_mode)) {
        bp_error_set(error, error_size,
                     "rootfs burning-progress path is not a regular file or symlink: %s",
                     progress_path);
        return -1;
    }
    if (lstat(progress_path, &status) != 0 && errno != ENOENT) {
        bp_error_set(error, error_size, "inspect %s: %s",
                     progress_path, strerror(errno));
        return -1;
    }
    if (bp_atomic_copy("/proc/self/exe", progress_path, 0755,
                       error, error_size) != 0) {
        return -1;
    }
    return 0;
}

static int command_install(const char *root, int argc, char **argv)
{
    char directory[BP_PATH_CAPACITY];
    char default_init[BP_PATH_CAPACITY];
    char default_progress[BP_PATH_CAPACITY];
    const char *init_source = NULL;
    const char *progress_source = NULL;
    char error[BP_ERROR_CAPACITY] = {0};
    int index;

    if (executable_directory(directory, sizeof(directory)) != 0 ||
        snprintf(default_init, sizeof(default_init), "%s/burning-init", directory) < 0 ||
        snprintf(default_progress, sizeof(default_progress), "%s/burning-progress", directory) < 0) {
        fprintf(stderr, "burning-progress: cannot determine executable directory\n");
        return -1;
    }
    init_source = default_init;
    progress_source = default_progress;
    for (index = 0; index < argc; ++index) {
        if (strcmp(argv[index], "--init-binary") == 0 && index + 1 < argc) {
            init_source = argv[++index];
        } else if (strcmp(argv[index], "--progress-binary") == 0 && index + 1 < argc) {
            progress_source = argv[++index];
        } else {
            fprintf(stderr, "burning-progress: invalid install argument\n");
            return -1;
        }
    }
    if (bp_install(root, init_source, progress_source, error, sizeof(error)) != 0) {
        fprintf(stderr, "burning-progress: %s\n", error);
        return -1;
    }
    return 0;
}

static int command_enable(const char *root, int once)
{
    char enable[BP_PATH_CAPACITY];
    char only_once[BP_PATH_CAPACITY];
    char error[BP_ERROR_CAPACITY];
    int removed;

    if (!bp_is_installed(root)) {
        fprintf(stderr, "burning-progress: burning-progress is not installed; run install first\n");
        return -1;
    }
    if (full_path(enable, sizeof(enable), root, BP_ENABLE_PATH) != 0 ||
        full_path(only_once, sizeof(only_once), root, BP_ONLY_ONCE_PATH) != 0) {
        return -1;
    }
    if (once) {
        if (bp_atomic_write(only_once, "", 0U, 0600, error, sizeof(error)) != 0) {
            fprintf(stderr, "burning-progress: %s\n", error);
            return -1;
        }
    } else if (bp_remove_synced(only_once, &removed, error, sizeof(error)) != 0) {
        fprintf(stderr, "burning-progress: %s\n", error);
        return -1;
    }
    if (bp_atomic_write(enable, "", 0U, 0600, error, sizeof(error)) != 0) {
        fprintf(stderr, "burning-progress: %s\n", error);
        return -1;
    }
    return 0;
}

static int command_disable(const char *root)
{
    char path[BP_PATH_CAPACITY];
    char error[BP_ERROR_CAPACITY];
    int removed;

    if (full_path(path, sizeof(path), root, BP_ENABLE_PATH) != 0) {
        return -1;
    }
    if (bp_remove_synced(path, &removed, error, sizeof(error)) != 0) {
        fprintf(stderr, "burning-progress: %s\n", error);
        return -1;
    }
    return 0;
}

static int command_config(const char *root, int argc, char **argv)
{
    struct bp_host_config config;
    int changed = 0;
    int index;

    if (load_host_config(root, &config, 1) != 0) {
        return -1;
    }
    for (index = 0; index < argc; ++index) {
        if (strcmp(argv[index], "--timeout") == 0 && index + 1 < argc) {
            char *end;
            unsigned long value = strtoul(argv[++index], &end, 10);
            if (*end != '\0' || value > BP_MAX_TIMEOUT) {
                fprintf(stderr, "burning-progress: invalid timeout\n");
                return -1;
            }
            config.timeout = (unsigned int)value;
            changed = 1;
        } else if (strcmp(argv[index], "--default") == 0 && index + 1 < argc) {
            if (bp_boot_choice_parse(argv[++index], &config.default_choice) != 0) {
                fprintf(stderr, "burning-progress: invalid default option\n");
                return -1;
            }
            changed = 1;
        } else {
            fprintf(stderr, "burning-progress: invalid config argument\n");
            return -1;
        }
    }
    if (!changed) {
        fprintf(stderr, "burning-progress: config requires an option\n");
        return -1;
    }
    return save_host_config(root, &config);
}

static void print_rootfs_info(const struct bp_rootfs_info *info)
{
    printf("format=%s\n", bp_rootfs_format_name(info->format));
    printf("entries=%zu\n", info->entries);
    printf("dataBytes=%llu\n", (unsigned long long)info->data_bytes);
    printf("entryMode=%s\n", bp_entry_mode_name(info->runtime.entry_mode));
    printf("entry=%s\n", info->runtime.entry);
}

static char *trim_answer(char *text)
{
    char *end;

    while (isspace((unsigned char)*text)) {
        ++text;
    }
    end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1])) {
        --end;
    }
    *end = '\0';
    return text;
}

static int read_answer(const char *prompt, char *buffer, size_t buffer_size,
                       char **answer, char *error, size_t error_size)
{
    size_t length;

    if (fputs(prompt, stdout) == EOF || fflush(stdout) != 0) {
        bp_error_set(error, error_size, "write interactive prompt: %s",
                     strerror(errno));
        return -1;
    }
    if (fgets(buffer, (int)buffer_size, stdin) == NULL) {
        bp_error_set(error, error_size,
                     "interactive input ended before configuration was complete");
        return -1;
    }
    length = strlen(buffer);
    if (length > 0U && buffer[length - 1U] == '\n') {
        buffer[length - 1U] = '\0';
    } else if (!feof(stdin)) {
        int character;
        do {
            character = fgetc(stdin);
        } while (character != '\n' && character != EOF);
        return 1;
    }
    *answer = trim_answer(buffer);
    return 0;
}

static int prompt_entry_mode(struct bp_runtime_config *config,
                             char *error, size_t error_size)
{
    char prompt[128];
    char buffer[64];

    if (snprintf(prompt, sizeof(prompt),
                 "Entry mode (supervised/handoff) [%s]: ",
                 bp_entry_mode_name(config->entry_mode)) < 0) {
        bp_error_set(error, error_size, "cannot format entry mode prompt");
        return -1;
    }
    for (;;) {
        char *answer;
        int result = read_answer(prompt, buffer, sizeof(buffer), &answer,
                                 error, error_size);
        if (result < 0) {
            return -1;
        }
        if (result > 0) {
            puts("Input is too long; enter supervised or handoff.");
            continue;
        }
        if (answer[0] == '\0') {
            return 0;
        }
        if (strcmp(answer, "supervised") == 0) {
            config->entry_mode = BP_ENTRY_SUPERVISED;
            return 0;
        }
        if (strcmp(answer, "handoff") == 0) {
            config->entry_mode = BP_ENTRY_HANDOFF;
            return 0;
        }
        puts("Invalid entry mode; enter supervised or handoff.");
    }
}

static int prompt_entry_path(struct bp_runtime_config *config,
                             char *error, size_t error_size)
{
    char prompt[BP_PATH_CAPACITY + 32U];
    char buffer[BP_PATH_CAPACITY + 2U];

    if (snprintf(prompt, sizeof(prompt), "Entry path [%s]: ", config->entry) < 0) {
        bp_error_set(error, error_size, "cannot format entry path prompt");
        return -1;
    }
    for (;;) {
        struct bp_runtime_config candidate = *config;
        char formatted[BP_PATH_CAPACITY + 64U];
        char validation_error[BP_ERROR_CAPACITY] = {0};
        char *answer;
        int result = read_answer(prompt, buffer, sizeof(buffer), &answer,
                                 error, error_size);
        if (result < 0) {
            return -1;
        }
        if (result > 0 || strlen(answer) >= sizeof(candidate.entry)) {
            puts("Invalid entry path; enter an absolute path without . or .. components.");
            continue;
        }
        if (answer[0] != '\0') {
            strcpy(candidate.entry, answer);
        }
        if (bp_runtime_config_format(&candidate, formatted, sizeof(formatted),
                                     validation_error,
                                     sizeof(validation_error)) != 0) {
            printf("%s\n", validation_error);
            continue;
        }
        *config = candidate;
        return 0;
    }
}

static int configure_rootfs_directory(const char *directory, int *entry_created,
                                      char *error, size_t error_size)
{
    struct bp_runtime_config config;
    int created = 0;

    if (bp_rootfs_runtime_config_load(directory, &config,
                                      error, error_size) != 0 ||
        prompt_entry_mode(&config, error, error_size) != 0 ||
        prompt_entry_path(&config, error, error_size) != 0 ||
        bp_rootfs_default_entry_ensure(directory, &config, &created,
                                       error, error_size) != 0 ||
        bp_rootfs_runtime_config_save(directory, &config,
                                      error, error_size) != 0) {
        return -1;
    }
    if (entry_created != NULL) {
        *entry_created = created;
    }
    return 0;
}

static int command_rootfs_configure(const char *directory)
{
    struct bp_runtime_config config;
    char error[BP_ERROR_CAPACITY] = {0};
    char config_path[BP_PATH_CAPACITY];
    int entry_created;

    if (configure_rootfs_directory(directory, &entry_created,
                                   error, sizeof(error)) != 0 ||
        bp_rootfs_runtime_config_load(directory, &config,
                                      error, sizeof(error)) != 0 ||
        bp_path(config_path, sizeof(config_path), directory,
                BP_RUNTIME_CONFIG_PATH, error, sizeof(error)) != 0) {
        fprintf(stderr, "burning-progress: %s\n", error);
        return -1;
    }
    printf("configuration=%s\n", config_path);
    printf("entryMode=%s\n", bp_entry_mode_name(config.entry_mode));
    printf("entry=%s\n", config.entry);
    printf("entryCreated=%s\n", entry_created ? "true" : "false");
    return 0;
}

static int command_rootfs_install(const char *root, const char *source,
                                  const char *entry_source,
                                  struct bp_rootfs_info *info)
{
    enum bp_rootfs_format format = BP_ROOTFS_CPIO;
    char archive_dir[BP_PATH_CAPACITY];
    char archive_path[BP_PATH_CAPACITY];
    char error[BP_ERROR_CAPACITY] = {0};
    char workspace[BP_PATH_CAPACITY];
    const char *workspace_path = NULL;
    struct stat status;
    int archive_dir_created = 0;
    int config_exists = 0;
    int workspace_created = 0;
    int result = -1;

    if (lstat(source, &status) != 0) {
        fprintf(stderr, "burning-progress: inspect %s: %s\n", source, strerror(errno));
        return -1;
    }
    if (S_ISDIR(status.st_mode)) {
        if (checked_directory(source, workspace, sizeof(workspace),
                              error, sizeof(error)) != 0) {
            fprintf(stderr, "burning-progress: %s\n", error);
            return -1;
        }
        workspace_path = workspace;
    } else if (S_ISREG(status.st_mode)) {
        if (create_temporary_directory(workspace, sizeof(workspace),
                                       "/tmp/burning-progress-install.XXXXXX",
                                       error, sizeof(error)) != 0) {
            fprintf(stderr, "burning-progress: %s\n", error);
            return -1;
        }
        workspace_created = 1;
        workspace_path = workspace;
        if (bp_rootfs_unpack(source, workspace_path, info, error, sizeof(error)) != 0) {
            fprintf(stderr, "burning-progress: %s\n", error);
            goto cleanup;
        }
        format = info->format;
    } else {
        fprintf(stderr, "burning-progress: rootfs install source must be a directory or archive file: %s\n",
                source);
        return -1;
    }
    if (ensure_pack_burning_progress(workspace_path, error, sizeof(error)) != 0) {
        fprintf(stderr, "burning-progress: %s\n", error);
        goto cleanup;
    }
    if (rootfs_config_exists(workspace_path, &config_exists,
                             error, sizeof(error)) != 0) {
        fprintf(stderr, "burning-progress: %s\n", error);
        goto cleanup;
    }
    if (config_exists) {
        if (bp_rootfs_runtime_config_load(workspace_path, &info->runtime,
                                          error, sizeof(error)) != 0) {
            fprintf(stderr, "burning-progress: %s\n", error);
            goto cleanup;
        }
    } else {
        if (configure_rootfs_directory(workspace_path, NULL,
                                        error, sizeof(error)) != 0 ||
            bp_rootfs_runtime_config_load(workspace_path, &info->runtime,
                                          error, sizeof(error)) != 0) {
            fprintf(stderr, "burning-progress: %s\n", error);
            goto cleanup;
        }
    }
    if (install_rootfs_entry(workspace_path, entry_source, &info->runtime,
                             error, sizeof(error)) != 0) {
        fprintf(stderr, "burning-progress: %s\n", error);
        goto cleanup;
    }
    if (bp_rootfs_source_verify(workspace_path, &info->runtime,
                                error, sizeof(error)) != 0) {
        fprintf(stderr, "burning-progress: %s\n", error);
        goto cleanup;
    }
    if (create_temporary_directory(archive_dir, sizeof(archive_dir),
                                   "/tmp/burning-progress-rootfs.XXXXXX",
                                   error, sizeof(error)) != 0) {
        fprintf(stderr, "burning-progress: %s\n", error);
        goto cleanup;
    }
    archive_dir_created = 1;
    {
        int written = snprintf(archive_path, sizeof(archive_path), "%s/rootfs.%s",
                               archive_dir, bp_rootfs_format_name(format));
        if (written < 0 || (size_t)written >= sizeof(archive_path)) {
            fprintf(stderr, "burning-progress: temporary archive path is too long\n");
            goto cleanup;
        }
    }
    if (bp_rootfs_pack(workspace_path, archive_path, format, info,
                       error, sizeof(error)) != 0) {
        fprintf(stderr, "burning-progress: %s\n", error);
        goto cleanup;
    }
    if (bp_rootfs_install(root, archive_path, info,
                          error, sizeof(error)) != 0) {
        fprintf(stderr, "burning-progress: %s\n", error);
        goto cleanup;
    }
    result = 0;

cleanup:
    if (archive_dir_created) {
        char cleanup_error[BP_ERROR_CAPACITY] = {0};
        (void)remove_tree(archive_dir, cleanup_error, sizeof(cleanup_error));
    }
    if (workspace_created) {
        char cleanup_error[BP_ERROR_CAPACITY] = {0};
        (void)remove_tree(workspace, cleanup_error, sizeof(cleanup_error));
    }
    return result;
}

static int command_rootfs(const char *root, int argc, char **argv)
{
    struct bp_rootfs_info info;
    enum bp_rootfs_format format = BP_ROOTFS_CPIO;
    const char *output = NULL;
    int output_explicit = 0;
    int format_explicit = 0;
    char error[BP_ERROR_CAPACITY] = {0};
    int result;

    if (argc >= 2 && strcmp(argv[0], "pack") == 0) {
        int index = 2;

        if (argc == 2) {
            /* Default output stays at ./rootfs.cpio. */
        } else {
            while (index < argc) {
                if (strcmp(argv[index], "--output") == 0) {
                    if (index + 1 >= argc) {
                        usage(stderr);
                        return -1;
                    }
                    output = argv[index + 1];
                    output_explicit = 1;
                    index += 2;
                } else if (strcmp(argv[index], "--format") == 0) {
                    if (index + 1 >= argc ||
                        bp_rootfs_format_parse(argv[index + 1], &format) != 0) {
                        usage(stderr);
                        return -1;
                    }
                    format_explicit = 1;
                    index += 2;
                } else {
                    usage(stderr);
                    return -1;
                }
            }
        }
        if (!output_explicit) {
            output = format == BP_ROOTFS_TAR_GZIP ? "./rootfs.tar.gz"
                                                  : "./rootfs.cpio";
        } else if (!format_explicit && format == BP_ROOTFS_CPIO) {
            size_t length = strlen(output);
            if ((length >= 7U && strcmp(output + length - 7U, ".tar.gz") == 0) ||
                (length >= 4U && strcmp(output + length - 4U, ".tgz") == 0)) {
                format = BP_ROOTFS_TAR_GZIP;
            }
        }
        if (ensure_pack_burning_progress(argv[1], error, sizeof(error)) != 0) {
            fprintf(stderr, "burning-progress: %s\n", error);
            return -1;
        }
        result = bp_rootfs_pack(argv[1], output, format, &info, error, sizeof(error));
    } else if (argc == 4 && strcmp(argv[0], "unpack") == 0 &&
               strcmp(argv[2], "--output") == 0) {
        result = bp_rootfs_unpack(argv[1], argv[3], &info, error, sizeof(error));
    } else if (argc == 2 && strcmp(argv[0], "configure") == 0) {
        return command_rootfs_configure(argv[1]);
    } else if (argc == 2 && strcmp(argv[0], "verify") == 0) {
        result = bp_rootfs_verify(argv[1], &info, error, sizeof(error));
    } else if (argc >= 2 && strcmp(argv[0], "install") == 0) {
        const char *entry_source = NULL;
        int index = 2;

        while (index < argc) {
            if ((strcmp(argv[index], "--entry-file") == 0 ||
                 strcmp(argv[index], "--entry") == 0)) {
                if (index + 1 >= argc) {
                    usage(stderr);
                    return -1;
                }
                entry_source = argv[index + 1];
                index += 2;
            } else {
                usage(stderr);
                return -1;
            }
        }
        result = command_rootfs_install(root, argv[1], entry_source, &info);
        if (result != 0) {
            return -1;
        }
        print_rootfs_info(&info);
        return 0;
    } else {
        usage(stderr);
        return -1;
    }
    if (result != 0) {
        fprintf(stderr, "burning-progress: %s\n", error);
        return -1;
    }
    print_rootfs_info(&info);
    return 0;
}

int main(int argc, char **argv)
{
    const char *root = "/";
    int index = 1;
    const char *command;

    if (argc > 1 && strcmp(argv[1], "--stage1") == 0) {
        int first_original = 2;
        if (first_original < argc && strcmp(argv[first_original], "--") == 0) {
            ++first_original;
        }
        return bp_stage1(argc - first_original, &argv[first_original]);
    }
    if (argc > 1 && strcmp(argv[1], "--stage2") == 0) {
        return bp_stage2();
    }
    if (index + 1 < argc && strcmp(argv[index], "--root") == 0) {
        root = argv[index + 1];
        index += 2;
    }
    if (index >= argc) {
        usage(stderr);
        return 2;
    }
    command = argv[index++];
    if (strcmp(command, "status") == 0 && index == argc) {
        return command_status(root);
    }
    if (strcmp(command, "rootfs") == 0) {
        return command_rootfs(root, argc - index, &argv[index]);
    }
    if (geteuid() != 0) {
        fprintf(stderr, "burning-progress: this command requires root\n");
        return 1;
    }
    if (strcmp(command, "enable") == 0) {
        int once = 1;
        if (index < argc) {
            if (index + 1 != argc) {
                usage(stderr);
                return 2;
            }
            if (strcmp(argv[index], "--persistent") == 0) {
                once = 0;
            } else if (strcmp(argv[index], "--once") != 0) {
                usage(stderr);
                return 2;
            }
        }
        return command_enable(root, once);
    }
    if (strcmp(command, "install") == 0) {
        return command_install(root, argc - index, &argv[index]);
    }
    if (strcmp(command, "uninstall") == 0 && index == argc) {
        char error[BP_ERROR_CAPACITY] = {0};
        if (bp_uninstall(root, error, sizeof(error)) != 0) {
            fprintf(stderr, "burning-progress: %s\n", error);
            return 1;
        }
        return 0;
    }
    if (strcmp(command, "disable") == 0 && index == argc) {
        return command_disable(root);
    }
    if (strcmp(command, "config") == 0) {
        return command_config(root, argc - index, &argv[index]);
    }
    usage(stderr);
    return 2;
}
