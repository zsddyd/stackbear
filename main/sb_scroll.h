// main/sb_scroll.h —— 列表/长内容的滚动窗口计算(纯逻辑,不依赖 SDK/LVGL,便于 host 测试)。
//
// 为什么单独成文件:"当前该显示哪一段""光标移出可视区后窗口怎么跟"是纯算术,却最容易
// 写错边界(顶部越界、底部越界、可视行数为 0、条目少于可视行数)。把它从绘制代码里
// 剥出来,就能用 host 测试穷举,而不是靠刷机点按键去试。
//
// 约定:scroll 是"可视区第一项的下标",不是像素偏移。像素由调用方按行高换算,
// 这样逻辑与具体行高/字体无关,换字号也不用改这里。
#pragma once

typedef struct {
    int count;    // 总条目数
    int visible;  // 一屏能放下的条目数(<=0 视为"一屏放得下全部")
    int cursor;   // 当前选中项下标
    int scroll;   // 可视区第一项下标
} sb_scroll_t;

// scroll 的最大值:条目少于可视区时为 0(内容比屏幕短,不滚)。
static inline int sb_scroll_max(int count, int visible)
{
    if (count <= 0 || visible <= 0) {
        return 0;
    }
    const int extra = count - visible;
    return extra > 0 ? extra : 0;
}

// 把 cursor 夹进 [0, count-1],并调整 scroll 让 cursor 落在可视区内。
// 空列表(cursor=0, scroll=0)也安全。每次移动后调用它即可,不必自己算边界。
static inline void sb_scroll_follow(sb_scroll_t *s)
{
    if (s->count <= 0) {
        s->cursor = 0;
        s->scroll = 0;
        return;
    }
    if (s->cursor < 0) {
        s->cursor = 0;
    }
    if (s->cursor >= s->count) {
        s->cursor = s->count - 1;
    }

    // visible 非法时按"全部可见"处理:不滚动,永远显示完整内容。
    const int v = (s->visible > 0 && s->visible < s->count) ? s->visible : s->count;
    const int maxs = sb_scroll_max(s->count, v);

    if (s->scroll < 0) {
        s->scroll = 0;
    }
    if (s->scroll > maxs) {
        s->scroll = maxs;
    }
    // 光标跑到可视区上/下方时,窗口刚好跟到能看见它为止(移动一格而不是居中),
    // 这样列表滚动的幅度可预期,也不会出现"光标跳了一屏"的突兀感。
    if (s->cursor < s->scroll) {
        s->scroll = s->cursor;
    } else if (s->cursor >= s->scroll + v) {
        s->scroll = s->cursor - v + 1;
    }
    if (s->scroll > maxs) {
        s->scroll = maxs;
    }
    if (s->scroll < 0) {
        s->scroll = 0;
    }
}

// 移动光标并让窗口跟随。返回新的 cursor。
static inline int sb_scroll_move(sb_scroll_t *s, int delta)
{
    s->cursor += delta;
    sb_scroll_follow(s);
    return s->cursor;
}

// 光标落在可视区的第几个槽(0 起)。界面用它换算高亮的 y 坐标。
//
// 单独做成函数而不是在两处各写一遍 cursor - scroll:界面里"建页时把高亮放到哪儿"
// 与"移动后把高亮动画到哪儿"必须是**同一个**算式。两处各写一遍时曾经漂移过 ——
// 建页时写死了第 0 个槽,而光标是跨页面释放/重建保留的,于是重建后高亮停在第一行、
// 光标却在最后一行:再按 DOWN 因为光标已被夹在底部而毫无反应,得先按 UP 才正常。
//
// 越界的光标返回夹取后的槽,不会返回负数 —— 界面直接拿它算 y,不必再防一次。
static inline int sb_scroll_slot(const sb_scroll_t *s)
{
    if (s->count <= 0) {
        return 0;
    }
    int cursor = s->cursor;
    if (cursor < 0) {
        cursor = 0;
    }
    if (cursor >= s->count) {
        cursor = s->count - 1;
    }
    int slot = cursor - s->scroll;
    if (slot < 0) {
        slot = 0;
    }
    return slot;
}
