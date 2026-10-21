// main/sb_ui.h —— 应用界面(全英文)。不复用基线 demo 菜单/像素主题。
//
// 页面层级(两层,任何页面长按 DOWN 逐级返回,长按 OK 全局进配网):
//
//   HOME            大状态卡(聚合结论)+ SEG/SENT/DROP
//                   + 两行可选动作:Stop recording(红)/ Settings
//    └ SETTINGS     (原来的 STATUS)分组菜单,每行右侧带一行摘要
//        ├ RECORDER  Recording / Segments / Sent / Dropped
//        ├ NETWORK   Wi-Fi / SSID / Device IP / Server IP / Uplink
//        ├ STORAGE   Cache / Size / Used / Pending / Dropped
//        ├ DEVICE    Device ID / Firmware / Slot / Commit / Branch / Built / update
//        ├ DISPLAY   Brightness / Dim after(OK 选中亮度后可上下调,长按下取消)
//        └ HELP      只讲长按 UP 与长按 OK 两条(其余靠高亮自解释)
//
// 设计取舍:HOME 独占那个"聚合状态"(它融合了凭据/链路/暂停/上行四层判断),
// 子页只讲自己那一层,避免同一结论在多处重复;NETWORK 里 Wi-Fi 与 Uplink 分列,
// 因为"WiFi 通"不等于"能上传"。
//
// 列表页是**虚拟窗口**:页面声明条目数,只创建一屏能放下的行槽,滚动时重指——
// 所以条目数不增加 LVGL 对象,加内容只会变成可滚动(见 sb_scroll.h)。
#pragma once

#include "lvgl.h"
#include <stdbool.h>

typedef enum {
    SB_SCREEN_HOME = 0,
    SB_SCREEN_MENU,
    SB_SCREEN_NET,
    SB_SCREEN_REC,
    SB_SCREEN_DEV,
    SB_SCREEN_STORAGE,
    SB_SCREEN_DISPLAY,
    SB_SCREEN_HELP,
    SB_SCREEN_PROVISION,
} sb_screen_t;

// 在 LVGL 任务/已持锁上下文里创建界面并显示首页。
void sb_ui_init(void);

// 切换页面(需在 LVGL 上下文或持 bsp_lvgl_lock 时调用)。
void sb_ui_show(sb_screen_t screen);
sb_screen_t sb_ui_current(void);

// 设置配网页展示的热点名(进入配网模式时调用)。
void sb_ui_set_ap_name(const char *name);

// HOME 页当前选中的动作。OK 在 HOME 上的含义由它决定:
// 起点 = 开始/停止录音(会话级), - 设置 = 进入 SETTINGS。
typedef enum {
    SB_HOME_STARTSTOP = 0,   // "Start recording" / "Stop recording"
    SB_HOME_SETTINGS,        // "Settings"
} sb_home_action_t;

sb_home_action_t sb_ui_home_action(void);

// 列表页导航。由 main 的按键处理转发,光标与页面状态由本模块自己维护,
// 上层不需要知道"第几行是哪一项"。
void sb_ui_move(int delta);     // UP/DOWN 单击
void sb_ui_activate(void);      // OK 单击:进子页 / 切换 / 触发
void sb_ui_back(void);          // 长按 DOWN:返回上一级

// 当前行是否有动作(供界面提示与按键判断;无动作的行 OK 不响应)。
bool sb_ui_row_actionable(void);

// 上报"用户动了按键"。空闲调暗据此计算空闲时长,并立刻把背光恢复到用户设置的亮度。
// 任何按键事件(含长按、配网页里的按键)都应调用它。
void sb_ui_note_input(void);
