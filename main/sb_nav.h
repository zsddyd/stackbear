// main/sb_nav.h —— 页面层级与光标约束(纯逻辑,不依赖 LVGL/ESP-IDF,可主机测试)。
//
// 为什么要单独成文件:界面的层级关系("这一页的上一级是谁""这一行按 OK 去
// 哪里""光标能停在第几行")是纯数据,却最容易写错 —— 之前把状态页拆成子页时,
// 返回路径和可操作行的判断就出过偏差。把它从绘制代码里剥出来,就能用主机测试
// 穷举验证,而不是靠刷机点按键。
#pragma once

#include <stdbool.h>

typedef enum {
    SB_NAV_HOME = 0,
    SB_NAV_MENU,        // SETTINGS(分组菜单)
    SB_NAV_NET,         // NETWORK
    SB_NAV_REC,         // RECORDER
    SB_NAV_DEV,         // DEVICE
    SB_NAV_STORAGE,     // STORAGE
    SB_NAV_DISPLAY,     // DISPLAY(亮度与空闲调暗)
    SB_NAV_HELP,        // HELP(整页滚动文字,没有光标)
    SB_NAV_OTA,         // OTA(升级进度与结果;DEVICE 的 Upgrade 行进入)
    SB_NAV_PROVISION,   // 配网页(不参与常规导航层级)
    SB_NAV_PAGE_COUNT
} sb_nav_page_t;

// 分组菜单的行,顺序必须与 sb_ui.c 里的菜单文案一致。
// 顺序按"最常看的排在前面"定:录音状态 → 网络 → 存储 → 设备 → 显示 → 帮助。
typedef enum {
    SB_MENU_RECORDER = 0,
    SB_MENU_NETWORK,
    SB_MENU_STORAGE,
    SB_MENU_DEVICE,
    SB_MENU_DISPLAY,
    SB_MENU_HELP,
    SB_MENU_ROWS
} sb_menu_row_t;

// 各列表页的行数。做成宏(而不是只用函数返回)是为了让界面侧能用编译期断言
// 把"实际画了几行"和这里钉在一起 —— 两处一旦漂移,光标就会停到不存在的行。
#define SB_NAV_NET_ROWS     5
#define SB_NAV_REC_ROWS     4
#define SB_NAV_DEV_ROWS     7
#define SB_NAV_STORAGE_ROWS 5
#define SB_NAV_DISPLAY_ROWS 2

// 该页有几行可停光标。非列表页返回 0。
int sb_nav_rows(sb_nav_page_t page);

// 把光标夹到 [0, rows-1]。空页返回 0。
int sb_nav_clamp(sb_nav_page_t page, int cursor);

// 长按返回键后应显示的页。HOME/PROVISION 返回自身(无处可退)。
sb_nav_page_t sb_nav_parent(sb_nav_page_t page);

// 分组菜单某一行对应的子页。越界返回 SB_NAV_MENU。
sb_nav_page_t sb_nav_menu_target(int row);

// 该页该行按 OK 是否有动作。true 表示这行可操作(进子页/切换/触发)。
bool sb_nav_row_actionable(sb_nav_page_t page, int row);
