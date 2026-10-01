# 构建 e1MinoBSD

本文档描述如何在 macOS 上从源码构建 e1MinoBSD ISO。

## 前置条件

- macOS（Apple Silicon 或 Intel），已安装 Xcode Command Line Tools
- VirtualBox（用于 `run-vbox.sh` 实测）
- 约 25GB 可用磁盘空间（包闭包 + sparseimage + ISO）
- root 权限（构建过程需要挂载 sparseimage、chroot）

## 构建步骤

```sh
git clone https://github.com/yhcmac-prog/e1MinoBSD.git
cd e1MinoBSD

# 桌面变体（默认 MATE）
sudo E1BSD_VARIANT=desktop ./build.sh

# 或最小变体
sudo E1BSD_VARIANT=minimal ./build.sh
```

构建流程：

1. **下载**：FreeBSD 14.5-RELEASE 的 `base.txz` / `kernel.txz`，desktop 变体额外下载 526 个包的离线闭包到 `build/packages/All/`
2. **组装**：解压 base/kernel 到 sparseimage（`build/stage.sparseimage`，挂载于 `build/stage/`），安装包闭包，叠加 `rootfs/` 与 `rootfs-desktop/`（desktop 变体）
3. **打包**：生成可引导 ISO 到 `build/e1MinoBSD-1.0-<variant>-amd64.iso`

## 实测

```sh
# Live 启动（串口日志写入 .vbox/serial.log）
E1BSD_ISO=build/e1MinoBSD-1.0-desktop-amd64.iso ./run-vbox.sh live

# 无头模式
E1BSD_HEADLESS=1 E1BSD_ISO=build/e1MinoBSD-1.0-desktop-amd64.iso ./run-vbox.sh live

# 安装到虚拟硬盘后从硬盘启动
./run-vbox.sh install
```

## 重新构建

```sh
sudo ./build.sh            # 复用已下载的包
sudo ./build.sh clean      # 清理 stage 与 ISO
```

## 已知限制

- 仅支持 amd64
- desktop ISO 约 7.3GB（含完整离线包闭包）；minimal 约 500MB
