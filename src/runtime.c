#include "burning.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/swap.h>
#include <sys/syscall.h>
#include <sys/sysinfo.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define BP_NEW_ROOT "/mnt/burning-root"
#define BP_OLD_ROOT "/oldroot"
#define BP_MEMORY_MARGIN (32ULL * 1024ULL * 1024ULL)
#define BP_PROC_SUPER_MAGIC 0x00009fa0UL
#define BP_SYSFS_MAGIC 0x62656572UL
#define BP_TMPFS_MAGIC 0x01021994UL

static int open_console(void)
{
    return open("/dev/console", O_RDWR | O_CLOEXEC | O_NOCTTY);
}

static void console_printf(int console, const char *format, ...)
{
    va_list arguments;
    char buffer[2048];
    int length;

    va_start(arguments, format);
    length = vsnprintf(buffer, sizeof(buffer), format, arguments);
    va_end(arguments);
    if (length > 0) {
        size_t size = (size_t)length < sizeof(buffer) ? (size_t)length : sizeof(buffer) - 1U;
        size_t offset = 0U;
        while (offset < size) {
            ssize_t written = write(console, buffer + offset, size - offset);
            if (written < 0 && errno == EINTR) {
                continue;
            }
            if (written <= 0) {
                break;
            }
            offset += (size_t)written;
        }
    }
}

static int load_boot_config(struct bp_host_config *config, int console)
{
    char *contents = NULL;
    char error[BP_ERROR_CAPACITY];

    if (bp_read_text_file(BP_HOST_CONFIG_PATH, &contents, 65536U,
                          error, sizeof(error)) != 0 ||
        bp_host_config_parse(contents == NULL ? "" : contents, config,
                             error, sizeof(error)) != 0) {
        bp_host_config_default(config);
        console_printf(console, "Warning: %s; using timeout=5 default=normal\n", error);
        free(contents);
        return -1;
    }
    free(contents);
    return 0;
}

static int64_t monotonic_milliseconds(void)
{
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) {
        return 0;
    }
    return (int64_t)value.tv_sec * 1000LL + value.tv_nsec / 1000000LL;
}

static enum bp_boot_choice read_menu_choice(int console,
                                             const struct bp_host_config *config)
{
    int64_t deadline = monotonic_milliseconds() + (int64_t)config->timeout * 1000LL;

    console_printf(console,
                   "\nBurning Progress\n"
                   "  1. Enter burning shell\n"
                   "  2. Continue normal boot\n"
                   "  3. Power off\n"
                   "Default: %s, timeout: %u seconds\n> ",
                   bp_boot_choice_name(config->default_choice), config->timeout);
    for (;;) {
        struct pollfd descriptor;
        int64_t remaining = deadline - monotonic_milliseconds();
        int timeout = remaining <= 0 ? 0 : (remaining > INT32_MAX ? INT32_MAX : (int)remaining);
        char input[64];
        ssize_t count;
        ssize_t index;
        int polled;

        descriptor.fd = console;
        descriptor.events = POLLIN;
        descriptor.revents = 0;
        polled = poll(&descriptor, 1U, timeout);
        if (polled == 0 || (polled < 0 && errno != EINTR)) {
            console_printf(console, "\nUsing default: %s\n",
                           bp_boot_choice_name(config->default_choice));
            return config->default_choice;
        }
        if (polled < 0) {
            continue;
        }
        count = read(console, input, sizeof(input));
        if (count <= 0) {
            return config->default_choice;
        }
        for (index = 0; index < count; ++index) {
            int character = tolower((unsigned char)input[index]);
            if (character == '1' || character == 's') {
                return BP_BOOT_SHELL;
            }
            if (character == '2' || character == 'n') {
                return BP_BOOT_NORMAL;
            }
            if (character == '3' || character == 'p') {
                return BP_BOOT_POWEROFF;
            }
        }
        console_printf(console, "Choose 1, 2, or 3: ");
    }
}

static void exec_original(int original_argc, char **original_argv, int console)
{
    char **arguments = calloc((size_t)original_argc + 2U, sizeof(*arguments));
    int source;
    int destination = 1;

    if (arguments == NULL) {
        console_printf(console, "Cannot allocate original init arguments\n");
        _exit(127);
    }
    arguments[0] = (char *)BP_INIT_PATH;
    for (source = 0; source < original_argc; ++source) {
        if (strcmp(original_argv[source], BP_TRIGGER) != 0) {
            arguments[destination++] = original_argv[source];
        }
    }
    arguments[destination] = NULL;
    execv(BP_ORIGINAL_INIT_PATH, arguments);
    console_printf(console, "Cannot execute original init: %s\n", strerror(errno));
    _exit(127);
}

