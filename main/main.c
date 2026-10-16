// main/main.c —— stackbear「全天候录音卡」应用入口。
//
// 与基线 demo 的差别:这里没有测试菜单,启动即进入自研界面(Home / Status / Setup),
// 联网后自动开始"常开 VAD 录音 -> IMA-ADPCM 编码 -> 私有协议上传 ECS"。
//
// 按键语义(逐页一致,详见 docs/reference/zsddyd/stackbear/README.md 的按键表):
//   UP / DOWN 短按   : 列表页移动光标;主页进入 Status
//   OK   短按        : 主页 = 软暂停 / 继续;列表页 = 激活当前行(仅可操作行)
//   UP   长按        : 主页 = 开始 / 停止录音(会话级);列表页不响应,避免浏览时误停
//   DOWN 长按        : 逐级返回(子页 -> 菜单 -> 主页)
//   OK   长按        : 进入配网模式;配网中再长按 = 取消并返回
//
// 不做"双击"手势:按键组件把间隔小于短按窗口(本板 180ms)的连按合并成多击,
// 于是"双击"的含义会随按键速度变化,且与"连按两次移动两格"冲突。录音这种会话级
// 破坏性操作只绑定长按,不绑定依赖速度的手势。
#include <string.h>

#include "bsp_audio.h"
#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "sb_config.h"
#include "sb_cache.h"
#include "sb_key_nav.h"
#include "sb_net.h"
#include "sb_recorder.h"
#include "sb_ui.h"
#include "sb_uplink.h"

static const char *TAG = "stackbear";

#define INPUT_QUEUE_DEPTH 8

typedef struct {
    bsp_btn_t btn;
    bsp_btn_ev_t event;
} input_event_t;

static QueueHandle_t s_input_queue;
static volatile bool s_input_ready;
static volatile bool s_setup_mode;
static volatile bool s_uplink_started;

// 当前页面只由 sb_ui 持有。main 以前自己留了一份副本,但 sb_ui_back() 只改 UI 内部
// 状态,副本不会跟着更新:长按 DOWN 返回主页后,main 仍以为在列表页,于是之后的单按
// 被当成"列表导航"(在主页上 sb_ui_move() 无效果),表现为"暂停后按上键进不去 Status"。
// 现在统一向 sb_ui 查询,不再维护第二份状态。

// 列表页的轻点判定(见 sb_key_nav.h):按键组件会把快速连按合并成多击且只在结束时
// 按次数派发,3 次及以上不派发任何事件,所以列表页不能依赖 SINGLE_CLICK。
static sb_key_nav_t s_key_nav;

static void ui_show_locked(sb_screen_t screen)
{
    if (!bsp_lvgl_lock(300)) {
        return;
    }
    sb_ui_show(screen);
    bsp_lvgl_unlock();
}

