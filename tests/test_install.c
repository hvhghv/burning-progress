#include "burning.h"

#include <archive.h>
#include <archive_entry.h>
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static void make_file(const char *path, const char *contents, mode_t mode)
{
    int descriptor = open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
    assert(descriptor >= 0);
    assert(write(descriptor, contents, strlen(contents)) == (ssize_t)strlen(contents));
    assert(close(descriptor) == 0);
}

static void make_dir(const char *path)
{
    assert(mkdir(path, 0755) == 0);
}

static void run_shell_command(const char *command)
{
    int status = system(command);
    assert(status != -1);
    assert(WIFEXITED(status));
    assert(WEXITSTATUS(status) == 0);
}

static void cpio_write_all(FILE *file, const void *data, size_t size)
{
    assert(fwrite(data, 1U, size, file) == size);
}

static void cpio_write_padding(FILE *file, size_t size)
{
    static const unsigned char zeros[3] = {0U, 0U, 0U};
    size_t count = (4U - (size % 4U)) % 4U;
    if (count != 0U) {
        cpio_write_all(file, zeros, count);
    }
}

static void cpio_write_header(FILE *file, uint32_t inode, mode_t mode, uid_t uid,
                              gid_t gid, uint32_t mtime, uint32_t file_size,
                              uint32_t name_size)
{
    char header[111];
    int length = snprintf(header, sizeof(header),
                          "070701%08x%08x%08x%08x%08x%08x%08x"
                          "%08x%08x%08x%08x%08x%08x",
                          inode, (uint32_t)mode, (uint32_t)uid, (uint32_t)gid,
                          1U, mtime, file_size, 0U, 0U, 0U, 0U, name_size, 0U);
    assert(length == 110);
    cpio_write_all(file, header, 110U);
}

static void write_minimal_cpio(const char *path)
{
    FILE *file = fopen(path, "wb");

    assert(file != NULL);
    cpio_write_header(file, 1U, S_IFDIR | 0755U, 0U, 0U, 0U, 0U, 4U);
    cpio_write_all(file, "bin\0", 4U);
    cpio_write_padding(file, 110U + 4U);
    cpio_write_header(file, 2U, S_IFREG | 0755U, 0U, 0U, 0U, 2U, 7U);
    cpio_write_all(file, "bin/sh\0", 7U);
    cpio_write_padding(file, 110U + 7U);
    cpio_write_all(file, "sh", 2U);
    cpio_write_padding(file, 2U);
    cpio_write_header(file, 0U, 0U, 0U, 0U, 0U, 0U, 11U);
    cpio_write_all(file, "TRAILER!!!\0", 11U);
    cpio_write_padding(file, 110U + 11U);
    assert(fclose(file) == 0);
}

static void tar_add_entry(struct archive *writer, const char *path, mode_t type,
                          const char *data)
{
    struct archive_entry *entry = archive_entry_new();
    size_t size = data == NULL ? 0U : strlen(data);

    assert(entry != NULL);
    archive_entry_set_pathname(entry, path);
    archive_entry_set_filetype(entry, type);
    archive_entry_set_perm(entry, 0755);
    archive_entry_set_size(entry, (la_int64_t)size);
    assert(archive_write_header(writer, entry) == ARCHIVE_OK);
    if (size != 0U) {
        assert(archive_write_data(writer, data, size) == (la_ssize_t)size);
    }
    archive_entry_free(entry);
}

static void write_minimal_tar_gz(const char *path)
{
    struct archive *writer = archive_write_new();

    assert(writer != NULL);
    assert(archive_write_add_filter_gzip(writer) == ARCHIVE_OK);
    assert(archive_write_set_format_pax_restricted(writer) == ARCHIVE_OK);
    assert(archive_write_open_filename(writer, path) == ARCHIVE_OK);
    tar_add_entry(writer, "rootfs/", AE_IFDIR, NULL);
    tar_add_entry(writer, "rootfs/bin/", AE_IFDIR, NULL);
    tar_add_entry(writer, "rootfs/bin/sh", AE_IFREG, "sh");
    assert(archive_write_close(writer) == ARCHIVE_OK);
    assert(archive_write_free(writer) == ARCHIVE_OK);
}