static int ensure_directory(const char *path, char *error, size_t error_size)
{
    struct stat status;
    if (mkdir(path, 0755) == 0) {
        return 0;
    }
    if (errno == EEXIST && stat(path, &status) == 0 && S_ISDIR(status.st_mode)) {
        return 0;
    }
    bp_error_set(error, error_size, "create directory %s: %s", path, strerror(errno));
    return -1;
}

static int mount_has_magic(const char *path, unsigned long magic)
{
    struct statfs status;
    return statfs(path, &status) == 0 && (unsigned long)status.f_type == magic;
}

static int ensure_runtime_mount(const char *path, const char *source,
                                const char *filesystem, unsigned long magic,
                                unsigned long flags, const char *data,
                                char *error, size_t error_size)
{
    if (ensure_directory(path, error, error_size) != 0) {
        return -1;
    }
    if (mount_has_magic(path, magic)) {
        return 0;
    }
    if (mount(source, path, filesystem, flags, data) != 0) {
        bp_error_set(error, error_size, "mount %s: %s", path, strerror(errno));
        return -1;
    }
    return 0;
}

static int prepare_host_runtime_mounts(char *error, size_t error_size)
{
    if (ensure_runtime_mount("/proc", "proc", "proc", BP_PROC_SUPER_MAGIC,
                             MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL,
                             error, error_size) != 0 ||
        ensure_runtime_mount("/sys", "sysfs", "sysfs", BP_SYSFS_MAGIC,
                             MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL,
                             error, error_size) != 0 ||
        ensure_runtime_mount("/dev", "devtmpfs", "devtmpfs", BP_TMPFS_MAGIC,
                             MS_NOSUID, "mode=0755", error, error_size) != 0 ||
        ensure_runtime_mount("/run", "tmpfs", "tmpfs", BP_TMPFS_MAGIC,
                             MS_NOSUID | MS_NODEV, "mode=0755", error, error_size) != 0) {
        return -1;
    }
    return 0;
}

static int bind_runtime_mount(const char *source, const char *new_root,
                              char *target, size_t target_size,
                              char *error, size_t error_size)
{
    int length = snprintf(target, target_size, "%s%s", new_root, source);
    if (length < 0 || (size_t)length >= target_size ||
        ensure_directory(target, error, error_size) != 0) {
        return -1;
    }
    if (mount(source, target, NULL, MS_BIND | MS_REC, NULL) != 0) {
        bp_error_set(error, error_size, "bind mount %s: %s", source, strerror(errno));
        return -1;
    }
    return 0;
}

