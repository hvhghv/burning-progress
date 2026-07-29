# Burning Progress Implementation Plan

## 1. Objective

Build a pure C99 Linux recovery and flashing launcher that can be installed into an
existing system, enter a RAM-backed root filesystem before the normal init is
started, and either expose a supervised maintenance shell or hand PID 1 to an
entry script supplied by the recovery rootfs.

The normal boot path must remain short and fail toward the original init. No
operation may write the system disk after a failed `pivot_root` or while the old
root is still mounted.

## 2. Delivered Programs

Two Linux executables will be built with a C99 compiler and Linux system APIs:

- `burning-init`: minimal PID 1 dispatcher installed at `/sbin/burning-init`.
  `/sbin/init` is atomically changed to point to this program.
- `burning-progress`: installer, configuration utility, rootfs archive tool, stage 1 boot
  menu, and stage 2 recovery supervisor.

`burning-init` is intentionally small. It only detects the boot trigger and
then uses `execve` to replace itself with either the original init or
`burning-progress --stage1`. It never forks a long-running child and never
implements a menu.

## 3. Installed Layout

```text
/sbin/init
/sbin/init.burning-original
/sbin/burning-init
/sbin/burning-progress

/etc/BurningProcess/config
/etc/BurningProcess/enable
/etc/BurningProcess/onlyOnce
/etc/BurningProcess/rootfs.cpio
/etc/BurningProcess/install-state
```

`install-state` records the installed program version, expected `/sbin/init`
link target, backup type, and integrity information needed by `uninstall`.

The recovery root contains its own runtime configuration and does not reuse the
host configuration after the root switch:

```text
/etc/burning-progress.conf
/entry.sh
```

## 4. Configuration Contracts

Host `/etc/BurningProcess/config`:

```ini
version=1
timeout=5
default=normal
```

- `timeout`: integer seconds, initially limited to 0 through 3600.
- `default`: `normal`, `shell`, or `poweroff`.
- Missing or invalid host configuration fails to `timeout=5` and
  `default=normal`.

Recovery `/etc/burning-progress.conf`:

```ini
version=1
entryMode=supervised
entry=/entry.sh
```

- `entryMode`: `supervised` or `handoff`.
- `entry`: absolute path inside the RAM root, without `..` components.
- Missing runtime configuration defaults to supervised `/entry.sh`.
- Interactive rootfs configuration creates a missing default `/entry.sh` as
  an executable `exec /bin/sh` script, but never overwrites an existing entry.
- An exact `handoff` value is required to transfer PID 1.
- `handoff` with a missing or invalid entry is rejected before `pivot_root`.

Configuration files are parsed as data, never sourced as shell scripts.

## 5. Trigger and Menu State Machine

The recovery menu is entered when either condition is true:

- the init arguments or kernel command line contain the exact
  `--BurningProcessEnable` token;
- `/etc/BurningProcess/enable` exists.

Without a trigger, `burning-init` immediately executes the original init.

With a trigger:

1. `burning-init` executes `burning-progress --stage1`, preserving PID 1.
2. Stage 1 loads the host configuration.
3. If both `enable` and `onlyOnce` exist, stage 1 attempts to remove `enable`
   and sync its parent directory. Failure is reported but does not stop boot.
4. Stage 1 opens `/dev/console`, displays the three choices, and waits for the
   configured timeout.
5. Explicit input overrides the configured default.

Menu results:

- `normal`: PID 1 executes the original init with compatible `argv[0]`.
- `poweroff`: synchronize filesystems and invoke the poweroff syscall.
- `shell`: validate and enter the RAM root.

## 6. RAM Root Transition

Stage 1 performs the following sequence for the shell option:

1. Detect and verify the CPIO or tar.gz rootfs, its required files, paths, and
   memory requirements.
2. Mount a private `tmpfs` at `/mnt/burning-root`.
3. Extract the archive without allowing absolute paths or parent traversal.
4. Read and validate `/etc/burning-progress.conf` from the extracted root.
5. Make mount propagation private.
6. Prepare or move `/dev`, `/proc`, `/sys`, and `/run` into the new root.
7. Create the new root's `/oldroot` directory and call `pivot_root`.
8. Change directory to `/` and execute `/sbin/burning-progress --stage2` from
   the RAM root.
9. Stage 2 closes inherited descriptors referencing the old root and performs
   a normal, non-lazy unmount of `/oldroot`.

Failure before `pivot_root` returns to the stage 1 menu and still permits a
normal boot. Failure after `pivot_root` never attempts to execute the original
init. Failure to unmount `/oldroot` disables system-disk flashing.

## 7. Runtime Entry Modes

### Supervised

`burning-progress` remains PID 1. If the configured entry exists, it is run as
a child and reaped. After the entry exits, or when no entry exists, PID 1
repeatedly starts `/bin/sh` on `/dev/console`. Shell launch failures use bounded
backoff rather than a busy loop.

### Handoff