static void enter_setup_locked(void);
static void exit_setup_locked(void);
static void toggle_recording(void);

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
    // 先把上下键的"按下/长按/抬起"喂给轻点判定,配网页里也照喂:否则退出配网后
    // 可能留下一次陈旧的按下记录,抬起时算成轻点,产生一次幽灵移动。
    if (in->btn == BSP_BTN_UP || in->btn == BSP_BTN_DOWN) {
        if (in->event == BSP_BTN_PRESS) {
            sb_key_nav_press(&s_key_nav);
        } else if (in->event == BSP_BTN_LONG) {
            sb_key_nav_long(&s_key_nav);
        }
    }
    // 抬起:只有"这次按压没变成长按"才算一次轻点。列表页用它移动光标;主页不消费
    // 这个结果,但仍要在这里更新状态,免得留下陈旧的按下记录。
    const bool tap =
        (in->btn == BSP_BTN_UP || in->btn == BSP_BTN_DOWN) &&
        in->event == BSP_BTN_RELEASE && sb_key_nav_release(&s_key_nav);

    if (s_setup_mode) {
        // 配网页也是"长按返回":每个页面的长按 DOWN 都是往回走,配网页不能例外
        // (用户学会"长按 DOWN 退出"之后,在这里按它却没有反应,是最容易让人以为
        // 设备卡住的地方)。
        //
        // 长按 OK 依然保留:它是"进 / 出配网"的同一个开关,前面用它进来的用户不会
        // 突然失灵。
        if (in->event == BSP_BTN_LONG &&
            (in->btn == BSP_BTN_DOWN || in->btn == BSP_BTN_OK)) {
            exit_setup_locked();
        }
        return;
    }
    if (in->btn == BSP_BTN_OK && in->event == BSP_BTN_LONG) {
        enter_setup_locked();
        return;
    }
    // 长按下键 = 逐级返回(子页 -> 分组菜单 -> 主页)。
    if (in->event == BSP_BTN_LONG && in->btn == BSP_BTN_DOWN) {
        if (bsp_lvgl_lock(300)) {
            sb_ui_back();
            bsp_lvgl_unlock();
        }
        return;
    }

    // 页面相关的路由需要知道当前页面,而当前页面由 sb_ui 持有:统一加锁后查询,
    // 不再维护第二份副本(副本会与 sb_ui_back()/sb_ui_activate() 的内部跳转失步)。
    if (!bsp_lvgl_lock(300)) {
        return;  // 拿不到锁就不猜页面,丢弃这次按键,避免误路由
    }
    const sb_screen_t screen = sb_ui_current();
    // HOME 也有"选中 + 确认":UP/DOWN 在 Start/Stop 与 Settings 之间移光标,OK 执行
    // 选中那一行。它不是列表页(没有滚动),所以单独一条分支。
    //
    // 注意长按 UP **不能**被这条分支吞掉:它是"开关录音"的快捷键,与选中行无关,
    // 无论光标停在哪一行都必须生效(下面的 CLICK 分支只处理轻点)。
    if (screen == SB_SCREEN_HOME) {
        if (tap && (in->btn == BSP_BTN_UP || in->btn == BSP_BTN_DOWN)) {
            sb_ui_move(in->btn == BSP_BTN_DOWN ? 1 : -1);
            bsp_lvgl_unlock();
            return;
        } else if (in->btn == BSP_BTN_OK && in->event == BSP_BTN_CLICK) {
            if (sb_ui_home_action() == SB_HOME_SETTINGS) {
                sb_ui_show(SB_SCREEN_MENU);
            } else {
                // 与长按 UP 同一个动作(会话级开始/停止),不是软暂停:按钮上写的就是
                // "Start/Stop recording",点下去就该是那件事。
                toggle_recording();
            }
            bsp_lvgl_unlock();
            return;
        }
        bsp_lvgl_unlock();
        // 其余(长按 UP 这个快捷键)落到下面的统一分支。长按 DOWN/OK 在上面已处理。
    }
    // 列表页(含 SETTINGS 菜单):UP/DOWN 移光标、OK 执行当前行。Help 是纯文字页,
    // 上下键在那里没有作用,所以不在这里。
    const bool is_list = screen == SB_SCREEN_MENU || screen == SB_SCREEN_NET ||
                         screen == SB_SCREEN_REC || screen == SB_SCREEN_DEV ||
                         screen == SB_SCREEN_STORAGE;
    if (is_list) {
        // 列表页:UP/DOWN 每次轻点移动一格,OK 单击激活当前行。每一行是不是可操作
        // 由 UI 自己判断(见 sb_ui_row_actionable),上层不必知道"第几行是哪一项"。
        // 这里用轻点而非 CLICK:CLICK 只在孤立单按时才产生,连按会被组件合并成多击
        // 且 3 次以上完全不派发,光标就不会动。
        //
        // 列表页不响应长按 UP:录音是会话级破坏操作(停止会结束当前会话并封文件),
        // 放在浏览菜单时最容易误触。切录音只在主页长按 UP。
        if (tap && (in->btn == BSP_BTN_UP || in->btn == BSP_BTN_DOWN)) {
            sb_ui_move(in->btn == BSP_BTN_DOWN ? 1 : -1);
        } else if (in->btn == BSP_BTN_OK && in->event == BSP_BTN_CLICK) {
            sb_ui_activate();
        }
        bsp_lvgl_unlock();
        return;
    }
    bsp_lvgl_unlock();

    // 主页:长按 UP = 开始 / 停止录音(会话级:停止会结束当前会话并封文件)。
    if (in->btn == BSP_BTN_UP && in->event == BSP_BTN_LONG) {
        toggle_recording();
        return;
    }
    // 其余在 HOME 上的动作都在上面按"选中 + 确认"处理完了。
}