static int enter_ram_root(int console)
{
    struct bp_rootfs_info info;
    struct sysinfo memory;
    char error[BP_ERROR_CAPACITY] = {0};
    char old_root[BP_PATH_CAPACITY];
    char targets[4][BP_PATH_CAPACITY];
    const char *sources[4] = {"/dev", "/proc", "/sys", "/run"};
    size_t mounted = 0U;
    uint64_t available;
    size_t index;

    if (bp_rootfs_verify_installed("/", &info, error, sizeof(error)) != 0) {
        console_printf(console, "Rootfs validation failed: %s\n", error);
        return -1;
    }
    if (sysinfo(&memory) != 0) {
        console_printf(console, "Cannot query memory: %s\n", strerror(errno));
        return -1;
    }
    available = (uint64_t)memory.freeram * memory.mem_unit;
    if (available < info.data_bytes + BP_MEMORY_MARGIN) {
        console_printf(console, "Insufficient RAM: need at least %llu bytes free\n",
                       (unsigned long long)(info.data_bytes + BP_MEMORY_MARGIN));
        return -1;
    }
    if (prepare_host_runtime_mounts(error, sizeof(error)) != 0 ||
        ensure_directory("/mnt", error, sizeof(error)) != 0 ||
        ensure_directory(BP_NEW_ROOT, error, sizeof(error)) != 0) {
        console_printf(console, "%s\n", error);
        return -1;
    }
    if (mount("tmpfs", BP_NEW_ROOT, "tmpfs", MS_NOSUID | MS_NODEV, "mode=0755") != 0) {
        console_printf(console, "Cannot mount recovery tmpfs: %s\n", strerror(errno));
        return -1;
    }
    if (bp_rootfs_extract(BP_ROOTFS_PATH, BP_NEW_ROOT, &info,
                          error, sizeof(error)) != 0) {
        console_printf(console, "Cannot extract recovery rootfs: %s\n", error);
        (void)umount(BP_NEW_ROOT);
        return -1;
    }
    if (snprintf(old_root, sizeof(old_root), "%s%s", BP_NEW_ROOT, BP_OLD_ROOT) < 0 ||
        ensure_directory(old_root, error, sizeof(error)) != 0) {
        console_printf(console, "%s\n", error);
        (void)umount(BP_NEW_ROOT);
        return -1;
    }
    for (index = 0U; index < 4U; ++index) {
        if (bind_runtime_mount(sources[index], BP_NEW_ROOT, targets[index],
                               sizeof(targets[index]), error, sizeof(error)) != 0) {
            console_printf(console, "%s\n", error);
            goto rollback;
        }
        ++mounted;
    }
    if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) != 0) {
        console_printf(console, "Cannot make mount propagation private: %s\n", strerror(errno));
        goto rollback;
    }
    if (chdir(BP_NEW_ROOT) != 0 || syscall(SYS_pivot_root, ".", "oldroot") != 0) {
        console_printf(console, "pivot_root failed: %s\n", strerror(errno));
        if (chdir("/") != 0) {
            console_printf(console, "Cannot restore working directory: %s\n", strerror(errno));
        }
        goto rollback;
    }
    if (chdir("/") != 0) {
        console_printf(console, "Cannot enter new root: %s; rebooting\n", strerror(errno));
        sync();
        (void)reboot(RB_AUTOBOOT);
        for (;;) {
            pause();
        }
    }
    {
        char *arguments[] = {(char *)BP_PROGRESS_PATH, (char *)"--stage2", NULL};
        execv(BP_PROGRESS_PATH, arguments);
    }
    console_printf(console, "Cannot execute RAM-root stage2: %s; rebooting\n", strerror(errno));
    sync();
    (void)reboot(RB_AUTOBOOT);
    for (;;) {
        pause();
    }

rollback:
    while (mounted > 0U) {
        --mounted;
        (void)umount(targets[mounted]);
    }
    (void)umount(BP_NEW_ROOT);
    return -1;
}

static int disable_swap(int console)
{
    FILE *file = fopen("/proc/swaps", "r");
    char line[BP_PATH_CAPACITY + 256U];
    int first = 1;

    if (file == NULL) {
        console_printf(console, "Cannot inspect swap: %s\n", strerror(errno));
        return -1;
    }
    while (fgets(line, sizeof(line), file) != NULL) {
        char *end;
        if (first) {
            first = 0;
            continue;
        }
        end = line;
        while (*end != '\0' && !isspace((unsigned char)*end)) {
            ++end;
        }
        *end = '\0';
        if (*line != '\0' && swapoff(line) != 0) {
            console_printf(console, "Cannot disable swap %s: %s\n", line, strerror(errno));
            fclose(file);
            return -1;
        }
    }
    fclose(file);
    return 0;
}

static void decode_mount_path(char *path)
{
    char *source = path;
    char *destination = path;
    while (*source != '\0') {
        if (source[0] == '\\' && source[1] >= '0' && source[1] <= '7' &&
            source[2] >= '0' && source[2] <= '7' &&
            source[3] >= '0' && source[3] <= '7') {
            *destination++ = (char)((source[1] - '0') * 64 +
                                    (source[2] - '0') * 8 + source[3] - '0');
            source += 4;
        } else {
            *destination++ = *source++;
        }
    }
    *destination = '\0';
}

static int compare_mount_depth(const void *left, const void *right)
{
    const char *const *left_path = left;
    const char *const *right_path = right;
    size_t left_length = strlen(*left_path);
    size_t right_length = strlen(*right_path);
    if (left_length < right_length) {
        return 1;
    }
    if (left_length > right_length) {
        return -1;
    }
    return strcmp(*right_path, *left_path);
}