static void verify_installed_rootfs(const char *root, enum bp_rootfs_format format)
{
    char config_path[BP_PATH_CAPACITY];
    char *contents = NULL;
    char error[BP_ERROR_CAPACITY] = {0};
    struct bp_rootfs_info info;

    assert(bp_rootfs_verify_installed(root, &info, error, sizeof(error)) == 0);
    assert(info.format == format);
    assert(bp_path(config_path, sizeof(config_path), root,
                   BP_HOST_CONFIG_PATH, error, sizeof(error)) == 0);
    assert(bp_read_text_file(config_path, &contents, 1024U, error, sizeof(error)) == 0);
    assert(strstr(contents, "timeout=5") != NULL);
    assert(strstr(contents, "default=normal") != NULL);
    free(contents);
}

static void verify_installed_entry_script(const char *root, const char *fragment)
{
    char archive_path[BP_PATH_CAPACITY];
    char entry_path[BP_PATH_CAPACITY];
    char unpack_template[] = "/tmp/burning-progress-rootfs-unpack.XXXXXX";
    char *contents = NULL;
    char *unpack_root = mkdtemp(unpack_template);
    char error[BP_ERROR_CAPACITY] = {0};
    struct bp_rootfs_info info;

    assert(unpack_root != NULL);
    assert(bp_rootfs_verify_installed(root, &info, error, sizeof(error)) == 0);
    assert(bp_path(archive_path, sizeof(archive_path), root, BP_ROOTFS_PATH,
                   error, sizeof(error)) == 0);
    assert(bp_rootfs_unpack(archive_path, unpack_root, &info,
                            error, sizeof(error)) == 0);
    assert(bp_path(entry_path, sizeof(entry_path), unpack_root,
                   BP_DEFAULT_ENTRY_PATH, error, sizeof(error)) == 0);
    assert(bp_read_text_file(entry_path, &contents, 1024U,
                             error, sizeof(error)) == 0);
    assert(strstr(contents, fragment) != NULL);
    free(contents);
}

