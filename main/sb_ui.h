// main/sb_ui.h —— 重新设计的应用界面(全英文)。不复用基线 demo 菜单/像素主题。
#pragma once

#include "lvgl.h"
#include <stdbool.h>

typedef enum {
    SB_SCREEN_HOME = 0,
    SB_SCREEN_STATS,
    SB_SCREEN_SETTINGS,
    SB_SCREEN_PROVISION,
} sb_screen_t;

// 在当前 LVGL 任务/已持锁上下文里创建界面并显示首页。
void sb_ui_init(void);

// 切换页面(需在 LVGL 上下文或持 bsp_lvgl_lock 时调用)。
void sb_ui_show(sb_screen_t screen);

// 设置配网页展示的热点名(进入配网模式时调用)。
void sb_ui_set_ap_name(const char *name);

// 设置页交互(需在 LVGL 上下文或持 bsp_lvgl_lock 时调用)。
// move: 在 1..SB_CACHE_MAX_LIMIT_MB 之间调整容量;toggle: 开关离线队列。
void sb_ui_settings_move(int delta);
void sb_ui_settings_toggle(void);