static int unmount_old_root(int console)
{
    FILE *file = fopen("/proc/self/mountinfo", "r");
    char line[16384];
    char **mounts = NULL;
    size_t count = 0U;
    size_t capacity = 0U;
    size_t index;
    int result = -1;

    if (file == NULL) {
        console_printf(console, "Cannot read mountinfo: %s\n", strerror(errno));
        return -1;
    }
    while (fgets(line, sizeof(line), file) != NULL) {
        char *save = NULL;
        char *token = strtok_r(line, " ", &save);
        int field = 1;
        while (token != NULL && field < 5) {
            token = strtok_r(NULL, " ", &save);
            ++field;
        }
        if (token == NULL) {
            continue;
        }
        decode_mount_path(token);
        if (strcmp(token, BP_OLD_ROOT) != 0 &&
            !(strncmp(token, BP_OLD_ROOT "/", strlen(BP_OLD_ROOT) + 1U) == 0)) {
            continue;
        }
        if (count == capacity) {
            size_t new_capacity = capacity == 0U ? 16U : capacity * 2U;
            char **resized = realloc(mounts, new_capacity * sizeof(*resized));
            if (resized == NULL) {
                console_printf(console, "Out of memory collecting old-root mounts\n");
                goto cleanup;
            }
            mounts = resized;
            capacity = new_capacity;
        }
        mounts[count] = strdup(token);
        if (mounts[count] == NULL) {
            console_printf(console, "Out of memory copying mount path\n");
            goto cleanup;
        }
        ++count;
    }
    fclose(file);
    file = NULL;
    if (count > 1U) {
        qsort(mounts, count, sizeof(*mounts), compare_mount_depth);
    }
    for (index = 0U; index < count; ++index) {
        if (umount2(mounts[index], 0) != 0 && errno != EINVAL && errno != ENOENT) {
            console_printf(console, "Cannot unmount %s: %s\n", mounts[index], strerror(errno));
            goto cleanup;
        }
    }
    result = 0;

cleanup:
    if (file != NULL) {
        fclose(file);
    }
    for (index = 0U; index < count; ++index) {
        free(mounts[index]);
    }
    free(mounts);
    return result;
}

static void attach_console(void)
{
    int console = open_console();
    if (console < 0) {
        return;
    }
    (void)dup2(console, STDIN_FILENO);
    (void)dup2(console, STDOUT_FILENO);
    (void)dup2(console, STDERR_FILENO);
    if (console > STDERR_FILENO) {
        close(console);
    }
}

static void close_extra_descriptors(void)
{
    long maximum = sysconf(_SC_OPEN_MAX);
    int descriptor;
    if (maximum < 0 || maximum > 65536) {
        maximum = 65536;
    }
    for (descriptor = STDERR_FILENO + 1; descriptor < maximum; ++descriptor) {
        close(descriptor);
    }
}

static int wait_for_child(pid_t child)
{
    int status;
    for (;;) {
        pid_t result = waitpid(child, &status, 0);
        if (result == child) {
            return status;
        }
        if (result < 0 && errno != EINTR) {
            return -1;
        }
    }
}

static void shell_loop(void)
{
    for (;;) {
        pid_t child = fork();
        if (child == 0) {
            char *arguments[] = {(char *)"/bin/sh", NULL};
            execv("/bin/sh", arguments);
            _exit(127);
        }
        if (child < 0) {
            dprintf(STDERR_FILENO, "burning-progress: fork shell: %s\n", strerror(errno));
            sleep(1U);
            continue;
        }
        (void)wait_for_child(child);
        dprintf(STDOUT_FILENO, "\nShell exited; restarting.\n");
        sleep(1U);
    }
}

static int load_runtime_config(struct bp_runtime_config *config)
{
    char *contents = NULL;
    char error[BP_ERROR_CAPACITY];
    if (bp_read_text_file(BP_RUNTIME_CONFIG_PATH, &contents, 65536U,
                          error, sizeof(error)) != 0) {
        if (errno == ENOENT) {
            bp_runtime_config_default(config);
            return 0;
        }
        dprintf(STDERR_FILENO, "burning-progress: %s\n", error);
        return -1;
    }
    if (bp_runtime_config_parse(contents, config, error, sizeof(error)) != 0) {
        dprintf(STDERR_FILENO, "burning-progress: %s\n", error);
        free(contents);
        return -1;
    }
    free(contents);
    return 0;
}

