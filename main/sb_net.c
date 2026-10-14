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
        if (!s_provisioning) {
            esp_wifi_connect();
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_state = SB_NET_DOWN;
        s_ip[0] = '\0';
        if (s_events) {
            xEventGroupSetBits(s_events, STA_FAIL_BIT);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_state = SB_NET_UP;
        set_ip_from_netif();
        if (s_events) {
            xEventGroupSetBits(s_events, STA_CONNECT_BIT);
        }
        ESP_LOGI(TAG, "STA 已联网, IP=%s", s_ip);
    }
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
        IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    s_wifi_inited = true;
    return ESP_OK;
}

esp_err_t sb_net_sta_connect(const char *ssid, const char *pass)
{
    if (ssid == NULL || ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    wifi_config_t wc = {0};
    strlcpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, pass ? pass : "", sizeof(wc.sta.password));
    wc.sta.threshold.authmode = WIFI_AUTH_OPEN;
    xEventGroupClearBits(s_events, STA_CONNECT_BIT | STA_FAIL_BIT);
    s_state = SB_NET_CONNECTING;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        s_state = SB_NET_DOWN;
        return err;
    }
    // 15 s 内要么拿到 IP,要么失败。
    const EventBits_t bits = xEventGroupWaitBits(
        s_events, STA_CONNECT_BIT | STA_FAIL_BIT, pdTRUE, pdFALSE, pdMS_TO_TICKS(15000));
    if (bits & STA_CONNECT_BIT) {
        return ESP_OK;
    }
    s_state = SB_NET_DOWN;
    esp_wifi_disconnect();
    return ESP_FAIL;
}

void sb_net_sta_disconnect(void)
{
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
    // APSTA:一边开热点给手机配网,一边准备验证家里热点。
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    if (s_ap_netif == NULL) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
    }
    wifi_config_t ap = {0};
    uint8_t mac[6] = {0};
    esp_wifi_get_mac(WIFI_IF_AP, mac);
    snprintf((char *)ap.ap.ssid, sizeof(ap.ap.ssid), "Passport-%02X%02X", mac[4], mac[5]);
    ap.ap.ssid_len = strlen((char *)ap.ap.ssid);
    strlcpy((char *)ap.ap.password, "passport1234", sizeof(ap.ap.password));
    ap.ap.max_connection = 2;
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_start());
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
