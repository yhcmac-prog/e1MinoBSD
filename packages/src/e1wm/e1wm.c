/* e1wm — e1LibreOS 自研 framebuffer 图形界面（Workstation 版）
 *
 * 直接写 /dev/fb0 绘制桌面：无 X11/Wayland、无任何外部库依赖，
 * 静态编译后即为单一可执行文件，符合 e1LibreOS 的极简自研哲学。
 *
 * 功能：
 *   - 桌面：渐变背景 + 顶部状态栏（品牌 / 时钟 / 切换提示）
 *   - 应用：About、System（系统信息）、e1pkg（软件包管理）
 *   - 键盘：↑/↓ 或 j/k 选择，Enter 打开，Esc/q 返回，L 切换语言
 *   - 鼠标：evdev（/dev/input/event*），移动光标 / 点击打开应用 /
 *           点击标题栏关闭 / 滚轮滚动（桌面滚轮切换选中项）
 *   - 双语：中文 / English（L 键切换，/etc/e1lang 持久化，默认中文）
 *   - 字体：ASCII 用 8x8 位图字库；中文等 CJK 用 16x16 Unifont 字形
 *           （由 tools/mkfont.py 按源码中实际用到的字符生成 font16_cjk.h）
 *
 * 由 init(8) 在 tty1 上 respawn 启动；运行期间将 VT 置为图形模式，
 * tty2 保留文字控制台（Alt+F2），ttyS0 保留串口控制台。
 */

#define _GNU_SOURCE     /* strcasestr 等 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <errno.h>
#include <termios.h>
#include <signal.h>
#include <stdarg.h>
#include <poll.h>
#include <libutil.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/consio.h>
#include <sys/kbio.h>
#include <sys/fbio.h>
#include <sys/sysctl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <dev/evdev/input.h>

#include "font8x8_basic.h"
#include "font16_cjk.h"
#include "pinyin_dict.h"

#define FB_DEV    "/dev/fb0"

typedef unsigned int u32;
typedef unsigned char u8;

/* ---- 帧缓冲 ---- */
static int fbfd = -1;
static u32 *fbp;
static int W, H;

/* ---- 调色板（e1LibreOS 蓝）---- */
#define C_BG_TOP    0x18305e
#define C_BG_BOT    0x040c1e
#define C_BAR       0x0c1424
#define C_SEL       0x1f6feb
#define C_FG        0xe8eef8
#define C_DIM       0x7d8aa5
#define C_WIN       0x0b1220
#define C_TITLE     0x161d2e
#define C_ACCENT    0x4f9cf9

/* ---- macOS 风格（workstation）---- */
#define C_MENU      0x1a1a20         /* 菜单栏/ dock 底色（近黑） */
#define C_SHADOW    0x000000         /* 窗口投影 */
#define C_TL_CLOSE  0xff5f57         /* 红绿灯：关闭 */
#define C_TL_MIN    0xfebc2e         /* 红绿灯：最小化 */
#define C_TL_MAX    0x28c840         /* 红绿灯：最大化 */

/* ---- HarmonyOS 风格（mobile）---- */
#define C_HM_ACCENT 0x0a59f7         /* 鸿蒙蓝 */
#define C_HM_CARD   0x14171f         /* 卡片底色 */
#define C_HM_PILL   0xe8eef8         /* 手势条 */

/* ---- 前向声明（终端/浏览器在 draw_window 之前使用）---- */
static int key_raw;          /* 本次按键原始字符 */
static void redraw(void);
static void file_field(const char *path, const char *key, char *out, size_t n);

/* ---- 窗口动画 / 手势 / 布局前向声明 ---- */
static int anim_kind;        /* 0 无 1 打开 2 关闭 3 最小化（自 e1wm 主循环驱动） */
static int anim_frame;
#define ANIM_FRAMES 5
#define ANIM_MS     28       /* 每帧间隔（总动画约 140ms） */
static struct timeval anim_t0;
static u32 bg_at(int y);     /* 桌面渐变底色（圆角遮挡用） */
static void setup_start(void);   /* 图形安装向导入口（workstation） */
static int kbd_visible;      /* 虚拟键盘（mobile，实现在后） */
static void kbd_hide(void);

/* ---- 终端原始模式 ---- */
static struct termios saved_tio;

/* ================= 双语字符串 ================= */

static int lang = 0;            /* 0=中文  1=English */

/* 移动模式：/etc/e1mode=mobile 时启用（大字体 + 虚拟键盘 + 大触摸目标） */
static int mode_mobile = 0;

enum {
    /* 应用名块：必须与 APP_* 枚举顺序严格一致（启动器按下标取名/图标） */
    S_APP_FINDER, S_APP_SETTINGS, S_APP_ABOUT, S_APP_SYS, S_APP_ACTIVITY,
    S_APP_DISKUTIL, S_APP_CONSOLE, S_APP_TERM, S_APP_BROWSER, S_APP_PKG,
    S_APP_CALC, S_APP_CALENDAR, S_APP_CLOCK, S_APP_NOTES, S_APP_REMIND,
    S_APP_CONTACTS, S_APP_WEATHER, S_APP_STOCKS, S_APP_DICT, S_APP_FINDMY,
    S_APP_SCRIPT, S_APP_AIRPORT, S_APP_SHOT, S_APP_INSTALL,
    S_MENU_HINT, S_BOTTOM_HINT, S_BACK, S_LANG_SW,
    S_ABOUT_D1, S_ABOUT_D2, S_ABOUT_GFX, S_ABOUT_LIC,
    S_HOST, S_CPU, S_MEM, S_UPTIME, S_GFX, S_SELFMADE,
    S_PKG_TIP,
    S_TERM_HINT, S_TERM_HINT2,
    S_BR_URL, S_BR_GO, S_BR_HINT, S_BR_LOADING, S_BR_EMPTY, S_BR_ERR,
    S_BR_HOME, S_BR_LINK_HINT,
    S_INSTALL_TIP, S_INSTALL_DISK, S_INSTALL_CMD,
    S_MT_TITLE, S_GEST_HINT, S_RUN_DOT,
    S_COUNT
};

static const char *S[S_COUNT][2] = {
    [S_APP_FINDER]  = { "访达",             "Finder" },
    [S_APP_SETTINGS]={ "系统设置",          "Settings" },
    [S_APP_ABOUT]   = { "关于 e1LibreOS",   "About e1LibreOS" },
    [S_APP_SYS]     = { "系统信息",         "System Info" },
    [S_APP_ACTIVITY]={ "活动监视器",        "Activity Monitor" },
    [S_APP_DISKUTIL]={ "磁盘工具",         "Disk Utility" },
    [S_APP_CONSOLE] = { "控制台",           "Console" },
    [S_APP_TERM]    = { "终端",             "Terminal" },
    [S_APP_BROWSER] = { "e1 浏览器",        "e1 Browser" },
    [S_APP_PKG]     = { "软件包管理",       "e1pkg Manager" },
    [S_APP_CALC]    = { "计算器",           "Calculator" },
    [S_APP_CALENDAR]={ "日历",             "Calendar" },
    [S_APP_CLOCK]   = { "时钟",             "Clock" },
    [S_APP_NOTES]   = { "备忘录",           "Notes" },
    [S_APP_REMIND]  = { "提醒事项",         "Reminders" },
    [S_APP_CONTACTS]={ "通讯录",           "Contacts" },
    [S_APP_WEATHER] = { "天气",             "Weather" },
    [S_APP_STOCKS]  = { "股市",             "Stocks" },
    [S_APP_DICT]    = { "词典",             "Dictionary" },
    [S_APP_FINDMY]  = { "查找",             "Find" },
    [S_APP_SCRIPT]  = { "脚本编辑器",       "Script Editor" },
    [S_APP_AIRPORT] = { "AirPort 实用工具", "AirPort Utility" },
    [S_APP_SHOT]    = { "截屏",             "Screenshot" },
    [S_APP_INSTALL] = { "安装 e1MinoBSD",   "Install e1MinoBSD" },
    [S_MENU_HINT] = { "j/k 选择 · 回车打开 · L 语言",
                      "j/k select · Enter open · L lang" },
    [S_BOTTOM_HINT] = { "Alt+F2: 文字控制台   cuau0: 串口",
                        "Alt+F2: text console   cuau0: serial" },
    [S_BACK]      = { "Esc: 返回", "Esc: back" },
    [S_LANG_SW]   = { "L: English", "L: 中文" },
    [S_ABOUT_D1]  = { "一个参照 GhostBSD 的极简自研 BSD 发行版",
                      "A minimal self-made BSD distro, inspired by GhostBSD" },
    [S_ABOUT_D2]  = { "内核 + pkg + e1pkg / setup-minobsd / e1wm",
                      "kernel + pkg + e1pkg / setup-minobsd / e1wm" },
    [S_ABOUT_GFX] = { "图形界面：e1wm（framebuffer，无 X11）",
                      "Graphics: e1wm (framebuffer, no X11)" },
    [S_ABOUT_LIC] = { "许可：各组件遵循其自身许可",
                      "License: each component under its own license" },
    [S_HOST]      = { "主机名",   "Hostname" },
    [S_CPU]       = { "处理器",   "CPU" },
    [S_MEM]       = { "内存",     "Memory" },
    [S_UPTIME]    = { "运行时间", "Uptime" },
    [S_GFX]       = { "图形",     "Graphics" },
    [S_SELFMADE]  = { "e1wm framebuffer（自研）", "e1wm framebuffer (self-made)" },
    [S_PKG_TIP]   = { "提示：在 tty2 (Alt+F2) 执行 e1pkg install <包名>",
                      "Tip: run e1pkg install <pkg> on tty2 (Alt+F2)" },
    [S_TERM_HINT] = { "Ctrl+X Ctrl+C 关闭终端",   "Ctrl+X Ctrl+C close terminal" },
    [S_TERM_HINT2]= { "键入的命令直接被 /bin/sh 执行", "Commands run via /bin/sh" },
    [S_BR_URL]    = { "地址",       "Address" },
    [S_BR_GO]     = { "回车: 打开", "Enter: open" },
    [S_BR_HINT]   = { "示例: http://example.com  http://10.0.2.2:8000/",
                      "Examples: http://example.com  http://10.0.2.2:8000/" },
    [S_BR_LOADING]= { "加载中…",   "Loading…" },
    [S_BR_EMPTY]  = { "（空白页面）", "(empty page)" },
    [S_BR_ERR]    = { "无法打开地址：",  "Failed to open: " },
    [S_BR_HOME]   = { "主页: http://example.com", "Home: http://example.com" },
    [S_BR_LINK_HINT]={ "链接按数字 + 回车 跳转: [1]  首屏 0",
                      "Links: [N]+Enter — press 0 for top of page" },
    [S_INSTALL_TIP]={ "数据将被擦除，请提前备份",
                      "All data will be erased — back up first" },
    [S_INSTALL_DISK]={ "目标磁盘",        "Target disk" },
    [S_INSTALL_CMD]={ "命令",             "Command" },
    [S_MT_TITLE]  = { "最近任务",         "Recent tasks" },
    [S_GEST_HINT] = { "上滑主页 · 上滑停住多任务 · 侧边滑动返回",
                      "Swipe up: home · hold: multitask · edge swipe: back" },
    [S_RUN_DOT]   = { "运行中",           "Running" },
};

#define L(id) (S[id][lang])

static void lang_load(void)
{
    FILE *f = fopen("/etc/e1lang", "r");
    char b[8];
    if (f) {
        if (fgets(b, sizeof b, f) && (b[0] == 'e' || b[0] == 'E'))
            lang = 1;
        fclose(f);
    }
}

static void lang_save(void)
{
    FILE *f = fopen("/etc/e1lang", "w");
    if (f) { fputs(lang ? "en\n" : "zh\n", f); fclose(f); }
}

static void mode_load(void)
{
    FILE *f = fopen("/etc/e1mode", "r");
    char b[8];
    if (f) {
        if (fgets(b, sizeof b, f) && (b[0] == 'm' || b[0] == 'M'))
            mode_mobile = 1;
        fclose(f);
    }
}

/* 当前变体名（从 /etc/os-release 的 VARIANT 字段读取，缺省 Workstation） */
static char variant_str[32] = "Workstation";
static void read_variant(void)
{
    file_field("/etc/brand.conf", "VARIANT", variant_str, sizeof variant_str);
    if (!variant_str[0]) snprintf(variant_str, sizeof variant_str, "Workstation");
}

/* 移动模式下的字号与触摸目标尺寸（workstation 保持原值） */
#define SCALE_TXT    (mode_mobile ? 2 : 1)
#define MENU_ITEM_H  (mode_mobile ? 44 : 32)

/* ================= 基础绘制 ================= */

static void px(int x, int y, u32 c)
{
    if (x >= 0 && x < W && y >= 0 && y < H)
        fbp[y * W + x] = c;
}

static void fill(int x, int y, int w, int h, u32 c)
{
    int i, j;
    for (j = y; j < y + h && j < H; j++)
        for (i = x; i < x + w && i < W; i++)
            fbp[j * W + i] = c;
}

/* 实心圆（macOS 红绿灯用） */
static void disc(int cx, int cy, int r, u32 c)
{
    int dy, dx;
    for (dy = -r; dy <= r; dy++)
        for (dx = -r; dx <= r; dx++)
            if (dx * dx + dy * dy <= r * r)
                px(cx + dx, cy + dy, c);
}

/* ---- UTF-8 ---- */
static int utf8_next(const char *s, unsigned *cp)
{
    unsigned char c = (unsigned char)*s;
    unsigned v;
    int n, i;
    if (c < 0x80) { *cp = c; return 1; }
    n = (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 :
        (c & 0xF8) == 0xF0 ? 4 : 1;
    v = c & (0x7F >> n);
    for (i = 1; i < n && (s[i] & 0xC0) == 0x80; i++)
        v = (v << 6) | (s[i] & 0x3F);
    *cp = v;
    return n;
}

/* CJK 字形二分查找（font16_cjk.h 中 index 升序） */
static const unsigned char *cjk_lookup(unsigned cp)
{
    int lo = 0, hi = CJK_COUNT - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (cjk_index[mid] == cp) return cjk_bitmap[mid];
        if (cjk_index[mid] < cp) lo = mid + 1; else hi = mid - 1;
    }
    return NULL;
}

static void glyph8(int x, int y, unsigned char ch, u32 c, int scale)
{
    const char *bits = font8x8_basic[ch];
    int row, col, i, j;
    for (row = 0; row < 8; row++)
        for (col = 0; col < 8; col++)
            if ((bits[row] >> col) & 1)
                for (j = 0; j < scale; j++)
                    for (i = 0; i < scale; i++)
                        px(x + col * scale + i, y + row * scale + j, c);
}

static void glyph16(const unsigned char *g, int x, int y, u32 c, int scale)
{
    int row, col, i, j;
    for (row = 0; row < 16; row++)
        for (col = 0; col < 16; col++)
            if ((g[row * 2 + (col >> 3)] >> (7 - (col & 7))) & 1)
                for (j = 0; j < scale; j++)
                    for (i = 0; i < scale; i++)
                        px(x + col * scale + i, y + row * scale + j, c);
}

/* 8x8 ASCII + 16x16 CJK 字模文本，scale 为放大倍数 */
static void text(int x, int y, const char *s, u32 c, int scale)
{
    for (; *s; ) {
        unsigned cp;
        int n = utf8_next(s, &cp);
        if (cp < 128) {
            glyph8(x, y, (unsigned char)*s, c, scale);
            x += 8 * scale;
        } else {
            const unsigned char *g = cjk_lookup(cp);
            if (g) glyph16(g, x, y, c, scale);
            else glyph8(x, y, '?', c, scale);
            x += 16 * scale;
        }
        s += n;
    }
}

static int text_w(const char *s, int scale)
{
    int w = 0;
    for (; *s; ) {
        unsigned cp;
        int n = utf8_next(s, &cp);
        w += (cp < 128 ? 8 : 16) * scale;
        s += n;
    }
    return w;
}

/* 按显示宽度（ASCII=1 列，CJK=2 列）右侧补空格到 cells 列 */
static void padto(char *out, size_t n, const char *s, int cells)
{
    int w = text_w(s, 1) / 8;
    size_t len = strlen(s);
    if (len >= n) { memcpy(out, s, n - 1); out[n - 1] = 0; return; }
    memcpy(out, s, len + 1);
    while (w < cells && len < n - 1) { out[len++] = ' '; w++; }
    out[len] = 0;
}

/* ================= 应用输出缓冲 ================= */

#define MAXLINES 256
static char *lines[MAXLINES];
static int nlines = 0;

static void free_lines(void)
{
    int i;
    for (i = 0; i < nlines; i++) { free(lines[i]); lines[i] = NULL; }
    nlines = 0;
}

static void add_line(const char *s)
{
    if (nlines >= MAXLINES) {
        free(lines[0]);
        memmove(lines, lines + 1, sizeof(char *) * (MAXLINES - 1));
        nlines--;
    }
    lines[nlines++] = strdup(s);
}

/* 过滤 ANSI 转义序列（e1pkg 等命令行工具的彩色输出在 GUI 里会残留 "[0m"） */
static void ansi_filter(char *s)
{
    char *w = s;
    while (*s) {
        if (*s == '\033' && s[1] == '[') {
            s += 2;
            while (*s && (*s < '@' || *s > '~')) s++;   /* 参数与中间字节 */
            if (*s) s++;                                /* 终止字母 */
        } else {
            *w++ = *s++;
        }
    }
    *w = 0;
}

/* popen 运行命令，输出进行缓冲 */
static void capture(const char *cmd)
{
    char buf[512];
    FILE *p = popen(cmd, "r");
    free_lines();
    if (!p) { add_line("(no output)"); return; }
    while (fgets(buf, sizeof buf, p)) {
        size_t n = strlen(buf);
        if (n && buf[n - 1] == '\n') buf[n - 1] = 0;
        ansi_filter(buf);
        add_line(buf);
    }
    pclose(p);
    if (nlines == 0) add_line("(no output)");
}

/* 逐行找字段，兼容 "key: value"（procfs）与 "KEY=value"（os-release）两种格式 */
static void file_field(const char *path, const char *key, char *out, size_t n)
{
    char buf[4096];
    size_t kl = strlen(key);
    FILE *f = fopen(path, "r");
    out[0] = 0;
    if (!f) return;
    while (fgets(buf, sizeof buf, f)) {
        char *v = NULL;
        if (!strncmp(buf, key, kl)) {
            char *p = buf + kl;
            while (*p == ' ' || *p == '\t') p++;    /* procfs 字段名后有 tab/空格 */
            if (*p == ':') {                        /* procfs: key: value */
                v = p + 1;
            } else if (p == buf + kl && *p == '=') { /* os-release: KEY=value */
                v = p + 1;
                /* 去掉 os-release 值两边的引号 */
                size_t vl;
                while (*v == ' ' || *v == '\t') v++;
                vl = strlen(v);
                if (vl && v[vl - 1] == '\n') v[--vl] = 0;
                if (vl >= 2 && v[0] == '"' && v[vl - 1] == '"') { v[vl - 1] = 0; v++; }
                snprintf(out, n, "%s", v);
                break;
            }
        }
        if (!v) continue;
        while (*v == ' ' || *v == '\t') v++;
        {
            size_t l = strlen(v);
            if (l && v[l - 1] == '\n') v[l - 1] = 0;
            snprintf(out, n, "%s", v);
            break;
        }
    }
    fclose(f);
}

/* ================= 应用 ================= */

enum {
    APP_FINDER = 0, APP_SETTINGS, APP_ABOUT, APP_SYS, APP_ACTIVITY,
    APP_DISKUTIL, APP_CONSOLE, APP_TERM, APP_BROWSER, APP_PKG,
    APP_CALC, APP_CALENDAR, APP_CLOCK, APP_NOTES, APP_REMIND,
    APP_CONTACTS, APP_WEATHER, APP_STOCKS, APP_DICT, APP_FINDMY,
    APP_SCRIPT, APP_AIRPORT, APP_SHOT, APP_INSTALL, APP_COUNT
};

/* 可见应用数：两种图形变体均在启动器中显示全部应用
 * （mobile 网格含「安装到磁盘」文字说明；workstation dock 为图形安装向导） */
static int app_count_visible(void) { return APP_COUNT; }

/* 活动应用类型：静态窗口内容（lines 驱动）vs 交互驱动型 */
static int menu_sel = 0;        /* 当前选中的应用 */
static int open_app = -1;       /* -1 桌面，否则应用编号 */
static int scroll = 0;          /* 应用内容滚动 */
static int dirty = 1;           /* 需要整屏重绘 */

