// 列表页轻点判定的 host 测试:不依赖 SDK,直接验证 sb_key_nav.h 的纯逻辑。
//
// 背景:按键组件把间隔小于短按窗口(本板 180ms)的连按合并成一次多击,并只在多击
// 结束时按次数派发 —— 3 次及以上什么都不派发。列表页若只认 SINGLE_CLICK,连按就
// 完全收不到事件、光标不动。改用每次按压都会产生的 PRESS/RELEASE 后,连按的每一次
// 都能算出一次轻点。
#include <assert.h>
#include <stdio.h>

#include "sb_key_nav.h"

// 一次完整的"按下后抬起",返回它是否算轻点。
static bool tap(sb_key_nav_t *k)
{
    sb_key_nav_press(k);
    return sb_key_nav_release(k);
}

// 一次"按下 -> 长按 -> 抬起",返回抬起是否算轻点。
static bool long_press_then_release(sb_key_nav_t *k)
{
    sb_key_nav_press(k);
    sb_key_nav_long(k);
    return sb_key_nav_release(k);
}

int main(void)
{
    sb_key_nav_t k = {0};

    // 孤立单按算一次轻点。
    assert(tap(&k) == true);

    // 快速连按:每一次都必须各算一次轻点(这正是列表页连续翻动的场景)。
    for (int i = 0; i < 5; i++) {
        assert(tap(&k) == true);
    }

    // 长按抬起不算轻点,否则"长按下返回"会先挪一格光标再返回。
    assert(long_press_then_release(&k) == false);

    // 长按之后的单按仍是正常的轻点(状态必须被长按正确清掉)。
    assert(tap(&k) == true);

    // 没有按下记录的抬起不产生幽灵移动。
    sb_key_nav_t fresh = {0};
    assert(sb_key_nav_release(&fresh) == false);

    // 抬起两次只算一次:第二次没有按下记录。
    assert(tap(&k) == true);
    assert(sb_key_nav_release(&k) == false);

    // 长按事件若在没有按下记录时到达,不得把后续抬起误判成长按。
    sb_key_nav_t stray = {0};
    sb_key_nav_long(&stray);
    assert(tap(&stray) == true);

    // 连按中夹一次长按:只有那一次被排除。
    assert(tap(&k) == true);
    assert(long_press_then_release(&k) == false);
    assert(tap(&k) == true);

    puts("test_sb_key_nav: PASS");
    return 0;
}