static void toggle_recording(void)
{
    if (sb_recorder_paused()) {
        sb_recorder_set_paused(false);
        sb_uplink_begin_session();
        ESP_LOGI(TAG, "开始录音");
    } else {
        sb_recorder_set_paused(true);
        sb_uplink_end_session();
        ESP_LOGI(TAG, "停止录音");
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
    // 把 NVS 里保存的凭据载入网络模块,之后 ensure_connected() 才能自动连接。
    // 少了这一步,重启后设备手里没有任何凭据,只能显示未配网 —— 而被误以为是
    // "又要重新配网"。
    if (sb_net_load_saved() != ESP_OK) {
        ESP_LOGI(TAG, "尚无已保存的 WiFi 凭据,等待配网");
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
        // 先起录音任务(它负责音频/I2S DMA 与 Opus 编码器的内存分配),再连 WiFi。
        // 顺序反了的话,WiFi 会先吃掉内部内存,音频 DMA 分配失败。
        if (!sb_recorder_running()) {
            sb_recorder_start();
            vTaskDelay(pdMS_TO_TICKS(200));
        }
        // 上行任务由应用循环看护:它自己负责 TCP 连接与重连,WiFi 通了就发 HELLO。
        // 这里每次循环都确认它在跑 —— OTA 下载前会临时停掉它以腾出内存,升级失败
        // 后需要重新拉起。全部函数都是幂等的。
        if (!sb_uplink_running()) {
            sb_uplink_start();
            sb_uplink_begin_session();
            s_uplink_started = true;
        }
        if (sb_net_state() == SB_NET_UP) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        // 连接与断线重连都由 sb_net 负责:这里周期性地"确保连接"即可,断线后
        // 会由它的指数退避定时器在后台重连,不需要应用层再写重试逻辑。
        (void)sb_net_ensure_connected();
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "stackbear 全天候录音卡启动");
    // OTA 回滚:确认本次启动的固件可用。只有跑过这一步,超时未确认的固件才会被
    // bootloader 回滚(见 sdkconfig 的 ROLLBACK 选项)。放在最前面,表示"能启动
    // 到这里就算健康";后续业务初始化失败也会保留这颗固件,避免反复回滚。
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;
    if (running && esp_ota_get_state_partition(running, &ota_state) == ESP_OK &&
        ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
        if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
            ESP_LOGI(TAG, "OTA 固件已确认(%s)", running->label);
        } else {
            ESP_LOGW(TAG, "标记固件有效失败");
        }
    }
    if (sb_config_init() != ESP_OK) {
        ESP_LOGE(TAG, "配置初始化失败");
    }
    // 离线队列(可选):启用时挂到 audioq 分区并恢复上电前的积压。失败只告警,
    // 不影响实时录音上传。
    if (sb_cache_init() != ESP_OK) {
        ESP_LOGW(TAG, "离线队列未启用或初始化失败(仅影响离线缓存)");
    }
    bsp_i2c_init();
    // 电量计是软依赖:读不到(CW2017 不应答)只让界面显示 "--%",不阻塞启动。
    if (bsp_battery_init() != ESP_OK) {
        ESP_LOGW(TAG, "电量计初始化失败,界面将显示 --%%");
    }

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
