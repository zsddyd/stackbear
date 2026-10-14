// main/sb_config.c —— 基于 NVS 的配置存储。
#include "sb_config.h"

#include <string.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "sb_config";
static const char *NS = "stackbear";

static sb_config_t s_cfg;
static bool s_ready;

static nvs_handle_t open_ns(void)
{
    nvs_handle_t h = 0;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) {
        return 0;
    }
    return h;
}

static void load_str(nvs_handle_t h, const char *key, char *dst, size_t cap, const char *def)
{
    size_t len = cap;
    if (nvs_get_str(h, key, dst, &len) != ESP_OK) {
        strlcpy(dst, def, cap);
    }
}

static void make_default_dev_id(char *out, size_t cap)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(out, cap, "%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

esp_err_t sb_config_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // 分区被占满或版本不匹配:擦除后重来。这里只影响本应用自己的键。
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        return err;
    }

    memset(&s_cfg, 0, sizeof(s_cfg));
    nvs_handle_t h = open_ns();
    if (h == 0) {
        return ESP_ERR_NVS_NOT_INITIALIZED;
    }
    load_str(h, "ssid", s_cfg.ssid, sizeof(s_cfg.ssid), "");
    load_str(h, "pass", s_cfg.pass, sizeof(s_cfg.pass), "");
    load_str(h, "host", s_cfg.srv_host, sizeof(s_cfg.srv_host), SB_DEFAULT_SRV_HOST);
    uint16_t port = SB_DEFAULT_SRV_PORT;
    nvs_get_u16(h, "port", &port);
    s_cfg.srv_port = port ? port : SB_DEFAULT_SRV_PORT;
    load_str(h, "devid", s_cfg.dev_id, sizeof(s_cfg.dev_id), "");
    if (s_cfg.dev_id[0] == '\0') {
        make_default_dev_id(s_cfg.dev_id, sizeof(s_cfg.dev_id));
        nvs_set_str(h, "devid", s_cfg.dev_id);
    }
    s_cfg.provisioned = (s_cfg.ssid[0] != '\0');
    nvs_commit(h);
    nvs_close(h);
    s_ready = true;
    ESP_LOGI(TAG, "配置就绪: dev=%s srv=%s:%u ssid=%s",
             s_cfg.dev_id, s_cfg.srv_host, (unsigned)s_cfg.srv_port,
             s_cfg.ssid[0] ? s_cfg.ssid : "(未配网)");
    return ESP_OK;
}

const sb_config_t *sb_config_get(void)
{
    return &s_cfg;
}

esp_err_t sb_config_set_wifi(const char *ssid, const char *pass)
{
    if (ssid == NULL || pass == NULL || ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h = open_ns();
    if (h == 0) {
        return ESP_ERR_NVS_NOT_INITIALIZED;
    }
    ESP_ERROR_CHECK(nvs_set_str(h, "ssid", ssid));
    ESP_ERROR_CHECK(nvs_set_str(h, "pass", pass));
    esp_err_t err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK) {
        strlcpy(s_cfg.ssid, ssid, sizeof(s_cfg.ssid));
        strlcpy(s_cfg.pass, pass, sizeof(s_cfg.pass));
        s_cfg.provisioned = true;
    }
    return err;
}

esp_err_t sb_config_set_server(const char *host, uint16_t port)
{
    if (host == NULL || host[0] == '\0' || port == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h = open_ns();
    if (h == 0) {
        return ESP_ERR_NVS_NOT_INITIALIZED;
    }
    ESP_ERROR_CHECK(nvs_set_str(h, "host", host));
    ESP_ERROR_CHECK(nvs_set_u16(h, "port", port));
    esp_err_t err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK) {
        strlcpy(s_cfg.srv_host, host, sizeof(s_cfg.srv_host));
        s_cfg.srv_port = port;
    }
    return err;
}

esp_err_t sb_config_clear_wifi(void)
{
    nvs_handle_t h = open_ns();
    if (h == 0) {
        return ESP_ERR_NVS_NOT_INITIALIZED;
    }
    nvs_erase_key(h, "ssid");
    nvs_erase_key(h, "pass");
    esp_err_t err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK) {
        s_cfg.ssid[0] = '\0';
        s_cfg.pass[0] = '\0';
        s_cfg.provisioned = false;
    }
    return err;
}