static void app_about(void)
{
    char v[128] = "", id[128] = "";
    file_field("/etc/os-release", "PRETTY_NAME", v, sizeof v);
    file_field("/etc/os-release", "VARIANT", id, sizeof id);
    add_line(v);
    add_line("");
    add_line(id[0] ? id : "VARIANT not set");
    add_line("");
    add_line(L(S_ABOUT_D1));
    add_line(L(S_ABOUT_D2));
    add_line("");
    add_line(L(S_ABOUT_GFX));
    add_line(L(S_ABOUT_LIC));
}

static void app_sys(void)
{
    char model[256] = "", mem[64] = "", uptime[64] = "";
    char line[512], pad[64];
    FILE *f;
    long up = 0;

    size_t mlen = sizeof model;
    if (sysctlbyname("hw.model", model, &mlen, NULL, 0) != 0) model[0] = 0;
    {
        long realmem = 0;
        size_t rlen = sizeof realmem;
        if (sysctlbyname("hw.realmem", &realmem, &rlen, NULL, 0) == 0 && realmem > 0)
            snprintf(mem, sizeof mem, "%ld MB", realmem / 1024 / 1024);
    }
    {
        struct timespec ts;
        if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
            up = ts.tv_sec;
            snprintf(uptime, sizeof uptime, "%ld min %ld sec", up / 60, up % 60);
        }
    }

    padto(pad, sizeof pad, L(S_HOST), 9);
    snprintf(line, sizeof line, "%s: %s", pad, "e1minobsd"); add_line(line);
    padto(pad, sizeof pad, L(S_CPU), 9);
    snprintf(line, sizeof line, "%s: %s", pad, model[0] ? model : "unknown"); add_line(line);
    padto(pad, sizeof pad, L(S_MEM), 9);
    snprintf(line, sizeof line, "%s: %s", pad, mem); add_line(line);
    padto(pad, sizeof pad, L(S_UPTIME), 9);
    snprintf(line, sizeof line, "%s: %s", pad, uptime); add_line(line);
    add_line("");
    padto(pad, sizeof pad, L(S_GFX), 9);
    snprintf(line, sizeof line, "%s: %s", pad, L(S_SELFMADE)); add_line(line);
    add_line("");
    /* uname 等更多系统信息 */
    f = popen("uname -a", "r");
    if (f) {
        if (fgets(line, sizeof line, f)) {
            size_t l = strlen(line);
            if (l && line[l - 1] == '\n') line[l - 1] = 0;
            add_line(line);
        }
        pclose(f);
    }
}

static void app_pkg(void)
{
    capture("e1pkg list available 2>&1; echo; "
            "echo '--- installed ---'; e1pkg list installed 2>&1");
    add_line("");
    add_line(L(S_PKG_TIP));
}

/* 安装到磁盘：检测磁盘 + 显示 setup-e1os 用法（不在 e1wm 内触发实际安装，
 * 避免图形界面下交互式安装器与 fb 抢终端；用户按 Alt+F2 到文字控制台执行） */
static void app_install(void)
{
    FILE *f;

    add_line(L(S_INSTALL_DISK));
    /* Detect raw disks via kern.disks, size via diskinfo */
    capture("for d in $(sysctl -n kern.disks); do\n"
            "  case \"$d\" in cd*|acd*) continue ;; esac\n"
            "  s=$(diskinfo -v /dev/$d 2>/dev/null | sed -n 's/.*# \\([0-9]*\\) bytes.*/\\1/p')\n"
            "  echo \"/dev/$d $(( s / 1024 / 1024 ))MB\"\n"
            "done");
    add_line("");

    add_line(L(S_INSTALL_CMD));
    f = popen("setup-minobsd --help 2>&1 | head -12", "r");
    if (f) {
        char buf[256];
        while (fgets(buf, sizeof buf, f)) {
            size_t l = strlen(buf);
            if (l && buf[l - 1] == '\n') buf[l - 1] = 0;
            add_line(buf);
        }
        pclose(f);
    } else {
        add_line("setup-minobsd not found");
    }
    add_line("");
    add_line(L(S_INSTALL_TIP));
    add_line("  setup-minobsd");
    add_line("  setup-minobsd --auto --fs ufs --disk ada0");
}

/* ================= 迷你应用集（访达/设置/计算器/日历/时钟/备忘录等） ================= */
/*
 * 设计：所有迷你应用共享一份 ui_* 状态（同一时刻只开一个应用）。
 *  - 信息型（活动监视器/磁盘工具/控制台/AirPort/通讯录/天气/股市/词典结果/查找结果）
 *    走通用 lines[] 渲染；
 *  - 交互型（访达/系统设置/计算器/日历/时钟/备忘录/提醒事项/脚本编辑器/截屏）
 *    由 app_draw_custom() 自绘，app_key_custom() 处理按键。
 */
#define UI_EROWS 12
#define UI_ECOLS 78
#define UI_LISTN 96
static char ui_ed[UI_EROWS][UI_ECOLS + 1];   /* 多行编辑器（备忘录/脚本） */
static int  ui_en, ui_er, ui_ec;             /* 编辑行数 / 光标行 / 列 */
static char ui_in[256];                      /* 单行输入框 */
static int  ui_il;
static int  ui_sel;                          /* 列表选择 */
static int  ui_mode;                         /* 0 主视图；1 次视图（Esc 返回） */
static int  ui_cal_m;                        /* 日历相对当前月的偏移 */
static char ui_msg[160];                     /* 应用内提示行 */
static char ui_path[256];                    /* 访达当前目录 */
static char ui_ls_name[UI_LISTN][64];        /* 访达目录项 */
static unsigned char ui_ls_dir[UI_LISTN];
static int  ui_ls_n;

/* 前向声明（迷你应用按键里要回调） */
static void run_app(int app);
static void app_close(void);
static void rmd_add(void);
static void fill_round(int x, int y, int w, int h, int r, u32 c);
static int kbd_layout;                       /* 正式定义在虚拟键盘段 */
static void kbd_toggle_layout(void);

static void ui_msgf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(ui_msg, sizeof ui_msg, fmt, ap);
    va_end(ap);
}

/* ---- 计算器：递归下降四则运算 ---- */
static const char *calc_p;
static double calc_expr(void);
static void calc_sp(void) { while (*calc_p == ' ') calc_p++; }

/* 数值词法：支持小数、括号与一元正负号 */
static double calc_num(void)
{
    double v = 0, frac = 0, divv = 1; int dot = 0;
    calc_sp();
    if (*calc_p == '(') { calc_p++; v = calc_expr(); calc_sp(); if (*calc_p == ')') calc_p++; return v; }
    if (*calc_p == '-') { calc_p++; return -calc_num(); }
    if (*calc_p == '+') { calc_p++; return calc_num(); }
    while (1) {
        char c = *calc_p;
        if (c >= '0' && c <= '9') {
            if (dot) { divv *= 10; frac += (c - '0') / divv; }
            else v = v * 10 + (c - '0');
            calc_p++;
        } else if (c == '.' && !dot) { dot = 1; calc_p++; }
        else break;
    }
    return v + frac;
}

static double calc_term(void)
{
    double v = calc_num();
    for (;;) {
        calc_sp();
        if (*calc_p == '*') { calc_p++; v *= calc_num(); }
        else if (*calc_p == '/') { calc_p++; v /= calc_num(); }
        else if (*calc_p == '%') { calc_p++; { double d = calc_num(); if (d) v = (long long)v % (long long)d; } }
        else break;
    }
    return v;
}

static double calc_expr(void)
{
    double v = calc_term();
    for (;;) {
        calc_sp();
        if (*calc_p == '+') { calc_p++; v += calc_term(); }
        else if (*calc_p == '-') { calc_p++; v -= calc_term(); }
        else break;
    }
    return v;
}

static void app_calc_run(void)
{
    char out[96];
    calc_p = ui_in;
    if (ui_il == 0) return;
    snprintf(out, sizeof out, "= %g", calc_expr());
    add_line(ui_in);
    add_line(out);
    ui_il = 0; ui_in[0] = 0;
    ui_msgf("%s", out);
    dirty = 1;
}

/* ---- 访达：目录浏览 + 文件预览 ---- */
static void finder_load(const char *path)
{
    char cmd[320];
    FILE *f;
    snprintf(ui_path, sizeof ui_path, "%s", path);
    if (ui_path[0] == 0) snprintf(ui_path, sizeof ui_path, "/");
    ui_ls_n = 0;
    snprintf(cmd, sizeof cmd,
             "ls -a1p \"%s\" 2>/dev/null | head -%d", ui_path, UI_LISTN);
    f = popen(cmd, "r");
    if (f) {
        while (ui_ls_n < UI_LISTN && fgets(ui_ls_name[ui_ls_n], 64, f)) {
            size_t l = strlen(ui_ls_name[ui_ls_n]);
            if (l && ui_ls_name[ui_ls_n][l - 1] == '\n')
                ui_ls_name[ui_ls_n][--l] = 0;
            if (strcmp(ui_ls_name[ui_ls_n], ".") == 0) continue;
            if (l && ui_ls_name[ui_ls_n][l - 1] == '/') {
                ui_ls_dir[ui_ls_n] = 1;
                ui_ls_name[ui_ls_n][l - 1] = 0;
            } else ui_ls_dir[ui_ls_n] = 0;
            if (strcmp(ui_ls_name[ui_ls_n], "..") == 0 ||
                strcmp(ui_ls_name[ui_ls_n], ".") != 0)
                ui_ls_n++;
        }
        pclose(f);
    }
    if (ui_sel >= ui_ls_n) ui_sel = 0;
}

static void finder_up(void)
{
    char tmp[256];
    size_t l;
    if (strcmp(ui_path, "/") == 0) return;
    snprintf(tmp, sizeof tmp, "%s", ui_path);
    l = strlen(tmp);
    while (l > 1 && tmp[l - 1] == '/') tmp[--l] = 0;
    while (l > 1 && tmp[l - 1] != '/') l--;
    if (l <= 1) finder_load("/");
    else { tmp[l - 1] = 0; finder_load(l > 1 ? tmp : "/"); }
}

static void finder_enter(void)
{
    char p[300];
    if (ui_ls_n == 0) return;
    if (strcmp(ui_ls_name[ui_sel], "..") == 0) {
        finder_up();
        ui_sel = 0;
        return;
    }
    snprintf(p, sizeof p, "%s%s%s", ui_path,
             ui_path[strlen(ui_path) - 1] == '/' ? "" : "/", ui_ls_name[ui_sel]);
    if (ui_ls_dir[ui_sel]) { finder_load(p); ui_sel = 0; }
    else {
        char cmd[400];
        snprintf(cmd, sizeof cmd,
                 "echo '== %s =='; "
                 "if [ -r \"%s\" ]; then head -40 \"%s\"; "
                 "else echo '(permission denied)'; fi", p, p, p);
        capture(cmd);
        ui_mode = 1;
    }
}

/* ---- 备忘录 / 提醒事项 / 脚本 的持久化 ---- */
static void notes_load(void)
{
    FILE *f = fopen("/root/.e1notes", "r");
    int i;
    ui_en = 0;
    if (!f) f = fopen("/tmp/e1notes.txt", "r");
    if (f) {
        while (ui_en < UI_EROWS && fgets(ui_ed[ui_en], UI_ECOLS + 1, f)) {
            size_t l = strlen(ui_ed[ui_en]);
            if (l && ui_ed[ui_en][l - 1] == '\n') ui_ed[ui_en][l - 1] = 0;
            ui_en++;
        }
        fclose(f);
    }
    if (ui_en == 0) { snprintf(ui_ed[0], UI_ECOLS + 1, "%s", ""); ui_en = 1; }
    for (i = 0; i < UI_EROWS; i++) if (!ui_ed[i][0] && i < ui_en) {}
    ui_er = ui_en - 1; ui_ec = (int)strlen(ui_ed[ui_er]);
}

static void notes_save(void)
{
    FILE *f = fopen("/root/.e1notes", "w");
    int i;
    if (!f) f = fopen("/tmp/e1notes.txt", "w");
    if (f) {
        for (i = 0; i < ui_en; i++) fprintf(f, "%s\n", ui_ed[i]);
        fclose(f);
    }
}

#define RMD_MAX 24
#define RMD_LEN 64
static char rmd_txt[RMD_MAX][RMD_LEN + 1];
static unsigned char rmd_done[RMD_MAX];
static int rmd_n;

static void rmd_load(void)
{
    FILE *f = fopen("/root/.e1remind", "r");
    rmd_n = 0;
    if (!f) f = fopen("/tmp/e1remind.txt", "r");
    if (f) {
        while (rmd_n < RMD_MAX && fgets(rmd_txt[rmd_n], RMD_LEN + 1, f)) {
            size_t l = strlen(rmd_txt[rmd_n]);
            if (l && rmd_txt[rmd_n][l - 1] == '\n') rmd_txt[rmd_n][l - 1] = 0;
            rmd_done[rmd_n] = (rmd_txt[rmd_n][0] == '1');
            memmove(rmd_txt[rmd_n], rmd_txt[rmd_n] + 2, strlen(rmd_txt[rmd_n] + 2) + 1);
            rmd_n++;
        }
        fclose(f);
    }
    if (ui_sel >= rmd_n) ui_sel = rmd_n ? rmd_n - 1 : 0;
}

static void rmd_save(void)
{
    FILE *f = fopen("/root/.e1remind", "w");
    int i;
    if (!f) f = fopen("/tmp/e1remind.txt", "w");
    if (f) {
        for (i = 0; i < rmd_n; i++) fprintf(f, "%d %s\n", rmd_done[i], rmd_txt[i]);
        fclose(f);
    }
}

static void script_load(void)
{
    FILE *f = fopen("/tmp/e1script.sh", "r");
    int i;
    ui_en = 0;
    if (f) {
        while (ui_en < UI_EROWS && fgets(ui_ed[ui_en], UI_ECOLS + 1, f)) {
            size_t l = strlen(ui_ed[ui_en]);
            if (l && ui_ed[ui_en][l - 1] == '\n') ui_ed[ui_en][l - 1] = 0;
            ui_en++;
        }
        fclose(f);
    }
    if (ui_en == 0) {
        snprintf(ui_ed[0], UI_ECOLS + 1, "#!/bin/sh");
        snprintf(ui_ed[1], UI_ECOLS + 1, "echo hello from e1LibreOS");
        ui_en = 2;
    }
    for (i = 0; i < UI_EROWS; i++) (void)0;
    ui_er = ui_en - 1; ui_ec = (int)strlen(ui_ed[ui_er]);
}

static void script_run(void)
{
    FILE *f; int i;
    f = fopen("/tmp/e1script.sh", "w");
    if (f) {
        for (i = 0; i < ui_en; i++) fprintf(f, "%s\n", ui_ed[i]);
        fclose(f);
    }
    chmod("/tmp/e1script.sh", 0755);
    capture("sh /tmp/e1script.sh 2>&1 | head -40");
    ui_mode = 1;
}

/* ---- 内置词典（离线中英小词典，输入即查） ---- */
static const char *dict_pairs[][2] = {
    {"hello", "你好"}, {"world", "世界"}, {"computer", "计算机"}, {"file", "文件"},
    {"folder", "文件夹"}, {"system", "系统"}, {"network", "网络"}, {"screen", "屏幕"},
    {"keyboard", "键盘"}, {"mouse", "鼠标"}, {"window", "窗口"}, {"terminal", "终端"},
    {"package", "软件包"}, {"kernel", "内核"}, {"memory", "内存"}, {"disk", "磁盘"},
    {"battery", "电池"}, {"cloud", "云"}, {"music", "音乐"}, {"photo", "照片"},
    {"clock", "时钟"}, {"calendar", "日历"}, {"mail", "邮件"}, {"map", "地图"},
    {"game", "游戏"}, {"book", "图书"}, {"note", "备忘录"}, {"weather", "天气"},
    {"你好", "hello"}, {"世界", "world"}, {"计算机", "computer"}, {"文件", "file"},
    {"系统", "system"}, {"网络", "network"}, {"终端", "terminal"}, {"内核", "kernel"},
    {"内存", "memory"}, {"磁盘", "disk"}, {"时钟", "clock"}, {"日历", "calendar"},
    {"天气", "weather"}, {"邮件", "mail"}, {"游戏", "game"}, {"备忘录", "note"},
    {0, 0}
};

static void app_dict_lookup(void)
{
    int i, hits = 0;
    if (ui_il == 0) return;
    add_line("");
    for (i = 0; dict_pairs[i][0]; i++) {
        if (strncmp(dict_pairs[i][0], ui_in, ui_il) == 0 ||
            strncmp(dict_pairs[i][1], ui_in, ui_il) == 0) {
            char row[160];
            snprintf(row, sizeof row, "  %s  =  %s",
                     dict_pairs[i][0], dict_pairs[i][1]);
            add_line(row);
            hits++;
        }
    }
    if (!hits) add_line(lang ? "  (no match - offline mini dictionary)"
                             : "  （无匹配 —— 离线迷你词典）");
    ui_il = 0; ui_in[0] = 0;
    dirty = 1;
}

/* ---- 截屏：当前帧缓冲转存 PPM ---- */
static void shot_save(void)
{
    char path[64];
    FILE *f;
    int n = 0, x, y;
    for (n = 1; n < 100; n++) {
        snprintf(path, sizeof path, "/tmp/e1shot-%02d.ppm", n);
        f = fopen(path, "r");
        if (!f) break; fclose(f);
    }
    f = fopen(path, "wb");
    if (!f) { ui_msgf("cannot write %s", path); return; }
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (y = 0; y < H; y++)
        for (x = 0; x < W; x++) {
            u32 c = fbp[y * W + x];
            unsigned char rgb[3] = { (c >> 16) & 255, (c >> 8) & 255, c & 255 };
            fwrite(rgb, 1, 3, f);
        }
    fclose(f);
    ui_msgf("%s  %dx%d", path, W, H);
    dirty = 1;
}

/* ---- 应用打开时初始化各自状态 / 内容 ---- */
static void app_open_custom(int app)
{
    ui_mode = 0; ui_sel = 0; ui_msg[0] = 0;
    ui_il = 0; ui_in[0] = 0; ui_cal_m = 0;
    switch (app) {
    case APP_FINDER:
        finder_load(getenv("HOME") ? getenv("HOME") : "/root");
        break;
    case APP_SETTINGS:
        break;
    case APP_CALC:
        add_line(lang ? "Calculator  (+ - * / %% and parentheses)"
                      : "计算器（+ - * / %% 与括号）");
        break;
    case APP_NOTES:
        memset(ui_ed, 0, sizeof ui_ed);
        notes_load();
        break;
    case APP_REMIND:
        rmd_load();
        ui_msgf(lang ? "type + Enter to add, space toggle, d delete"
                     : "输入后回车添加，空格勾选，d 删除");
        break;
    case APP_SCRIPT:
        memset(ui_ed, 0, sizeof ui_ed);
        script_load();
        break;
    case APP_ACTIVITY:
        capture("echo '== processes =='; ps aux | head -15; "
                "echo; echo '== loadavg =='; sysctl -n vm.loadavg; "
                "echo; echo '== memory =='; vmstat -s | head -5; "
                "echo; echo '== uptime =='; uptime");
        break;
    case APP_DISKUTIL:
        capture("echo '== filesystems =='; df -h; "
                "echo; echo '== disks =='; sysctl -n kern.disks; "
                "echo; echo '== mounts =='; mount | head -12");
        break;
    case APP_CONSOLE:
        capture("dmesg | tail -45; "
                "echo; echo '== last =='; last | head -5");
        break;
    case APP_AIRPORT:
        capture("echo '== interfaces =='; ifconfig -a; "
                "echo; echo '== routes =='; netstat -rn | head; "
                "echo; echo '== resolv.conf =='; cat /etc/resolv.conf");
        break;
    case APP_CONTACTS: {
        static const char *demo[][2] = {
            {"e1 团队",  "support@e1minobsd.local"},
            {"张三",     "138-0000-0001"},
            {"Alice",    "+1 555-0101"},
            {"Bob",      "+1 555-0102"},
            {"王芳",     "139-0000-0002"},
            {"李雷",     "137-0000-0003"},
            {0, 0}
        };
        int i;
        add_line(lang ? "Contacts (offline demo)" : "通讯录（离线演示）");
        add_line("");
        for (i = 0; demo[i][0]; i++) {
            char row[128];
            snprintf(row, sizeof row, "  %-12s %s", demo[i][0], demo[i][1]);
            add_line(row);
        }
        break;
    }
    case APP_WEATHER:
        add_line(lang ? "Weather (offline demo)" : "天气（离线演示）");
        add_line("");
        add_line(lang ? "  Beijing   cloudy   18 ~ 26 C" : "  北京       多云     18 ~ 26 C");
        add_line(lang ? "  Shanghai  rain     21 ~ 24 C" : "  上海       小雨     21 ~ 24 C");
        add_line(lang ? "  Shenzhen  sunny    26 ~ 32 C" : "  深圳       晴       26 ~ 32 C");
        add_line(lang ? "  Hangzhou  overcast 20 ~ 27 C" : "  杭州       阴       20 ~ 27 C");
        add_line("");
        add_line(lang ? "  (live data requires a network app)"
                      : "  （实时天气需联网应用支持）");
        break;
    case APP_STOCKS:
        add_line(lang ? "Stocks (offline demo)" : "股市（离线演示）");
        add_line("");
        add_line("  AAPL   189.45  +0.82%");
        add_line("  GOOG   142.10  -0.31%");
        add_line("  000001  3102.5  +0.45%");
        add_line("  600519  1688.0  +1.12%");
        add_line("");
        add_line(lang ? "  (quotes are static samples)" : "  （报价为静态样例）");
        break;
    case APP_DICT:
        add_line(lang ? "Dictionary (offline mini)" : "词典（离线迷你版）");
        add_line(lang ? "type a word/Chinese, Enter to search"
                      : "输入英文单词或中文，回车查询");
        break;
    case APP_FINDMY:
        add_line(lang ? "Find - local filename search" : "查找 —— 本地文件名搜索");
        add_line(lang ? "keyword, Enter: find / -name '*kw*'"
                      : "输入关键词，回车：find / -name '*关键词*'");
        break;
    case APP_CALENDAR:
    case APP_CLOCK:
    case APP_SHOT:
        break;
    default:
        break;
    }
}

