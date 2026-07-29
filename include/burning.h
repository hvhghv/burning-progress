#ifndef BURNING_PROGRESS_H
#define BURNING_PROGRESS_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define BP_VERSION "0.1.0"
#define BP_CONFIG_VERSION 1U
#define BP_DEFAULT_TIMEOUT 5U
#define BP_MAX_TIMEOUT 3600U
#define BP_PATH_CAPACITY 4096U
#define BP_ERROR_CAPACITY 512U

#define BP_INIT_PATH "/sbin/init"
#define BP_ORIGINAL_INIT_PATH "/sbin/init.burning-original"
#define BP_BURNING_INIT_PATH "/sbin/burning-init"
#define BP_PROGRESS_PATH "/sbin/burning-progress"
#define BP_HOST_DIR "/etc/BurningProcess"
#define BP_HOST_CONFIG_PATH "/etc/BurningProcess/config"
#define BP_ENABLE_PATH "/etc/BurningProcess/enable"
#define BP_ONLY_ONCE_PATH "/etc/BurningProcess/onlyOnce"
#define BP_ROOTFS_PATH "/etc/BurningProcess/rootfs.cpio"
#define BP_ROOTFS_SHA256_PATH "/etc/BurningProcess/rootfs.cpio.sha256"
#define BP_INSTALL_STATE_PATH "/etc/BurningProcess/install-state"
#define BP_RUNTIME_CONFIG_PATH "/etc/burning-progress.conf"
#define BP_DEFAULT_ENTRY_PATH "/entry.sh"
#define BP_TRIGGER "--BurningProcessEnable"

enum bp_boot_choice {
    BP_BOOT_NORMAL = 0,
    BP_BOOT_SHELL,
    BP_BOOT_POWEROFF
};

enum bp_entry_mode {
    BP_ENTRY_SUPERVISED = 0,
    BP_ENTRY_HANDOFF
};

enum bp_rootfs_format {
    BP_ROOTFS_CPIO = 0,
    BP_ROOTFS_TAR_GZIP
};

struct bp_host_config {
    unsigned int version;
    unsigned int timeout;
    enum bp_boot_choice default_choice;
};

struct bp_runtime_config {
    unsigned int version;
    enum bp_entry_mode entry_mode;
    char entry[BP_PATH_CAPACITY];
};

struct bp_rootfs_info {
    enum bp_rootfs_format format;
    size_t entries;
    uint64_t data_bytes;
    struct bp_runtime_config runtime;
};

void bp_error_set(char *error, size_t error_size, const char *format, ...);
int bp_path(char *output, size_t output_size, const char *root,
            const char *absolute_path, char *error, size_t error_size);
int bp_mkdir_p(const char *path, mode_t mode, char *error, size_t error_size);
int bp_read_text_file(const char *path, char **contents, size_t max_size,
                      char *error, size_t error_size);
int bp_atomic_write(const char *path, const void *data, size_t size, mode_t mode,
                    char *error, size_t error_size);
int bp_atomic_copy(const char *source, const char *destination, mode_t mode,
                   char *error, size_t error_size);
int bp_atomic_symlink(const char *target, const char *destination,
                      char *error, size_t error_size);
int bp_atomic_hardlink(const char *source, const char *destination,
                       char *error, size_t error_size);
int bp_remove_synced(const char *path, int *removed,
                     char *error, size_t error_size);
int bp_sync_parent(const char *path, char *error, size_t error_size);

void bp_host_config_default(struct bp_host_config *config);
int bp_host_config_parse(const char *text, struct bp_host_config *config,
                         char *error, size_t error_size);
int bp_host_config_format(const struct bp_host_config *config, char *output,
                          size_t output_size, char *error, size_t error_size);
int bp_host_config_ensure(const char *root, int *created,
                          char *error, size_t error_size);
const char *bp_boot_choice_name(enum bp_boot_choice choice);
int bp_boot_choice_parse(const char *text, enum bp_boot_choice *choice);

void bp_runtime_config_default(struct bp_runtime_config *config);
int bp_runtime_config_parse(const char *text, struct bp_runtime_config *config,
                            char *error, size_t error_size);
int bp_runtime_config_format(const struct bp_runtime_config *config, char *output,
                             size_t output_size, char *error, size_t error_size);
const char *bp_entry_mode_name(enum bp_entry_mode mode);

int bp_sha256_file(const char *path, char output[65], char *error, size_t error_size);
int bp_install(const char *root, const char *init_source, const char *progress_source,
               char *error, size_t error_size);
int bp_uninstall(const char *root, char *error, size_t error_size);
int bp_is_installed(const char *root);
int bp_cpio_pack(const char *source, const char *output,
                 struct bp_rootfs_info *info, char *error, size_t error_size);
int bp_cpio_verify(const char *archive, struct bp_rootfs_info *info,
                   char *error, size_t error_size);
int bp_cpio_extract(const char *archive, const char *destination,
                    struct bp_rootfs_info *info, char *error, size_t error_size);
int bp_tar_gzip_pack(const char *source, const char *output,
                     struct bp_rootfs_info *info, char *error, size_t error_size);
int bp_tar_gzip_verify(const char *archive, struct bp_rootfs_info *info,
                       char *error, size_t error_size);
int bp_tar_gzip_extract(const char *archive, const char *destination,
                        struct bp_rootfs_info *info, char *error, size_t error_size);
int bp_tar_gzip_unpack(const char *archive, const char *destination,
                       struct bp_rootfs_info *info, char *error, size_t error_size);
const char *bp_rootfs_format_name(enum bp_rootfs_format format);
int bp_rootfs_format_parse(const char *text, enum bp_rootfs_format *format);
int bp_rootfs_pack(const char *source, const char *output,
                   enum bp_rootfs_format format, struct bp_rootfs_info *info,
                   char *error, size_t error_size);
int bp_rootfs_verify(const char *archive, struct bp_rootfs_info *info,
                     char *error, size_t error_size);
int bp_rootfs_extract(const char *archive, const char *destination,
                      struct bp_rootfs_info *info, char *error, size_t error_size);
int bp_rootfs_unpack(const char *archive, const char *destination,
                     struct bp_rootfs_info *info, char *error, size_t error_size);
int bp_rootfs_source_verify(const char *rootfs,
                            struct bp_runtime_config *runtime,
                            char *error, size_t error_size);
int bp_rootfs_runtime_config_load(const char *rootfs,
                                  struct bp_runtime_config *config,
                                  char *error, size_t error_size);
int bp_rootfs_runtime_config_save(const char *rootfs,
                                  const struct bp_runtime_config *config,
                                  char *error, size_t error_size);
int bp_rootfs_default_entry_ensure(const char *rootfs,
                                   const struct bp_runtime_config *config,
                                   int *created, char *error, size_t error_size);
int bp_rootfs_install(const char *root, const char *archive,
                      struct bp_rootfs_info *info, char *error, size_t error_size);
int bp_rootfs_verify_installed(const char *root, struct bp_rootfs_info *info,
                               char *error, size_t error_size);
int bp_stage1(int original_argc, char **original_argv);
int bp_stage2(void);

#endif
