// 滚动窗口逻辑的 host 测试:不依赖 SDK,直接验证 sb_scroll.h。
//
// 边界是这类代码唯一的难点:顶部越界、底部越界、条目比可视区少、可视区为 0、
// 空列表。这里按组合穷举,而不是靠刷机点按键去试。
#include <assert.h>
#include <stdio.h>

#include "sb_scroll.h"

// 走一遍"从 0 一直向下再一直向上",返回过程中是否始终满足不变量。
static void sweep_down_up(int count, int visible)
{
    sb_scroll_t s = { .count = count, .visible = visible, .cursor = 0, .scroll = 0 };
    const int v = (visible > 0 && visible < count) ? visible : count;

    for (int step = 0; step < count * 2 + 2; step++) {
        sb_scroll_move(&s, +1);
        // 不变量:光标合法、窗口不越界、光标必须可见。
        assert(s.cursor >= 0 && s.cursor < count);
        assert(s.scroll >= 0 && s.scroll <= sb_scroll_max(count, v));
        assert(s.cursor >= s.scroll && s.cursor < s.scroll + v);
    }
    assert(s.cursor == count - 1);   // 一直向下必须停在最后一项

    for (int step = 0; step < count * 2 + 2; step++) {
        sb_scroll_move(&s, -1);
        assert(s.cursor >= 0 && s.cursor < count);
        assert(s.scroll >= 0 && s.scroll <= sb_scroll_max(count, v));
        assert(s.cursor >= s.scroll && s.cursor < s.scroll + v);
    }
    assert(s.cursor == 0);
    assert(s.scroll == 0);           // 回到顶部后窗口必须回到 0
}

int main(void)
{
    // 条目多于可视区:必须滚动,且首尾都不能越界。
    for (int count = 1; count <= 40; count++) {
        for (int visible = 1; visible <= 10; visible++) {
            sweep_down_up(count, visible);
        }
    }

    // 条目少于或等于可视区:不滚动,窗口恒为 0。
    for (int count = 1; count <= 6; count++) {
        sb_scroll_t s = { .count = count, .visible = 10, .cursor = 0, .scroll = 0 };
        for (int i = 0; i < count * 3; i++) {
            sb_scroll_move(&s, +1);
            assert(s.scroll == 0);           // 内容比屏幕短,不该滚
            assert(s.cursor <= count - 1);
        }
    }

    // 空列表:任何操作都安全,不产生越界。
    sb_scroll_t empty = { .count = 0, .visible = 5, .cursor = 0, .scroll = 0 };
    sb_scroll_move(&empty, +1);
    sb_scroll_move(&empty, -1);
    assert(empty.cursor == 0 && empty.scroll == 0);
    assert(sb_scroll_max(0, 5) == 0);

    // 可视区非法(0 或负数):按"全部可见"处理,不崩、不滚。
    for (int bad = -3; bad <= 0; bad++) {
        sb_scroll_t s = { .count = 12, .visible = bad, .cursor = 0, .scroll = 0 };
        sb_scroll_move(&s, +1);
        assert(s.scroll == 0);
        assert(s.cursor == 1);
    }

    // 移动一格而不是跳一屏:光标刚出下边界时,scroll 只前进 1。
    sb_scroll_t s = { .count = 20, .visible = 5, .cursor = 4, .scroll = 0 };
    sb_scroll_move(&s, +1);              // cursor=5,刚出可见区
    assert(s.cursor == 5 && s.scroll == 1);

    // 顶部回退同理:光标刚出上边界时,scroll 只后退 1。
    s = (sb_scroll_t){ .count = 20, .visible = 5, .cursor = 5, .scroll = 5 };
    sb_scroll_move(&s, -1);
    assert(s.cursor == 4 && s.scroll == 4);

    // 直接跳到末尾(模拟 scroll 被外部改坏后 follow 修正):窗口必须贴到底。
    s = (sb_scroll_t){ .count = 20, .visible = 5, .cursor = 19, .scroll = 0 };
    sb_scroll_follow(&s);
    assert(s.cursor == 19 && s.scroll == 15);

    // scroll 被外部设成越界值时,follow 必须把它夹回来。
    s = (sb_scroll_t){ .count = 20, .visible = 5, .cursor = 3, .scroll = 999 };
    sb_scroll_follow(&s);
    assert(s.scroll <= sb_scroll_max(20, 5));
    s = (sb_scroll_t){ .count = 20, .visible = 5, .cursor = 3, .scroll = -7 };
    sb_scroll_follow(&s);
    assert(s.scroll == 0);

    puts("test_sb_scroll: PASS");
    return 0;
}
