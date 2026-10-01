# e1MinoBSD 使用指南

## Live 环境

ISO 启动后 3 秒倒计时自动引导。根文件系统为只读 CD，`/var`、`/root` 等通过 tmpfs 提供可写层，所有改动重启后丢失。

- 登录：root，无密码（desktop 变体 LightDM 自动登录）
- minimal 变体直接在控制台进入 root shell
- desktop 变体默认进入 MATE 桌面

## 切换桌面

desktop 变体预装五种会话，在 LightDM 登录界面右下角选择：

| 会话 | 说明 |
|---|---|
| MATE | 默认，传统完整桌面 |
| Xfce | 轻量完整桌面 |
| e1wm Workstation | 自研平铺窗口管理器（工作站布局） |
| e1wm Mobile | 自研平铺窗口管理器（移动布局） |
| Gershwin | GNUstep 风格桌面 |

## 安装到硬盘

```sh
setup-minobsd
```

交互式选择磁盘与文件系统：

- **UFS**：传统 FreeBSD 文件系统
- **ZFS**：支持快照/压缩，推荐内存 ≥ 4GB

### 无人值守安装

在 loader 提示符设置：

```
set minobsd.autosetup=YES
set minobsd.fs=zfs        # 或 ufs（默认 ufs）
set minobsd.disk=ada0     # 可选，默认第一块盘
boot
```

安装日志写入 `/var/log/autosetup.log`。

## e1pkg 包管理器

e1pkg 与 FreeBSD 原生 `pkg` 并存，互不影响：

```sh
e1pkg list                 # 列出 e1pkg 包
e1pkg install minofetch    # 从本地仓库安装
e1pkg remove minofetch
minofetch                  # 系统信息横幅（e1pkg 样例包）
```

内置样例包：`e1wm`、`minofetch`、`cowsay`、`hello`。

原生 pkg 照常可用（desktop 变体已离线配置本地仓库）：

```sh
pkg install firefox
```