static void reset_for_handoff(void)
{
    struct sigaction action;
    sigset_t empty;
    int signal_number;

    memset(&action, 0, sizeof(action));
    action.sa_handler = SIG_DFL;
    sigemptyset(&action.sa_mask);
    for (signal_number = 1; signal_number < NSIG; ++signal_number) {
        if (signal_number != SIGKILL && signal_number != SIGSTOP) {
            (void)sigaction(signal_number, &action, NULL);
        }
    }
    sigemptyset(&empty);
    (void)sigprocmask(SIG_SETMASK, &empty, NULL);
    alarm(0U);
}

int bp_stage1(int original_argc, char **original_argv)
{
    struct bp_host_config config;
    enum bp_boot_choice choice;
    int console;
    int removed;
    char error[BP_ERROR_CAPACITY];

    if (getpid() != 1) {
        fprintf(stderr, "burning-progress: stage1 must run as PID 1\n");
        return 1;
    }
    console = open_console();
    if (console < 0) {
        exec_original(original_argc, original_argv, STDERR_FILENO);
    }
    (void)load_boot_config(&config, console);
    if (access(BP_ENABLE_PATH, F_OK) == 0 && access(BP_ONLY_ONCE_PATH, F_OK) == 0 &&
        bp_remove_synced(BP_ENABLE_PATH, &removed, error, sizeof(error)) != 0) {
        console_printf(console, "Warning: cannot consume one-shot enable: %s\n", error);
    }
    choice = read_menu_choice(console, &config);
    if (choice == BP_BOOT_NORMAL) {
        exec_original(original_argc, original_argv, console);
    }
    if (choice == BP_BOOT_POWEROFF) {
        console_printf(console, "Powering off.\n");
        sync();
        (void)reboot(RB_POWER_OFF);
        for (;;) {
            pause();
        }
    }
    if (enter_ram_root(console) != 0) {
        console_printf(console, "Recovery root transition failed; continuing normal boot.\n");
        exec_original(original_argc, original_argv, console);
    }
    return 1;
}

int bp_stage2(void)
{
    struct bp_runtime_config config;
    struct stat status;

    if (getpid() != 1) {
        fprintf(stderr, "burning-progress: stage2 must run as PID 1\n");
        return 1;
    }
    attach_console();
    if (chdir("/") != 0) {
        dprintf(STDERR_FILENO, "burning-progress: chdir /: %s\n", strerror(errno));
        shell_loop();
    }
    close_extra_descriptors();
    if (disable_swap(STDERR_FILENO) != 0 || unmount_old_root(STDERR_FILENO) != 0) {
        dprintf(STDERR_FILENO,
                "Old root is still mounted. System-disk flashing is disabled.\n");
        shell_loop();
    }
    if (load_runtime_config(&config) != 0) {
        shell_loop();
    }
    if (lstat(config.entry, &status) != 0) {
        if (config.entry_mode == BP_ENTRY_HANDOFF) {
            dprintf(STDERR_FILENO, "Handoff entry is missing: %s\n", config.entry);
            shell_loop();
        }
        shell_loop();
    }
    if (!S_ISREG(status.st_mode)) {
        dprintf(STDERR_FILENO, "Entry is not a regular file: %s\n", config.entry);
        shell_loop();
    }
    if (config.entry_mode == BP_ENTRY_HANDOFF) {
        char *arguments[] = {(char *)"/bin/sh", config.entry, NULL};
        reset_for_handoff();
        clearenv();
        (void)setenv("PATH", "/sbin:/bin:/usr/sbin:/usr/bin", 1);
        (void)setenv("HOME", "/root", 1);
        (void)setenv("TERM", "linux", 0);
        umask(022);
        execv("/bin/sh", arguments);
        dprintf(STDERR_FILENO, "Handoff exec failed: %s\n", strerror(errno));
        _exit(127);
    } else {
        pid_t child = fork();
        if (child == 0) {
            char *arguments[] = {(char *)"/bin/sh", config.entry, NULL};
            execv("/bin/sh", arguments);
            _exit(127);
        }
        if (child > 0) {
            int status_code = wait_for_child(child);
            dprintf(STDOUT_FILENO, "Entry exited with wait status %d.\n", status_code);
        } else {
            dprintf(STDERR_FILENO, "Cannot fork entry: %s\n", strerror(errno));
        }
        shell_loop();
    }
    return 1;
}
