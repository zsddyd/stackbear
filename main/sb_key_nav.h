// main/sb_key_nav.h —— 列表页把"按下 / 长按 / 抬起"折算成"一次轻点"的判定
// (纯逻辑,不依赖 SDK/LVGL,便于 host 测试)。
//
// 为什么需要它:按键组件会把间隔小于短按窗口(本板 180ms)的连按合并成一次"多击",
// 并且只在多击结束时按次数派发 —— 1 次给 SINGLE_CLICK,2 次给 DOUBLE_CLICK,
// 3 次及以上什么都不派发。列表页原来只认 SINGLE_CLICK,于是"连按上下"根本收不到
// 事件,光标不动,只有孤立的单按才移动。
//
// 这里改用每次物理按压都会产生的 PRESS/RELEASE:抬起时若这次按压没有变成长按,
// 就算一次"轻点"。长按有各自的全局动作(长按下 = 返回、长按上 = 录音开关),所以
// 长按抬起时不再算轻点 —— 否则"长按下返回"会先把光标挪一格再返回。
//
// 三颗键共用同一个 ADC 引脚,同一时刻只可能有一颗键处于按下状态,因此单个结构体
// 就够,不需要按按键分别记录。
#pragma once

#include <stdbool.h>

typedef struct {
    bool down;       // 当前是否正按着
    bool long_seen;  // 这次按压是否已经达到长按门限
} sb_key_nav_t;

static inline void sb_key_nav_press(sb_key_nav_t *k)
{
    k->down = true;
    k->long_seen = false;
}

static inline void sb_key_nav_long(sb_key_nav_t *k)
{
    if (k->down) {
        k->long_seen = true;
    }
}

// 抬起:返回 true 表示这次按压应算作一次轻点(列表页据此移动光标)。
// 没有按下记录(例如在配网页里漏掉了 PRESS)时返回 false,不产生幽灵移动。
static inline bool sb_key_nav_release(sb_key_nav_t *k)
{
    if (!k->down) {
        return false;
    }
    k->down = false;
    return !k->long_seen;
}