static void app_findmy_run(void)
{
    char cmd[320];
    if (ui_il == 0) return;
    snprintf(cmd, sizeof cmd,
             "find / -xdev -iname '*%s*' 2>/dev/null | head -40", ui_in);
    capture(cmd);
    add_line("");
    ui_il = 0; ui_in[0] = 0;
    dirty = 1;
}

/* ---- 通用输入框按键（可打印字符/退格/回车），回车返回 1 ---- */
static int ui_input_key(char *buf, int *len, int max, int k,
                        void (*on_enter)(void))
{
    if (k >= 0x20 && k < 0x7f && *len < max) {
        buf[(*len)++] = (char)k; buf[*len] = 0; dirty = 1;
    } else if (k == 8 || k == 127) {
        if (*len > 0) buf[--(*len)] = 0;
        dirty = 1;
    } else if (k == '\r' || k == '\n') {
        if (on_enter) on_enter();
        return 1;
    }
    return 0;
}

/* ---- 多行编辑器按键（备忘录/脚本编辑器） ---- */
static void ui_editor_key(int k)
{
    if (k >= 0x20 && k < 0x7f) {
        char *row = ui_ed[ui_er];
        int l = (int)strlen(row);
        if (l < UI_ECOLS) {
            memmove(row + ui_ec + 1, row + ui_ec, l - ui_ec + 1);
            row[ui_ec++] = (char)k;
            dirty = 1;
        }
    } else if (k == 8 || k == 127) {
        char *row = ui_ed[ui_er];
        if (ui_ec > 0) {
            memmove(row + ui_ec - 1, row + ui_ec, strlen(row + ui_ec) + 1);
            ui_ec--; dirty = 1;
        }
    } else if (k == '\r' || k == '\n') {
        if (ui_en < UI_EROWS) {
            int r;
            for (r = ui_en; r > ui_er + 1; r--)
                memcpy(ui_ed[r], ui_ed[r - 1], UI_ECOLS + 1);
            snprintf(ui_ed[ui_er + 1], UI_ECOLS + 1, "%s", ui_ed[ui_er] + ui_ec);
            ui_ed[ui_er][ui_ec] = 0;
            ui_en++; ui_er++; ui_ec = 0; dirty = 1;
        }
    } else if (k == 1 || k == 0x10b) {
        if (ui_er > 0) { ui_er--; ui_ec = (int)strlen(ui_ed[ui_er]); dirty = 1; }
    } else if (k == 2 || k == 0x10c) {
        if (ui_er < ui_en - 1) { ui_er++; ui_ec = (int)strlen(ui_ed[ui_er]); dirty = 1; }
    }
}

/* ---- 迷你应用按键总入口 ---- */
static void app_key_custom(int k)
{
    int app = open_app;

    /* 次视图（文件预览/脚本输出）：Esc 或 q 返回主视图 */
    if (ui_mode == 1) {
        if (k == 27 || k == 'q') {
            ui_mode = 0;
            if (app == APP_SCRIPT) { dirty = 1; return; }
            /* 访达返回列表时重新初始化行缓冲视图关闭 */
            scroll = 0; dirty = 1;
        } else if (k == 1) { scroll--; dirty = 1; }
        else if (k == 2) { scroll++; dirty = 1; }
        return;
    }

    switch (app) {
    case APP_FINDER:
        if (k == 1 && ui_sel > 0) { ui_sel--; dirty = 1; }
        else if (k == 2 && ui_sel < ui_ls_n - 1) { ui_sel++; dirty = 1; }
        else if (k == '\r' || k == '\n') finder_enter();
        else if (k == 'u' || k == 'U' || k == 8 || k == 127) finder_up();
        else if (k == 27 || k == 'q') app_close();
        break;
    case APP_SETTINGS:
        /* 面板列表 */
        if (k == 1 && ui_sel > 0) { ui_sel--; dirty = 1; }
        else if (k == 2 && ui_sel < 3) { ui_sel++; dirty = 1; }
        else if (k == '\r' || k == '\n') {
            ui_mode = 1; scroll = 0;
            if (ui_sel == 2)
                capture("ifconfig -a; echo; netstat -rn; "
                        "echo; cat /etc/resolv.conf");
            else if (ui_sel == 3) { free_lines(); app_sys(); }
            dirty = 1;
        }
        else if (k == 'l') { lang = 0; lang_save(); dirty = 1; }
        else if (k == 'e') { lang = 1; lang_save(); dirty = 1; }
        else if (k == 'k') kbd_toggle_layout();
        else if (k == 27 || k == 'q') app_close();
        break;
    case APP_CALC:
        if (k == 27 || k == 'q') app_close();
        else ui_input_key(ui_in, &ui_il, sizeof ui_in - 1, k, app_calc_run);
        break;
    case APP_NOTES:
        if (k == 27 || k == 'q') { notes_save(); app_close(); }
        else ui_editor_key(k);
        break;
    case APP_SCRIPT:
        if (k == 18) { script_run(); }              /* Ctrl+R 运行 */
        else if (k == 27 || k == 'q') {
            FILE *f = fopen("/tmp/e1script.sh", "w");
            int i;
            if (f) { for (i = 0; i < ui_en; i++) fprintf(f, "%s\n", ui_ed[i]); fclose(f); }
            app_close();
        } else ui_editor_key(k);
        break;
    case APP_REMIND:
        if (k == ' ') {
            if (rmd_n > 0) { rmd_done[ui_sel] = !rmd_done[ui_sel]; rmd_save(); dirty = 1; }
        } else if (k == 1 && ui_sel > 0) { ui_sel--; dirty = 1; }
        else if (k == 2 && ui_sel < rmd_n - 1) { ui_sel++; dirty = 1; }
        else if (k == 'd' || k == 'D') {
            if (rmd_n > 0) {
                int i;
                for (i = ui_sel; i < rmd_n - 1; i++) {
                    memcpy(rmd_txt[i], rmd_txt[i + 1], RMD_LEN + 1);
                    rmd_done[i] = rmd_done[i + 1];
                }
                rmd_n--; if (ui_sel >= rmd_n && rmd_n > 0) ui_sel = rmd_n - 1;
                rmd_save(); dirty = 1;
            }
        } else if (k == 27 || k == 'q') { rmd_save(); app_close(); }
        else ui_input_key(ui_in, &ui_il, sizeof ui_in - 1, k, rmd_add);
        break;
    case APP_CALENDAR:
        if (k == 1 || k == 'h') { ui_cal_m--; dirty = 1; }
        else if (k == 2 || k == 'l') { ui_cal_m++; dirty = 1; }
        else if (k == 't' || k == 'T') { ui_cal_m = 0; dirty = 1; }
        else if (k == 27 || k == 'q') app_close();
        break;
    case APP_SHOT:
        if (k == '\r' || k == '\n' || k == ' ') shot_save();
        else if (k == 27 || k == 'q') app_close();
        break;
    case APP_DICT:
        if (k == 27 || k == 'q') app_close();
        else ui_input_key(ui_in, &ui_il, sizeof ui_in - 1, k, app_dict_lookup);
        break;
    case APP_FINDMY:
        if (k == 27 || k == 'q') app_close();
        else ui_input_key(ui_in, &ui_il, sizeof ui_in - 1, k, app_findmy_run);
        break;
    case APP_CLOCK:
        if (k == 27 || k == 'q') app_close();
        break;
    default:
        /* 信息型应用：1/2 滚动，r 刷新，Esc 关闭 */
        if (k == 1) { scroll--; dirty = 1; }
        else if (k == 2) { scroll++; dirty = 1; }
        else if (k == 'r' || k == 'R') { run_app(app); scroll = 0; }
        else if (k == 27 || k == 'q') app_close();
        break;
    }
}

/* 提醒事项的回车添加（ui_input_key 回调形式） */
static void rmd_add(void)
{
    if (ui_il > 0 && rmd_n < RMD_MAX) {
        snprintf(rmd_txt[rmd_n], RMD_LEN + 1, "%s", ui_in);
        rmd_done[rmd_n] = 0;
        rmd_n++; ui_sel = rmd_n - 1;
        rmd_save();
    }
    ui_il = 0; ui_in[0] = 0;
    dirty = 1;
}

/* ---- 自绘：输入框 ---- */
static void draw_input_box(int x, int y, int w, const char *prompt)
{
    char buf[300];
    fill(x, y, w, 16 * SCALE_TXT + 6, C_TITLE);
    snprintf(buf, sizeof buf, "%s%s%s_", prompt ? prompt : "", ui_in, "");
    text(x + 6, y + 3, buf, C_ACCENT, SCALE_TXT);
}

/* ---- 自绘：通用行区域 ---- */
static void draw_lines_region(int wx, int y, int bottom)
{
    int line_h = 16 * SCALE_TXT;
    int i;
    for (i = scroll; i < nlines; i++, y += line_h) {
        if (y + line_h - 2 > bottom) break;
        text(wx + 10, y, lines[i], C_FG, SCALE_TXT);
    }
}

/* ---- 迷你应用绘制总入口：返回 1 表示已自绘 ---- */
static int app_draw_custom(int wx, int wy, int ww, int bottom)
{
    int app = open_app;
    int line_h = 16 * SCALE_TXT;
    int y = wy + 26 + 6;

    switch (app) {
    case APP_FINDER: {
        int i;
        char head[300];
        if (ui_mode == 1) { draw_lines_region(wx, y, bottom); return 1; }
        snprintf(head, sizeof head, "%s  [%s]", lang ? "Finder" : "访达", ui_path);
        text(wx + 10, y, head, C_ACCENT, SCALE_TXT);
        y += line_h + 4;
        fill(wx + 8, y, ww - 16, 1, C_DIM);
        y += 6;
        for (i = 0; i < ui_ls_n; i++, y += line_h) {
            if (y + line_h - 2 > bottom - 14) break;
            if (i == ui_sel) fill(wx + 6, y - 1, ww - 12, line_h, C_SEL);
            text(wx + 14, y, ui_ls_dir[i] ? "[]" : "  ", C_DIM, SCALE_TXT);
            text(wx + 34, y, ui_ls_name[i],
                 ui_ls_dir[i] ? C_ACCENT : C_FG, SCALE_TXT);
            if (ui_ls_dir[i]) text(wx + 34, y, ui_ls_name[i], C_ACCENT, SCALE_TXT);
        }
        text(wx + 8, bottom - 14,
             lang ? "Enter open  u/Backspace=up  Esc close"
                  : "回车打开  u/退格=上级  Esc 关闭", C_DIM, 1);
        return 1;
    }
    case APP_SETTINGS: {
        static const char *names_zh[4] =
            {"语言与输入法", "键盘布局", "网络信息", "关于本机"};
        static const char *names_en[4] =
            {"Language & Input", "Keyboard Layout", "Network", "About This PC"};
        int i;
        if (ui_mode == 1) {
            /* 面板内容 */
            if (ui_sel == 0) {
                text(wx + 10, y, lang ? "[l] zh   [e] en"
                                      : "[l] 中文  [e] English",
                     C_FG, SCALE_TXT);
                y += line_h + 4;
                text(wx + 10, y,
                     lang ? "Current: English" : "当前：中文", C_ACCENT, SCALE_TXT);
                y += line_h + 4;
                text(wx + 10, y,
                     lang ? "Alt+L also switches language"
                          : "Alt+L 也可随时切换语言", C_DIM, SCALE_TXT);
            } else if (ui_sel == 1) {
                text(wx + 10, y, lang ? "[k] toggle QWERTY/Dvorak"
                                      : "[k] 切换 QWERTY/德沃夏克",
                     C_FG, SCALE_TXT);
                y += line_h + 4;
                text(wx + 10, y + line_h,
                     kbd_layout ? "Current: Dvorak" : "Current: QWERTY",
                     C_ACCENT, SCALE_TXT);
            } else if (ui_sel == 2) {
                draw_lines_region(wx, y, bottom);
            } else {
                draw_lines_region(wx, y, bottom);
            }
            return 1;
        }
        text(wx + 10, y, lang ? "System Settings" : "系统设置", C_ACCENT, SCALE_TXT);
        y += line_h + 8;
        for (i = 0; i < 4; i++, y += line_h + 6) {
            if (i == ui_sel) fill(wx + 8, y - 2, ww - 16, line_h + 2, C_SEL);
            text(wx + 18, y, lang ? names_en[i] : names_zh[i], C_FG, SCALE_TXT);
        }
        text(wx + 10, bottom - 12,
             lang ? "Enter open  Esc back/close  l/e lang  k layout"
                  : "回车打开  Esc 返回/关闭  l/e 语言  k 布局", C_DIM, 1);
        return 1;
    }
    case APP_CALC: {
        if (ui_mode == 1) { draw_lines_region(wx, y, bottom); return 1; }
        text(wx + 10, y, lang ? "Calculator" : "计算器", C_DIM, SCALE_TXT);
        draw_input_box(wx + 10, y + line_h + 2, ww - 20, "=");
        /* 历史记录：lines[1..] 之后的算式 */
        y += line_h * 2 + 14;
        {
            int start = nlines - (bottom - y) / line_h;
            int i;
            if (start < 0) start = 0;
            for (i = start; i < nlines; i++, y += line_h)
                text(wx + 14, y, lines[i], C_DIM, SCALE_TXT);
        }
        return 1;
    }
    case APP_NOTES:
    case APP_SCRIPT: {
        int i;
        const char *hint = app == APP_SCRIPT
            ? (lang ? "Ctrl+R run   Esc save&quit" : "Ctrl+R 运行   Esc 保存退出")
            : (lang ? "Esc save & quit   arrows move" : "Esc 保存退出   方向键移动");
        fill(wx + 8, y, ww - 16, (line_h + 2) * (ui_en + 1), C_TITLE);
        for (i = 0; i < ui_en; i++) {
            char row[UI_ECOLS + 4];
            int ry = y + 2 + i * (line_h + 2);
            if (i == ui_er) fill(wx + 8, ry - 1, ww - 16, line_h, C_SEL);
            snprintf(row, sizeof row, "%2d %s%s", i + 1, ui_ed[i],
                     i == ui_er ? "_" : "");
            text(wx + 14, ry, row, C_FG, SCALE_TXT);
        }
        text(wx + 10, bottom - 12, hint, C_DIM, 1);
        return 1;
    }
    case APP_REMIND: {
        int i;
        draw_input_box(wx + 10, y, ww - 20, lang ? "+ " : "+ ");
        y += line_h + 10;
        for (i = 0; i < rmd_n; i++, y += line_h) {
            char row[RMD_LEN + 8];
            if (y + line_h - 2 > bottom - 14) break;
            if (i == ui_sel) fill(wx + 8, y - 1, ww - 16, line_h, C_SEL);
            snprintf(row, sizeof row, "[%c] %s", rmd_done[i] ? 'x' : ' ',
                     rmd_txt[i]);
            text(wx + 14, y, row, rmd_done[i] ? C_DIM : C_FG, SCALE_TXT);
        }
        text(wx + 10, bottom - 12, ui_msg[0] ? ui_msg :
             (lang ? "space toggle  d delete  Esc close"
                   : "空格勾选  d 删除  Esc 关闭"), C_DIM, 1);
        return 1;
    }
    case APP_CALENDAR: {
        time_t t = time(NULL) + (time_t)ui_cal_m * 31 * 86400;
        struct tm tm0, *tm_now = localtime(&t);
        int year, mon, lead, days, i, d, col0;
        static const char *mzh[12] =
            {"一月","二月","三月","四月","五月","六月","七月","八月",
             "九月","十月","十一月","十二月"};
        static const char *men[12] =
            {"January","February","March","April","May","June","July",
             "August","September","October","November","December"};
        static const char *wzh[7] = {"日","一","二","三","四","五","六"};
        static const char *wen[7] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
        char head[64];
        year = tm_now->tm_year + 1900; mon = tm_now->tm_mon;
        tm0 = *tm_now; tm0.tm_mday = 1; tm0.tm_hour = 12;
        mktime(&tm0);
        lead = tm0.tm_wday;
        tm0.tm_mday = 32; mktime(&tm0);
        days = 32 - tm0.tm_mday;
        snprintf(head, sizeof head, "%d  %s", year, lang ? men[mon] : mzh[mon]);
        text(wx + ww / 2 - text_w(head, 2) / 2, y, head, C_ACCENT, 2);
        y += line_h * 2 + 6;
        for (i = 0; i < 7; i++)
            text(wx + 14 + i * 60, y, lang ? wen[i] : wzh[i], C_DIM, SCALE_TXT);
        y += line_h + 4;
        col0 = wx + 14;
        for (d = 1; d <= days; d++) {
            char cell[8];
            int cx = col0 + ((lead + d - 1) % 7) * 60;
            int cy = y + ((lead + d - 1) / 7) * (line_h + 4);
            snprintf(cell, sizeof cell, "%2d", d);
            text(cx, cy, cell, C_FG, SCALE_TXT);
        }
        text(wx + 10, bottom - 12,
             lang ? "up/down prev/next month  t today  Esc close"
                  : "上/下月：上/下方向键  t 回到今天  Esc 关闭", C_DIM, 1);
        return 1;
    }
    case APP_CLOCK: {
        time_t t = time(NULL);
        struct tm *tm = localtime(&t);
        char big[16], date[64];
        int yy = wy + 60;
        snprintf(big, sizeof big, "%02d:%02d:%02d", tm->tm_hour,
                 tm->tm_min, tm->tm_sec);
        text(wx + ww / 2 - text_w(big, 4) / 2, yy, big, C_ACCENT, 4);
        snprintf(date, sizeof date, "%d-%02d-%02d",
                 tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday);
        text(wx + ww / 2 - text_w(date, 2) / 2, yy + 80, date, C_FG, 2);
        {
            struct timespec ts;
            if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
                double up = ts.tv_sec + ts.tv_nsec / 1e9;
                char s[64];
                snprintf(s, sizeof s, lang ? "uptime %dh %dm" : "已运行 %d 小时 %d 分",
                         (int)(up / 3600), (int)(up / 60) % 60);
                text(wx + ww / 2 - text_w(s, 2) / 2, yy + 120, s, C_DIM, 2);
            }
        }
        text(wx + 10, bottom - 12, lang ? "Esc close" : "Esc 关闭", C_DIM, 1);
        return 1;
    }
    case APP_SHOT: {
        text(wx + 10, y, lang ? "Screenshot" : "截屏", C_ACCENT, SCALE_TXT);
        y += line_h + 8;
        text(wx + 10, y,
             lang ? "Enter/Space: save /tmp/e1shot-NN.ppm (P6 raw framebuffer)"
                  : "回车/空格：保存 /tmp/e1shot-NN.ppm（P6 原始帧缓冲）",
             C_FG, SCALE_TXT);
        y += line_h + 4;
        text(wx + 10, y,
             lang ? "convert on a PC: ffmpeg -i shot.ppm shot.png"
                  : "在电脑上转换：ffmpeg -i shot.ppm shot.png",
             C_DIM, SCALE_TXT);
        y += line_h + 12;
        /* 缩略：每 8x8 像素取一点 */
        {
            int tw = ww - 32, th = tw * H / W, x2, y2;
            if (th > bottom - y - 20) { th = bottom - y - 20; tw = th * W / H; }
            fill_round(wx + 16, y, tw, th, 4, C_TITLE);
            for (y2 = 0; y2 < th; y2++)
                for (x2 = 0; x2 < tw; x2++) {
                    int sx = x2 * W / tw, sy = y2 * H / th;
                    px(wx + 16 + x2, y + y2, fbp[sy * W + sx]);
                }
            y += th + 8;
        }
        if (ui_msg[0]) text(wx + 10, y, ui_msg, C_ACCENT, SCALE_TXT);
        return 1;
    }
    case APP_DICT:
    case APP_FINDMY: {
        draw_lines_region(wx, y, y + line_h * 2);
        draw_input_box(wx + 10, y + line_h * 2 + 2, ww - 20, "> ");
        draw_lines_region(wx, y + line_h * 3 + 12, bottom);
        return 1;
    }
    default:
        return 0;
    }
}

