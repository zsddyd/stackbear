// main/main.c —— stackbear「全天候录音卡」应用入口。
//
// 与基线 demo 的差别:这里没有测试菜单,启动即进入自研界面(Home / Status / Setup),
// 联网后自动开始"常开 VAD 录音 -> Opus 编码 -> 私有协议上传 ECS"。
//
// 按键语义:
//   OK   短按 : 暂停 / 继续上传(常开录音的开关)
//   OK   长按 : 进入配网模式;配网中再长按 = 取消并返回
//   UP / DOWN : 在 Home 与 Status 页之间切换
#include <string.h>

#include "bsp_audio.h"
#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "sb_config.h"
#include "sb_net.h"
#include "sb_opus.h"
#include "sb_recorder.h"
#include "sb_ui.h"
#include "sb_uplink.h"

static const char *TAG = "stackbear";

#define INPUT_QUEUE_DEPTH 8
#define LINK_RETRY_MS     8000

typedef struct {
    bsp_btn_t btn;
    bsp_btn_ev_t event;
} input_event_t;

static QueueHandle_t s_input_queue;
static volatile bool s_input_ready;
static volatile bool s_setup_mode;
static sb_screen_t s_screen = SB_SCREEN_HOME;

static void ui_show_locked(sb_screen_t screen)
{
    if (!bsp_lvgl_lock(300)) {
        return;
    }
    sb_ui_show(screen);
    bsp_lvgl_unlock();
    s_screen = screen;
}

static void enter_setup_locked(void);
static void exit_setup_locked(void);

static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user)
{
    (void)user;
    if (!s_input_ready || s_input_queue == NULL) {
        return;  // 按键回调运行在共享 esp_timer 任务:只入队,立即返回
    }
    const input_event_t in = {.btn = btn, .event = ev};
    (void)xQueueSend(s_input_queue, &in, 0);
}

static void handle_input(const input_event_t *in)
{
    if (s_setup_mode) {
        if (in->btn == BSP_BTN_OK && in->event == BSP_BTN_LONG) {
            exit_setup_locked();
        }
        return;
    }
    if (in->btn == BSP_BTN_OK && in->event == BSP_BTN_LONG) {
        enter_setup_locked();
        return;
    }
    if (in->event != BSP_BTN_CLICK) {
        return;
    }
    if (in->btn == BSP_BTN_OK) {
        const bool paused = sb_recorder_paused();
        sb_recorder_set_paused(!paused);
        ESP_LOGI(TAG, "上传%s", !paused ? "已暂停" : "已恢复");
    } else if (in->btn == BSP_BTN_UP || in->btn == BSP_BTN_DOWN) {
        ui_show_locked(s_screen == SB_SCREEN_HOME ? SB_SCREEN_STATS : SB_SCREEN_HOME);
    }
}

static void input_task(void *arg)
{
    (void)arg;
    input_event_t in;
    for (;;) {
        if (xQueueReceive(s_input_queue, &in, portMAX_DELAY) == pdTRUE) {
            handle_input(&in);
        }
    }
}

static void enter_setup_locked(void)
{
    if (sb_net_provision_start() != ESP_OK) {
        ESP_LOGE(TAG, "进入配网模式失败");
        return;
    }
    s_setup_mode = true;
    if (bsp_lvgl_lock(300)) {
        sb_ui_set_ap_name("Passport-XXXX");
        sb_ui_show(SB_SCREEN_PROVISION);
        bsp_lvgl_unlock();
    }
    ESP_LOGI(TAG, "进入配网模式");
}

static void exit_setup_locked(void)
{
    sb_net_provision_stop();
    s_setup_mode = false;
    ui_show_locked(SB_SCREEN_HOME);
    ESP_LOGI(TAG, "退出配网模式");
}

// 网络与录音编排任务:配网成功后自动连热点、开始常开录音。
static void app_task(void *arg)
{
    (void)arg;
    if (sb_net_init() != ESP_OK) {
        ESP_LOGE(TAG, "网络初始化失败");
        vTaskDelete(NULL);
        return;
    }
    for (;;) {
        // 配网页里连接成功 -> 关闭 SoftAP/网页,回到主页继续正常流程。
        if (s_setup_mode && sb_net_provision_result() == 1) {
            sb_net_provision_stop();
            s_setup_mode = false;
            ui_show_locked(SB_SCREEN_HOME);
            ESP_LOGI(TAG, "配网成功,退出配网模式");
            continue;
        }
        if (s_setup_mode) {
            vTaskDelay(pdMS_TO_TICKS(300));
            continue;
        }
        const sb_config_t *cfg = sb_config_get();
        if (!cfg->provisioned) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        if (sb_net_state() == SB_NET_UP) {
            if (!sb_recorder_running()) {
                sb_uplink_start();
                sb_uplink_begin_session();
                sb_recorder_start();
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        ESP_LOGI(TAG, "连接热点 '%s' ...", cfg->ssid);
        if (sb_net_sta_connect(cfg->ssid, cfg->pass) != ESP_OK) {
            ESP_LOGW(TAG, "连接失败, %d ms 后重试", LINK_RETRY_MS);
            vTaskDelay(pdMS_TO_TICKS(LINK_RETRY_MS));
        }
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "stackbear 全天候录音卡启动");
    if (sb_config_init() != ESP_OK) {
        ESP_LOGE(TAG, "配置初始化失败");
    }
    // Opus 编码器要在 WiFi/LVGL 之前创建:它是常驻的大块连续内存(约 25 KB),
    // 等网络和界面起来后堆只剩 ~45 KB 且已碎片化,opus_encoder_create 会以
    // OPUS_ALLOC_FAIL(-7) 失败。抢在启动早期分配最稳妥。
    if (sb_opus_init() != ESP_OK) {
        ESP_LOGE(TAG, "Opus 编码器早期初始化失败,录音将不可用");
    }
    bsp_i2c_init();

    if (bsp_display_init() != ESP_OK || bsp_lvgl_init() == NULL) {
        ESP_LOGE(TAG, "显示/LVGL 初始化失败,应用无法继续");
        return;
    }
    bsp_display_backlight(100);

    s_input_queue = xQueueCreate(INPUT_QUEUE_DEPTH, sizeof(input_event_t));
    if (s_input_queue == NULL) {
        ESP_LOGE(TAG, "输入队列创建失败");
        return;
    }
    if (bsp_button_init(on_key, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "按键初始化失败");
    }

    if (bsp_lvgl_lock(1000)) {
        sb_ui_init();
        bsp_lvgl_unlock();
        s_input_ready = true;
    }
    xTaskCreate(input_task, "sb_input", 3072, NULL, 5, NULL);
    xTaskCreate(app_task, "sb_app", 4096, NULL, 4, NULL);
    ESP_LOGI(TAG, "就绪: dev=%s, srv=%s:%u", sb_config_get()->dev_id,
             sb_config_get()->srv_host, (unsigned)sb_config_get()->srv_port);
}
