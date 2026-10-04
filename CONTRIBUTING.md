# 贡献指南

感谢你对 e1MinoBSD 的关注！本文档说明仓库结构、常见贡献方式、构建与提交流程，帮助你快速上手。

## 快速开始

```sh
git clone https://github.com/yhcmac-prog/e1MinoBSD.git
cd e1MinoBSD
```

构建需要 **macOS**（`hdiutil` / `sparseimage`），依赖：`python3`、`curl`、`tar`、`xz`。首次构建会从 FreeBSD 镜像下载 base.txz / kernel.txz 到 `downloads/`。

```sh
sudo E1BSD_VARIANT=minimal ./build.sh   # 最小控制台版
sudo E1BSD_VARIANT=desktop ./build.sh   # 桌面版（含 526 个离线包）
```

产物：`build/e1MinoBSD-1.0-<variant>-amd64.iso`，可用 `./run-vbox.sh live` 在 VirtualBox 中启动测试。

> `build/`、`downloads/`、`.vbox/`、`.upload/` 已被 `.gitignore` 忽略，不要提交构建产物。

## 目录职责

```
build.sh               构建入口：下载 -> rootfs 组装 -> ISO 打包
run-vbox.sh            VirtualBox 一键启动脚本（live / install 模式）
rootfs/                通用 rootfs 叠加层（minimal 与 desktop 共用）
  etc/                 rc.conf、rc.local、品牌信息、e1pkg.conf、fstab
  usr/sbin/setup-minobsd   硬盘安装器（UFS / ZFS）
rootfs-desktop/        desktop 变体叠加层（在 rootfs 之上合并）
  etc/rc.conf.d/       dbus、e1desktop、e1liveaccounts 启动项
  usr/local/sbin/      deskselect、gershwin-session
packages/src/          e1pkg 包源码（自研组件）
  <name>/meta          包元数据
  <name>/payload/      安装到 rootfs 的文件树
tools/                 构建辅助脚本（pfetch、fetch_pkgs、build_desktop_blob）
docs/                  BUILD.md / USAGE.md
```

## 常见贡献场景

### 1. 修改 Live 默认配置

编辑 `rootfs/etc/` 下对应文件（`rc.conf`、`rc.local`、`motd`、`issue`、`e1pkg.conf` 等）。desktop 专属配置放到 `rootfs-desktop/` 下，避免污染 minimal。

- 启动服务：优先用 `rc.conf.d/<svc>` 小文件，而不是直接改 `rc.conf`
- 品牌信息：`rootfs/etc/brand.conf`、`rootfs/etc/motd`、`rootfs/etc/issue`
- 自动安装：`setup-minobsd` 读取 loader 变量 `minobsd.autosetup=ufs|zfs`

### 2. 新增 / 修改 e1pkg 包

在 `packages/src/` 下新建 `mypkg/` 目录：

```
packages/src/mypkg/
  meta          # 必填，K=V 格式
  payload/      # 文件树，按相对路径铺到 rootfs
```

`meta` 字段：

| 字段 | 说明 |
|---|---|
| `name` | 包名（与目录名一致） |
| `version` | 版本号 |
| `summary` | 一句话描述 |
| `depends` | 依赖的 e1pkg 包名，逗号或空格分隔，无依赖留空 |

示例（`packages/src/hello/meta`）：

```
name=hello
version=1.0
summary=e1MinoBSD hello demo package
depends=
```

`payload/` 内的文件按目标路径布局，例如 `payload/usr/local/bin/hello` 会被安装到 `/usr/local/bin/hello`。

### 3. 新增桌面环境

在 `rootfs-desktop/` 下：

1. `usr/local/etc/rc.d/` 加入启动脚本（参考 `e1desktop`）
2. `usr/local/sbin/` 加入会话启动器（如 `gershwin-session`）
3. `usr/local/share/xsessions/*.desktop` 注册 X 会话
4. 对应的 FreeBSD 包需加入离线包闭包：编辑 `tools/fetch_pkgs.py` 中 desktop 包列表，运行 `python3 tools/fetch_pkgs.py` 重新下载 `build/packages/All/`

### 4. 修改构建脚本

- `build.sh` 为主流水线，新增阶段请保持 `msg "stage: ..."` 的输出风格
- 网络下载统一走 `tools/pfetch.py`（带断点续传与多镜像回退）
- 不要把大二进制文件提交进仓库；依赖通过 `tools/fetch_pkgs.py` 或 `pfetch` 获取

## 构建与测试

```sh
# 仅 stage，不打包 ISO（便于检查 rootfs 内容）
sudo E1BSD_VARIANT=desktop ./build.sh --stage-only

# VirtualBox live 测试
./run-vbox.sh live
./run-vbox.sh install          # 测试安装到虚拟磁盘
```

提 PR 前至少运行一次完整构建并启动确认：
- minimal：能进入 root 控制台，`minofetch` 输出正常
- desktop：3 秒后自动进入 MATE，root 免密登录

## 提交规范

- 分支名：`<type>/<short-desc>`，例如 `fix/e1pkg-deps`、`feat/xfce-session`、`docs/contributing`
- Commit 信息使用简体中文，第一行 < 50 字，必要时正文空一行后展开：

```
feat(desktop): 新增 Gershwin 会话选项

- rootfs-desktop/usr/local/sbin/gershwin-session
- 注册 xsessions/gershwin.desktop
```

type 取值：`feat` / `fix` / `refactor` / `docs` / `chore` / `test`。

## PR 流程

1. Fork 仓库，在新分支上提交改动
2. 确保本地完整构建通过，且未误提交 `build/`、`downloads/` 等大文件
3. 提交 Pull Request，描述：
   - 改动动机
   - 验证方式（构建/启动截图或日志）
   - 是否影响 minimal / desktop 任一变体
4. 维护者 review 通过后合并至 `main`

## 代码风格

- Shell 脚本：`#!/bin/sh` 优先（保证可被 `sh` 执行），用 `set -eu`
- Python：`python3`，兼容 3.9+
- 路径：脚本内用相对仓库根的路径，`cd "$(dirname "$0")"`
- 自研 C 组件（如 `packages/src/e1wm/e1wm.c`）遵循 K&R 风格

## 许可

本仓库构建脚本与自研组件使用 **BSD-2-Clause**（见 `LICENSE`）。提交即视为同意以相同许可证发布你的改动；引入第三方代码时请保留原始许可证并在 PR 描述中说明来源。
