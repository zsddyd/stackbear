// main/sb_ui.c —— 应用界面。设计上刻意区别于基线 demo 的"像素菜单":
// 深色底 + 圆角卡片 + 顶部状态条 + 单一主状态,电量固定在右上角。
#include "sb_ui.h"

#include <stdio.h>
#include <string.h>

#include "bsp_battery.h"
#include "esp_timer.h"
#include "sb_config.h"
#include "sb_net.h"
#include "sb_cache.h"
#include "sb_recorder.h"
#include "sb_uplink.h"

#define C_BG      0x0E1216
#define C_CARD    0x1B222B
#define C_TEXT    0xEDF2F7
#define C_MUTED   0x8A97A6
#define C_OK      0x35C46A
#define C_WARN    0xE8A33D
#define C_BAD     0xE0524A
#define C_ACCENT  0x3B82F6

static lv_obj_t *s_home_scr;
static lv_obj_t *s_stats_scr;
static lv_obj_t *s_prov_scr;
static lv_obj_t *s_set_scr;
static lv_obj_t *s_set_toggle;
static lv_obj_t *s_set_sizes[3];
static lv_obj_t *s_set_status;
static lv_obj_t *s_home_state;
static lv_obj_t *s_home_sub;
static lv_obj_t *s_home_batt;
static lv_obj_t *s_home_seg;
static lv_obj_t *s_home_sent;
static lv_obj_t *s_home_drop;
static lv_obj_t *s_stats_rows[6];
static lv_obj_t *s_prov_ap;

static void refresh_settings(void);
static lv_timer_t *s_timer;
static char s_ap_name[40] = "";

static lv_obj_t *card(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_radius(o, 12, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    return o;
}

static lv_obj_t *label(lv_obj_t *parent, const char *text, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    return l;
}

static lv_obj_t *base_screen(const char *title)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, lv_color_hex(C_BG), 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    label(scr, title, &lv_font_montserrat_14, C_MUTED);
    lv_obj_t *t = lv_obj_get_child(scr, 0);
    lv_obj_set_pos(t, 12, 10);
    return scr;
}

static void build_home(void)
{
    s_home_scr = base_screen("STACKBEAR RECORDER");
    s_home_batt = label(s_home_scr, "--%", &lv_font_montserrat_14, C_TEXT);
    lv_obj_align(s_home_batt, LV_ALIGN_TOP_RIGHT, -12, 10);

    // 主页只放"现在怎么样":状态 + 三个计数。IP/服务器等细节都在 STATUS 页,
    // 不在这里重复,免得挤掉主信息。
    lv_obj_t *c = card(s_home_scr, 12, 44, 216, 132, C_CARD);
    s_home_state = label(c, "STARTING", &lv_font_montserrat_20, C_TEXT);
    lv_obj_align(s_home_state, LV_ALIGN_TOP_MID, 0, 42);
    s_home_sub = label(c, "", &lv_font_montserrat_14, C_MUTED);
    lv_label_set_long_mode(s_home_sub, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_home_sub, 196);
    lv_obj_set_style_text_align(s_home_sub, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_home_sub, LV_ALIGN_BOTTOM_MID, 0, -14);

    // 三列计数卡:每张卡都窄,所以标题用短名(SEG/SENT/DROP)。
    static const char *caps[3] = {"SEG", "SENT", "DROP"};
    lv_obj_t **vals[3] = {&s_home_seg, &s_home_sent, &s_home_drop};
    for (int i = 0; i < 3; i++) {
        lv_obj_t *k = card(s_home_scr, 12 + i * 74, 194, 68, 62, C_CARD);
        label(k, caps[i], &lv_font_montserrat_14, C_MUTED);
        lv_obj_align(lv_obj_get_child(k, 0), LV_ALIGN_TOP_MID, 0, 8);
        *vals[i] = label(k, "0", &lv_font_montserrat_14, C_TEXT);
        lv_obj_align(*vals[i], LV_ALIGN_BOTTOM_MID, 0, -10);
    }
}