/* ================= 终端模拟器 ================= */
/* openpty + fork/exec /bin/sh，master 端解析常用 ANSI 序列（光标定位/
 * 清屏/清行，SGR 颜色忽略），按行缓冲渲染到窗口 */

#define TERM_ROWS 24
#define TERM_BSZ  192           /* 每行字节缓冲 */
static int   term_fd = -1;
static pid_t term_pid = -1;
static char  term_scr[TERM_ROWS][TERM_BSZ];
static int   term_px = 0;       /* 光标 x（内容区像素偏移） */
static int   term_cy = 0;

static int term_wpx(void) { return W - 180; }   /* 内容区可用宽度 */

static void term_clear(void)
{
    int i;
    for (i = 0; i < TERM_ROWS; i++) term_scr[i][0] = 0;
    term_px = 0; term_cy = 0;
}

static void term_scroll_up(void)
{
    int i;
    for (i = 1; i < TERM_ROWS; i++)
        memcpy(term_scr[i - 1], term_scr[i], TERM_BSZ);
    term_scr[TERM_ROWS - 1][0] = 0;
}

static void term_newline(void)
{
    term_px = 0;
    term_cy++;
    if (term_cy >= TERM_ROWS) { term_cy = TERM_ROWS - 1; term_scroll_up(); }
}

/* 行像素宽（按 UTF-8 字符宽累计：ASCII 8，其余 16） */
static int term_row_px(int row)
{
    const char *s = term_scr[row];
    unsigned cp; int px = 0, n;
    while (*s) { n = utf8_next(s, &cp); px += cp < 0x80 ? 8 : 16; s += n; }
    return px;
}

static void term_putc(const char *s, int len)
{
    unsigned cp;
    int w;
    utf8_next(s, &cp);
    w = cp < 0x80 ? 8 : 16;
    if (term_px + w > term_wpx()) term_newline();
    {
        char *row = term_scr[term_cy];
        int l = strlen(row);
        if (l + len < TERM_BSZ - 1) {
            memcpy(row + l, s, len);
            row[l + len] = 0;
        }
    }
    term_px += w;
}

static void term_feed(const char *s, int n)
{
    int i = 0;
    while (i < n) {
        char c = s[i];
        if (c == '\033' && i + 1 < n && s[i + 1] == '[') {
            int j = i + 2, p[8], np = 0, cur = 0, has = 0;
            while (j < n && ((s[j] >= '0' && s[j] <= '9') ||
                             s[j] == ';' || s[j] == '?')) {
                if (s[j] >= '0' && s[j] <= '9') { cur = cur * 10 + s[j] - '0'; has = 1; }
                else if (s[j] == ';' && np < 8) { p[np++] = has ? cur : 0; cur = 0; has = 0; }
                j++;
            }
            if (np < 8) p[np++] = has ? cur : 0;
            if (j >= n) break;                  /* 序列不完整，等下次 */
            i = j + 1;
            {
                int p0 = p[0] ? p[0] : 1;
                switch (s[j]) {
                case 'H': case 'f':
                    term_cy = p0 - 1; if (term_cy >= TERM_ROWS) term_cy = TERM_ROWS - 1;
                    term_px = (np > 1 && p[1] ? p[1] - 1 : 0) * 8;
                    break;
                case 'J': if (p[0] == 2 || p[0] == 0) term_clear(); break;
                case 'K':
                    if (p[0] != 1) { term_scr[term_cy][0] = 0; term_px = term_row_px(term_cy); }
                    break;
                case 'A': term_cy -= p0; if (term_cy < 0) term_cy = 0; break;
                case 'B': term_cy += p0; if (term_cy >= TERM_ROWS) term_cy = TERM_ROWS - 1; break;
                default: break;             /* SGR(m) 等颜色忽略 */
                }
            }
            continue;
        }
        i++;
        if (c == '\n') term_newline();
        else if (c == '\r') term_px = 0;
        else if (c == 8 || c == 127) {      /* backspace：去行尾一字符 */
            char *row = term_scr[term_cy];
            int l = strlen(row);
            while (l > 0 && (row[l - 1] & 0xC0) == 0x80) l--;
            if (l > 0) l--;
            row[l] = 0;
            term_px = term_row_px(term_cy);
        }
        else if (c == '\t') { term_putc(" ", 1); while (term_px % 32) term_putc(" ", 1); }
        else if ((unsigned char)c >= 0x20) {
            int len = 1;
            if ((unsigned char)c >= 0x80) { /* UTF-8 多字节收集 */
                while (i + len < n && (s[i + len] & 0xC0) == 0x80) len++;
            }
            term_putc(s + i - 1, len);
            i += len - 1;
        }
    }
    dirty = 1;
}

static void app_term_start(void)
{
    int slave = -1;
    struct winsize ws;
    memset(&ws, 0, sizeof ws);
    ws.ws_row = TERM_ROWS; ws.ws_col = term_wpx() / 8;

    if (openpty(&term_fd, &slave, NULL, NULL, &ws) != 0) {
        term_fd = -1;
        add_line("(openpty failed)");
        return;
    }
    term_pid = fork();
    if (term_pid == 0) {
        close(term_fd);
        setsid();
        ioctl(slave, TIOCSCTTY, 0);
        dup2(slave, 0); dup2(slave, 1); dup2(slave, 2);
        if (slave > 2) close(slave);
        setenv("TERM", "xterm", 1);
        execl("/bin/sh", "sh", (char *)0);
        _exit(1);
    }
    close(slave);
    if (term_pid < 0) { close(term_fd); term_fd = -1; add_line("(fork failed)"); return; }
    fcntl(term_fd, F_SETFL, O_NONBLOCK);
    term_clear();
    term_feed("busybox sh - e1LibreOS terminal\r\n", 34);
}

static void term_key(int k)
{
    char seq[8];
    int n = 0;
    if (k == 1)      { memcpy(seq, "\033[A", 3); n = 3; }   /* ↑ */
    else if (k == 2) { memcpy(seq, "\033[B", 3); n = 3; }   /* ↓ */
    else if (k == 13 || k == 10) { seq[0] = '\r'; n = 1; }
    else if (key_raw) { seq[0] = key_raw; n = 1; }
    if (n && term_fd >= 0) write(term_fd, seq, n);
}

/* ================= e1 浏览器 ================= */
/* 自研极简 HTTP/HTML：GET + 标签剥离 + 标题/链接收集 + 自动换行，
 * 链接以 [N] 前缀列出，按数字键直接跳转 */

#define BR_MAX    16384
#define BR_LINKS  10
static char br_input[160];
static int  br_input_len;
static char br_links[BR_LINKS][192];
static int  br_nlinks;
static char br_cur_url[192];        /* 当前页（0 键回首页用） */

static void br_add_wrapped(const char *s)
{
    char seg[512];
    int i = 0, n = strlen(s);
    while (i < n) {
        int best = 0, j = 0;
        while (i + j < n && j < (int)sizeof seg - 1) {
            unsigned cp; int cl;
            seg[j] = s[i + j];
            cl = utf8_next(s + i + j, &cp);
            memcpy(seg + j, s + i + j, cl);
            seg[j + cl] = 0;
            if (*seg == ' ' || cp == ' ') best = j + 1;
            j += cl;
            if (text_w(seg, 1) > term_wpx() - 16) break;
        }
        seg[j] = 0;
        if (i + j >= n) best = 0;
        if (best && i + j < n) j = best;
        add_line(seg);
        i += j;
        while (i < n && s[i] == ' ') i++;
    }
}

/* 极简实体解码（就地） */
static void br_entities(char *s)
{
    char *w = s;
    while (*s) {
        if (*s == '&') {
            if (!strncmp(s, "&amp;", 5))  { *w++ = '&'; s += 5; continue; }
            if (!strncmp(s, "&lt;", 4))   { *w++ = '<'; s += 4; continue; }
            if (!strncmp(s, "&gt;", 4))   { *w++ = '>'; s += 4; continue; }
            if (!strncmp(s, "&quot;", 6)) { *w++ = '"'; s += 6; continue; }
            if (!strncmp(s, "&#39;", 5))  { *w++ = '\''; s += 5; continue; }
            if (!strncmp(s, "&nbsp;", 6)) { *w++ = ' '; s += 6; continue; }
        }
        *w++ = *s++;
    }
    *w = 0;
}

static void br_parse(const char *html)
{
    char txt[1024];
    int ti = 0, i = 0;
    int in_script = 0, link_no = 0;
    br_nlinks = 0;

    add_line("");
    while (html[i]) {
        if (html[i] == '<') {
            /* 输出积累文本 */
            if (ti) {
                txt[ti] = 0;
                br_entities(txt);
                br_add_wrapped(txt);
                ti = 0;
            }
            if (!strncmp(html + i, "<!--", 4)) {
                const char *e = strstr(html + i, "-->");
                if (!e) break;
                i = e - html + 3;
                continue;
            }
            if (!strncasecmp(html + i, "<script", 7) ||
                !strncasecmp(html + i, "<style", 6))
                in_script = 1;
            else if (in_script && (html[i] == '/' ||
                     !strncasecmp(html + i, "</s", 3)))
                in_script = 0;
            if (!in_script && html[i] == '<' &&
                !strncasecmp(html + i, "<a ", 3)) {
                /* 取 href */
                const char *h = strstr(html + i, "href=");
                if (h && link_no < BR_LINKS) {
                    char href[192], disp[128];
                    int di = 0, hi = 0;
                    const char *v = h + 5;
                    char q = *v;
                    if (q == '"' || q == '\'') {
                        v++;
                        while (*v && *v != q && hi < (int)sizeof href - 1) href[hi++] = *v++;
                    } else {
                        while (*v && *v != ' ' && *v != '>' && hi < (int)sizeof href - 1) href[hi++] = *v++;
                    }
                    href[hi] = 0;
                    /* 链接文字到 </a> */
                    {
                        const char *e = strcasestr(html + i, "</a>");
                        const char *t = strchr(html + i, '>');
                        if (e && t && t < e) {
                            t++;
                            while (t < e && di < (int)sizeof disp - 1) {
                                if (*t == '<') { while (t < e && *t != '>') t++; if (t >= e) break; }
                                else disp[di++] = *t;
                                t++;
                            }
                        }
                    }
                    disp[di] = 0;
                    br_entities(disp);
                    if (href[0] && strncmp(href, "#", 1) &&
                        strncmp(href, "javascript", 10)) {
                        snprintf(br_links[link_no], 192, "%s", href);
                        link_no++;
                        {
                            char lnl[192];
                            snprintf(lnl, sizeof lnl, "[%d] %s", link_no, disp);
                            br_add_wrapped(lnl);
                        }
                    } else if (disp[0]) {
                        br_add_wrapped(disp);
                    }
                    /* 跳过 </a> */
                    {
                        const char *e = strcasestr(html + i, "</a>");
                        i = (e ? e + 4 : html + i + strlen(html + i)) - html;
                        continue;
                    }
                }
            }
            /* 换行类标签 */
            {
                char t0 = (html[i + 1] == '/') ? html[i + 2] : html[i + 1];
                char lbuf[8] = { t0, 0 };
                if (strchr("phbdlit", t0) && t0) {
                    (void)lbuf;
                    add_line("");
                }
            }
            {
                const char *e = strchr(html + i, '>');
                if (!e) break;
                i = e - html + 1;
            }
            continue;
        }
        if (in_script) { i++; continue; }
        {
            char hc = html[i];
            if (hc == '\n' || hc == '\r' || hc == '\t') hc = ' ';
            if (hc == ' ' && ti && txt[ti - 1] == ' ') { i++; continue; }
            if (ti < (int)sizeof txt - 2) txt[ti++] = hc;
        }
        i++;
    }
    if (ti) { txt[ti] = 0; br_entities(txt); br_add_wrapped(txt); }
    if (nlines == 0) add_line(L(S_BR_EMPTY));
}

static void app_br_fetch(const char *url_in)
{
    char url[256], host[128] = "", path[256] = "/";
    int port = 80, sock = -1;
    struct addrinfo hints, *res = NULL;
    char req[512], *body;
    int len = 0, cap = BR_MAX;
    struct timeval tv;
    const char *bp;

    free_lines();
    scroll = 0;
    snprintf(url, sizeof url, "%s", url_in);

    /* 解析 http://host[:port]/path（仅 http） */
    if (strncmp(url, "http://", 7) != 0) {
        char e[320];
        snprintf(e, sizeof e, "%s%s", L(S_BR_ERR), url);
        add_line(e);
        add_line(L(S_BR_HINT));
        dirty = 1;
        return;
    }
    {
        char *p = url + 7, *q;
        q = strchr(p, '/');
        if (q) { snprintf(path, sizeof path, "%s", q); *q = 0; }
        else path[0] = '/', path[1] = 0;
        q = strchr(p, ':');
        if (q) { port = atoi(q + 1); *q = 0; }
        snprintf(host, sizeof host, "%s", p);
    }
    if (!host[0]) { add_line(L(S_BR_ERR)); dirty = 1; return; }

    add_line(L(S_BR_LOADING));
    dirty = 1;
    redraw();                        /* 先把“加载中”画出来 */
    dirty = 0;

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, NULL, &hints, &res) != 0 || !res) {
        char e[288];
        snprintf(e, sizeof e, "%s%s (DNS)", L(S_BR_ERR), host);
        free_lines(); add_line(e); dirty = 1;
        return;
    }
    ((struct sockaddr_in *)res->ai_addr)->sin_port = htons(port);
    sock = socket(AF_INET, SOCK_STREAM, 0);
    fcntl(sock, F_SETFL, O_NONBLOCK);
    if (connect(sock, res->ai_addr, res->ai_addrlen) != 0 && errno != EINPROGRESS) {
        freeaddrinfo(res); close(sock);
        free_lines(); add_line(L(S_BR_ERR)); add_line(url); dirty = 1;
        return;
    }
    if (errno == EINPROGRESS) {
        fd_set wset;
        FD_ZERO(&wset); FD_SET(sock, &wset);
        tv.tv_sec = 6; tv.tv_usec = 0;
        if (select(sock + 1, NULL, &wset, NULL, &tv) <= 0) {
            freeaddrinfo(res); close(sock);
            free_lines();
            add_line(L(S_BR_ERR)); add_line(url); add_line("(timeout)");
            dirty = 1;
            return;
        }
    }
    tv.tv_sec = 8; tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    freeaddrinfo(res);

    snprintf(req, sizeof req,
             "GET %s HTTP/1.0\r\nHost: %s\r\n"
             "User-Agent: e1Browser/1.0 (e1LibreOS)\r\n"
             "Accept: text/html\r\nConnection: close\r\n\r\n",
             path, host);
    if (write(sock, req, strlen(req)) <= 0) { close(sock); add_line(L(S_BR_ERR)); dirty = 1; return; }

    body = malloc(cap + 1);
    while (len < cap) {
        int r = read(sock, body + len, cap - len);
        if (r <= 0) break;
        len += r;
    }
    body[len] = 0;
    close(sock);

    /* 跳过 HTTP 头 */
    bp = strstr(body, "\r\n\r\n");
    if (bp) bp += 4; else bp = body;

    free_lines();
    scroll = 0;
    snprintf(br_cur_url, sizeof br_cur_url, "%s", url);
    {
        char t[224];
        snprintf(t, sizeof t, "%s  (%d B)", url, len);
        add_line(t);
        add_line("");
    }
    br_parse(bp);
    free(body);
    dirty = 1;
}

static void app_br_start(void)
{
    if (!br_input[0])
        snprintf(br_input, sizeof br_input, "http://example.com");
    br_input_len = strlen(br_input);
    add_line(L(S_BR_HINT));
    add_line(L(S_BR_LINK_HINT));
    dirty = 1;
}

static void br_key(int k)
{
    if (k == 13 || k == 10) {
        if (br_input[0]) app_br_fetch(br_input);
        return;
    }
    if (k == 8 || k == 127) {
        if (br_input_len > 0) br_input[--br_input_len] = 0;
        dirty = 1;
        return;
    }
    if (k >= '0' && k <= '9' && br_nlinks > 0) {
        int n = k - '0';
        if (n == 0 && br_cur_url[0]) {           /* 0 = 当前页顶部 */
            snprintf(br_input, sizeof br_input, "%s", br_cur_url);
            br_input_len = strlen(br_input);
            app_br_fetch(br_input);
            return;
        }
        if (n >= 1 && n <= br_nlinks) {
            const char *href = br_links[n - 1];
            if (strncmp(href, "http://", 7) != 0) {
                /* 相对地址拼接到当前页 */
                char full[256], root[192] = "";
                const char *s = br_cur_url + 7;
                const char *sl = strchr(s, '/');
                if (sl) snprintf(root, sl - s + 1, "%s", s);
                else snprintf(root, sizeof root, "%s", s);
                snprintf(full, sizeof full, "http://%s%s",
                         root, href[0] == '/' ? href + 1 : href);
                snprintf(br_input, sizeof br_input, "%s", full);
            } else {
                snprintf(br_input, sizeof br_input, "%s", href);
            }
            br_input_len = strlen(br_input);
            app_br_fetch(br_input);
            return;
        }
    }
    if (k == 1) { scroll -= 3; dirty = 1; }
    else if (k == 2) { scroll += 3; dirty = 1; }
    else if (key_raw >= 0x20 && key_raw < 0x7f &&
             br_input_len < (int)sizeof br_input - 1) {
        br_input[br_input_len++] = key_raw;
        br_input[br_input_len] = 0;
        dirty = 1;
    }
}

static void run_app(int app)
{
    free_lines();
    scroll = 0;
    switch (app) {
    case APP_ABOUT: app_about(); break;
    case APP_SYS:   app_sys();   break;
    case APP_PKG:   app_pkg();   break;
    case APP_TERM:  app_term_start(); break;
    case APP_BROWSER: app_br_start(); break;
    case APP_INSTALL:
        /* workstation = 图形安装向导；mobile = 磁盘检测说明页 */
        if (!mode_mobile) setup_start();
        else app_install();
        break;
    default:
        /* 迷你应用集（访达/设置/计算器/时钟等 18 个） */
        app_open_custom(app);
        break;
    }
}

/* ================= 视觉组件（macOS / HarmonyOS 风格共用） ================= */

static int isqrt(int v)
{
    int r = v, s;
    if (v <= 0) return 0;
    do { s = r; r = (r + v / r) / 2; } while (r < s);
    return r;
}

/* 圆角矩形填充（逐行内缩；无 alpha 混合的近似圆角） */
static void fill_round(int x, int y, int w, int h, int r, u32 c)
{
    int i;
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    for (i = 0; i < h; i++) {
        int inset = 0;
        if (i < r) {
            int dy = r - i;
            inset = r - isqrt(r * r - dy * dy);
        } else if (i >= h - r) {
            int dy = r - (h - 1 - i);
            inset = r - isqrt(r * r - dy * dy);
        }
        fill(x + inset, y + i, w - inset * 2, 1, c);
    }
}

/* 实心圆点（红绿灯 / dock 运行指示） */
static void draw_dot(int cx, int cy, int r, u32 c)
{
    int dx, dy;
    for (dy = -r; dy <= r; dy++)
        for (dx = -r; dx <= r; dx++)
            if (dx * dx + dy * dy <= r * r) px(cx + dx, cy + dy, c);
}

/* 应用图标（圆角方块 + 首字母） */
/* 应用图标底色（与 APP_* 顺序对应，配色参考 macOS 启动台） */
static const u32 app_icon_col[APP_COUNT] = {
    /* Finder Settings About  SysInfo Activity Disk    Console */
    0x1e90d8, 0x8e8e93, 0x5a6472, 0x5856d6, 0x30d158, 0x64d2ff, 0x1c1c1e,
    /* Term   Browser  pkg */
    0x2d2d30, 0x0a84ff, 0xbf5af2,
    /* Calc   Calendar Clock  Notes  Remind Contacts */
    0xff9500, 0xff3b30, 0x5ac8fa, 0xffe29b, 0xfff3b0, 0xc7c7cc,
    /* Weather Stocks Dict   Find   Script AirPort */
    0x4f9cf9, 0x1c1c1e, 0xff3b30, 0x34c759, 0xbf5af2, 0x0a84ff,
    /* Shot   Install */
    0x8e8e93, 0x30b0c7
};

