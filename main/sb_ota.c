// main/sb_ota.c —— OTA 客户端实现。
#include "sb_ota.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "mbedtls/sha256.h"
#include "sb_config.h"
#include "sb_recorder.h"
#include "sb_uplink.h"
#include "sb_version.h"

static const char *TAG = "sb_ota";

#define OTA_TASK_STACK 8192
#define OTA_CHUNK      4096

static volatile sb_ota_state_t s_state = SB_OTA_IDLE;
static char s_msg[96];

static void set_msg(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_msg, sizeof(s_msg), fmt, ap);
    va_end(ap);
}

sb_ota_state_t sb_ota_state(void) { return s_state; }
const char *sb_ota_message(void) { return s_msg; }
bool sb_ota_busy(void)
{
    return s_state == SB_OTA_CHECKING || s_state == SB_OTA_DOWNLOADING;
}

// --- HTTP(明文)最小实现:只需要 GET 一个 JSON 和一个二进制 -----------------

static int http_connect(const char *host, uint16_t port)
{
    struct sockaddr_in dest = {0};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &dest.sin_addr) != 1) {
        return -1;
    }
    const int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        return -1;
    }
    struct timeval tv = {.tv_sec = 8, .tv_usec = 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    if (connect(fd, (struct sockaddr *)&dest, sizeof(dest)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static bool http_send_request(int fd, const char *host, uint16_t port, const char *path)
{
    char req[256];
    const int n = snprintf(req, sizeof(req),
                           "GET %s HTTP/1.1\r\nHost: %s:%u\r\nConnection: close\r\n"
                           "User-Agent: stackbear-ota/1\r\n\r\n",
                           path, host, (unsigned)port);
    int sent = 0;
    while (sent < n) {
        const int w = send(fd, req + sent, n - sent, 0);
        if (w <= 0) {
            return false;
        }
        sent += w;
    }
    return true;
}

// 读 header 到 \r\n\r\n,解析状态码,并把已读到的 body 前缀前移到 buf 开头。
// 返回 body 前缀长度(>=0),失败返回 -1。
static int http_read_header(int fd, char *buf, size_t cap, int *status)
{
    size_t used = 0;
    *status = 0;
    while (used < cap - 1) {
        const int r = recv(fd, buf + used, cap - 1 - used, 0);
        if (r <= 0) {
            return -1;
        }
        used += (size_t)r;
        buf[used] = '\0';
        char *end = strstr(buf, "\r\n\r\n");
        if (end != NULL) {
            if (sscanf(buf, "HTTP/%*s %d", status) != 1) {
                return -1;
            }
            const size_t body_at = (size_t)(end + 4 - buf);
            const size_t body_len = used - body_at;
            memmove(buf, buf + body_at, body_len);
            buf[body_len] = '\0';
            return (int)body_len;
        }
    }
    return -1;
}

// 从 JSON 里取一个字符串字段(够用即可,不引入 JSON 依赖)。
static bool json_str(const char *json, const char *key, char *out, size_t cap)
{
    const size_t klen = strlen(key);
    const char *p = json;
    while ((p = strstr(p, key)) != NULL) {
        p += klen;
        if (*p != '"') {
            continue;
        }
        p++;
        size_t i = 0;
        while (*p && *p != '"' && i < cap - 1) {
            out[i++] = *p++;
        }
        out[i] = '\0';
        return true;
    }
    return false;
}

static bool hex_to_bytes(const char *hex, uint8_t *out, size_t out_len)
{
    if (strlen(hex) < out_len * 2) {
        return false;
    }
    for (size_t i = 0; i < out_len; i++) {
        unsigned v = 0;
        if (sscanf(hex + i * 2, "%2x", &v) != 1) {
            return false;
        }
        out[i] = (uint8_t)v;
    }
    return true;
}

// --- 升级流程 --------------------------------------------------------------

static esp_err_t fetch_manifest(const sb_config_t *cfg, char *url_out, size_t url_cap,
                                char *sha_out, size_t sha_cap, char *ver_out, size_t ver_cap)
{
    const int fd = http_connect(cfg->srv_host, cfg->srv_port);
    if (fd < 0) {
        set_msg("cannot reach server");
        return ESP_FAIL;
    }
    esp_err_t err = ESP_FAIL;
    static char buf[1024];
    if (http_send_request(fd, cfg->srv_host, cfg->srv_port, "/firmware/latest.json")) {
        int status = 0;
        const int body = http_read_header(fd, buf, sizeof(buf), &status);
        if (body >= 0 && status == 200 &&
            json_str(buf, "\"url\"", url_out, url_cap) &&
            json_str(buf, "\"sha256\"", sha_out, sha_cap)) {
            if (!json_str(buf, "\"version\"", ver_out, ver_cap)) {
                strlcpy(ver_out, "?", ver_cap);
            }
            err = ESP_OK;
        } else {
            set_msg("bad manifest (%d)", status);
        }
    }
    close(fd);
    return err;
}

static esp_err_t download_and_flash(const sb_config_t *cfg, const char *url,
                                    const char *expected_sha)
{
    // 只接受 http://.../<path>,主机与端口固定用配置里的服务器。
    const char *scheme = "http://";
    if (strncmp(url, scheme, strlen(scheme)) != 0) {
        set_msg("unsupported url scheme");
        return ESP_ERR_INVALID_ARG;
    }
    const char *path = strchr(url + strlen(scheme), '/');
    if (path == NULL) {
        set_msg("bad url");
        return ESP_ERR_INVALID_ARG;
    }

    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    if (target == NULL) {
        set_msg("no update partition");
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "目标槽 %s (%u 字节)", target->label, (unsigned)target->size);

    const int fd = http_connect(cfg->srv_host, cfg->srv_port);
    if (fd < 0) {
        set_msg("cannot reach server");
        return ESP_FAIL;
    }
    esp_err_t err = ESP_FAIL;
    esp_ota_handle_t handle = 0;
    uint8_t *chunk = NULL;
    uint8_t digest[32] = {0};
    mbedtls_sha256_context sha;
    static char hdr[1024];

    if (!http_send_request(fd, cfg->srv_host, cfg->srv_port, path)) {
        set_msg("send failed");
        goto done;
    }
    int status = 0;
    int have = http_read_header(fd, hdr, sizeof(hdr), &status);
    if (have < 0 || status != 200) {
        set_msg("download http %d", status);
        goto done;
    }
    if (esp_ota_begin(target, OTA_SIZE_UNKNOWN, &handle) != ESP_OK) {
        set_msg("ota begin failed");
        goto done;
    }
    chunk = malloc(OTA_CHUNK);
    if (chunk == NULL) {
        set_msg("no memory");
        goto done;
    }
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);
    size_t total = 0;
    for (;;) {
        // 先消费 header 里带出的 body 前缀,再继续收。
        int n;
        if (have > 0) {
            memcpy(chunk, hdr, (size_t)have);
            n = have;
            have = 0;
        } else {
            n = recv(fd, chunk, OTA_CHUNK, 0);
        }
        if (n < 0) {
            set_msg("download failed");
            goto done;
        }
        if (n == 0) {
            break;
        }
        if (esp_ota_write(handle, chunk, (size_t)n) != ESP_OK) {
            set_msg("flash write failed");
            goto done;
        }
        mbedtls_sha256_update(&sha, chunk, (size_t)n);
        total += (size_t)n;
        set_msg("downloading %u KB", (unsigned)(total / 1024));
    }
    mbedtls_sha256_finish(&sha, digest);
    if (total < 1024) {
        set_msg("image too small");
        goto done;
    }

    uint8_t want[32];
    if (expected_sha[0] && hex_to_bytes(expected_sha, want, sizeof(want))) {
        if (memcmp(want, digest, sizeof(want)) != 0) {
            set_msg("sha256 mismatch");
            ESP_LOGE(TAG, "sha256 校验失败,放弃升级");
            goto done;
        }
        ESP_LOGI(TAG, "sha256 校验通过 (%u 字节)", (unsigned)total);
    } else {
        ESP_LOGW(TAG, "manifest 未给出可用 sha256,跳过校验");
    }

    if (esp_ota_end(handle) != ESP_OK) {
        set_msg("image invalid");
        handle = 0;
        goto done;
    }
    handle = 0;
    if (esp_ota_set_boot_partition(target) != ESP_OK) {
        set_msg("set boot failed");
        goto done;
    }
    set_msg("updated, restarting");
    err = ESP_OK;

done:
    free(chunk);
    if (handle) {
        esp_ota_abort(handle);
    }
    close(fd);
    return err;
}

static void ota_task(void *arg)
{
    (void)arg;
    const sb_config_t *cfg = sb_config_get();
    s_state = SB_OTA_CHECKING;
    set_msg("checking...");

    char url[192] = "";
    char sha[80] = "";
    char ver[48] = "";
    if (fetch_manifest(cfg, url, sizeof(url), sha, sizeof(sha), ver, sizeof(ver)) != ESP_OK) {
        s_state = SB_OTA_FAILED;
        set_msg("check failed");
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "远端版本 %s", ver);

    // 版本串里带了 git 短提交;相同即认为无需升级。
    if (strcmp(ver, sb_version_short()) == 0) {
        s_state = SB_OTA_UP_TO_DATE;
        set_msg("up to date");
        vTaskDelete(NULL);
        return;
    }

    // 升级会重启:先把录音与会话优雅停下,别把正在写的会话弄断。
    sb_recorder_set_paused(true);
    sb_uplink_end_session();
    vTaskDelay(pdMS_TO_TICKS(600));

    s_state = SB_OTA_DOWNLOADING;
    if (download_and_flash(cfg, url, sha) == ESP_OK) {
        ESP_LOGI(TAG, "升级完成,准备重启");
        vTaskDelay(pdMS_TO_TICKS(800));
        esp_restart();
    }
    // 失败:恢复录音,继续跑原固件。
    s_state = SB_OTA_FAILED;
    sb_recorder_set_paused(false);
    ESP_LOGE(TAG, "升级失败: %s", s_msg);
    vTaskDelete(NULL);
}

esp_err_t sb_ota_check_start(void)
{
    if (sb_ota_busy()) {
        return ESP_ERR_INVALID_STATE;
    }
    s_state = SB_OTA_CHECKING;
    set_msg("checking...");
    if (xTaskCreate(ota_task, "sb_ota", OTA_TASK_STACK, NULL, 4, NULL) != pdPASS) {
        s_state = SB_OTA_FAILED;
        set_msg("no memory");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