static void build_stats(void)
{
    // Wi-Fi 行同时给出 SSID;地址拆成 DEVICE IP / SERVER IP,不再单列 Server。
    // 最后两行是离线队列:关着也显示,让人知道有这个功能、当前是什么状态。
    static const char *names[8] = {"State", "Wi-Fi", "Device IP", "Server IP",
                                   "Device", "Seg/Sent", "Cache", "Queue"};
    s_stats_scr = base_screen("STATUS");
    for (int i = 0; i < 8; i++) {
        lv_obj_t *c = card(s_stats_scr, 12, 38 + i * 35, 216, 31, C_CARD);
        label(c, names[i], &lv_font_montserrat_14, C_MUTED);
        lv_obj_t *k = lv_obj_get_child(c, 0);
        lv_obj_align(k, LV_ALIGN_LEFT_MID, 12, 0);
        s_stats_rows[i] = label(c, "-", &lv_font_montserrat_14, C_TEXT);
        lv_obj_align(s_stats_rows[i], LV_ALIGN_RIGHT_MID, -12, 0);
    }
}

// 设置页:离线队列。按 taste 规范,这里不用一堆"卡片+图标+阴影",而是
// 大字号选中的值 + 一行说明 + 一个明确的开关状态,靠尺度而不是装饰来表达层级。
static lv_obj_t *s_set_title;
static lv_obj_t *s_set_cap;
static lv_obj_t *s_set_usage;

static void build_settings(void)
{
    s_set_scr = base_screen("SETTINGS");
    lv_obj_t *t = label(s_set_scr, "Offline queue", &lv_font_montserrat_14, C_MUTED);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 12, 40);

    // 主状态:一行大字说清开/关,下面一行小字说明当前用掉多少。
    lv_obj_t *state = card(s_set_scr, 12, 60, 216, 76, C_CARD);
    s_set_toggle = label(state, "OFF", &lv_font_montserrat_20, C_MUTED);
    lv_obj_align(s_set_toggle, LV_ALIGN_TOP_MID, 0, 16);
    s_set_usage = label(state, "", &lv_font_montserrat_14, C_MUTED);
    lv_obj_align(s_set_usage, LV_ALIGN_BOTTOM_MID, 0, -12);

    // 容量选择:三个值一排,选中的用强调色 + 更亮的底,不做边框/阴影堆叠。
    lv_obj_t *cap = label(s_set_scr, "Queue size", &lv_font_montserrat_14, C_MUTED);
    lv_obj_align(cap, LV_ALIGN_TOP_LEFT, 12, 146);
    static const char *sizes[3] = {"1 MB", "2 MB", "3 MB"};
    for (int i = 0; i < 3; i++) {
        lv_obj_t *b = card(s_set_scr, 12 + i * 74, 166, 68, 44, C_CARD);
        s_set_sizes[i] = label(b, sizes[i], &lv_font_montserrat_14, C_MUTED);
        lv_obj_center(s_set_sizes[i]);
    }

    // 说明文字写全(规范要求:信息写全,而不是靠图标暗示)。
    lv_obj_t *hint = label(s_set_scr,
                           "Keeps audio in flash when the\n"
                           "network is down, then uploads\n"
                           "it. When full, old audio stays\n"
                           "and new audio is dropped.",
                           &lv_font_montserrat_14, C_MUTED);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(hint, 216);
    lv_obj_set_style_text_line_space(hint, 2, 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_LEFT, 12, -14);
}

static void build_provision(void)
{
    s_prov_scr = base_screen("SETUP MODE");
    lv_obj_t *c = card(s_prov_scr, 12, 44, 216, 176, C_CARD);
    label(c, "Join this Wi-Fi hotspot:", &lv_font_montserrat_14, C_MUTED);
    lv_obj_t *l = lv_obj_get_child(c, 0);
    lv_obj_align(l, LV_ALIGN_TOP_LEFT, 14, 14);
    s_prov_ap = label(c, "Passport-XXXX", &lv_font_montserrat_20, C_OK);
    lv_obj_align(s_prov_ap, LV_ALIGN_TOP_LEFT, 14, 40);
    label(c, "Password: passport1234", &lv_font_montserrat_14, C_TEXT);
    lv_obj_align(lv_obj_get_child(c, 2), LV_ALIGN_TOP_LEFT, 14, 74);
    label(c, "Then open in a browser:", &lv_font_montserrat_14, C_MUTED);
    lv_obj_align(lv_obj_get_child(c, 3), LV_ALIGN_TOP_LEFT, 14, 104);
    label(c, "http://192.168.4.1/", &lv_font_montserrat_14, C_ACCENT);
    lv_obj_align(lv_obj_get_child(c, 4), LV_ALIGN_TOP_LEFT, 14, 128);
    label(s_prov_scr, "hold OK: cancel", &lv_font_montserrat_14, C_MUTED);
    lv_obj_align(lv_obj_get_child(s_prov_scr, 1), LV_ALIGN_BOTTOM_MID, 0, -8);
}

