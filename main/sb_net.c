// main/sb_net.c —— WiFi STA 连接 + SoftAP 配网网页。
//
// 配网模型(参考社区 SoftAP 玩法):短时配网会话。开 APSTA,手机连上 SoftAP 打开
// 配置页,选热点/填密码 -> 设备先用该凭据尝试 STA 连接 -> 成功才写 NVS -> 关 AP。
// 失败保留旧凭据,避免手滑把设备配死。
#include "sb_net.h"

#include <stdlib.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"

#define STA_CONNECT_BIT BIT0
#define STA_FAIL_BIT    BIT1

static const char *TAG = "sb_net";

static esp_netif_t *s_sta_netif;
static esp_netif_t *s_ap_netif;
static EventGroupHandle_t s_events;
static volatile sb_net_state_t s_state = SB_NET_DOWN;
static char s_ip[16] = "";
static bool s_wifi_inited;
static bool s_provisioning;
static volatile int s_prov_result;

// --- 断线重连(由本模块统一负责) -------------------------------------------
// 目标:热点重启/短暂掉线后尽快恢复,并且不依赖应用主循环的轮询。
// 做法:收到 DISCONNECTED 事件后起一个轻量定时器,按指数退避(1s→2s→4s→8s 封顶)
// 重新 esp_wifi_connect()。ESP-IDF 的自动重连没有退避控制,这里自己实现以便观察和
// 在界面上显示状态。重连用的凭据仍在驱动里(上次 set_config 的 STA 配置),不需要
// 重新下发,所以重连路径不会碰到 ESP_ERR_WIFI_STATE 那类竞态。
#define RECONNECT_MIN_MS   1000
#define RECONNECT_MAX_MS   8000
// 掉线多久算"真正失败"(用于把状态从 CONNECTING 降级,仅影响显示)。
#define CONNECT_TIMEOUT_MS 15000

static esp_timer_handle_t s_reconnect_timer;
static uint32_t s_backoff_ms = RECONNECT_MIN_MS;
static volatile bool s_reconnecting;
static volatile bool s_want_connect;   // 有凭据且未在配网 => 期望保持连接
static char s_ssid[33];
static char s_pass[65];
static SemaphoreHandle_t s_connect_lock;

static void reconnect_timer_cb(void *arg);
static void schedule_reconnect(void);
static esp_err_t apply_sta_config(void);

static void set_ip_from_netif(void)
{
    if (s_sta_netif == NULL) {
        return;
    }
    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(s_sta_netif, &ip) == ESP_OK && ip.ip.addr != 0) {
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&ip.ip));
    }
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        // 不在 STA_START 里自动连接:那样会把 STA 置为 connecting,之后
        // sb_net_sta_connect() 再调 esp_wifi_set_config() 会返回
        // ESP_ERR_WIFI_STATE("sta is connecting, cannot set config")。
        // 连接统一由 sb_net_sta_connect() 负责。
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_state = SB_NET_DOWN;
        s_ip[0] = '\0';
        if (s_events) {
            xEventGroupSetBits(s_events, STA_FAIL_BIT);
        }
        // 只要还要保持连接(有凭据且未配网),就安排退避重连。断言连接请求的那次
        // 主动断开也算这里,所以由 s_want_connect 决定要不要重连,而不是看原因码。
        if (s_want_connect && !s_provisioning) {
            schedule_reconnect();
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_state = SB_NET_UP;
        set_ip_from_netif();
        s_reconnecting = false;
        s_backoff_ms = RECONNECT_MIN_MS;   // 连上即重置退避
        if (s_events) {
            xEventGroupSetBits(s_events, STA_CONNECT_BIT);
        }
        ESP_LOGI(TAG, "STA 已联网, IP=%s", s_ip);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_LOST_IP) {
        // 租约丢失(热点重启常见):不等 DISCONNECTED,先降级状态并安排重连。
        s_state = SB_NET_DOWN;
        s_ip[0] = '\0';
        if (s_want_connect && !s_provisioning) {
            schedule_reconnect();
        }
    }
}

// 用上次保存的凭据下发 STA 配置并连接。断开在前,避免 "sta is connecting,
// cannot set config" 的状态竞态。
static esp_err_t apply_sta_config(void)
{
    if (s_provisioning) {
        return ESP_ERR_INVALID_STATE;   // 配网期间由网页那侧直接连接
    }
    wifi_config_t wc = {0};
    strlcpy((char *)wc.sta.ssid, s_ssid, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, s_pass, sizeof(wc.sta.password));
    wc.sta.threshold.authmode = WIFI_AUTH_OPEN;
    esp_err_t m = esp_wifi_set_mode(WIFI_MODE_STA);
    if (m != ESP_OK && m != ESP_ERR_WIFI_STATE) {
        return m;
    }
    (void)esp_wifi_disconnect();
    esp_err_t e = esp_wifi_set_config(WIFI_IF_STA, &wc);
    if (e != ESP_OK) {
        return e;
    }
    return esp_wifi_connect();
}

