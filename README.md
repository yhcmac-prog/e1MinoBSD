# e1MinoBSD

基于 **FreeBSD 14.5-RELEASE** 的发行版，代号 **Mino**。使用 FreeBSD 官方二进制包组装，不自行编译内核与 world。

## 特性

- **两种变体**：`minimal`（纯控制台）与 `desktop`（图形桌面），共享同一条构建流水线
- **五种桌面**：e1wm Workstation / e1wm Mobile / MATE（默认）/ Xfce / Gershwin
- **离线包闭包**：desktop 变体内置 526 个 FreeBSD 包，断网环境可完整安装桌面
- **e1pkg 包管理器**：自研轻量包管理器，与原生 FreeBSD `pkg` 并存
- **setup-minobsd 安装器**：支持 UFS 与 ZFS 根布局，可无人值守自动安装
- **Live 介质**：只读 CD 根 + tmpfs 可写层（/var、/root 等），开箱即用

## 下载

预构建 ISO 见 [Releases](https://github.com/yhcmac-prog/e1MinoBSD/releases)。

GitHub 单文件上限 2GB，desktop ISO 以分卷提供。下载全部分卷后合并：

```sh
cat e1MinoBSD-1.0-desktop-amd64.iso.part-* > e1MinoBSD-1.0-desktop-amd64.iso
```

合并后大小应为 **7,809,312,768** 字节。

## 使用

1. 将 ISO 作为光盘启动（VirtualBox / bhyve / 物理机刻录均可）
2. 3 秒倒计时后自动进入 Live 环境，root 自动登录
3. desktop 变体默认进入 MATE 桌面（LightDM 免密登录）
4. 安装到硬盘：终端执行 `setup-minobsd`，或设置 loader 变量 `minobsd.autosetup=YES` 自动安装

## 从源码构建（macOS）

```sh
# 下载 FreeBSD 14.5 base/kernel 与包闭包后：
sudo E1BSD_VARIANT=desktop ./build.sh     # 桌面版
sudo E1BSD_VARIANT=minimal ./build.sh     # 最小版
```

产物位于 `build/e1MinoBSD-1.0-<variant>-amd64.iso`。

详细构建说明见 [docs/BUILD.md](docs/BUILD.md)，日常使用见 [docs/USAGE.md](docs/USAGE.md)。

## 目录结构

```
build.sh          构建入口（下载/组装/ISO 打包）
run-vbox.sh       VirtualBox 一键启动脚本（live/install 模式）
rootfs/           通用 rootfs 叠加层（rc.local、品牌信息、安装器）
rootfs-desktop/   desktop 变体叠加层（LightDM、MATE、e1wm 会话）
packages/src/     e1pkg 包源码（e1wm、minofetch、cowsay、hello）
tools/            构建与打包辅助工具
```

## 许可

FreeBSD 基本系统及第三方包遵循各自许可证（BSD/GPL 等）；本仓库中的构建脚本与自研组件以 BSD-2-Clause 发布。
