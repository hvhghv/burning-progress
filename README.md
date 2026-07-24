# Burning Progress

[![Build and test](https://github.com/hvhghv/burning-progress/actions/workflows/build-test.yml/badge.svg)](https://github.com/hvhghv/burning-progress/actions/workflows/build-test.yml)

Burning Progress 是一个纯 C99 编写的 Linux 预启动恢复环境启动器。它通过在安装时接管 `/sbin/init`，在正常 init 启动前提供恢复菜单，并可将预先准备的 CPIO 或 tar.gz 根文件系统展开到 tmpfs 后切换为新的根目录。

项目本身不包含具体设备的刷机逻辑。实际刷机、分区、下载固件或启动其他 init 的流程应放在 recovery rootfs 的 `/entry.sh` 及其配套程序中。

> [!WARNING]
> 安装程序会修改系统的 `/sbin/init` 启动链。实现错误、架构不匹配、rootfs 缺失或断电都可能造成系统无法启动。请先在虚拟机或可恢复设备上验证，并保留串口控制台、Bootloader 旁路入口和外部恢复介质。

## 功能

- 使用独立的 `burning-init` 作为最小 PID 1 分发器。
- 未启用恢复模式时直接 `exec` 原来的 init，不引入常驻父进程。
- 支持单次启用和持久启用恢复菜单。
- 支持 `normal`、`shell`、`poweroff` 三种启动选项。
- 支持可配置的等待时间和超时默认选项。
- 支持打包、校验、解包和安装 SVR4 `newc` CPIO 与 gzip 压缩 tar 根文件系统。
- 将 recovery rootfs 解压到 tmpfs，并通过 `pivot_root` 切换根文件系统。
- 支持 `supervised` 和 `handoff` 两种 entry 执行模式。
- 安装和配置文件更新采用临时文件、`fsync` 和原子替换。
- 卸载前校验已安装文件，避免覆盖安装后被其他程序修改的 init。

## 项目结构

```text
.
|-- Makefile
|-- LICENSE
|-- THIRD_PARTY_LICENSES.md
|-- docs/
|   `-- IMPLEMENTATION_PLAN.md
|-- include/
|   `-- burning.h
|-- src/
|   |-- burning_init.c
|   |-- burning_progress.c
|   |-- common.c
|   |-- config.c
|   |-- cpio.c
|   |-- install.c
|   |-- rootfs.c
|   |-- runtime.c
|   |-- sha256.c
|   `-- tar.c
|-- third_party/
|   |-- SHA256SUMS
|   |-- libarchive-3.8.8.tar.gz
|   `-- zlib-1.3.2.tar.gz
`-- tests/
    |-- test_config.c
    |-- test_cpio.c
    `-- test_install.c
```

## 构建

要求：

- Linux
- 支持 C99 的 C 编译器
- GNU Make
- Linux 系统调用和头文件
- libarchive 和 zlib 开发文件

Ubuntu/Debian 可安装：

```sh
sudo apt-get install build-essential libarchive-dev zlib1g-dev
```

静态发布构建使用的 zlib 1.3.2 与 libarchive 3.8.8 上游源码包已固定在 `third_party/`。构建或更新依赖前可验证：

```sh
cd third_party
sha256sum -c SHA256SUMS
```

本机开发构建：

```sh
make clean
make all
make test
```

输出文件：

```text
build/bin/burning-init
build/bin/burning-progress
```

构建 musl 静态调试版和发布版：

```sh
make static
```

输出目录：

```text
build/static-debug/bin/    -Og、-g3，保留调试符号
build/static/bin/          -Os、section GC、strip --strip-all
```

部署和制作 recovery rootfs 应使用 `build/static/bin/`。`static-debug` 用于 GDB、崩溃定位和源码级调试，不应作为空间受限设备的正式发布文件。

Actions 中的调试包会同时使用带 `-g3` 的 zlib 和 libarchive；本地调试包能否进入第三方库源码取决于所提供静态库是否包含调试信息。CI 暂以 256 KiB 和 768 KiB 分别作为发布版 `burning-init`、`burning-progress` 的防回归上限。

该目标默认使用 `musl-gcc` 和 `strip`，并要求工具链可找到静态 `libarchive.a` 与 `libz.a`。也可以显式指定交叉编译器、strip 和静态依赖目录：

```sh
make clean
make MUSL_CC=/path/to/aarch64-linux-musl-gcc \
  MUSL_STRIP=/path/to/aarch64-linux-musl-strip \
  CPPFLAGS="-D_GNU_SOURCE -Iinclude -I/path/to/static-deps/include" \
  LDFLAGS="-L/path/to/static-deps/lib" static
```

## 持续集成

GitHub Actions 会执行以下检查：

- 在 Ubuntu 22.04 上完成本机严格 C99 构建和全部单元测试。
- 使用 `hvhghv/musl-gcc` 工具链构建 x86_64、ARM、AArch64 和 RISC-V 64。
- 每种架构生成 `static-debug` 与 strip 后的 `static` 两套包，并检查二进制不存在动态加载器依赖。
- 发布版使用 `-Os`、function/data sections 和链接器垃圾回收，并设置体积回归上限。
- 使用 QEMU 和对应架构的 BusyBox 1.38.0 rootfs 运行目标测试程序。
- 将目标程序注入 BusyBox rootfs，实际执行 CPIO 与 tar.gz 打包、解包、校验和隔离目录安装测试。
- 使用 `qemu-system-x86_64` 从 tar.gz recovery rootfs 启动真实 Linux 客体内核，验证 PID 1、挂载、`pivot_root` 和旧根目录卸载。
- 将调试版和发布版分别打包为带 SHA-256 校验文件的 Actions artifact。

测试 rootfs 来自 [`hvhghv/cross-software` 的 `v1.38.0-busybox` release](https://github.com/hvhghv/cross-software/releases/tag/v1.38.0-busybox)。上游 rootfs 中的 BusyBox 使用动态 musl，但本项目注入、测试和发布的 `burning-init` 与 `burning-progress` 均为静态 musl 程序。用户态 QEMU 矩阵负责跨架构程序测试；额外的 x86_64 QEMU system job 使用真实客体内核验证完整根切换流程。ARM、AArch64、RISC-V 和具体设备驱动仍需对应虚拟机或实际设备验证。

## 在临时根目录中测试安装

在操作真实系统前，可以使用 `--root` 在临时目录验证安装和卸载事务：

```sh
test_root=$(mktemp -d)
mkdir -p "$test_root/sbin"
ln -s ../lib/systemd/systemd "$test_root/sbin/init"

sudo build/bin/burning-progress --root "$test_root" install \
  --init-binary "$PWD/build/bin/burning-init" \
  --progress-binary "$PWD/build/bin/burning-progress"

build/bin/burning-progress --root "$test_root" status
sudo build/bin/burning-progress --root "$test_root" uninstall
```

`--root` 只影响管理命令的目标路径，不能模拟 PID 1、挂载和 `pivot_root` 行为。

## 安装到系统

确认二进制架构和链接方式正确后，以 root 身份安装：

```sh
sudo build/bin/burning-progress install
sudo build/bin/burning-progress status
```

默认安装布局：

```text
/sbin/init                         -> /sbin/burning-init
/sbin/init.burning-original        原 init 的备份
/sbin/burning-init
/sbin/burning-progress
/etc/BurningProcess/config
/etc/BurningProcess/install-state
```

安装程序要求 `burning-init` 和 `burning-progress` 位于同一个构建目录。也可以通过 `--init-binary` 和 `--progress-binary` 显式指定来源。

## 主系统配置

`/etc/BurningProcess/config` 使用严格的 `key=value` 格式：

```ini
version=1
timeout=5
default=normal
```

- `timeout`：菜单等待秒数，范围为 `0` 到 `3600`。
- `default`：超时后的操作，可设为 `normal`、`shell` 或 `poweroff`。

使用管理命令修改配置：

```sh
sudo /sbin/burning-progress config --timeout 10
sudo /sbin/burning-progress config --default shell
```

## 制作 recovery rootfs

待打包目录至少应包含：

```text
/bin/sh
/sbin/burning-progress
/etc/burning-progress.conf
/entry.sh
```

推荐使用完整的 BusyBox rootfs，并放入与目标设备架构匹配的 `burning-progress`。示例配置：

```ini
version=1
entryMode=supervised
entry=/entry.sh
```

CPIO 打包、解包、校验和安装：

```sh
build/bin/burning-progress rootfs pack ./rootfs --output ./rootfs.cpio
build/bin/burning-progress rootfs verify ./rootfs.cpio
build/bin/burning-progress rootfs unpack ./rootfs.cpio --output ./unpacked-rootfs
sudo build/bin/burning-progress rootfs install ./rootfs.cpio
```

tar.gz 使用相同命令；`.tar.gz` 和 `.tgz` 后缀会让 `pack` 自动选择格式，也可用 `--format` 显式指定：

```sh
build/bin/burning-progress rootfs pack ./rootfs --output ./rootfs.tar.gz
build/bin/burning-progress rootfs pack ./rootfs --output ./recovery.img --format tar.gz
build/bin/burning-progress rootfs verify ./rootfs.tar.gz
build/bin/burning-progress rootfs unpack ./rootfs.tar.gz --output ./unpacked-rootfs
sudo build/bin/burning-progress rootfs install ./rootfs.tar.gz
```

`verify`、`unpack`、`install` 和 PID 1 启动流程按文件 magic 检测格式，不依赖文件扩展名。为保持旧版本布局兼容及单文件原子替换，安装后的归档仍统一命名为 `/etc/BurningProcess/rootfs.cpio`，其中内容可以是 CPIO 或 tar.gz。

tar.gz 的 `rootfs unpack` 可用于准备尚未加入 `burning-progress` 的基础 rootfs；它执行完整的路径和类型安全检查，但不要求 recovery 入口文件已经存在。如果归档中所有内容都位于唯一的显式顶层目录内，该目录会自动剥离。`rootfs verify`、`rootfs install` 和启动流程仍要求 `/bin/sh`、`/sbin/burning-progress` 及 handoff 入口完整有效。

解包目标目录不存在时会自动创建；如果目录已经存在，则必须为空。归档校验或解包失败时命令返回非零状态。解包不是事务操作，失败后应删除目标目录，不要使用其中可能残留的文件。

安装后生成：

```text
/etc/BurningProcess/rootfs.cpio
/etc/BurningProcess/rootfs.cpio.sha256
```

两种打包器都不会跟随符号链接或跨越挂载点。校验器拒绝绝对路径、`.`/`..` 路径分量、重复路径、设备节点、FIFO 和 socket。tar.gz 仅允许指向归档内已出现普通文件的硬链接，拒绝越界、前向和链式硬链接。当前不保存 ACL、扩展属性、SELinux 标签、file capabilities 或稀疏文件属性，因此 recovery 环境应优先使用静态程序，或在启动时重新建立所需元数据。

## Entry 模式

### supervised

这是默认模式。`burning-progress` 保持为 PID 1，将 `/entry.sh` 作为子进程运行并回收。entry 退出后，程序会持续在 `/dev/console` 启动 `/bin/sh`。

### handoff

配置示例：

```ini
version=1
entryMode=handoff
entry=/entry.sh
```

该模式会由 PID 1 直接执行：

```text
/bin/sh /entry.sh
```

执行成功后不再提供兜底。若 `/entry.sh` 需要把 PID 1 交给其他 init，脚本最后必须使用 `exec`：

```sh
#!/bin/sh
exec /sbin/your-init
```

## 启用恢复模式

下次启动进入恢复菜单，默认按单次方式启用：

```sh
sudo /sbin/burning-progress enable
# 等价于
sudo /sbin/burning-progress enable --once
```

持久启用：

```sh
sudo /sbin/burning-progress enable --persistent
```

取消启用：

```sh
sudo /sbin/burning-progress disable
```

也可以在内核命令行中加入精确的 `--BurningProcessEnable` 标记。恢复菜单会连接 `/dev/console`，显式输入优先于配置的超时默认选项。

## 启动流程

```text
kernel
  -> /sbin/init -> burning-init (PID 1)
       |-- 未触发恢复模式 -> exec 原 init
       `-- 已触发恢复模式 -> exec burning-progress --stage1
                                |-- normal   -> exec 原 init
                                |-- poweroff -> 关机
                                 `-- shell    -> tmpfs + 解压 CPIO/tar.gz + pivot_root
                                                -> burning-progress --stage2
```

stage 2 会先关闭继承的额外文件描述符、关闭 swap，并正常卸载旧根目录。若旧根目录仍然挂载，程序不会执行 entry，而是打印警告并进入维护 shell。维护 shell 仍具有手工操作能力，此时不得写入系统盘。

## 卸载

```sh
sudo /sbin/burning-progress uninstall
```

卸载程序会验证已安装的 `burning-init` 和原 init 备份。若文件在安装后被修改，卸载会拒绝覆盖，必须先查明系统当前的 init 状态。

## 安全边界

- 正常启动回退不能代替独立的外部恢复机制。
- `rootfs.cpio.sha256` 只能检测已安装归档的意外损坏，不能验证发布者身份；不可信更新渠道需要额外的数字签名。
- 刷机镜像必须在卸载旧根目录前复制到内存，或在 recovery 环境中通过网络获取。
- recovery rootfs 必须自带系统盘卸载后仍需要的程序、共享库、内核模块和固件。
- CI 已使用 x86_64 完整虚拟机验证真实 PID 1、mount namespace 和 `pivot_root`；其他架构及板级存储、Bootloader 和驱动仍需在对应虚拟机或实际设备上测试。

更完整的设计约束和验收条件见 [`docs/IMPLEMENTATION_PLAN.md`](docs/IMPLEMENTATION_PLAN.md)。

## 许可证

本项目采用 [BSD 3-Clause License](LICENSE)。静态发布程序所含 libarchive 与 zlib 的许可证见 [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md)。