int main(void)
{
    char template[] = "/tmp/burning-progress-test.XXXXXX";
    char *root = mkdtemp(template);
    char sbin[BP_PATH_CAPACITY];
    char init[BP_PATH_CAPACITY];
    char backup[BP_PATH_CAPACITY];
    char source_init[BP_PATH_CAPACITY];
    char source_progress[BP_PATH_CAPACITY];
    char target[BP_PATH_CAPACITY];
    char error[BP_ERROR_CAPACITY] = {0};
    char burning_progress[BP_PATH_CAPACITY];
    ssize_t length;

    assert(root != NULL);
    assert(realpath("build/bin/burning-progress", burning_progress) != NULL);
    assert(bp_path(sbin, sizeof(sbin), root, "/sbin", error, sizeof(error)) == 0);
    assert(mkdir(sbin, 0755) == 0);
    assert(bp_path(init, sizeof(init), root, BP_INIT_PATH, error, sizeof(error)) == 0);
    assert(symlink("../lib/systemd/systemd", init) == 0);
    assert(bp_path(source_init, sizeof(source_init), root, "/source-init", error, sizeof(error)) == 0);
    assert(bp_path(source_progress, sizeof(source_progress), root, "/source-progress", error, sizeof(error)) == 0);
    make_file(source_init, "dispatcher", 0755);
    make_file(source_progress, "progress", 0755);

    assert(bp_install(root, source_init, source_progress, error, sizeof(error)) == 0);
    assert(bp_is_installed(root));
    length = readlink(init, target, sizeof(target) - 1U);
    assert(length > 0);
    target[length] = '\0';
    assert(strcmp(target, BP_BURNING_INIT_PATH) == 0);
    assert(bp_path(backup, sizeof(backup), root,
                   BP_ORIGINAL_INIT_PATH, error, sizeof(error)) == 0);
    length = readlink(backup, target, sizeof(target) - 1U);
    assert(length > 0);
    target[length] = '\0';
    assert(strcmp(target, "../lib/systemd/systemd") == 0);

    assert(bp_uninstall(root, error, sizeof(error)) == 0);
    length = readlink(init, target, sizeof(target) - 1U);
    assert(length > 0);
    target[length] = '\0';
    assert(strcmp(target, "../lib/systemd/systemd") == 0);

    {
        char source_template[] = "/tmp/burning-progress-rootfs-source.XXXXXX";
        char target_template[] = "/tmp/burning-progress-rootfs-target.XXXXXX";
        char archive[BP_PATH_CAPACITY];
        char source_bin[BP_PATH_CAPACITY];
        char source_sbin[BP_PATH_CAPACITY];
        char source_sh[BP_PATH_CAPACITY];
        char source_progress[BP_PATH_CAPACITY];
        char config_path[BP_PATH_CAPACITY];
        char *source_root = mkdtemp(source_template);
        char *target_root = mkdtemp(target_template);
        char *contents = NULL;
        struct bp_rootfs_info rootfs_info;

        assert(source_root != NULL);
        assert(target_root != NULL);
        assert(bp_path(source_bin, sizeof(source_bin), source_root,
                       "/bin", error, sizeof(error)) == 0);
        assert(bp_path(source_sbin, sizeof(source_sbin), source_root,
                       "/sbin", error, sizeof(error)) == 0);
        make_dir(source_bin);
        make_dir(source_sbin);
        assert(bp_path(source_sh, sizeof(source_sh), source_root,
                       "/bin/sh", error, sizeof(error)) == 0);
        assert(bp_path(source_progress, sizeof(source_progress), source_root,
                       "/sbin/burning-progress", error, sizeof(error)) == 0);
        make_file(source_sh, "sh", 0755);
        make_file(source_progress, "progress", 0755);
        assert(bp_path(archive, sizeof(archive), target_root,
                       "/rootfs.cpio", error, sizeof(error)) == 0);
        assert(bp_cpio_pack(source_root, archive, &rootfs_info,
                            error, sizeof(error)) == 0);
        assert(bp_rootfs_install(target_root, archive, &rootfs_info,
                                 error, sizeof(error)) == 0);
        assert(bp_path(config_path, sizeof(config_path), target_root,
                       BP_HOST_CONFIG_PATH, error, sizeof(error)) == 0);
        assert(bp_read_text_file(config_path, &contents, 1024U,
                                 error, sizeof(error)) == 0);
        assert(strstr(contents, "timeout=5") != NULL);
        assert(strstr(contents, "default=normal") != NULL);
        free(contents);
    }

    {
        char template[] = "/tmp/burning-progress-rootfs-pack.XXXXXX";
        char source_bin[BP_PATH_CAPACITY];
        char source_sh[BP_PATH_CAPACITY];
        char source_root[BP_PATH_CAPACITY];
        char output[BP_PATH_CAPACITY];
        char *command = NULL;
        char *workdir = mkdtemp(template);
        struct bp_rootfs_info rootfs_info;

        assert(workdir != NULL);
        assert(bp_path(source_root, sizeof(source_root), workdir,
                       "/rootfs", error, sizeof(error)) == 0);
        assert(bp_path(source_bin, sizeof(source_bin), source_root,
                       "/bin", error, sizeof(error)) == 0);
        make_dir(source_root);
        make_dir(source_bin);
        assert(bp_path(source_sh, sizeof(source_sh), source_root,
                       "/bin/sh", error, sizeof(error)) == 0);
        make_file(source_sh, "sh", 0755);
        assert(asprintf(&command, "cd '%s' && '%s' rootfs pack rootfs",
                        workdir, burning_progress) >= 0);
        run_shell_command(command);
        free(command);
        assert(bp_path(output, sizeof(output), workdir,
                       "/rootfs.cpio", error, sizeof(error)) == 0);
        assert(access(output, F_OK) == 0);
        assert(bp_rootfs_verify(output, &rootfs_info, error, sizeof(error)) == 0);
        assert(rootfs_info.format == BP_ROOTFS_CPIO);
        assert(bp_path(source_sh, sizeof(source_sh), source_root,
                       BP_PROGRESS_PATH, error, sizeof(error)) == 0);
        assert(access(source_sh, F_OK) == 0);
    }

    {
        char source_template[] = "/tmp/burning-progress-rootfs-install-dir-source.XXXXXX";
        char target_template[] = "/tmp/burning-progress-rootfs-install-dir-target.XXXXXX";
        char source_bin[BP_PATH_CAPACITY];
        char source_root[BP_PATH_CAPACITY];
        char source_sh[BP_PATH_CAPACITY];
        char source_sbin[BP_PATH_CAPACITY];
        char source_config[BP_PATH_CAPACITY];
        char source_entry[BP_PATH_CAPACITY];
        char *command = NULL;
        char *source_dir = mkdtemp(source_template);
        char *target_root = mkdtemp(target_template);

        assert(source_dir != NULL);
        assert(target_root != NULL);
        strcpy(source_root, source_dir);
        assert(bp_path(source_bin, sizeof(source_bin), source_root,
                       "/bin", error, sizeof(error)) == 0);
        make_dir(source_bin);
        assert(bp_path(source_sh, sizeof(source_sh), source_root,
                       "/bin/sh", error, sizeof(error)) == 0);
        make_file(source_sh, "sh", 0755);
        assert(asprintf(&command,
                        "printf '\\n\\n' | '%s' --root '%s' rootfs install '%s'",
                        burning_progress, target_root, source_root) >= 0);
        run_shell_command(command);
        free(command);
        assert(bp_path(source_sbin, sizeof(source_sbin), source_root,
                       BP_PROGRESS_PATH, error, sizeof(error)) == 0);
        assert(bp_path(source_config, sizeof(source_config), source_root,
                       BP_RUNTIME_CONFIG_PATH, error, sizeof(error)) == 0);
        assert(bp_path(source_entry, sizeof(source_entry), source_root,
                       BP_DEFAULT_ENTRY_PATH, error, sizeof(error)) == 0);
        assert(access(source_sbin, F_OK) == 0);
        assert(access(source_config, F_OK) == 0);
        assert(access(source_entry, X_OK) == 0);
        verify_installed_rootfs(target_root, BP_ROOTFS_CPIO);
    }

    {
        char source_template[] = "/tmp/burning-progress-rootfs-install-dir-entry-source.XXXXXX";
        char target_template[] = "/tmp/burning-progress-rootfs-install-dir-entry-target.XXXXXX";
        char entry_template[] = "/tmp/burning-progress-rootfs-install-dir-entry-file.XXXXXX";
        char source_bin[BP_PATH_CAPACITY];
        char source_root[BP_PATH_CAPACITY];
        char source_sh[BP_PATH_CAPACITY];
        char source_entry[BP_PATH_CAPACITY];
        char entry_file[BP_PATH_CAPACITY];
        char *command = NULL;
        char *source_dir = mkdtemp(source_template);
        char *target_root = mkdtemp(target_template);
        char *entry_dir = mkdtemp(entry_template);
        char *contents = NULL;

        assert(source_dir != NULL);
        assert(target_root != NULL);
        assert(entry_dir != NULL);
        strcpy(source_root, source_dir);
        assert(bp_path(source_bin, sizeof(source_bin), source_root,
                       "/bin", error, sizeof(error)) == 0);
        make_dir(source_bin);
        assert(bp_path(source_sh, sizeof(source_sh), source_root,
                       "/bin/sh", error, sizeof(error)) == 0);
        make_file(source_sh, "sh", 0755);
        assert(bp_path(entry_file, sizeof(entry_file), entry_dir,
                       "/entry.sh", error, sizeof(error)) == 0);
        make_file(entry_file, "#!/bin/sh\necho custom-entry\n", 0755);
        assert(asprintf(&command,
                        "printf '\\n\\n' | '%s' --root '%s' rootfs install '%s' --entry-file '%s'",
                        burning_progress, target_root, source_root, entry_file) >= 0);
        run_shell_command(command);
        free(command);
        assert(bp_path(source_entry, sizeof(source_entry), source_root,
                       BP_DEFAULT_ENTRY_PATH, error, sizeof(error)) == 0);
        assert(bp_read_text_file(source_entry, &contents, 1024U,
                                 error, sizeof(error)) == 0);
        assert(strstr(contents, "custom-entry") != NULL);
        free(contents);
        verify_installed_rootfs(target_root, BP_ROOTFS_CPIO);
        verify_installed_entry_script(target_root, "custom-entry");
    }

    {
        char source_template[] = "/tmp/burning-progress-rootfs-install-cpio-source.XXXXXX";
        char target_template[] = "/tmp/burning-progress-rootfs-install-cpio-target.XXXXXX";
        char entry_template[] = "/tmp/burning-progress-rootfs-install-cpio-entry.XXXXXX";
        char archive[BP_PATH_CAPACITY];
        char entry_file[BP_PATH_CAPACITY];
        char *command = NULL;
        char *source_root = mkdtemp(source_template);
        char *target_root = mkdtemp(target_template);
        char *entry_dir = mkdtemp(entry_template);

        assert(source_root != NULL);
        assert(target_root != NULL);
        assert(entry_dir != NULL);
        assert(bp_path(archive, sizeof(archive), source_root,
                       "/rootfs.cpio", error, sizeof(error)) == 0);
        write_minimal_cpio(archive);
        assert(bp_path(entry_file, sizeof(entry_file), entry_dir,
                       "/entry.sh", error, sizeof(error)) == 0);
        make_file(entry_file, "#!/bin/sh\necho archive-entry\n", 0755);
        assert(asprintf(&command,
                        "printf '\\n\\n' | '%s' --root '%s' rootfs install '%s' --entry-file '%s'",
                        burning_progress, target_root, archive, entry_file) >= 0);
        run_shell_command(command);
        free(command);
        verify_installed_rootfs(target_root, BP_ROOTFS_CPIO);
        verify_installed_entry_script(target_root, "archive-entry");
    }

    {
        char source_template[] = "/tmp/burning-progress-rootfs-install-cpio-handoff-source.XXXXXX";
        char target_template[] = "/tmp/burning-progress-rootfs-install-cpio-handoff-target.XXXXXX";
        char archive[BP_PATH_CAPACITY];
        char *command = NULL;
        char *source_root = mkdtemp(source_template);
        char *target_root = mkdtemp(target_template);

        assert(source_root != NULL);
        assert(target_root != NULL);
        assert(bp_path(archive, sizeof(archive), source_root,
                       "/rootfs.cpio", error, sizeof(error)) == 0);
        write_minimal_cpio(archive);
        assert(asprintf(&command,
                        "printf 'handoff\\n/bin/sh\\n' | '%s' --root '%s' rootfs install '%s'",
                        burning_progress, target_root, archive) >= 0);
        run_shell_command(command);
        free(command);
        verify_installed_rootfs(target_root, BP_ROOTFS_CPIO);
    }

    {
        char source_template[] = "/tmp/burning-progress-rootfs-install-tar-source.XXXXXX";
        char target_template[] = "/tmp/burning-progress-rootfs-install-tar-target.XXXXXX";
        char archive[BP_PATH_CAPACITY];
        char *command = NULL;
        char *source_root = mkdtemp(source_template);
        char *target_root = mkdtemp(target_template);

        assert(source_root != NULL);
        assert(target_root != NULL);
        assert(bp_path(archive, sizeof(archive), source_root,
                       "/rootfs.tar.gz", error, sizeof(error)) == 0);
        write_minimal_tar_gz(archive);
        assert(asprintf(&command,
                        "printf '\\n\\n' | '%s' --root '%s' rootfs install '%s'",
                        burning_progress, target_root, archive) >= 0);
        run_shell_command(command);
        free(command);
        verify_installed_rootfs(target_root, BP_ROOTFS_TAR_GZIP);
    }

    printf("test_install: OK\n");
    return 0;
}