After closing descriptors, resetting signal state, clearing timers, setting
the working directory, sanitizing the environment, and attaching standard
streams to `/dev/console`, PID 1 executes:

```text
/bin/sh <entry>
```

The entry script must use `exec` to start the final init or other PID 1
program. There is deliberately no fallback after a successful handoff.

## 8. Management Commands

The non-PID-1 `burning-progress` interface will provide:

```text
burning-progress install
burning-progress uninstall
burning-progress status
burning-progress enable [--once|--persistent]
burning-progress disable
burning-progress config --timeout <seconds>
burning-progress config --default <normal|shell|poweroff>
burning-progress rootfs pack <directory> --output <file> [--format cpio|tar.gz]
burning-progress rootfs unpack <file> --output <directory>
burning-progress rootfs configure <directory>
burning-progress rootfs verify <file>
burning-progress rootfs install <file>
```

Packing preflights the recovery source before opening a temporary archive, so
missing required files or an invalid handoff entry fail without writing the
rootfs payload. CPIO output uses uncompressed `newc`; tar.gz is the compressed
distribution format.

State writes and installation use a same-filesystem temporary file, `fsync`,
atomic rename, and parent-directory `fsync`. Uninstall restores the backup only
when `/sbin/init` still matches the installed dispatcher state.

## 9. Rootfs Archive Contract

- Supported formats are SVR4 `newc` CPIO and gzip-compressed tar. Format is
  detected from file magic during verify, unpack, install, and boot.
- `pack` defaults to CPIO, infers tar.gz from `.tar.gz` or `.tgz`, and accepts an
  explicit `--format cpio|tar.gz` override.
- CPIO paths are relative, normalized, and deterministically ordered.
- Symlinks are archived as symlinks and are not followed.
- Absolute paths, parent traversal, duplicate paths, devices, FIFOs, sockets,
  and unsupported types are rejected. Tar hardlinks are accepted only when
  they point backward to an in-root regular file; chained and forward links
  are rejected.
- Tar unpack may strip one explicit directory containing every archive entry,
  allowing a packaged base rootfs to be prepared before recovery files are
  added. Verify, install, and boot retain the full recovery-root requirements.
- Packing does not cross mount points by default.
- The output cannot reside inside the packed directory.
- Unpacking requires a missing or empty destination directory.
- Installation verifies the archive before atomically replacing
  `/etc/BurningProcess/rootfs.cpio`.
- The installed filename remains `rootfs.cpio` for backward compatibility and
  atomic replacement; the filename does not imply its content format.
- Archives do not preserve ACLs, xattrs, SELinux labels, file capabilities, or
  sparse-file metadata; recovery roots should use static programs or recreate
  required metadata.

The initial implementation will provide SHA-256 integrity metadata. A release
intended for untrusted update channels must add signature verification with an
embedded public key before enabling installation or boot.

## 10. Safety and Recovery Boundaries

- Both installed executables should be statically linked with musl for release
  targets. Development builds may use the system C library.
- The original init backup alone is not an external recovery mechanism. A
  production deployment should retain a bootloader entry that bypasses the
  dispatcher or invokes a verified original init/recovery image.
- `burning-init` falls back to the original init when the trigger cannot be
  evaluated or stage 1 cannot be executed.
- Stage 1 falls back to the original init on configuration or rootfs failures
  before the root transition.
- `umount -l` is never accepted as proof that the target disk is safe to flash.
- Firmware stored on the target disk must be copied to RAM or fetched over the
  network before the old root is unmounted.
- The stage 2 root must contain every module and firmware file needed after the
  system disk is unavailable.

## 11. Implementation Phases

1. C99 project skeleton, strict config parser, state model, and unit tests.
2. Management CLI, atomic state writes, install/uninstall transaction, and
   exact preservation of symlink or regular-file init backups.
3. Deterministic newc and tar.gz pack, verify, safe extraction, and atomic rootfs install.
4. Minimal `burning-init` dispatcher and stage 1 menu with console I/O.
5. Linux mount, `pivot_root`, descriptor cleanup, and stage 2 supervision.
6. Handoff cleanup and PID 1 transfer.
7. Strict `-std=c99 -Wall -Wextra -Werror` builds, musl static Linux builds,
   namespace-based non-destructive integration tests, and
   finally controlled VM testing as real PID 1.

## 12. Acceptance Criteria

- A normal boot executes the original init without displaying or waiting on a
  recovery menu.
- One-shot and persistent enable states behave deterministically across power
  loss at each atomic-write boundary.
- Invalid configuration defaults to normal boot.
- Invalid CPIO or tar.gz content cannot escape the extraction root.
- The old root must be normally unmounted before a system disk is declared
  writable.
- Supervised children are reaped and shell exit does not terminate PID 1.
- Handoff preserves PID 1 through the entry script's final `exec`.
- Uninstall refuses to overwrite an init that changed after installation.
- Unit and integration tests do not modify the host machine's real
  `/sbin/init` or mount namespace.