static const char *state_text(uint32_t *color)
{
    if (!sb_config_get()->provisioned) {
        *color = C_WARN;
        return "NOT SET UP";
    }
    switch (sb_net_state()) {
    case SB_NET_UP: break;
    case SB_NET_CONNECTING: *color = C_WARN; return "CONNECTING";
    default:
        // 退避重连中:这不是故障,给不同的措辞与颜色,免得看着像坏了。
        if (sb_net_reconnecting()) {
            *color = C_WARN;
            return "RECONNECTING";
        }
        *color = C_BAD;
        return "NO WI-FI";
    }
    if (sb_recorder_paused()) {
        *color = C_WARN;
        return "PAUSED";
    }
    switch (sb_uplink_state()) {
    case SB_UL_UP: *color = C_OK; return "RECORDING";
    case SB_UL_CONNECTING: *color = C_WARN; return "LINKING";
    case SB_UL_ERROR: *color = C_BAD; return "UPLOAD ERR";
    default: *color = C_WARN; return "IDLE";
    }
}

static void refresh(lv_timer_t *t)
{
    (void)t;
    uint32_t color = C_TEXT;
    const char *st = state_text(&color);
    if (s_home_state) {
        lv_label_set_text(s_home_state, st);
        lv_obj_set_style_text_color(s_home_state, lv_color_hex(color), 0);
    }
    const int soc = bsp_battery_soc();
    if (s_home_batt) {
        if (soc < 0) {
            lv_label_set_text(s_home_batt, "--%");
        } else {
            lv_label_set_text_fmt(s_home_batt, "%d%%", soc);
        }
    }
    if (s_home_sub) {
        if (sb_net_state() == SB_NET_UP) {
            lv_label_set_text(s_home_sub, "uploading speech to cloud");
        } else {
            lv_label_set_text(s_home_sub, "waiting for Wi-Fi\nhold OK to set up");
        }
    }
    if (s_home_seg) {
        lv_label_set_text_fmt(s_home_seg, "%lu", (unsigned long)sb_recorder_segments());
    }
    if (s_home_sent) {
        lv_label_set_text_fmt(s_home_sent, "%lu", (unsigned long)sb_uplink_frames_sent());
    }
    if (s_home_drop) {
        lv_label_set_text_fmt(s_home_drop, "%lu", (unsigned long)sb_uplink_frames_dropped());
    }
    if (s_stats_rows[0]) {
        lv_label_set_text(s_stats_rows[0], st);
        lv_obj_set_style_text_color(s_stats_rows[0], lv_color_hex(color), 0);
        const sb_config_t *cfg = sb_config_get();
        // Wi-Fi:连上时直接显示热点名(SSID),断开才显示 down。
        if (sb_net_state() == SB_NET_UP && cfg->ssid[0]) {
            lv_label_set_text(s_stats_rows[1], cfg->ssid);
        } else {
            lv_label_set_text(s_stats_rows[1], sb_net_state() == SB_NET_CONNECTING ? "connecting" : "down");
        }
        lv_label_set_text(s_stats_rows[2], sb_net_state() == SB_NET_UP ? sb_net_ip() : "-");
        // 服务器只显示地址,不显示端口。
        lv_label_set_text(s_stats_rows[3], cfg->srv_host);
        lv_label_set_text(s_stats_rows[4], cfg->dev_id);
        lv_label_set_text_fmt(s_stats_rows[5], "%lu/%lu/%lu",
                              (unsigned long)sb_recorder_segments(),
                              (unsigned long)sb_uplink_frames_sent(),
                              (unsigned long)sb_uplink_frames_dropped());
        const sb_config_t *c = sb_config_get();
        // 队列关着时明确写 off,而不是留空或显示 0,避免被误解成"开了但没数据"。
        lv_label_set_text(s_stats_rows[6], c->cache_enabled ? "on" : "off");
        if (c->cache_enabled) {
            lv_label_set_text_fmt(s_stats_rows[7], "%u KB / %u MB",
                                  (unsigned)(sb_cache_used_bytes() / 1024u),
                                  (unsigned)c->cache_limit_mb);
        } else {
            lv_label_set_text(s_stats_rows[7], "-");
        }
    }
    refresh_settings();
}

