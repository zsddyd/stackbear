// tests/test_sb_display_cfg.c —— 背光设置与空闲调暗纯逻辑的主机测试。
//
// 这里钉住三件容易写错的事:亮度步进后越界(按到底还要能停住,不能绕回)、
// NVS 里读回的非法档位要被就近纠正、以及 lv_tick 回绕时不能瞬间判成"空闲了 49 天"。
#include <stdio.h>

#include "sb_display_cfg.h"

static int g_failures;

#define CHECK(cond, ...)                                                   \
    do {                                                                   \
        if (!(cond)) {                                                     \
            printf("FAIL %s:%d: %s", __FILE__, __LINE__, #cond);           \
            printf(" " __VA_ARGS__);                                       \
            printf("\n");                                                  \
            g_failures++;                                                  \
        }                                                                  \
    } while (0)

static void test_clamp(void)
{
    CHECK(sb_bright_clamp(-5) == SB_BRIGHT_MIN, "");
    CHECK(sb_bright_clamp(0) == SB_BRIGHT_MIN, "0 must not be reachable");
    CHECK(sb_bright_clamp(SB_BRIGHT_MIN) == SB_BRIGHT_MIN, "");
    CHECK(sb_bright_clamp(50) == 50, "");
    CHECK(sb_bright_clamp(SB_BRIGHT_MAX) == SB_BRIGHT_MAX, "");
    CHECK(sb_bright_clamp(255) == SB_BRIGHT_MAX, "");
}

static void test_step(void)
{
    CHECK(sb_bright_step(50, 1) == 50 + SB_BRIGHT_STEP, "");
    CHECK(sb_bright_step(50, -1) == 50 - SB_BRIGHT_STEP, "");
    CHECK(sb_bright_step(50, 0) == 50, "zero steps is a no-op");
    // 步进到两端必须停住,不能绕回另一端(绕回会让用户"越按越暗,忽然全亮")。
    CHECK(sb_bright_step(SB_BRIGHT_MAX, 1) == SB_BRIGHT_MAX, "");
    CHECK(sb_bright_step(SB_BRIGHT_MIN, -1) == SB_BRIGHT_MIN, "");
    CHECK(sb_bright_step(SB_BRIGHT_MIN + 1, -1) == SB_BRIGHT_MIN, "clamps mid-step");
    CHECK(sb_bright_step(SB_BRIGHT_MAX - 1, 1) == SB_BRIGHT_MAX, "clamps mid-step");
    // 大步长(连按被合并成一次调用)同样夹取。
    CHECK(sb_bright_step(50, 999) == SB_BRIGHT_MAX, "");
    CHECK(sb_bright_step(50, -999) == SB_BRIGHT_MIN, "");
}

static void test_preset_index(void)
{
    // 表内值必须精确命中自己那一档(否则读回 NVS 会把用户设的档位悄悄改掉)。
    for (int i = 0; i < SB_DIM_PRESET_COUNT; i++) {
        CHECK(sb_dim_preset_index(SB_DIM_PRESETS[i]) == i, "preset %d", i);
    }
    // 表外值就近落位,且不能越界。
    CHECK(sb_dim_preset_index(0) == 0, "0 is off");
    CHECK(sb_dim_preset_index(45) >= 0 && sb_dim_preset_index(45) < SB_DIM_PRESET_COUNT, "");
    CHECK(sb_dim_preset_index(60000) == SB_DIM_PRESET_COUNT - 1, "far above clamps to last");
    // 默认档位必须是表里的合法下标。
    CHECK(SB_DIM_DEFAULT_INDEX >= 0 && SB_DIM_DEFAULT_INDEX < SB_DIM_PRESET_COUNT, "");
}

static void test_next_index(void)
{
    // 循环到末尾绕回第一档,越界下标回默认档(而不是 -1 或越界读表)。
    CHECK(sb_dim_next_index(0) == 1, "");
    CHECK(sb_dim_next_index(SB_DIM_PRESET_COUNT - 1) == 0, "wraps to the first preset");
    CHECK(sb_dim_next_index(-1) == SB_DIM_DEFAULT_INDEX, "");
    CHECK(sb_dim_next_index(SB_DIM_PRESET_COUNT) == SB_DIM_DEFAULT_INDEX, "");
    // 走完一整圈必须回到起点(否则"循环"其实是单调漂移)。
    int idx = 0;
    for (int i = 0; i < SB_DIM_PRESET_COUNT; i++) {
        idx = sb_dim_next_index(idx);
    }
    CHECK(idx == 0, "a full cycle returns to the start");
}

static void test_dim_elapsed(void)
{
    // 关闭档(0)永远不调暗,哪怕空闲了很久。
    CHECK(!sb_dim_elapsed(1000000, 0, 0), "timeout 0 disables dimming");
    // 未到超时不算,到了算。
    CHECK(!sb_dim_elapsed(59 * 1000, 0, 60), "");
    CHECK(sb_dim_elapsed(60 * 1000, 0, 60), "");
    CHECK(sb_dim_elapsed(61 * 1000, 0, 60), "");
    // lv_tick 是 uint32 毫秒,约 49.7 天回绕。用无符号差值算,回绕后不能瞬间
    // 判成"空闲了 49 天"而突然变暗。
    const uint32_t near_wrap = 0xFFFFFFFFu - 1000u;   // 距回绕还有 1 秒
    CHECK(!sb_dim_elapsed(0x00000000u, near_wrap, 60), "wrap must not fake a long idle");
    CHECK(!sb_dim_elapsed(30 * 1000, near_wrap, 60), "31s idle across wrap is under 60s");
    CHECK(sb_dim_elapsed(61 * 1000, near_wrap, 60), "crossing the wrap still measures elapsed");
}

int main(void)
{
    test_clamp();
    test_step();
    test_preset_index();
    test_next_index();
    test_dim_elapsed();
    if (g_failures == 0) {
        printf("test_sb_display_cfg: PASS\n");
        return 0;
    }
    printf("test_sb_display_cfg: %d failure(s)\n", g_failures);
    return 1;
}