static void draw_app_icon(int x, int y, int size, int app, u32 label_col)
{
    char glyph[2];
    int scale = size >= 56 ? 3 : 2;
    fill_round(x, y, size, size, size / 5, app_icon_col[app]);
    glyph[0] = "F#iI~DC>WP+c@NvB*$L/{Aod"[app];
    glyph[1] = 0;
    text(x + (size - text_w(glyph, scale)) / 2,
         y + (size - 8 * scale) / 2, glyph, label_col, scale);
}

/* ================= 窗口几何与动画 ================= */

/* workstation 多行 Dock 几何（绘制与鼠标命中共用，避免 24 个图标溢出 640 宽） */
#define DOCK_ISZ 40
#define DOCK_GAP 8
#define DOCK_PAD 12
static void dock_geom(int *cols, int *rows, int *dx, int *dy,
                      int *barh, int *isz)
{
    int n = APP_COUNT, c, r;
    int is = DOCK_ISZ, gap = DOCK_GAP, pad = DOCK_PAD;
    c = (W - 20 - pad * 2 + gap) / (is + gap);
    if (c > n) c = n;
    if (c < 1) c = 1;
    r = (n + c - 1) / c;
    *cols = c; *rows = r; *isz = is;
    *barh = r * (is + 12) + 10;
    *dx = (W - (c * (is + gap) - gap + pad * 2)) / 2;
    *dy = H - *barh - 10;
}

/* 单个 Dock 图标的左上坐标 */
static void dock_slot(int i, int *ix, int *iy, int isz)
{
    int cols, rows, dx, dy, barh, is2;
    int col = i, row = 0;
    dock_geom(&cols, &rows, &dx, &dy, &barh, &is2);
    row = i / cols;
    col = i % cols;
    *ix = dx + DOCK_PAD + col * (isz + DOCK_GAP);
    *iy = dy + 6 + row * (isz + 12);
}

/* mobile 主屏网格几何（行数随应用数自适应，保证全部图标可见） */
static void grid_geom(int *cols, int *chh, int *isz, int *gy0)
{
    int rows;
    *cols = 4;
    rows = (APP_COUNT + *cols - 1) / *cols;
    *gy0 = 60;
    *chh = (H - *gy0 - 52) / rows;
    *isz = *chh - 28;
    if (*isz > 72) *isz = 72;
    if (*isz < 40) *isz = 40;
}

static int win_max = 0;         /* workstation 绿灯最大化状态 */

/* 当前窗口矩形（含动画帧的 y 偏移） */
static void win_rect(int *wx, int *wy, int *ww, int *wh)
{
    int y_off = 0;
    if (anim_kind == 1)      y_off = (ANIM_FRAMES - anim_frame) * 8;
    else if (anim_kind == 2 || anim_kind == 3) y_off = anim_frame * 10;

    if (mode_mobile) {
        *wx = 10; *wy = 34;
        *ww = W - 20; *wh = H - 34 - 18;        /* 底部留手势条区域 */
    } else if (win_max) {
        *wx = 0; *wy = 28; *ww = W; *wh = H - 28;
    } else {
        *wx = 80; *wy = 60;
        *ww = W - 160; *wh = H - 120;
    }
    *wy += y_off;
}

static void anim_start(int kind)
{
    anim_kind = kind;
    anim_frame = 0;
    gettimeofday(&anim_t0, NULL);
    dirty = 1;
}

/* 动画帧结束后的落定动作（主循环在播完帧后调用一次） */
static void anim_finish(void)
{
    int kind = anim_kind;
    anim_kind = 0;
    if (kind == 2) {
        /* 关闭：终端等进程一并结束 */
        if (mode_mobile && kbd_visible) kbd_hide();
        if (open_app == APP_TERM && term_fd >= 0) {
            close(term_fd);
            term_fd = -1;
            if (term_pid > 0) {
                kill(term_pid, SIGKILL);
                waitpid(term_pid, 0, 0);
                term_pid = -1;
            }
        }
        open_app = -1;
        free_lines();
        scroll = 0;
        win_max = 0;
        dirty = 1;
    } else if (kind == 3) {
        /* 最小化（移动版主页 / workstation 黄灯）：终端进程保持运行 */
        open_app = -1;
        scroll = 0;
        dirty = 1;
    }
}

/* 关闭当前应用：先播动画，落定动作在 anim_finish */
static void app_close_anim(void)
{
    if (anim_kind) return;
    anim_start(2);
}

/* 最小化（保留终端进程等运行状态） */
static void app_minimize(void)
{
    if (open_app < 0) return;
    if (anim_kind) return;
    anim_start(3);
}

/* ================= 移动版手势导航（HarmonyOS 风格） ================= */

#define PILL_W   120
#define PILL_H   5
#define EDGE_W   14              /* 左右返回手势触发带宽度 */

static int mt_mode = 0;          /* 多任务卡片视图 */
static int gest_kind = 0;        /* 0 无 1 手势条上滑 2 左缘 3 右缘 */
static int gest_dy = 0, gest_dx = 0;
static int pill_lift = 0;        /* 手势条拖拽反馈（像素） */
static int gest_ptr_down = 0;    /* 指针按下中（mouse_poll 维护） */

static void gest_reset(void)
{
    gest_kind = 0;
    gest_dy = 0;
    gest_dx = 0;
    pill_lift = 0;
}

/* 主页动作：最小化当前应用 */
static void gest_home(void)
{
    mt_mode = 0;
    if (open_app >= 0) app_minimize();
    dirty = 1;
}

static void gest_back(void)
{
    if (kbd_visible) { kbd_hide(); dirty = 1; return; }
    if (open_app >= 0) { app_close_anim(); return; }
    if (mt_mode) { mt_mode = 0; dirty = 1; }
}

static void gest_multitask(void)
{
    if (open_app >= 0) {
        /* 直接收起（无动画），进入多任务视图 */
        open_app = -1;
        scroll = 0;
    }
    mt_mode = 1;
    dirty = 1;
}

/* 手势评估：返回 1 = 已消费（不触发 click） */
static int gesture_finish(void)
{
    int consumed = 0;
    if (gest_kind == 1) {
        if (gest_dy < -150)      { gest_multitask(); consumed = 1; }
        else if (gest_dy < -40)  { gest_home(); consumed = 1; }
    } else if (gest_kind == 2 || gest_kind == 3) {
        if (gest_dx > 60 || gest_dx < -60) { gest_back(); consumed = 1; }
    }
    gest_reset();
    return consumed;
}

/* ================= 图形安装向导（workstation） ================= */
/* 四种界面语言：English（默认）/ 简体中文 / 繁體中文 / Français */

enum { ILG_EN = 0, ILG_ZH_CN, ILG_ZH_TW, ILG_FR, ILG_COUNT };
enum {
    I_LANGQ, I_PROFILE, I_PROF_WS, I_PROF_SV, I_DISKQ, I_DISK_NONE,
    I_WARN, I_CONFIRM_Q, I_INSTALL_NOW, I_RUNNING, I_DONE, I_FAIL,
    I_REBOOT, I_CANCEL, I_BACK, I_NEXT, I_COUNT4
};

static int iset_lang = ILG_EN;   /* 安装器界面语言（默认英文） */
static int setup_page = 0;       /* 0=语言 1=类型 2=磁盘 3=确认 4=安装中 5=完成 6=失败 */
static int setup_prof = 0;       /* 0=workstation 1=server */
static int setup_disk_sel = -1;
static int setup_n_disks = 0;
static char setup_disks[8][96];  /* "/dev/vda 8G" */
static int setup_pid = -1;
static int setup_fd = -1;
static int setup_exit = -1;
static char setup_dev[64];

#define IL(id) (I4[id][iset_lang])

/* UTF-8 中文由 mkfont.py 按源码字符集生成字库；繁体/法语字符需在表内 */
static const char *I4[I_COUNT4][ILG_COUNT] = {
    [I_LANGQ] = { "Choose your language",
                  "\xe9\x80\x89\xe6\x8b\xa9\xe8\xaf\xad\xe8\xa8\x80",
                  "\xe9\x81\xb8\xe6\x93\x87\xe8\xaa\x9e\xe8\xa8\x80",
                  "Choisissez la langue" },
    [I_PROFILE] = { "Installation type",
                    "\xe9\x80\x89\xe6\x8b\xa9\xe5\xae\x89\xe8\xa3\x85\xe7\xb1\xbb\xe5\x9e\x8b",
                    "\xe9\x81\xb8\xe6\x93\x87\xe5\xae\x89\xe8\xa3\x9d\xe9\xa1\x9e\xe5\x9e\x8b",
                    "Type d'installation" },
    [I_PROF_WS] = { "Workstation (graphical desktop)",
                    "\xe5\xb7\xa5\xe4\xbd\x9c\xe7\xab\x99\xef\xbc\x88\xe5\x9b\xbe\xe5\xbd\xa2\xe6\xa1\x8c\xe9\x9d\xa2\xef\xbc\x89",
                    "\xe5\xb7\xa5\xe4\xbd\x9c\xe7\xab\x99\xef\xbc\x88\xe5\x9c\x96\xe5\xbd\xa2\xe6\xa1\x8c\xe9\x9d\xa2\xef\xbc\x89",
                    "Station de travail (bureau graphique)" },
    [I_PROF_SV] = { "Server (command line)",
                    "\xe6\x9c\x8d\xe5\x8a\xa1\xe5\x99\xa8\xef\xbc\x88\xe5\x91\xbd\xe4\xbb\xa4\xe8\xa1\x8c\xef\xbc\x89",
                    "\xe4\xbc\xba\xe6\x9c\x8d\xe5\x99\xa8\xef\xbc\x88\xe5\x91\xbd\xe4\xbb\xa4\xe5\x88\x97\xef\xbc\x89",
                    "Serveur (ligne de commande)" },
    [I_DISKQ] = { "Select the target disk",
                  "\xe9\x80\x89\xe6\x8b\xa9\xe7\x9b\xae\xe6\xa0\x87\xe7\xa3\x81\xe7\x9b\x98",
                  "\xe9\x81\xb8\xe6\x93\x87\xe7\x9b\xae\xe6\xa8\x99\xe7\xa3\x81\xe7\xa2\x9f",
                  "S\xc3\xa9lectionnez le disque cible" },
    [I_DISK_NONE] = { "No disk detected",
                      "\xe6\x9c\xaa\xe6\xa3\x80\xe6\xb5\x8b\xe5\x88\xb0\xe7\xa3\x81\xe7\x9b\x98",
                      "\xe6\x9c\xaa\xe5\x81\xb5\xe6\xb8\xac\xe5\x88\xb0\xe7\xa3\x81\xe7\xa2\x9f",
                      "Aucun disque d\xc3\xa9tect\xc3\xa9" },
    [I_WARN] = { "WARNING: ALL data on the disk will be erased.",
                 "\xe8\xad\xa6\xe5\x91\x8a\xef\xbc\x9a\xe7\xa3\x81\xe7\x9b\x98\xe4\xb8\x8a\xe7\x9a\x84\xe6\x89\x80\xe6\x9c\x89\xe6\x95\xb0\xe6\x8d\xae\xe5\xb0\x86\xe8\xa2\xab\xe6\x93\xa6\xe9\x99\xa4\xe3\x80\x82",
                 "\xe8\xad\xa6\xe5\x91\x8a\xef\xbc\x9a\xe7\xa3\x81\xe7\xa2\x9f\xe4\xb8\x8a\xe7\x9a\x84\xe6\x89\x80\xe6\x9c\x89\xe8\xb3\x87\xe6\x96\x99\xe5\xb0\x87\xe8\xa2\xab\xe6\x93\xa6\xe9\x99\xa4\xe3\x80\x82",
                 "ATTENTION : toutes les donn\xc3\xa9" "es du disque seront effac\xc3\xa9" "es." },
    [I_CONFIRM_Q] = { "Start installation now?",
                      "\xe7\xab\x8b\xe5\x8d\xb3\xe5\xbc\x80\xe5\xa7\x8b\xe5\xae\x89\xe8\xa3\x85\xef\xbc\x9f",
                      "\xe7\xab\x8b\xe5\x8d\xb3\xe9\x96\x8b\xe5\xa7\x8b\xe5\xae\x89\xe8\xa3\x9d\xef\xbc\x9f",
                      "Commencer l'installation maintenant ?" },
    [I_INSTALL_NOW] = { "Install",
                        "\xe5\xbc\x80\xe5\xa7\x8b\xe5\xae\x89\xe8\xa3\x85",
                        "\xe9\x96\x8b\xe5\xa7\x8b\xe5\xae\x89\xe8\xa3\x9d",
                        "Installer" },
    [I_RUNNING] = { "Installing e1LibreOS... please wait",
                    "\xe6\xad\xa3\xe5\x9c\xa8\xe5\xae\x89\xe8\xa3\x85 e1LibreOS\xe2\x80\xa6 \xe8\xaf\xb7\xe7\xa8\x8d\xe5\x80\x99",
                    "\xe6\xad\xa3\xe5\x9c\xa8\xe5\xae\x89\xe8\xa3\x9d e1LibreOS\xe2\x80\xa6 \xe8\xab\x8b\xe7\xa8\x8d\xe5\x80\x99",
                    "Installation de e1LibreOS\xe2\x80\xa6 veuillez patienter" },
    [I_DONE] = { "Install complete! Reboot into the disk system.",
                 "\xe5\xae\x89\xe8\xa3\x85\xe5\xae\x8c\xe6\x88\x90\xef\xbc\x81\xe5\x8f\xaf\xe9\x87\x8d\xe5\x90\xaf\xe8\xbf\x9b\xe5\x85\xa5\xe7\xa3\x81\xe7\x9b\x98\xe7\xb3\xbb\xe7\xbb\x9f\xe3\x80\x82",
                 "\xe5\xae\x89\xe8\xa3\x9d\xe5\xae\x8c\xe6\x88\x90\xef\xbc\x81\xe5\x8f\xaf\xe9\x87\x8d\xe5\x95\x9f\xe9\x80\xb2\xe5\x85\xa5\xe7\xa3\x81\xe7\xa2\x9f\xe7\xb3\xbb\xe7\xb5\xb1\xe3\x80\x82",
                 "Installation termin\xc3\xa9" "e ! Red\xc3\xa9" "marrez sur le disque." },
    [I_FAIL] = { "Installation failed - see log above.",
                 "\xe5\xae\x89\xe8\xa3\x85\xe5\xa4\xb1\xe8\xb4\xa5\xe2\x80\x94\xe2\x80\x94\xe8\xaf\xa6\xe8\xa7\x81\xe4\xb8\x8a\xe6\x96\xb9\xe6\x97\xa5\xe5\xbf\x97\xe3\x80\x82",
                 "\xe5\xae\x89\xe8\xa3\x9d\xe5\xa4\xb1\xe6\x95\x97\xe2\x80\x94\xe2\x80\x94\xe8\xa9\xb3\xe8\xa6\x8b\xe4\xb8\x8a\xe6\x96\xb9\xe6\x97\xa5\xe8\xaa\x8c\xe3\x80\x82",
                 "\xc3\x89" "chec de l'installation - voir le journal ci-dessus." },
    [I_REBOOT] = { "Reboot now",
                   "\xe7\xab\x8b\xe5\x8d\xb3\xe9\x87\x8d\xe5\x90\xaf",
                   "\xe7\xab\x8b\xe5\x8d\xb3\xe9\x87\x8d\xe5\x95\x9f",
                   "Red\xc3\xa9marrer" },
    [I_CANCEL] = { "Cancel", "\xe5\x8f\x96\xe6\xb6\x88", "\xe5\x8f\x96\xe6\xb6\x88", "Annuler" },
    [I_BACK]   = { "Back", "\xe4\xb8\x8a\xe4\xb8\x80\xe6\xad\xa5", "\xe4\xb8\x8a\xe4\xb8\x80\xe6\xad\xa5", "Retour" },
    [I_NEXT]   = { "Continue", "\xe7\xbb\xa7\xe7\xbb\xad", "\xe7\xb9\xbc\xe7\xba\x8c", "Continuer" },
};

/* 语言自称（page0 选项固定显示，不随 iset_lang 变化） */
static const char *ILGN[ILG_COUNT] = {
    "English",
    "\xe7\xae\x80\xe4\xbd\x93\xe4\xb8\xad\xe6\x96\x87",      /* 简体中文 */
    "\xe7\xb9\x81\xe9\xab\x94\xe4\xb8\xad\xe6\x96\x87",      /* 繁體中文 */
    "Fran\xc3\xa7" "ais"                                      /* Français */
};

/* 安装向导页面绘制（workstation APP_INSTALL 专属；page4/5/6 日志走 lines） */
static void draw_setup(int wx, int wy, int ww, int wh, int bottom)
{
    int y = wy + 26 + 10;
    int lh = 16 * SCALE_TXT;
    int i;

    if (setup_page >= 4) {
        /* 安装中(4)/完成(5)/失败(6)：日志行渲染（setup_launch/poll 填充） */
        int vis = (bottom - y) / lh;
        int max_scroll = nlines > vis ? nlines - vis : 0;
        if (scroll > max_scroll) scroll = max_scroll;
        if (scroll < 0) scroll = 0;
        for (i = scroll; i < nlines; i++, y += lh) {
            if (y + lh - 2 > bottom) break;
            text(wx + 10, y, lines[i],
                 setup_page == 5 ? C_ACCENT : C_FG, SCALE_TXT);
        }
        if (setup_page == 5) {                 /* Reboot now 主按钮 */
            int bt = wy + wh - 44;
            const char *lb = IL(I_REBOOT);
            int bw = text_w(lb, SCALE_TXT) + 40;
            fill_round(wx + ww - 150, bt, bw, 30, 6, C_ACCENT);
            text(wx + ww - 150 + 20, bt + 8, lb, 0xffffff, SCALE_TXT);
        }
        return;
    }

    /* ---- 交互页面：0 语言 1 类型 2 磁盘 3 确认 ---- */
    switch (setup_page) {
    case 0:
        text(wx + 10, y, IL(I_LANGQ), C_FG, SCALE_TXT); y += lh + 10;
        for (i = 0; i < ILG_COUNT; i++) {
            if (i == iset_lang)
                fill_round(wx + 6, y - 3, ww - 12, lh + 6, 6, C_SEL);
            text(wx + 20, y, ILGN[i], i == iset_lang ? C_FG : C_DIM, SCALE_TXT);
            y += lh + 4;
        }
        break;
    case 1:
        text(wx + 10, y, IL(I_PROFILE), C_FG, SCALE_TXT); y += lh + 10;
        for (i = 0; i < 2; i++) {
            const char *s = i == 0 ? IL(I_PROF_WS) : IL(I_PROF_SV);
            if (i == setup_prof)
                fill_round(wx + 6, y - 3, ww - 12, lh + 6, 6, C_SEL);
            text(wx + 20, y, s, i == setup_prof ? C_FG : C_DIM, SCALE_TXT);
            y += lh + 4;
        }
        break;
    case 2:
        text(wx + 10, y, IL(I_DISKQ), C_FG, SCALE_TXT); y += lh + 10;
        if (setup_n_disks == 0) {
            text(wx + 20, y, IL(I_DISK_NONE), C_DIM, SCALE_TXT);
            break;
        }
        for (i = 0; i < setup_n_disks; i++) {
            if (i == setup_disk_sel)
                fill_round(wx + 6, y - 3, ww - 12, lh + 6, 6, C_SEL);
            text(wx + 20, y, setup_disks[i],
                 i == setup_disk_sel ? C_FG : C_DIM, SCALE_TXT);
            y += lh + 4;
        }
        break;
    case 3:
        text(wx + 10, y, IL(I_WARN), 0xd7a13d, SCALE_TXT); y += lh + 10;
        text(wx + 10, y, IL(I_CONFIRM_Q), C_FG, SCALE_TXT); y += lh + 10;
        {
            char sum[200];
            snprintf(sum, sizeof sum, "  %s: %s", IL(I_PROFILE),
                     setup_prof ? IL(I_PROF_SV) : IL(I_PROF_WS));
            text(wx + 10, y, sum, C_DIM, SCALE_TXT); y += lh + 4;
            snprintf(sum, sizeof sum, "  %s: %s", IL(I_DISKQ),
                     setup_disk_sel >= 0 ? setup_disks[setup_disk_sel] : "-");
            text(wx + 10, y, sum, C_DIM, SCALE_TXT);
        }
        break;
    }

    /* 底部按钮（几何与 setup_hit_button / mouse_click 严格一致） */
    if (setup_page <= 3) {
        int bt = wy + wh - 44;
        const char *p = setup_page == 3 ? IL(I_INSTALL_NOW) : IL(I_NEXT);
        int pw2 = text_w(p, SCALE_TXT) + 40;
        fill_round(wx + ww - 150, bt, pw2, 30, 6, C_ACCENT);
        text(wx + ww - 150 + 20, bt + 8, p, 0xffffff, SCALE_TXT);
        if (setup_page > 0) {
            const char *b = IL(I_BACK);
            int bw2 = text_w(b, SCALE_TXT) + 40;
            fill_round(wx + ww - 260, bt, bw2, 30, 6, C_TITLE);
            text(wx + ww - 260 + 20, bt + 8, b, C_FG, SCALE_TXT);
        }
    }
}

