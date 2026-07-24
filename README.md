# Burning Progress

[![Build and test](https://github.com/hvhghv/burning-progress/actions/workflows/build-test.yml/badge.svg)](https://github.com/hvhghv/burning-progress/actions/workflows/build-test.yml)

Burning Progress 是一个纯 C99 编写的 Linux 预启动恢复环境启动器。它通过在安装时接管 `/sbin/init`，在正常 init 启动前提供恢复菜单，并可将预先准备的 CPIO 根文件系统展开到 tmpfs 后切换为新的根目录。

项目本身不包含具体设备的刷机逻辑。实际刷机、分区、下载固件或启动其他 init 的流程应放在 recovery rootfs 的 `/entry.sh` 及其配套程序中。

> [!WARNING]
> 安装程序会修改系统的 `/sbin/init` 启动链。实现错误、架构不匹配、rootfs 缺失或断电都可能造成系统无法启动。请先在虚拟机或可恢复设备上验证，并保留串口控制台、Bootloader 旁路入口和外部恢复介质。

## 功能

- 使用独立的 `burning-init` 作为最小 PID 1 分发器。
- 未启用恢复模式时直接 `exec` 原来的 init，不引入常驻父进程。
- 支持单次启用和持久启用恢复菜单。
- 支持 `normal`、`shell`、`poweroff` 三种启动选项。
- 支持可配置的等待时间和超时默认选项。
- 支持打包、校验和安装 SVR4 `newc` 格式的 `rootfs.cpio`。
- 将 recovery rootfs 解压到 tmpfs，并通过 `pivot_root` 切换根文件系统。
- 支持 `supervised` 和 `handoff` 两种 entry 执行模式。
- 安装和配置文件更新采用临时文件、`fsync` 和原子替换。
- 卸载前校验已安装文件，避免覆盖安装后被其他程序修改的 init。

## 项目结构

```text
.
|-- Makefile
|-- LICENSE
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
|   |-- runtime.c
|   `-- sha256.c
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

发布到目标设备时建议使用与设备架构匹配的 musl 静态链接版本：

```sh
make static
```

该目标默认使用 `musl-gcc`。也可以显式指定交叉编译器：

```sh
make clean
make CC=/path/to/aarch64-linux-musl-gcc LDFLAGS=-static all
```

## 持续集成

GitHub Actions 会执行以下检查：

- 在 Ubuntu 22.04 上完成本机严格 C99 构建和全部单元测试。
- 使用 `hvhghv/musl-gcc` 工具链构建 x86_64、ARM、AArch64 和 RISC-V 64。
- 每种架构分别生成并检查 dynamic、static 两种链接版本。
- 使用 QEMU 和对应架构的 BusyBox 1.38.0 rootfs 运行目标测试程序。
- 将目标程序注入 BusyBox rootfs，实际执行 CPIO 打包、校验和隔离目录安装测试。
- 将两个程序打包为带 SHA-256 校验文件的 Actions artifact。

测试 rootfs 来自 [`hvhghv/cross-software` 的 `v1.38.0-busybox` release](https://github.com/hvhghv/cross-software/releases/tag/v1.38.0-busybox)。用户态 QEMU 测试不覆盖真实 PID 1、挂载和 `pivot_root`，这些路径仍需在完整虚拟机或实际设备上验证。

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

打包、校验和安装：

```sh
build/bin/burning-progress rootfs pack ./rootfs --output ./rootfs.cpio
build/bin/burning-progress rootfs verify ./rootfs.cpio
sudo build/bin/burning-progress rootfs install ./rootfs.cpio
```

安装后生成：

```text
/etc/BurningProcess/rootfs.cpio
/etc/BurningProcess/rootfs.cpio.sha256
```

CPIO 打包器不会跟随符号链接，也不允许路径逃逸；socket 会被拒绝。CPIO 不保存 ACL、SELinux 标签和 file capabilities，因此 recovery 环境应优先使用静态程序，或在启动时重新建立所需元数据。

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
                                `-- shell    -> tmpfs + 解压 CPIO + pivot_root
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
- `rootfs.cpio.sha256` 只能检测意外损坏，不能验证发布者身份；不可信更新渠道需要额外的数字签名。
- 刷机镜像必须在卸载旧根目录前复制到内存，或在 recovery 环境中通过网络获取。
- recovery rootfs 必须自带系统盘卸载后仍需要的程序、共享库、内核模块和固件。
- 用户态容器或 QEMU user mode 不能完整验证真实 PID 1、mount namespace 和 `pivot_root`；正式部署前应进行整机虚拟机或实际设备测试。

更完整的设计约束和验收条件见 [`docs/IMPLEMENTATION_PLAN.md`](docs/IMPLEMENTATION_PLAN.md)。

## 许可证

本项目采用 [BSD 3-Clause License](LICENSE)。