static void reconnect_timer_cb(void *arg)
{
    (void)arg;
    if (!s_want_connect || s_provisioning || s_state == SB_NET_UP) {
        s_reconnecting = false;
        return;
    }
    ESP_LOGI(TAG, "重连 '%s' (第 %u ms 退避后)", s_ssid, (unsigned)s_backoff_ms);
    s_state = SB_NET_CONNECTING;
    if (apply_sta_config() != ESP_OK) {
        schedule_reconnect();   // 连请求都发不出去,继续退避
    }
    // 成功与否由 GOT_IP / DISCONNECTED 事件决定。
}

static void schedule_reconnect(void)
{
    if (s_reconnect_timer == NULL) {
        return;
    }
    if (esp_timer_is_active(s_reconnect_timer)) {
        return;   // 已有一次待触发,不重复排队
    }
    s_reconnecting = true;
    const uint32_t delay = s_backoff_ms;
    if (s_backoff_ms < RECONNECT_MAX_MS) {
        s_backoff_ms = (s_backoff_ms * 2 > RECONNECT_MAX_MS) ? RECONNECT_MAX_MS
                                                             : s_backoff_ms * 2;
    }
    esp_timer_start_once(s_reconnect_timer, (uint64_t)delay * 1000);
}

esp_err_t sb_net_init(void)
{
    if (s_wifi_inited) {
        return ESP_OK;
    }
    ESP_ERROR_CHECK(esp_netif_init());
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    s_events = xEventGroupCreate();
    if (s_events == NULL) {
        return ESP_ERR_NO_MEM;
    }
    s_sta_netif = esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    // 已启动时再调会返回 ESP_ERR_WIFI_STATE/ESP_OK;只容忍状态类错误,不 abort。
    esp_err_t se = esp_wifi_start();
    if (se != ESP_OK && se != ESP_ERR_WIFI_STATE) {
        return se;
    }
    s_connect_lock = xSemaphoreCreateMutex();
    if (s_connect_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    const esp_timer_create_args_t timer_args = {
        .callback = reconnect_timer_cb,
        .name = "sb_reconnect",
    };
    if (esp_timer_create(&timer_args, &s_reconnect_timer) != ESP_OK) {
        return ESP_ERR_NO_MEM;
    }
    s_wifi_inited = true;
    return ESP_OK;
}

esp_err_t sb_net_sta_connect(const char *ssid, const char *pass)
{
    if (ssid == NULL || ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    // 记住凭据:重连同一条网络时不重新下发配置(避免状态竞态),但第一次或换了
    // 热点必须重新 set_config。
    const bool same = (strcmp(s_ssid, ssid) == 0) && (strcmp(s_pass, pass ? pass : "") == 0);
    strlcpy(s_ssid, ssid, sizeof(s_ssid));
    strlcpy(s_pass, pass ? pass : "", sizeof(s_pass));

    xEventGroupClearBits(s_events, STA_CONNECT_BIT | STA_FAIL_BIT);
    s_state = SB_NET_CONNECTING;
    s_backoff_ms = RECONNECT_MIN_MS;

    if (xSemaphoreTake(s_connect_lock, pdMS_TO_TICKS(5000)) != pdTRUE) {
        s_state = SB_NET_DOWN;
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err;
    if (same && !s_provisioning) {
        // 同一热点:直接重连,不碰 set_config。
        err = esp_wifi_connect();
    } else {
        err = apply_sta_config();
    }
    xSemaphoreGive(s_connect_lock);
    if (err != ESP_OK) {
        s_state = SB_NET_DOWN;
        return err;
    }
    // 15 s 内要么拿到 IP,要么失败。
    const EventBits_t bits = xEventGroupWaitBits(
        s_events, STA_CONNECT_BIT | STA_FAIL_BIT, pdTRUE, pdFALSE, pdMS_TO_TICKS(15000));
    if (bits & STA_CONNECT_BIT) {
        s_want_connect = true;   // 连上后开启断线自动重连
        return ESP_OK;
    }
    s_state = SB_NET_DOWN;
    // 交给退避重连继续尝试(而不是把凭据丢掉)。
    if (!s_provisioning) {
        s_want_connect = true;
        schedule_reconnect();
    }
    return ESP_FAIL;
}

esp_err_t sb_net_ensure_connected(void)
{
    if (s_provisioning) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_ssid[0] == '\0') {
        return ESP_ERR_INVALID_STATE;   // 还没有凭据
    }
    s_want_connect = true;
    if (s_state == SB_NET_UP || s_state == SB_NET_CONNECTING) {
        return ESP_OK;                  // 已连上或正在连(含退避后的重连)
    }
    // 处于 DOWN 且没有排队的重连:立刻发起一次。
    if (s_reconnecting) {
        return ESP_OK;
    }
    ESP_LOGI(TAG, "发起连接 '%s'", s_ssid);
    s_state = SB_NET_CONNECTING;
    if (xSemaphoreTake(s_connect_lock, pdMS_TO_TICKS(5000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    const esp_err_t err = apply_sta_config();
    xSemaphoreGive(s_connect_lock);
    if (err != ESP_OK) {
        s_state = SB_NET_DOWN;
        schedule_reconnect();
    }
    return err;
}

bool sb_net_reconnecting(void)
{
    return s_reconnecting;
}

void sb_net_sta_disconnect(void)
{
    // 显式断开 = 用户意图,停止自动重连(配网/切换网络时用)。
    s_want_connect = false;
    if (s_reconnect_timer && esp_timer_is_active(s_reconnect_timer)) {
        esp_timer_stop(s_reconnect_timer);
    }
    s_reconnecting = false;
    esp_wifi_disconnect();
    s_state = SB_NET_DOWN;
    s_ip[0] = '\0';
}

sb_net_state_t sb_net_state(void)
{
    return s_state;
}

const char *sb_net_ip(void)
{
    return s_ip;
}

// ---------------------------------------------------------------------------
// 配网(SoftAP + 配置网页)。网页与扫描列表放在独立文件,保持本文件聚焦连接逻辑。
// ---------------------------------------------------------------------------
esp_err_t sb_net_provision_ap_start(void);
esp_err_t sb_net_provision_web_start(void);
esp_err_t sb_net_provision_web_stop(void);

// 扫描结果快照(供配网页读取)。
#define SB_SCAN_MAX 20
typedef struct {
    char ssid[33];
    int8_t rssi;
} sb_scan_item_t;

int sb_net_scan(sb_scan_item_t *out, int max);

esp_err_t sb_net_provision_start(void)
{
    if (s_provisioning) {
        return ESP_OK;
    }
    s_prov_result = 0;
    s_provisioning = true;
    // 配网期间暂停自动重连,免得后台重连干扰 APSTA 与网页验证。
    s_want_connect = false;
    if (s_reconnect_timer && esp_timer_is_active(s_reconnect_timer)) {
        esp_timer_stop(s_reconnect_timer);
    }
    s_reconnecting = false;
    // 先建 netif 再设模式:netif 创建应在 driver 模式切换之前完成。
    if (s_ap_netif == NULL) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
    }
    // APSTA:一边开热点给手机配网,一边准备验证家里热点。
    esp_err_t m = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (m != ESP_OK) {
        s_provisioning = false;
        return m;
    }
    wifi_config_t ap = {0};
    uint8_t mac[6] = {0};
    esp_wifi_get_mac(WIFI_IF_AP, mac);
    snprintf((char *)ap.ap.ssid, sizeof(ap.ap.ssid), "Passport-%02X%02X", mac[4], mac[5]);
    ap.ap.ssid_len = strlen((char *)ap.ap.ssid);
    strlcpy((char *)ap.ap.password, "passport1234", sizeof(ap.ap.password));
    ap.ap.max_connection = 2;
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    esp_err_t ap_err = esp_wifi_set_config(WIFI_IF_AP, &ap);
    if (ap_err != ESP_OK) {
        s_provisioning = false;
        return ap_err;
    }
    esp_err_t se = esp_wifi_start();
    if (se != ESP_OK && se != ESP_ERR_WIFI_STATE) {
        s_provisioning = false;
        return se;
    }
    ESP_ERROR_CHECK(sb_net_provision_web_start());
    ESP_LOGI(TAG, "配网已开启: 热点 %s, 配置页 http://192.168.4.1/", ap.ap.ssid);
    return ESP_OK;
}

esp_err_t sb_net_provision_stop(void)
{
    if (!s_provisioning) {
        return ESP_OK;
    }
    sb_net_provision_web_stop();
    s_provisioning = false;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    // 配网成功后恢复"保持连接",让掉线能自动恢复。
    if (s_ssid[0] != '\0') {
        s_want_connect = true;
    }
    return ESP_OK;
}

int sb_net_provision_result(void)
{
    return s_prov_result;
}

void sb_net_provision_mark(int result)
{
    s_prov_result = result;
}

bool sb_net_provisioning(void)
{
    return s_provisioning;
}

int sb_net_scan(sb_scan_item_t *out, int max)
{
    wifi_scan_config_t sc = {0};
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) {
        return 0;
    }
    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    if (ap_count == 0) {
        return 0;
    }
    if (ap_count > (uint16_t)max) {
        ap_count = (uint16_t)max;
    }
    wifi_ap_record_t *recs = calloc(ap_count, sizeof(wifi_ap_record_t));
    if (recs == NULL) {
        return 0;
    }
    int n = 0;
    if (esp_wifi_scan_get_ap_records(&ap_count, recs) == ESP_OK) {
        n = ap_count;
        for (int i = 0; i < n; i++) {
            strlcpy(out[i].ssid, (const char *)recs[i].ssid, sizeof(out[i].ssid));
            out[i].rssi = recs[i].rssi;
        }
    }
    free(recs);
    return n;
}
