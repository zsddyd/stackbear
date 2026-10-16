// main/sb_nav.c —— 页面层级的唯一事实来源。
#include "sb_nav.h"

// 行数定义在 sb_nav.h(界面侧要用它做编译期断言),这里只引用。
#define NET_ROWS     SB_NAV_NET_ROWS
#define REC_ROWS     SB_NAV_REC_ROWS
#define DEV_ROWS     SB_NAV_DEV_ROWS
#define STORAGE_ROWS SB_NAV_STORAGE_ROWS

#define DEV_ROW_OTA     6   // "Check for update"
#define STORAGE_ROW_CACHE 0
#define STORAGE_ROW_SIZE  1

int sb_nav_rows(sb_nav_page_t page)
{
    switch (page) {
    case SB_NAV_MENU:    return SB_MENU_ROWS;
    case SB_NAV_NET:     return NET_ROWS;
    case SB_NAV_REC:     return REC_ROWS;
    case SB_NAV_DEV:     return DEV_ROWS;
    case SB_NAV_STORAGE: return STORAGE_ROWS;
    case SB_NAV_HELP:    return 0;   // Help 是整页滚动文字,没有可停的光标
    default:             return 0;   // HOME / PROVISION 不是列表页
    }
}

int sb_nav_clamp(sb_nav_page_t page, int cursor)
{
    const int rows = sb_nav_rows(page);
    if (rows <= 0) {
        return 0;
    }
    if (cursor < 0) {
        return 0;
    }
    if (cursor >= rows) {
        return rows - 1;
    }
    return cursor;
}

sb_nav_page_t sb_nav_parent(sb_nav_page_t page)
{
    switch (page) {
    case SB_NAV_MENU:
        return SB_NAV_HOME;          // 菜单上一层是主页
    case SB_NAV_NET:
    case SB_NAV_REC:
    case SB_NAV_DEV:
    case SB_NAV_STORAGE:
    case SB_NAV_HELP:
        return SB_NAV_MENU;          // 子页上一层是菜单
    default:
        return page;                 // HOME / PROVISION 无处可退
    }
}

sb_nav_page_t sb_nav_menu_target(int row)
{
    switch (row) {
    case SB_MENU_RECORDER: return SB_NAV_REC;
    case SB_MENU_NETWORK: return SB_NAV_NET;
    case SB_MENU_STORAGE: return SB_NAV_STORAGE;
    case SB_MENU_DEVICE: return SB_NAV_DEV;
    case SB_MENU_HELP: return SB_NAV_HELP;
    default: return SB_NAV_MENU;
    }
}

bool sb_nav_row_actionable(sb_nav_page_t page, int row)
{
    if (row < 0 || row >= sb_nav_rows(page)) {
        return false;
    }
    switch (page) {
    case SB_NAV_MENU:
        return true;                                   // 每行都进子页
    case SB_NAV_DEV:
        return row == DEV_ROW_OTA;                     // 只有 Check for update
    case SB_NAV_STORAGE:
        return row == STORAGE_ROW_CACHE || row == STORAGE_ROW_SIZE;
    default:
        return false;                                  // NET / REC 纯展示
    }
}
