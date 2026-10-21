// main/sb_display_cfg.h —— 背光设置与空闲调暗的纯逻辑(不依赖 ESP-IDF/LVGL,便于 host 测试)。
//
// 为什么单独成文件:亮度步进、取值夹取、"空闲多久调暗"的档位与循环,都是纯算术,
// 却有三处容易写错的地方 —— 步进后越界、从 NVS 读回一个非法档位、循环到末尾绕回。
// 把它们从按键/绘制代码里剥出来,就能在主机上把边界穷举一遍,而不是靠刷机点按键试。
#pragma once

#include <stdbool.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// 亮度:用户可以设置的常态亮度。
//
// 下限刻意不是 0:0% 与"设备已关机"在视觉上完全一样,用户会把屏幕调到看不见后
// 再也找不到把它调回来的按键。要省电、要"暗下来"由下面的空闲调暗负责。
#define SB_BRIGHT_MIN   10
#define SB_BRIGHT_MAX   100
// 步进 5%:三键小设备上从 10% 走到 100% 只要按 18 次;步进 1% 要按 90 次,
// 实际不可用。
#define SB_BRIGHT_STEP  5

#define SB_BRIGHT_DEFAULT SB_BRIGHT_MAX

// 把任意整数夹到合法亮度区间。用于读取 NVS 与接收按键结果。
static inline int sb_bright_clamp(int percent)
{
    if (percent < SB_BRIGHT_MIN) {
        return SB_BRIGHT_MIN;
    }
    if (percent > SB_BRIGHT_MAX) {
        return SB_BRIGHT_MAX;
    }
    return percent;
}

// delta < 0 变暗,> 0 变亮,0 不变。返回夹取后的新亮度。
static inline int sb_bright_step(int percent, int steps)
{
    return sb_bright_clamp(percent + steps * SB_BRIGHT_STEP);
}

// ---------------------------------------------------------------------------
// 空闲调暗档位(秒)。0 = 关闭该功能。
//
// 用固定档位而不是秒数输入:三键设备上没法输入数字,而"每次按一下加 10 秒"走过
// 0..3600 要走 360 次。档位表同时也是 sb_config 校验 NVS 取值的唯一依据 ——
// 存储里出现表外的值(旧固件、手工改过的 NVS)一律按最近档位纠正。
// ---------------------------------------------------------------------------
static const unsigned short SB_DIM_PRESETS[] = {0, 30, 60, 180, 300};
#define SB_DIM_PRESET_COUNT ((int)(sizeof(SB_DIM_PRESETS) / sizeof(SB_DIM_PRESETS[0])))
#define SB_DIM_DEFAULT_INDEX 2   // 60 秒

// 空闲调暗后的亮度(%)。刻意略高于 SB_BRIGHT_MIN:调暗的目的是省电和"别在黑暗里
// 刺眼",不是让屏幕消失 —— 用户瞥一眼仍要能看清当前状态和电量。
#define SB_DIM_LEVEL 15

// 最接近 seconds 的档位下标。表外值(含 0 未列出的情况)也能安全落位。
static inline int sb_dim_preset_index(unsigned short seconds)
{
    int best = 0;
    unsigned short best_diff = 0xFFFFu;
    for (int i = 0; i < SB_DIM_PRESET_COUNT; i++) {
        const unsigned short v = SB_DIM_PRESETS[i];
        const unsigned short diff = (v > seconds) ? (unsigned short)(v - seconds)
                                                  : (unsigned short)(seconds - v);
        if (diff < best_diff) {
            best_diff = diff;
            best = i;
        }
    }
    return best;
}

// OK 在档位行上的动作:循环到下一档,越界绕回。
static inline int sb_dim_next_index(int index)
{
    if (index < 0 || index >= SB_DIM_PRESET_COUNT) {
        return SB_DIM_DEFAULT_INDEX;
    }
    return (index + 1) % SB_DIM_PRESET_COUNT;
}

// 自 last_ms 起是否已经空闲够久,该调暗了。timeout_s == 0 表示关闭该功能。
//
// 用"无符号差值"而不是比较两个绝对值:lv_tick_get() 的 uint32 约 49.7 天回绕,
// 直接比大小会在回绕后瞬间判成"空闲了 49 天",屏幕毫无预兆地变暗。
static inline bool sb_dim_elapsed(uint32_t now_ms, uint32_t last_ms, uint16_t timeout_s)
{
    if (timeout_s == 0) {
        return false;
    }
    return (uint32_t)(now_ms - last_ms) >= (uint32_t)timeout_s * 1000u;
}