/* 繁體字形（安裝/選/擇/盤/腦 等）必须出现在字库中 —— mkfont.py 扫描本文件，
 * 上述 \x 转义字符串不会被扫描到，故用注释字形补齐：
 * 選擇語言安裝類型工作圖形桌面伺服器命令列目標磁碟偵測未資料將被擦除警告
 * 立開始現正續裝完成重啟進入系統失敗詳見誌繼請稍候當
 */

static void setup_scan_disks(void)
{
    /* Enumerate raw disks via kern.disks, size in MB via diskinfo */
    FILE *f = popen(
        "for b in $(sysctl -n kern.disks); do\n"
        "  case \"$b\" in cd*|acd*) continue ;; esac\n"
        "  s=$(diskinfo /dev/$b 2>/dev/null | awk '{print $3}')\n"
        "  echo \"/dev/$b $(( s / 1024 / 1024 ))MB\"\n"
        "done",
        "r");
    setup_n_disks = 0;
    setup_disk_sel = -1;
    if (f) {
        char buf[128];
        while (setup_n_disks < 8 && fgets(buf, sizeof buf, f)) {
            size_t l = strlen(buf);
            if (l && buf[l - 1] == '\n') buf[l - 1] = 0;
            snprintf(setup_disks[setup_n_disks], sizeof setup_disks[0], "%s", buf);
            setup_n_disks++;
        }
        pclose(f);
    }
    if (setup_n_disks > 0) setup_disk_sel = 0;
}

static void setup_go(int page)
{
    setup_page = page;
    if (page == 2) setup_scan_disks();
    if (page == 4) { free_lines(); scroll = 0; }
    dirty = 1;
}

static void setup_start(void)
{
    iset_lang = ILG_EN;
    setup_prof = 0;
    setup_exit = -1;
    setup_go(0);
}

static void setup_launch(void)
{
    char cmd[300];
    int fds[2];
    char *sp;

    if (setup_disk_sel < 0 || setup_disk_sel >= setup_n_disks) return;
    snprintf(setup_dev, sizeof setup_dev, "%s", setup_disks[setup_disk_sel]);
    sp = strchr(setup_dev, ' ');
    if (sp) *sp = 0;

    snprintf(cmd, sizeof cmd,
             "setup-minobsd --auto --fs ufs --disk %s 2>&1",
             setup_dev);
    if (pipe(fds) != 0) return;
    setup_pid = fork();
    if (setup_pid == 0) {
        close(fds[0]);
        dup2(fds[1], 1);
        dup2(fds[1], 2);
        close(fds[1]);
        setsid();
        execl("/bin/sh", "sh", "-c", cmd, (char *)0);
        _exit(127);
    }
    close(fds[1]);
    setup_fd = fds[0];
    fcntl(setup_fd, F_SETFL, O_NONBLOCK);
    setup_go(4);
    add_line(IL(I_RUNNING));
    add_line("");
}

/* 安装输出管道喂数据（主循环 select 就绪时调用） */
static void setup_poll(void)
{
    char buf[512];
    int rn, i, start;
    if (setup_fd < 0) return;
    while ((rn = read(setup_fd, buf, sizeof buf)) > 0) {
        start = 0;
        for (i = 0; i < rn; i++) {
            if (buf[i] == '\n') {
                buf[i] = 0;
                if (i > start) {
                    ansi_filter(buf + start);
                    add_line(buf + start);
                }
                start = i + 1;
            }
        }
        if (rn > start) {
            buf[rn] = 0;
            ansi_filter(buf + start);
            add_line(buf + start);
        }
        dirty = 1;
    }
    if (rn == 0) {                       /* EOF：安装进程退出 */
        int st = 1;
        close(setup_fd);
        setup_fd = -1;
        if (setup_pid > 0) {
            waitpid(setup_pid, &st, 0);
            setup_pid = -1;
        }
        setup_exit = (WIFEXITED(st) && WEXITSTATUS(st) == 0) ? 0 : 1;
        add_line("");
        add_line(setup_exit == 0 ? IL(I_DONE) : IL(I_FAIL));
        scroll = nlines;                  /* 完成/失败页滚到日志底部 */
        setup_go(setup_exit == 0 ? 5 : 6);
    }
}

/* 安装向导按钮点击：返回 1 已消费。bl/bt=按钮区几何 */
static int setup_hit_button(int X, int Y, int bl, int bt, int primary)
{
    const char *label = primary
        ? (setup_page == 3 ? IL(I_INSTALL_NOW) :
           setup_page == 5 ? IL(I_REBOOT) : IL(I_NEXT))
        : IL(I_BACK);
    int bw = text_w(label, SCALE_TXT) + 40;
    if (X >= bl && X <= bl + bw && Y >= bt && Y <= bt + 30) {
        switch (setup_page) {
        case 0: if (primary) setup_go(1); break;
        case 1: if (primary) setup_go(2); else setup_go(0); break;
        case 2: if (primary) setup_go(3); else setup_go(1); break;
        case 3: if (primary) setup_launch(); else setup_go(2); break;
        case 5: if (primary) { sync(); system("reboot"); } break;
        case 6: if (primary) setup_start(); break;
        }
        return 1;
    }
    return 0;
}

/* 安装向导键盘：1=上 2=下 Enter=确认 Esc=返回 */
static void setup_key(int k)
{
    int max;
    switch (setup_page) {
    case 0:
        max = ILG_COUNT - 1;
        if (k == 1) iset_lang = iset_lang > 0 ? iset_lang - 1 : max;
        if (k == 2) iset_lang = iset_lang < max ? iset_lang + 1 : 0;
        if (k == '\r' || k == '\n') setup_go(1);
        break;
    case 1:
        if (k == 1 || k == 2) setup_prof = !setup_prof;
        if (k == '\r' || k == '\n') setup_go(2);
        if (k == 27) setup_go(0);
        break;
    case 2:
        max = setup_n_disks - 1;
        if (max < 0) break;
        if (k == 1) setup_disk_sel = setup_disk_sel > 0 ? setup_disk_sel - 1 : max;
        if (k == 2) setup_disk_sel = setup_disk_sel < max ? setup_disk_sel + 1 : 0;
        if (k == '\r' || k == '\n') setup_go(3);
        if (k == 27) setup_go(1);
        break;
    case 3:
        if (k == '\r' || k == '\n') setup_launch();
        if (k == 27) setup_go(2);
        break;
    case 5: if (k == '\r' || k == '\n') { sync(); system("reboot"); } break;
    case 6: if (k == '\r' || k == '\n') setup_start(); break;
    }
    dirty = 1;
}

/* ================= 鼠标（evdev） ================= */

#define MAXEV 8
static int evfd[MAXEV] = { -1, -1, -1, -1, -1, -1, -1, -1 };
static int mx, my;              /* 光标位置 */
static int cursor_on = 0;       /* 光标已绘制（背景已保存） */
static u32 cursor_save[16 * 18];

/* 触摸屏：EV_ABS 的 ABS_X/ABS_Y 取值范围，由 input event 的 absinfo.maximum 推断，
 * 缺省按常见触摸屏 0..1023/0..767；首次 ABS 事件时从 value>max 推断真实 max。
 * 简化方案：固定参考分辨率，触摸坐标按比例映射到屏幕 (W,H)。 */
static int abs_xmax = 1024, abs_ymax = 768;
static int touch_down = 0;        /* 当前 ABS 触摸按下状态 */

/* 箭头光标（10x16）：'#'=黑描边  '*'=白填充  空格=透明 */
#define CUR_W 10
#define CUR_H 16
static const char *cursor_map[CUR_H] = {
    "#         ",
    "##        ",
    "#*#       ",
    "#**#      ",
    "#***#     ",
    "#****#    ",
    "#*****#   ",
    "#******#  ",
    "#*******# ",
    "#********#",
    "#****#### ",
    "#***#*#   ",
    "#**# ##   ",
    "#*#  ##   ",
    "##   ##   ",
    "      ##  ",
};

static void draw_cursor(void)
{
    int r, c;
    for (r = 0; r < CUR_H; r++) {
        for (c = 0; c < CUR_W; c++) {
            cursor_save[r * CUR_W + c] =
                fbp[(my + r) * W + (mx + c)];
        }
    }
    for (r = 0; r < CUR_H; r++) {
        for (c = 0; c < CUR_W; c++) {
            char ch = cursor_map[r][c];
            if (ch == '#') px(mx + c, my + r, 0x000000);
            else if (ch == '*') px(mx + c, my + r, 0xffffff);
        }
    }
    cursor_on = 1;
}

static void erase_cursor(void)
{
    int r, c;
    if (!cursor_on) return;
    for (r = 0; r < CUR_H; r++)
        for (c = 0; c < CUR_W; c++)
            fbp[(my + r) * W + (mx + c)] = cursor_save[r * CUR_W + c];
    cursor_on = 0;
}

static void cursor_move(int dx, int dy)
{
    if (cursor_on) erase_cursor();
    mx += dx; my += dy;
    if (mx < 0) mx = 0;
    if (my < 0) my = 0;
    if (mx > W - CUR_W) mx = W - CUR_W;
    if (my > H - CUR_H) my = H - CUR_H;
    draw_cursor();
}

/* 打开所有可用 evdev 设备（键盘设备混进来也没关系，
 * 下面只处理 EV_REL / 鼠标按键 / 触摸事件） */
static void mouse_try_open(void)
{
    int i;
    for (i = 0; i < MAXEV; i++) {
        char path[32];
        int fd;
        struct input_absinfo ai;
        if (evfd[i] >= 0) continue;
        snprintf(path, sizeof path, "/dev/input/event%d", i);
        fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;
        evfd[i] = fd;
        /* 取 ABS_X / ABS_Y 真实最大值（触摸屏坐标系到屏幕的映射基准） */
        if (ioctl(fd, EVIOCGABS(ABS_X), &ai) == 0 && ai.maximum > 0)
            abs_xmax = ai.maximum;
        if (ioctl(fd, EVIOCGABS(ABS_Y), &ai) == 0 && ai.maximum > 0)
            abs_ymax = ai.maximum;
    }
}

/* ================= 虚拟键盘（mobile 模式） ================= */
/* 虚拟键盘双布局：QWERTY 与 Dvorak 简化键盘（DSK），/etc/e1kbd 持久化 */
static const char *kbd_qwerty[] = {
    "1234567890",
    "qwertyuiop",
    "asdfghjkl",
    "zxcvbnm,."
};
static const char *kbd_dvorak[] = {
    "1234567890",
    "',.pyfgcrl",
    "aoeuidhtns",
    ";qjkxbmwvz"
};
static int kbd_layout = 0;          /* 0=QWERTY  1=Dvorak */

static const char **kbd_rows_active(void) { return kbd_layout ? kbd_dvorak : kbd_qwerty; }

static void kbd_load(void)
{
    FILE *f = fopen("/etc/e1kbd", "r");
    char b[8];
    if (f) {
        if (fgets(b, sizeof b, f) && (b[0] == 'd' || b[0] == 'D'))
            kbd_layout = 1;
        fclose(f);
    }
}

static void kbd_save(void)
{
    FILE *f = fopen("/etc/e1kbd", "w");
    if (f) { fputs(kbd_layout ? "dvorak\n" : "qwerty\n", f); fclose(f); }
}

/* 5 行布局：4 行字母 + 1 行控制键（Esc/中EN/Spc/Ent/Bak/Lay）
 * 覆盖屏幕底部 250px，触屏点击注入字符到当前激活输入（浏览器地址栏 / 终端） */
#define KBD_ROWS    5
#define KBD_KEY_H   50

static int kbd_visible = 0;        /* 是否显示 */
static int kbd_target = 0;         /* 0=none 1=browser 2=term */

static int kbd_top(void) { return H - KBD_ROWS * KBD_KEY_H - 4; }
static int kbd_key_w(void) { return W / 10; }   /* 前四行每行最多 10 键 */

/* ================= 拼音输入法 ================= */
static int ime_active = 0;             /* 0=英文直通 1=拼音模式 */
static char ime_buf[32];                /* 拼音缓冲区（无声调小写） */
static int  ime_buf_len = 0;
static char ime_cands[5][16];           /* 候选项 UTF-8（最多5个，可含2-3字组合） */
static int  ime_ncands = 0;