void sb_ui_init(void)
{
    build_home();
    build_stats();
    build_settings();
    build_provision();
    s_timer = lv_timer_create(refresh, 500, NULL);
    lv_screen_load(s_home_scr);
}

void sb_ui_show(sb_screen_t screen)
{
    switch (screen) {
    case SB_SCREEN_STATS: lv_screen_load(s_stats_scr); break;
    case SB_SCREEN_SETTINGS: lv_screen_load(s_set_scr); break;
    case SB_SCREEN_PROVISION: lv_screen_load(s_prov_scr); break;
    default: lv_screen_load(s_home_scr); break;
    }
}

// --- 设置页交互(由 main 的按键处理调用;都在 LVGL 上下文或持锁时调用) -------

void sb_ui_settings_move(int delta)
{
    const sb_config_t *cfg = sb_config_get();
    // 关着的时候上下键直接开(并落到默认 2MB),省一步操作。
    if (!cfg->cache_enabled) {
        sb_config_set_cache(true, cfg->cache_limit_mb);
        return;
    }
    int mb = (int)cfg->cache_limit_mb + delta;
    if (mb < 1) {
        mb = 1;
    }
    if (mb > (int)SB_CACHE_MAX_LIMIT_MB) {
        mb = (int)SB_CACHE_MAX_LIMIT_MB;
    }
    if ((uint8_t)mb != cfg->cache_limit_mb) {
        sb_config_set_cache(true, (uint8_t)mb);
    }
}

void sb_ui_settings_toggle(void)
{
    const sb_config_t *cfg = sb_config_get();
    sb_config_set_cache(!cfg->cache_enabled, cfg->cache_limit_mb);
}

static void refresh_settings(void)
{
    if (s_set_scr == NULL) {
        return;
    }
    const sb_config_t *cfg = sb_config_get();
    const bool on = cfg->cache_enabled;
    if (s_set_toggle) {
        lv_label_set_text(s_set_toggle, on ? "ON" : "OFF");
        lv_obj_set_style_text_color(s_set_toggle, lv_color_hex(on ? C_OK : C_MUTED), 0);
    }
    if (s_set_usage) {
        if (on) {
            // 用掉多少 / 队列里还有多少条待补传 —— 让"存满丢弃"的行为可预期。
            lv_label_set_text_fmt(s_set_usage, "%u KB used   %u waiting   %u dropped",
                                  (unsigned)(sb_cache_used_bytes() / 1024u),
                                  (unsigned)sb_cache_pending(),
                                  (unsigned)sb_cache_dropped());
        } else {
            lv_label_set_text(s_set_usage, "disabled - audio is not buffered");
        }
    }
    for (int i = 0; i < 3; i++) {
        if (s_set_sizes[i] == NULL) {
            continue;
        }
        const bool selected = on && ((int)cfg->cache_limit_mb == i + 1);
        lv_obj_set_style_text_color(s_set_sizes[i],
                                    lv_color_hex(selected ? C_ACCENT : C_MUTED), 0);
        lv_obj_set_style_bg_color(lv_obj_get_parent(s_set_sizes[i]),
                                  lv_color_hex(selected ? 0x24303F : C_CARD), 0);
    }
}

void sb_ui_set_ap_name(const char *name)
{
    if (name == NULL) {
        return;
    }
    strlcpy(s_ap_name, name, sizeof(s_ap_name));
    if (s_prov_ap) {
        lv_label_set_text(s_prov_ap, s_ap_name);
    }
}