/* UTF-8 字符步进：返回一个字符的字节数 */
static int utf8_len(unsigned char c)
{
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

/* 在字典中查找精确匹配的音节 */
static const struct ime_entry *ime_find(const char *py)
{
    int i;
    for (i = 0; i < ime_dict_count; i++)
        if (strcmp(ime_dict[i].py, py) == 0)
            return &ime_dict[i];
    return NULL;
}

/* 判断是否为有效拼音音节 */
static int ime_valid(const char *py)
{
    return ime_find(py) != NULL;
}

/* 从 chars 字符串中提取第 n 个 UTF-8 字符到 out（out 至少 8 字节） */
static void utf8_char_at(const char *s, int idx, char *out)
{
    int i, n = 0;
    while (*s && n < idx) { s += utf8_len((unsigned char)*s); n++; }
    if (*s) {
        int l = utf8_len((unsigned char)*s);
        for (i = 0; i < l && i < 7; i++) out[i] = s[i];
        out[l] = 0;
    } else out[0] = 0;
}

/* 组合两个音节的第 i1/i2 字为组合候选，写入 ime_cands[k]，返回 1=成功 */
static int ime_combo2(const char *chars1, int i1,
                      const char *chars2, int i2, int k)
{
    char c1[8], c2[8];
    utf8_char_at(chars1, i1, c1);
    utf8_char_at(chars2, i2, c2);
    if (c1[0] && c2[0]) {
        snprintf(ime_cands[k], 16, "%s%s", c1, c2);
        return 1;
    }
    return 0;
}

/* 组合三个音节的第 i1/i2/i3 字为组合候选 */
static int ime_combo3(const char *c1s, int i1,
                      const char *c2s, int i2,
                      const char *c3s, int i3, int k)
{
    char c1[8], c2[8], c3[8];
    utf8_char_at(c1s, i1, c1);
    utf8_char_at(c2s, i2, c2);
    utf8_char_at(c3s, i3, c3);
    if (c1[0] && c2[0] && c3[0]) {
        snprintf(ime_cands[k], 16, "%s%s%s", c1, c2, c3);
        return 1;
    }
    return 0;
}

/* 尝试将拼音缓冲区拆分为有效音节，生成候选项 */
static void ime_update(void)
{
    int k = 0;
    ime_ncands = 0;
    if (ime_buf_len == 0) return;

    /* 1. 精确匹配整个缓冲区 */
    {
        const struct ime_entry *e = ime_find(ime_buf);
        if (e) {
            int i;
            const char *s = e->chars;
            for (i = 0; s[0] && k < 5; i++) {
                int l = utf8_len((unsigned char)*s);
                if (l > 7) break;
                memcpy(ime_cands[k], s, l); ime_cands[k][l] = 0;
                k++; s += l;
            }
        }
    }

    /* 2. 多音节拆分：贪心最长有效音节前缀 + 组合候选 */
    if (k < 5) {
        int plen;
        for (plen = ime_buf_len > 6 ? 6 : ime_buf_len; plen >= 1; plen--) {
            char prefix[8];
            const struct ime_entry *e;
            if (plen >= (int)sizeof(prefix)) continue;
            memcpy(prefix, ime_buf, plen); prefix[plen] = 0;
            if (plen == ime_buf_len) continue;
            e = ime_find(prefix);
            if (!e) continue;

            /* 剩余部分尝试完整 2 音节拆分 */
            {
                const char *rest = ime_buf + plen;
                const struct ime_entry *e2 = ime_find(rest);
                if (e2) {
                    /* 两个完整音节：对角线生成组合候选 */
                    int pairs[][2] = {{0,0},{0,1},{1,0},{0,2},{1,1},
                                      {2,0},{0,3},{1,2},{2,1},{3,0}};
                    int i;
                    for (i = 0; i < 10 && k < 5; i++)
                        if (ime_combo2(e->chars, pairs[i][0],
                                       e2->chars, pairs[i][1], k))
                            k++;
                } else {
                    /* rest 可能是多音节，递归拆分 */
                    int rlen = ime_buf_len - plen;
                    int rplen;
                    int found = 0;
                    for (rplen = rlen > 6 ? 6 : rlen; rplen >= 1; rplen--) {
                        char rprefix[8];
                        const struct ime_entry *re;
                        if (rplen >= (int)sizeof(rprefix)) continue;
                        memcpy(rprefix, rest, rplen);
                        rprefix[rplen] = 0;
                        if (rplen == rlen) continue;
                        re = ime_find(rprefix);
                        if (!re) continue;
                        {
                            const char *rr = rest + rplen;
                            const struct ime_entry *re2 = ime_find(rr);
                            if (re2) {
                                /* 三音节组合：各取首字 */
                                if (k < 5 && ime_combo3(
                                        e->chars, 0, re->chars, 0,
                                        re2->chars, 0, k)) k++;
                                if (k < 5 && ime_combo3(
                                        e->chars, 0, re->chars, 1,
                                        re2->chars, 0, k)) k++;
                                if (k < 5 && ime_combo3(
                                        e->chars, 0, re->chars, 0,
                                        re2->chars, 1, k)) k++;
                                found = 1;
                                break;
                            }
                        }
                    }
                    if (!found) {
                        /* rest 做前缀匹配，组合首字 */
                        int ri;
                        for (ri = 0; ri < ime_dict_count && k < 5; ri++) {
                            if (strncmp(ime_dict[ri].py, rest,
                                        strlen(rest)) == 0
                                && strcmp(ime_dict[ri].py, rest) != 0) {
                                if (ime_combo2(e->chars, 0,
                                               ime_dict[ri].chars, 0, k))
                                    k++;
                                break;
                            }
                        }
                    }
                }
            }
            if (k >= 5) break;
            if (k > 0) break;  /* 取最长有效前缀 */
        }
    }

    /* 3. 前缀匹配补充（整缓冲区是某音节前缀） */
    if (k < 5) {
        int i;
        for (i = 0; i < ime_dict_count && k < 5; i++) {
            if (strncmp(ime_dict[i].py, ime_buf, ime_buf_len) == 0
                && strcmp(ime_dict[i].py, ime_buf) != 0) {
                utf8_char_at(ime_dict[i].chars, 0, ime_cands[k]);
                if (ime_cands[k][0]) k++;
            }
        }
    }

    ime_ncands = k;
}

/* 注入 UTF-8 字符串到当前输入目标 */
static void kbd_inject_str(const char *s)
{
    /* kbd_target=0（物理键盘，虚拟键盘未显示）时，根据 open_app 决定注入目标 */
    int target = kbd_target;
    if (target == 0) {
        if (open_app == APP_BROWSER) target = 1;
        else if (open_app == APP_TERM) target = 2;
    }
    if (target == 1) {
        while (*s && br_input_len < (int)sizeof(br_input) - 4)
            br_input[br_input_len++] = *s++;
        br_input[br_input_len] = 0;
    } else if (target == 2) {
        if (term_fd >= 0) write(term_fd, s, strlen(s));
    }
    dirty = 1;
}

/* IME 处理一个按键，返回 1=已处理（拦截） */
static int ime_key(int c)
{
    if (!ime_active) return 0;
    if (c >= 'a' && c <= 'z') {
        if (ime_buf_len < 31) {
            ime_buf[ime_buf_len++] = c;
            ime_buf[ime_buf_len] = 0;
            ime_update();
            dirty = 1;
        }
        return 1;
    }
    /* 撇号分隔强制音节边界：xi'an → xi+an */
    if (c == '\'' && ime_buf_len > 0 && ime_buf_len < 31) {
        ime_buf[ime_buf_len++] = '\'';
        ime_buf[ime_buf_len] = 0;
        ime_update();
        dirty = 1;
        return 1;
    }
    if (c >= '1' && c <= '5' && ime_ncands > 0) {
        int n = c - '1';
        if (n < ime_ncands) {
            kbd_inject_str(ime_cands[n]);
            ime_buf_len = 0; ime_ncands = 0;
            dirty = 1;
        }
        return 1;
    }
    if (c == ' ' && ime_ncands > 0) {
        kbd_inject_str(ime_cands[0]);
        ime_buf_len = 0; ime_ncands = 0;
        dirty = 1;
        return 1;
    }
    if ((c == 8 || c == 127) && ime_buf_len > 0) {
        ime_buf[--ime_buf_len] = 0;
        ime_update();
        dirty = 1;
        return 1;
    }
    if (c == 27) {                  /* ESC 取消拼音 */
        ime_buf_len = 0; ime_ncands = 0;
        dirty = 1;
        return 1;
    }
    /* 中文标点 */
    if (c == '.')  { kbd_inject_str("\xE3\x80\x82"); return 1; }      /* 。 */
    if (c == ',')  { kbd_inject_str("\xEF\xBC\x8C"); return 1; }      /* ， */
    if (c == '?')  { kbd_inject_str("\xEF\xBC\x9F"); return 1; }      /* ？ */
    if (c == '!')  { kbd_inject_str("\xEF\xBC\x81"); return 1; }      /* ！ */
    if (c == ':')  { kbd_inject_str("\xEF\xBC\x9A"); return 1; }      /* ： */
    if (c == ';')  { kbd_inject_str("\xEF\xBC\x9B"); return 1; }      /* ； */
    if (c == '(')  { kbd_inject_str("\xEF\xBC\x88"); return 1; }      /* （ */
    if (c == ')')  { kbd_inject_str("\xEF\xBC\x89"); return 1; }      /* ） */
    return 0;                       /* 未处理，走正常注入 */
}

static void ime_toggle(void)
{
    ime_active = !ime_active;
    ime_buf_len = 0; ime_ncands = 0;
    dirty = 1;
}

static void kbd_inject(int c)
{
    /* IME 拦截：拼音模式下字母和标点经 IME 处理 */
    if (ime_key(c)) return;
    key_raw = c;
    if (kbd_target == 1) br_key(c);
    else if (kbd_target == 2) term_key(c);
    dirty = 1;
}

static void kbd_show(int target)
{
    kbd_target = target;
    kbd_visible = 1;
    dirty = 1;
}

static void kbd_hide(void)
{
    kbd_visible = 0;
    kbd_target = 0;
    dirty = 1;
}

static void kbd_toggle_layout(void)
{
    kbd_layout = !kbd_layout;
    kbd_save();
    dirty = 1;
}

static void kbd_draw(void)
{
    int y = kbd_top();
    int kw = kbd_key_w();
    const char **rows = kbd_rows_active();
    int r;

    /* 拼音候选栏（IME 激活时显示在键盘上方） */
    if (ime_active) {
        int cbar_h = 32;
        int cy = y - cbar_h - 2;
        fill(0, cy, W, cbar_h, C_TITLE);
        fill(0, cy, W, 1, C_ACCENT);
        /* 左侧：当前拼音 */
        {
            char buf[24];
            snprintf(buf, sizeof buf, "%s%s", ime_buf, ime_buf_len ? "" : "_");
            text(8, cy + 8, buf, C_ACCENT, 2);
        }
        /* 右侧：候选列表 "1.你 2.尼 ..." */
        {
            int i, x = 80;
            for (i = 0; i < ime_ncands && i < 5; i++) {
                char buf[16];
                int label_w;
                snprintf(buf, sizeof buf, "%d.", i + 1);
                text(x, cy + 8, buf, C_DIM, 2);
                label_w = text_w(buf, 2);
                text(x + label_w + 2, cy + 8, ime_cands[i], C_FG, 2);
                x += label_w + 2 + text_w(ime_cands[i], 2) + 12;
            }
            if (ime_ncands == 0 && ime_buf_len > 0) {
                text(80, cy + 8, lang ? "(no match)" : "(无匹配)", C_DIM, 2);
            }
        }
    }

    fill(0, y - 2, W, KBD_ROWS * KBD_KEY_H + 4, C_TITLE);
    for (r = 0; r < 4; r++) {
        int n = strlen(rows[r]);
        int x = (W - n * kw) / 2;
        int i;
        for (i = 0; i < n; i++) {
            char buf[2] = { rows[r][i], 0 };
            fill(x + 2, y + 2, kw - 4, KBD_KEY_H - 4, C_WIN);
            text(x + (kw - 8 * SCALE_TXT) / 2,
                 y + (KBD_KEY_H - 8 * SCALE_TXT) / 2, buf, C_FG, SCALE_TXT);
            x += kw;
        }
        y += KBD_KEY_H;
    }
    /* 第 5 行 6 键：Esc/中EN/Spc/Ent/Bak/Lay */
    {
        const char *labels[6] = {
            "Esc", ime_active ? "\xE4\xB8\xAD" : "EN",
            "Spc", "Ent", "Bak", kbd_layout ? "DV" : "QW"
        };
        int cw = W / 6;
        int i;
        for (i = 0; i < 6; i++) {
            int x = i * cw;
            fill(x + 2, y + 2, cw - 4, KBD_KEY_H - 4, C_WIN);
            /* IME 激活时中/EN 键高亮 */
            if (i == 1 && ime_active) {
                fill(x + 2, y + 2, cw - 4, KBD_KEY_H - 4, C_SEL);
                text(x + (cw - text_w(labels[i], SCALE_TXT)) / 2,
                     y + (KBD_KEY_H - 8 * SCALE_TXT) / 2, labels[i], 0xffffff, SCALE_TXT);
            } else {
                text(x + (cw - text_w(labels[i], SCALE_TXT)) / 2,
                     y + (KBD_KEY_H - 8 * SCALE_TXT) / 2, labels[i], C_ACCENT, SCALE_TXT);
            }
        }
    }
}

/* 返回 1=已处理（点击落在键盘区） */
static int kbd_click(int x, int y)
{
    int kw, row, kt = kbd_top();
    const char **rows;
    if (y < kt) return 0;
    kw = kbd_key_w();
    row = (y - kt) / KBD_KEY_H;
    if (row < 0 || row >= KBD_ROWS) return 1;
    rows = kbd_rows_active();
    if (row < 4) {
        int n = strlen(rows[row]);
        int x0 = (W - n * kw) / 2;
        int col;
        if (x < x0 || x >= x0 + n * kw) return 1;
        col = (x - x0) / kw;
        kbd_inject((unsigned char)rows[row][col]);
    } else {
        /* 第 5 行 6 键 */
        int cw = W / 6;
        int col = x / cw;
        switch (col) {
        case 0: kbd_hide(); break;               /* Esc */
        case 1: ime_toggle(); break;            /* 中/EN 切换 */
        case 2: kbd_inject(' '); break;          /* Space */
        case 3: kbd_inject('\n'); break;         /* Enter */
        case 4: kbd_inject(8); break;            /* Backspace */
        case 5: kbd_toggle_layout(); break;     /* Layout */
        default: break;
        }
    }
    return 1;
}

/* 点击命中测试与动作（tap=按下→松开，见 mouse_poll） */
static void mouse_click(void)
{
    /* mobile 模式下键盘可见时，优先处理键盘点击 */
    if (mode_mobile && kbd_visible && kbd_click(mx, my)) return;

    /* ---- 多任务卡片视图（mobile） ---- */
    if (mode_mobile && mt_mode && open_app < 0) {
        int cw = 240, ch = 150, gap = 24;
        int n = APP_COUNT;
        int row_w = n * (cw + gap) - gap;
        int x0 = (W - row_w) / 2, y0 = H / 2 - ch / 2 - 20;
        int i;
        for (i = 0; i < n; i++) {
            int cx = x0 + i * (cw + gap);
            if (mx >= cx && mx <= cx + cw && my >= y0 && my <= y0 + ch) {
                mt_mode = 0;
                menu_sel = i;
                open_app = i;
                run_app(i);
                dirty = 1;
                return;
            }
        }
        mt_mode = 0;                    /* 点空白处退出多任务 */
        dirty = 1;
        return;
    }

    /* ---- 桌面启动器 ---- */
    if (open_app < 0) {
        int i;
        int n = app_count_visible();
        if (!mode_mobile) {
            /* workstation Dock：多行自适应图标条 */
            int cols, rows, dx, dy, barh, isz;
            dock_geom(&cols, &rows, &dx, &dy, &barh, &isz);
            for (i = 0; i < n; i++) {
                int ix, iy;
                dock_slot(i, &ix, &iy, isz);
                if (mx >= ix && mx <= ix + isz &&
                    my >= iy && my <= iy + isz + 12) {
                    menu_sel = i;
                    open_app = i;
                    run_app(i);
                    anim_start(1);
                    dirty = 1;
                    return;
                }
            }
        } else {
            /* mobile 主屏网格：4 列图标（行高自适应） */
            int cols, chh, isz, gy0;
            int cw;
            grid_geom(&cols, &chh, &isz, &gy0);
            cw = W / cols;
            for (i = 0; i < n; i++) {
                int cx = (i % cols) * cw;
                int cy = gy0 + (i / cols) * chh;
                int ix = cx + (cw - isz) / 2;
                int iy = cy + 6;
                if (mx >= ix && mx <= ix + isz && my >= iy && my <= iy + isz + 34) {
                    menu_sel = i;
                    open_app = i;
                    run_app(i);
                    anim_start(1);
                    dirty = 1;
                    return;
                }
            }
        }
        return;
    }

    /* ---- 应用窗口内 ---- */
    {
        int wx, wy, ww, wh;
        win_rect(&wx, &wy, &ww, &wh);

        if (!mode_mobile) {
            /* macOS 红绿灯：关闭 / 最小化 / 最大化 */
            int ty = wy + 13;
            int dx2, dy2;
            dx2 = mx - (wx + 16); dy2 = my - ty;
            if (dx2 * dx2 + dy2 * dy2 <= 100) { app_close_anim(); return; }
            dx2 = mx - (wx + 36); dy2 = my - ty;
            if (dx2 * dx2 + dy2 * dy2 <= 100) { app_minimize(); return; }
            dx2 = mx - (wx + 56); dy2 = my - ty;
            if (dx2 * dx2 + dy2 * dy2 <= 100) { win_max = !win_max; dirty = 1; return; }

            /* 图形安装向导按钮 */
            if (open_app == APP_INSTALL &&
                (setup_page <= 3 || setup_page == 5)) {
                int bt = wy + wh - 44;
                if (setup_hit_button(mx, my, wx + ww - 150, bt, 1)) return;
                if (setup_page > 0 && setup_hit_button(mx, my, wx + ww - 260, bt, 0))
                    return;
            }
        }

        /* mobile 模式：点击浏览器地址栏 / 终端窗口内容 → 弹出虚拟键盘 */
        if (mode_mobile) {
            if (open_app == APP_BROWSER) {
                int bar_y = wy + 26 + 6;
                if (mx >= wx && mx <= wx + ww &&
                    my >= bar_y - 2 && my <= bar_y + 20) {
                    kbd_show(1);
                    return;
                }
            } else if (open_app == APP_TERM) {
                if (mx >= wx && mx <= wx + ww &&
                    my >= wy + 26 && my <= wy + wh) {
                    kbd_show(2);
                    return;
                }
            }
        }
    }
}

static void mouse_back(void)
{
    if (open_app >= 0) {
        open_app = -1;
        free_lines();
        scroll = 0;
        dirty = 1;
    }
}

static void mouse_wheel(int dir)        /* dir: +1 下翻 / -1 上翻 */
{
    if (open_app >= 0) {
        scroll += dir * 3;
    } else {
        int maxsel = app_count_visible() - 1;
        menu_sel += dir;
        if (menu_sel < 0) menu_sel = 0;
        if (menu_sel > maxsel) menu_sel = maxsel;
    }
    dirty = 1;
}

/* 非阻塞读取全部 evdev 事件。
 * 点击语义 = 按下→松开且位移 <10px（tap）；拖动进入手势流程：
 *   mobile：手势条上滑=主页（>150px 为多任务）、左右缘滑动=返回；
 *   workstation：仅 tap（无系统手势，与 macOS 桌面操作一致）。 */
static void mouse_poll(void)
{
    int i;
    int moved = 0, wheel = 0;
    int old_mx = mx, old_my = my;
    int ptr_down = gest_ptr_down;   /* 指针按下中（鼠标左键或触摸） */
    static int down_x = 0, down_y = 0;
    int release_click = 0;

    for (i = 0; i < MAXEV; i++) {
        struct input_event e;
        if (evfd[i] < 0) continue;
        while (read(evfd[i], &e, sizeof e) == (ssize_t)sizeof e) {
            if (e.type == EV_REL) {
                if (e.code == REL_X) { mx += (int)e.value * 2; moved = 1; }
                else if (e.code == REL_Y) { my += (int)e.value * 2; moved = 1; }
                else if (e.code == REL_WHEEL) wheel += (int)e.value;
            } else if (e.type == EV_ABS) {
                /* 触摸屏 / 触摸板：ABS_X/ABS_Y 是绝对坐标，
                 * 按比例映射到屏幕 (W,H)，坐标更新即视为移动 */
                if (e.code == ABS_X) {
                    if (e.value > abs_xmax) abs_xmax = e.value;
                    mx = (int)((long)e.value * (W - CUR_W) / (abs_xmax ? abs_xmax : 1));
                    moved = 1;
                } else if (e.code == ABS_Y) {
                    if (e.value > abs_ymax) abs_ymax = e.value;
                    my = (int)((long)e.value * (H - CUR_H) / (abs_ymax ? abs_ymax : 1));
                    moved = 1;
                }
            } else if (e.type == EV_KEY) {
                if (e.code == BTN_LEFT || e.code == BTN_TOUCH) {
                    /* 统一按下/松开状态机：触摸设备可能同时报 BTN_TOUCH 与
                     * BTN_LEFT，用 ptr_down 去重（第一个按下事件生效） */
                    if (e.value == 1 && !ptr_down) {
                        ptr_down = 1;
                        gest_ptr_down = 1;
                        down_x = mx; down_y = my;
                        if (mode_mobile) {
                            if (my >= H - 28)                    gest_kind = 1;
                            else if (mx < EDGE_W)                gest_kind = 2;
                            else if (mx >= W - EDGE_W)           gest_kind = 3;
                            else                                 gest_kind = 0;
                            gest_dy = 0; gest_dx = 0;
                        } else {
                            gest_kind = 0;
                        }
                    } else if (e.value == 0 && ptr_down) {
                        ptr_down = 0;
                        gest_ptr_down = 0;
                        if (mode_mobile && gesture_finish())
                            ;                              /* 手势已消费 */
                        else if (abs(mx - down_x) + abs(my - down_y) < 10)
                            release_click = 1;             /* tap → click */
                    }
                } else if (e.code == BTN_RIGHT && e.value == 1) {
                    mouse_back();
                }
            }
        }
    }
    if (ptr_down && moved && gest_kind) {
        /* 手势拖动反馈：更新位移并请求重绘（画手势条上移 / 侧缘高亮） */
        gest_dy = my - down_y;
        gest_dx = mx - down_x;
        if (gest_kind == 1) {
            int lift = -gest_dy;
            if (lift < 0) lift = 0;
            if (lift > 60) lift = 60;
            if (lift != pill_lift) { pill_lift = lift; dirty = 1; }
        } else {
            dirty = 1;
        }
    }
    if (moved) {
        if (mx < 0) mx = 0;
        if (my < 0) my = 0;
        if (mx > W - CUR_W) mx = W - CUR_W;
        if (my > H - CUR_H) my = H - CUR_H;
        if (cursor_on) {
            /* 用旧位置把背景还回去（cursor_save 是旧位置保存的像素），
             * 再在新位置保存背景+画光标 */
            int r, c;
            for (r = 0; r < CUR_H; r++)
                for (c = 0; c < CUR_W; c++)
                    fbp[(old_my + r) * W + (old_mx + c)] = cursor_save[r * CUR_W + c];
        }
        draw_cursor();
    }
    if (wheel) mouse_wheel(wheel > 0 ? 1 : -1);
    if (release_click) mouse_click();
}

/* ================= 场景绘制 ================= */

/* 桌面渐变底色（圆角遮挡时用于"抠出"背景） */
static u32 bg_at(int y)
{
    u32 top = C_BG_TOP, bot = C_BG_BOT, c = 0;
    u8 *a = (u8 *)&c;
    u8 *t8 = (u8 *)&top, *b8 = (u8 *)&bot;
    int k;
    for (k = 0; k < 3; k++)
        a[k] = t8[k] + (b8[k] - t8[k]) * y / (H ? H : 1);
    return c;
}

#define BAR_H 28

static void draw_desktop(void)
{
    int i, y;
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    char clockbuf[32];

    /* 垂直渐变背景 */
    for (y = 0; y < H; y++) {
        u32 c = bg_at(y);
        for (i = 0; i < W; i++)
            fbp[y * W + i] = c;
    }

    snprintf(clockbuf, sizeof clockbuf, "%02d:%02d",
             tm->tm_hour, tm->tm_min);

    if (!mode_mobile) {
        /* ============ workstation：macOS 风格菜单栏 ============ */
        fill(0, 0, W, BAR_H, C_MENU);
        text(10, 10, " e1LibreOS", C_FG, SCALE_TXT);
        if (open_app >= 0)
            text(150, 10, L(S_APP_FINDER + open_app), C_FG, SCALE_TXT);
        else
            text(150, 10, L(S_MENU_HINT), C_DIM, SCALE_TXT);
        text(W - text_w(clockbuf, 2) - text_w(L(S_LANG_SW), 1) - 24, 10,
             L(S_LANG_SW), C_DIM, 1);
        text(W - text_w(clockbuf, 2) - 12, 8, clockbuf, C_FG, 2);
    } else {
        /* ============ mobile：HarmonyOS 风格状态栏 ============ */
        fill(0, 0, W, BAR_H, C_HM_CARD);
        text(12, 8, clockbuf, C_FG, 2);
        {
            char sub[32];
            int s;
            for (s = 0; variant_str[s] && s < (int)sizeof sub - 1; s++)
                sub[s] = (variant_str[s] >= 'a' && variant_str[s] <= 'z') ?
                         variant_str[s] - 32 : variant_str[s];
            sub[s] = 0;
            text(W - text_w(sub, 1) - 12, 10, sub, C_HM_ACCENT, 1);
        }
    }

    if (open_app >= 0) return;                  /* 应用窗口下不画启动器 */

    if (!mode_mobile) {
        /* ---- workstation 桌面：中央品牌 + Dock ---- */
        text(W / 2 - text_w("e1LibreOS", 5) / 2, H / 3, "e1LibreOS", C_FG, 5);
        text(W / 2 - text_w(variant_str, 2) / 2, H / 3 + 88, variant_str, C_ACCENT, 2);

        /* Dock：底部居中圆角条（多行自适应）+ 图标 + 运行指示点 */
        {
            int n = APP_COUNT;
            int cols, rows, dx, dy, barh, isz;
            dock_geom(&cols, &rows, &dx, &dy, &barh, &isz);
            fill_round(dx, dy, W - 2 * dx, barh, 12, C_MENU);
            for (i = 0; i < n; i++) {
                int ix, iy;
                int hovered;
                dock_slot(i, &ix, &iy, isz);
                hovered = (mx >= ix && mx <= ix + isz &&
                          my >= iy && my <= iy + isz + 12);
                if (hovered)
                    fill_round(ix - 3, iy - 3, isz + 6, isz + 6, 10, C_SEL);
                draw_app_icon(ix, iy, isz, i, 0xffffff);
                /* 运行指示：终端常驻或当前打开的应用 */
                if ((i == APP_TERM && term_fd >= 0) || open_app == i)
                    draw_dot(ix + isz / 2, iy + isz + 6, 2, C_FG);
                /* 图标名 hover 提示 */
                if (hovered)
                    text(ix + isz / 2 - text_w(L(S_APP_FINDER + i), 1) / 2,
                         iy - 14, L(S_APP_FINDER + i), C_FG, 1);
            }
        }
        text(10, H / 3 + 124, L(S_BOTTOM_HINT), C_DIM, 1);
    } else if (mt_mode) {
        /* ---- mobile 多任务卡片视图 ---- */
        {
            int cw = 240, ch = 150, gap = 24;
            int n = APP_COUNT;
            int row_w = n * (cw + gap) - gap;
            int x0 = (W - row_w) / 2, y0 = H / 2 - ch / 2 - 20;
            text(W / 2 - text_w(L(S_MT_TITLE), 2) / 2, y0 - 34,
                 L(S_MT_TITLE), C_FG, 2);
            for (i = 0; i < n; i++) {
                int cx = x0 + i * (cw + gap);
                fill_round(cx, y0, cw, ch, 10, C_HM_CARD);
                fill(cx, y0, cw, 40, app_icon_col[i]);
                text(cx + 10, y0 + 12, L(S_APP_FINDER + i), 0xffffff, SCALE_TXT);
                if (i == APP_TERM && term_fd >= 0)
                    text(cx + cw - text_w(L(S_RUN_DOT), 1) - 10,
                         y0 + 14, L(S_RUN_DOT), 0xffffff, 1);
            }
        }
    } else {
        /* ---- mobile 主屏：HarmonyOS 风格应用网格（行高自适应） ---- */
        {
            int n = APP_COUNT;
            int cols, chh, isz, gy0, cw;
            grid_geom(&cols, &chh, &isz, &gy0);
            cw = W / cols;
            for (i = 0; i < n; i++) {
                int cx = (i % cols) * cw;
                int cy = gy0 + (i / cols) * chh;
                int ix = cx + (cw - isz) / 2;
                int iy = cy + 6;
                draw_app_icon(ix, iy, isz, i, 0xffffff);
                text(cx + (cw - text_w(L(S_APP_FINDER + i), 1)) / 2,
                     iy + isz + 8, L(S_APP_FINDER + i), C_FG, 1);
            }
        }
        text(W / 2 - text_w(L(S_GEST_HINT), 1) / 2, H - 52,
             L(S_GEST_HINT), C_DIM, 1);
    }

    /* ============ mobile 手势条（HarmonyOS pill）============ */
    if (mode_mobile) {
        int py = H - 16 - pill_lift;
        int pw = PILL_W + (pill_lift / 3);      /* 拖拽时略微变长 */
        fill_round((W - pw) / 2, py, pw, PILL_H, PILL_H / 2, C_HM_PILL);
        /* 侧缘返回手势高亮 */
        if (gest_kind == 2 && gest_ptr_down)
            fill(0, BAR_H, EDGE_W, H - BAR_H, C_SEL);
        if (gest_kind == 3 && gest_ptr_down)
            fill(W - EDGE_W, BAR_H, EDGE_W, H - BAR_H, C_SEL);
    }
}

static void draw_window(void)
{
    const char *title;
    int wx, wy, ww, wh, content_h, i, y;
    int line_h = 16 * SCALE_TXT;           /* 行高随字号 */
    int bottom;                            /* 内容渲染下边界（键盘可见时上移） */

    if (open_app < 0 || open_app >= APP_COUNT) return;
    title = L(S_APP_FINDER + open_app);

    win_rect(&wx, &wy, &ww, &wh);   /* mobile 全屏卡片 / workstation 窗口 / 动画位移 */
    /* mobile 模式下键盘可见时，窗口内容渲染到键盘上方为止 */
    bottom = wy + wh - 8;
    if (mode_mobile && kbd_visible) {
        int kt = kbd_top();
        if (kt - 8 < bottom) bottom = kt - 8;
    }
    fill(wx, wy, ww, wh, C_WIN);
    fill(wx, wy, ww, 26, C_TITLE);
    fill(wx, wy + wh - 2, ww, 2, C_ACCENT);
    if (!mode_mobile) {
        /* macOS 红绿灯：关闭 / 最小化 / 最大化（标题文字右移避让） */
        int ty = wy + 13;
        disc(wx + 16, ty, 6, C_TL_CLOSE);
        disc(wx + 36, ty, 6, C_TL_MIN);
        disc(wx + 56, ty, 6, C_TL_MAX);
        text(wx + 78, wy + 9, title, C_FG, SCALE_TXT);
    } else {
        text(wx + 8, wy + 9, title, C_FG, SCALE_TXT);
    }
    text(wx + ww - text_w(L(S_BACK), 1) - 8, wy + 9, L(S_BACK), C_DIM, 1);

    content_h = bottom - (wy + 26 + 6);

    if (open_app == APP_TERM) {
        /* 终端：直接渲染 term_scr 行 */
        y = wy + 26 + 6;
        for (i = 0; i < TERM_ROWS; i++, y += line_h) {
            if (y + line_h - 2 > bottom) break;
            if (term_scr[i][0])
                text(wx + 10, y, term_scr[i], C_FG, SCALE_TXT);
        }
        /* 底部提示 */
        text(wx + 8, wy + wh - 16, L(S_TERM_HINT), C_DIM, 1);
    } else if (open_app == APP_BROWSER) {
        /* 浏览器：地址栏 + 页面内容 */
        int bar_y = wy + 26 + 6;
        char bar[200];
        fill(wx + 6, bar_y - 2, ww - 12, 16 * SCALE_TXT + 4, C_TITLE);
        snprintf(bar, sizeof bar, "> %s_", br_input);
        text(wx + 10, bar_y, bar, C_ACCENT, SCALE_TXT);
        y = bar_y + 16 * SCALE_TXT + 8;
        {
            int vis = (bottom - y) / line_h;
            int max_scroll = nlines > vis ? nlines - vis : 0;
            if (scroll > max_scroll) scroll = max_scroll;
            if (scroll < 0) scroll = 0;
        }
        for (i = scroll; i < nlines; i++, y += line_h) {
            if (y + line_h - 2 > bottom) break;
            text(wx + 10, y, lines[i], C_FG, SCALE_TXT);
        }
        if (nlines > (content_h - 16 * SCALE_TXT) / line_h) {
            char s[32];
            snprintf(s, sizeof s, "%d/%d", scroll + 1, nlines);
            text(wx + ww - text_w(s, 1) - 8, wy + wh - 16, s, C_DIM, 1);
        }
    } else if (open_app == APP_INSTALL && !mode_mobile) {
        draw_setup(wx, wy, ww, wh, bottom);     /* 图形安装向导 */
    } else if (!app_draw_custom(wx, wy, ww, bottom)) {
        /* 静态窗口：lines 驱动 */
        {
            int vis = content_h / line_h;
            int max_scroll = nlines > vis ? nlines - vis : 0;
            if (scroll > max_scroll) scroll = max_scroll;
            if (scroll < 0) scroll = 0;
        }
        y = wy + 26 + 6;
        for (i = scroll; i < nlines; i++, y += line_h) {
            if (y + line_h - 2 > bottom) break;
            text(wx + 10, y, lines[i], C_FG, SCALE_TXT);
        }
        if (nlines > content_h / line_h) {
            char s[32];
            snprintf(s, sizeof s, "+/- %d/%d", scroll + 1, nlines);
            text(wx + ww - text_w(s, 1) - 8, wy + wh - 16, s, C_DIM, 1);
        }
    }
}

static void redraw(void)
{
    draw_desktop();
    if (open_app >= 0) draw_window();
    if (mode_mobile && kbd_visible) kbd_draw();
    /* IME 候选栏（workstation 始终显示；mobile 仅在虚拟键盘未显示时显示在屏幕底部） */
    if (ime_active && (!mode_mobile || !kbd_visible)) {
        int cy = H - 36;
        fill(0, cy, W, 32, C_TITLE);
        fill(0, cy, W, 1, C_ACCENT);
        {
            char buf[24];
            snprintf(buf, sizeof buf, "%s%s", ime_buf, ime_buf_len ? "" : "_");
            text(8, cy + 8, buf, C_ACCENT, 2);
        }
        {
            int i, x = 80;
            for (i = 0; i < ime_ncands && i < 5; i++) {
                char buf[16];
                int lw;
                snprintf(buf, sizeof buf, "%d.", i + 1);
                text(x, cy + 8, buf, C_DIM, 2);
                lw = text_w(buf, 2);
                text(x + lw + 2, cy + 8, ime_cands[i], C_FG, 2);
                x += lw + 2 + text_w(ime_cands[i], 2) + 12;
            }
            if (ime_ncands == 0 && ime_buf_len > 0)
                text(80, cy + 8, lang ? "(no match)" : "(无匹配)", C_DIM, 2);
        }
    }
}

/* ================= 终端与输入 ================= */

static void tty_raw(void)
{
    struct termios tio;
    if (tcgetattr(0, &saved_tio) != 0) return;
    tio = saved_tio;
    tio.c_lflag &= ~(ICANON | ECHO);
    tio.c_cc[VMIN] = 0;
    tio.c_cc[VTIME] = 0;
    tcsetattr(0, TCSANOW, &tio);
    fcntl(0, F_SETFL, O_NONBLOCK);
}

static void tty_restore(void)
{
    tcsetattr(0, TCSANOW, &saved_tio);
}

/* ---- VT 切换（Alt+Fn）：VT_PROCESS 模式下内核经信号征求同意 ---- */
static volatile sig_atomic_t vt_active = 1;

static void vt_release(int s)
{
    (void)s;
    vt_active = 0;
    ioctl(0, VT_RELDISP, 1);        /* 同意切换走 */
}

static void vt_acquire(int s)
{
    (void)s;
    vt_active = 1;
    dirty = 1;                      /* 切回来时整屏重绘 */
}

static void vt_setup(void)
{
    struct vt_mode vtm;
    if (ioctl(0, VT_GETMODE, &vtm) != 0) return;
    signal(SIGUSR1, vt_release);
    signal(SIGUSR2, vt_acquire);
    vtm.mode = VT_PROCESS;
    vtm.waitv = 0;
    vtm.relsig = SIGUSR1;
    vtm.acqsig = SIGUSR2;
    vtm.frsig = 0;
    ioctl(0, VT_SETMODE, &vtm);
}

/* 读入一个逻辑按键: 返回 ASCII，或 1=↑ 2=↓, 0=无 */
static int pushback = -1;       /* ESC 序列误读时退回的字符 */
static int alt_modifier = 0;    /* 当前按键带 Alt 修饰（handle_key 检查后清零） */

static int getkey(void)
{
    unsigned char c;
    int k;
    key_raw = 0;
    alt_modifier = 0;
    if (pushback >= 0) { k = pushback; pushback = -1; key_raw = k; return k; }
    if (read(0, &c, 1) != 1) return 0;
    if (c == 27) {                              /* ESC 序列 */
        unsigned char e1 = 0, e2 = 0;
        struct pollfd pfd = { 0, POLLIN, 0 };
        /* tty 原始模式下 Alt+字母 = ESC(0x1b)+字母（meta 编码）。
         * 用 poll 等 5ms 区分：无后续 = 单独 ESC，有后续 = Alt 组合或 CSI。
         * Alt+F1/F2 等由内核 VT 切换截获，不会到 e1wm。 */
        if (poll(&pfd, 1, 5) <= 0) return 27;   /* 单独 ESC（关应用/返回） */
        read(0, &e1, 1);
        if (e1 == '[') {                        /* CSI 光标键序列 */
            struct pollfd p2 = { 0, POLLIN, 0 };
            if (poll(&p2, 1, 5) <= 0) return 27;
            read(0, &e2, 1);
            if (e2 == 'A') return 1;            /* ↑ */
            if (e2 == 'B') return 2;            /* ↓ */
            return 27;
        }
        /* ESC+字母 = Alt+字母：标记修饰，返回字母供 handle_key 分发快捷键 */
        alt_modifier = 1;
        key_raw = e1;
        return e1;
    }
    if (c == 'k' || c == 'j') {                 /* 应用内也收 'j'/'k'：key_raw 保留 */
        key_raw = c;
        return c == 'k' ? 1 : 2;
    }
    key_raw = c;
    return c;
}

/* 关闭当前应用（终端需杀掉 shell 子进程并关闭 pty） */
static void app_close(void)
{
    if (mode_mobile && kbd_visible) kbd_hide();
    if (setup_pid > 0) {                       /* 关向导窗口 = 取消安装 */
        kill(setup_pid, SIGKILL);
        waitpid(setup_pid, 0, 0);
        setup_pid = -1;
    }
    if (setup_fd >= 0) { close(setup_fd); setup_fd = -1; }
    if (open_app == APP_TERM && term_fd >= 0) {
        close(term_fd);
        term_fd = -1;
        if (term_pid > 0) {
            kill(term_pid, SIGKILL);
            waitpid(term_pid, 0, 0);
            term_pid = -1;
        }
    }
    open_app = -1;
    free_lines();
    scroll = 0;
    dirty = 1;
}

static void handle_key(int k)
{
    /* Alt+* 全局快捷键（与当前应用无关，最高优先级）。
     * 原 L 切换语言已迁至 Alt+L，避免与终端/浏览器输入冲突。
     * 注意：Alt+F1/F2 等由内核 VT 切换截获，不会到 e1wm。 */
    if (alt_modifier) {
        alt_modifier = 0;
        if (mode_mobile) return;   /* mobile 移除所有键盘快捷键：导航全靠手势/触摸 */
        switch (k) {
        case 'l': case 'L':                   /* Alt+L 切换语言 */
            lang = !lang;
            lang_save();
            if (open_app == APP_ABOUT || open_app == APP_SYS ||
                open_app == APP_PKG || open_app == APP_INSTALL) {
                run_app(open_app);
                scroll = 0;
            }
            dirty = 1;
            return;
        case 'k': case 'K':                   /* Alt+K 切换键盘布局 QWERTY↔Dvorak */
            kbd_toggle_layout();
            return;
        case 'q': case 'Q':                   /* Alt+Q 退出当前应用 */
            if (open_app >= 0) app_close();
            return;
        case 'i': case 'I':                   /* Alt+I 安装到磁盘（mobile） */
            if (open_app != APP_INSTALL) {
                open_app = APP_INSTALL;
                run_app(APP_INSTALL);
                dirty = 1;
            }
            return;
        case 'm': case 'M':                   /* Alt+M 切换拼音输入法 */
            ime_toggle();
            return;
        default:
            if (k >= '1' && k <= '9') {        /* Alt+1..N 直接打开应用 */
                int n = k - '1';
                if (n < APP_COUNT) {
                    menu_sel = n;
                    open_app = n;
                    run_app(n);
                    dirty = 1;
                }
                return;
            }
            return;
        }
    }
    /* IME 拦截：拼音模式下终端/浏览器的字母和标点经 IME 处理 */
    if (ime_active && (open_app == APP_TERM || open_app == APP_BROWSER)) {
        if (ime_key(k)) return;
    }
    if (open_app < 0) {                         /* 桌面 */
        switch (k) {
        case 1: if (menu_sel > 0) menu_sel--; dirty = 1; break;
        case 2: if (menu_sel < app_count_visible() - 1) menu_sel++; dirty = 1; break;
        case '\r': case '\n':
            open_app = menu_sel;
            run_app(open_app);
            dirty = 1;
            break;
        default:                                 /* 桌面无退出（init respawn） */
            break;
        }
    } else if (open_app == APP_TERM) {          /* 终端：全键直通，Esc 关闭 */
        if (k == 27) {
            if (mode_mobile && kbd_visible) kbd_hide();
            else app_close();
            return;
        }
        term_key(k);
    } else if (open_app == APP_BROWSER) {       /* 浏览器：地址输入，Esc 关闭 */
        if (k == 27) {
            if (mode_mobile && kbd_visible) kbd_hide();
            else app_close();
            return;
        }
        br_key(k);
    } else if (open_app == APP_INSTALL && !mode_mobile) {
        /* workstation 图形安装向导（Esc 关闭向导） */
        if (k == 27 && setup_page != 4) { app_close(); return; }
        setup_key(k);
    } else if (open_app == APP_FINDER || open_app == APP_SETTINGS ||
               (open_app >= APP_ACTIVITY && open_app <= APP_SHOT)) {
        /* 迷你应用集：自定义按键（滚动/输入/交互由各自处理） */
        app_key_custom(k);
    } else {                                    /* 静态窗口 */
        switch (k) {
        case 1: scroll--; dirty = 1; break;
        case 2: scroll++; dirty = 1; break;
        case 27: case 'q':
            app_close();
            break;
        }
    }
}

/* ================= 主流程 ================= */

static int fb_init(void)
{
    struct fbtype fb;
    int dbgfd2 = open("/dev/cuau0", O_WRONLY | O_NONBLOCK);
    char dbuf[128];

    fbfd = open(FB_DEV, O_RDWR);
    if (fbfd < 0) { perror("open " FB_DEV); return -1; }
    if (ioctl(fbfd, FBIOGTYPE, &fb)) { perror("fbtype"); return -1; }
    if (fb.fb_depth != 32) {
        fprintf(stderr, "e1wm: only 32bpp supported (got %d)\n", fb.fb_depth);
        return -1;
    }
    W = fb.fb_width; H = fb.fb_height;
    if (dbgfd2 >= 0) {
        snprintf(dbuf, sizeof dbuf,
                 "e1wm: fb_size=%d type=%d\r\n"
                 "e1wm: xres=%d yres=%d depth=%d\r\n",
                 fb.fb_size, fb.fb_type,
                 fb.fb_width, fb.fb_height, fb.fb_depth);
        write(dbgfd2, dbuf, strlen(dbuf));
        close(dbgfd2);
    }
    fbp = mmap(NULL, (size_t)fb.fb_size, PROT_READ | PROT_WRITE, MAP_SHARED,
               fbfd, 0);
    if (fbp == MAP_FAILED) { perror("mmap fb"); return -1; }
    return 0;
}

int main(void)
{
    time_t last_sec = 0;
    int dbgfd = open("/dev/cuau0", O_WRONLY | O_NONBLOCK);

    if (dbgfd >= 0) {
        const char *m = "e1wm: starting\r\n";
        write(dbgfd, m, strlen(m));
    }
    if (fb_init() != 0) {
        if (dbgfd >= 0) {
            const char *m = "e1wm: fb_init FAILED\r\n";
            write(dbgfd, m, strlen(m));
        }
        fprintf(stderr, "e1wm: framebuffer init failed\n");
        return 1;
    }
    if (dbgfd >= 0) {
        char m[64];
        snprintf(m, sizeof m, "e1wm: fb OK %dx%d %dbpp\r\n", W, H, 32);
        write(dbgfd, m, strlen(m));
    }
    lang_load();
    mode_load();
    kbd_load();
    read_variant();
    if (dbgfd >= 0) {
        char m[80];
        snprintf(m, sizeof m, "e1wm: variant=%s mobile=%d kbd=%s\r\n",
                 variant_str, mode_mobile, kbd_layout ? "dvorak" : "qwerty");
        write(dbgfd, m, strlen(m));
    }

    /* 隐藏 fbcon 文字层（仅本 VT），防止内核消息破坏图形画面 */
    ioctl(0, KDSETMODE, KD_GRAPHICS);
    tty_raw();
    vt_setup();

    if (dbgfd >= 0) {
        const char *m = "e1wm: KD_GRAPHICS set, entering main loop\r\n";
        write(dbgfd, m, strlen(m));
    }

    mx = W / 2; my = H / 2;

    for (;;) {
        fd_set fds;
        struct timeval tv = {0, 120000};
        int maxfd = 0, k;
        if (anim_kind) tv.tv_usec = ANIM_MS * 1000;  /* 动画期间快速唤醒推进帧 */

        FD_ZERO(&fds);
        FD_SET(0, &fds);
        for (k = 0; k < MAXEV; k++) {
            if (evfd[k] >= 0) {
                FD_SET(evfd[k], &fds);
                if (evfd[k] > maxfd) maxfd = evfd[k];
            }
        }
        if (term_fd >= 0) {
            FD_SET(term_fd, &fds);
            if (term_fd > maxfd) maxfd = term_fd;
        }
        if (setup_fd >= 0) {
            FD_SET(setup_fd, &fds);
            if (setup_fd > maxfd) maxfd = setup_fd;
        }
        if (select(maxfd + 1, &fds, NULL, NULL, &tv) > 0) {
            if (FD_ISSET(0, &fds)) {
                while ((k = getkey()) != 0) handle_key(k);
            }
            for (k = 0; k < MAXEV; k++) {
                if (evfd[k] >= 0 && FD_ISSET(evfd[k], &fds)) mouse_poll();
            }
            if (term_fd >= 0 && FD_ISSET(term_fd, &fds)) {
                char tbuf[512];
                int rn;
                while ((rn = read(term_fd, tbuf, sizeof tbuf)) > 0)
                    term_feed(tbuf, rn);
            }
            if (setup_fd >= 0 && FD_ISSET(setup_fd, &fds)) setup_poll();
        }
        mouse_try_open();               /* 鼠标设备就绪较晚（模块加载）→ 重试 */

        /* 窗口打开/关闭/最小化动画：按经过时间推进帧，播完执行落定动作 */
        if (anim_kind) {
            struct timeval anow;
            long ams;
            int tgt;
            gettimeofday(&anow, NULL);
            ams = (anow.tv_sec - anim_t0.tv_sec) * 1000 +
                  (anow.tv_usec - anim_t0.tv_usec) / 1000;
            tgt = (int)(ams / ANIM_MS);
            if (tgt > ANIM_FRAMES) tgt = ANIM_FRAMES;
            if (tgt > anim_frame) { anim_frame = tgt; dirty = 1; }
            if (ams >= ANIM_FRAMES * ANIM_MS) anim_finish();
        }

        if (!vt_active) { dirty = 0; continue; }    /* 本 VT 在后台，不动 fb */

        if (time(NULL) != last_sec) { last_sec = time(NULL); dirty = 1; }

        if (dirty) {
            cursor_on = 0;      /* 整屏重绘覆盖光标区域，需重画并重存背景 */
            redraw();
            draw_cursor();
            dirty = 0;
            if (dbgfd >= 0) {
                const char *m = "e1wm: first redraw done\r\n";
                write(dbgfd, m, strlen(m));
                close(dbgfd);   /* 只报告一次 */
                dbgfd = -1;
            }
        }
    }

    ioctl(0, KDSETMODE, KD_TEXT);
    tty_restore();
    close(fbfd);
    return 0;
}
