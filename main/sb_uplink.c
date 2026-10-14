// main/sb_uplink.c —— TCP 上传任务。
//
// 结构:采集任务把 Opus 帧 push 进队列,本任务的唯一职责是"连接 + 发送 + 收 ACK"。
// 这样网络抖动/重连都不会阻塞录音采集;连接断开时帧被丢弃(实时音频,补发无意义),
// 由 dropped 计数在界面上暴露。
#include "sb_uplink.h"

#include <string.h>

#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "sb_config.h"
#include "sb_diag.h"
#include "sb_adpcm.h"
#include "sb_proto.h"

static const char *TAG = "sb_uplink";

#define SB_UL_QUEUE_LEN   24                 // 24 x 60ms ≈ 1.4s 缓冲
#define SB_UL_TASK_STACK  4096
#define SB_UL_CONNECT_WAIT_MS 1500

typedef struct {
    uint16_t len;
    uint8_t  flags;
    uint8_t  data[SB_PROTO_MAX_PAYLOAD];
} sb_ul_item_t;

static QueueHandle_t s_queue;
static TaskHandle_t s_task;
static volatile sb_uplink_state_t s_state = SB_UL_IDLE;
static volatile bool s_running;
static volatile bool s_need_hello;
static volatile bool s_end_requested;
static volatile uint32_t s_sent;
static volatile uint32_t s_dropped;
static uint16_t s_seq;

static int connect_to_server(void)
{
    const sb_config_t *cfg = sb_config_get();
    struct sockaddr_in dest = {0};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(cfg->srv_port);
    if (inet_pton(AF_INET, cfg->srv_host, &dest.sin_addr) != 1) {
        ESP_LOGW(TAG, "服务器地址不是 IPv4: %s", cfg->srv_host);
        return -1;
    }
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        return -1;
    }
    struct timeval tv = {.tv_sec = 4, .tv_usec = 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    if (connect(fd, (struct sockaddr *)&dest, sizeof(dest)) != 0) {
        close(fd);
        return -1;
    }
    ESP_LOGI(TAG, "已连接 %s:%u", cfg->srv_host, (unsigned)cfg->srv_port);
    return fd;
}

// 返回 false 表示这条连接已经不可用(对端关闭/发不出去),调用方应重连。
// 之前这里静默吞掉发送错误,导致服务端重启后设备只是"往死 socket 里写",
// 永远不会重新建立连接。
static bool send_frame(int fd, uint8_t type, uint8_t flags, uint16_t seq,
                       const uint8_t *payload, uint16_t len)
{
    uint8_t buf[SB_PROTO_MAX_FRAME];
    const size_t n = sb_proto_encode(buf, sizeof(buf), type, flags, seq, payload, len);
    if (n == 0) {
        return true;  // 编码失败(不该发生)不是连接问题
    }
    size_t off = 0;
    while (off < n) {
        const int w = send(fd, buf + off, n - off, 0);
        if (w <= 0) {
            return false;
        }
        off += (size_t)w;
    }
    return true;
}

static bool send_hello(int fd)
{
    const sb_config_t *cfg = sb_config_get();
    g_sb_diag.free_heap = esp_get_free_heap_size();
    char hello[256];
    const int n = snprintf(hello, sizeof(hello),
                           "{\"dev\":\"%s\",\"v\":1,\"codec\":\"adpcm\",\"sample_rate\":%d,"
                           "\"channels\":1,\"frame_ms\":%d,\"boot_ms\":%u,"
                           "\"rec_err\":%d,\"opus_err\":%d,\"heap\":%u}",
                           cfg->dev_id, SB_AUDIO_RATE, SB_AUDIO_FRAME_MS,
                           (unsigned)(xTaskGetTickCount() * portTICK_PERIOD_MS),
                           (int)g_sb_diag.rec_err, (int)g_sb_diag.opus_err,
                           (unsigned)g_sb_diag.free_heap);
    s_seq = 0;
    if (n <= 0) {
        return true;
    }
    return send_frame(fd, SB_FRAME_HELLO, 0, 0, (const uint8_t *)hello, (uint16_t)n);
}

static void drain_acks(int fd)
{
    uint8_t buf[256];
    // 非阻塞排空:ACK 只是流控信息,丢了不影响音频。
    for (;;) {
        const int r = recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
        if (r <= 0) {
            return;
        }
    }
}

static void uplink_task(void *arg)
{
    (void)arg;
    int fd = -1;
    sb_ul_item_t item;
    while (s_running) {
        if (fd < 0) {
            s_state = SB_UL_CONNECTING;
            fd = connect_to_server();
            if (fd < 0) {
                s_state = SB_UL_ERROR;
                vTaskDelay(pdMS_TO_TICKS(SB_UL_CONNECT_WAIT_MS));
                continue;
            }
            s_state = SB_UL_UP;
            s_need_hello = true;
        }
        if (s_need_hello) {
            if (!send_hello(fd)) {
                close(fd);
                fd = -1;
                s_state = SB_UL_ERROR;
                vTaskDelay(pdMS_TO_TICKS(SB_UL_CONNECT_WAIT_MS));
                continue;
            }
            s_need_hello = false;
        }

        if (xQueueReceive(s_queue, &item, pdMS_TO_TICKS(200)) == pdTRUE) {
            if (!send_frame(fd, SB_FRAME_AUDIO, item.flags, s_seq, item.data, item.len)) {
                close(fd);
                fd = -1;
                s_state = SB_UL_ERROR;
                vTaskDelay(pdMS_TO_TICKS(SB_UL_CONNECT_WAIT_MS));
                continue;
            }
            s_seq++;
            s_sent++;
            drain_acks(fd);
        } else {
            // 空闲时发一个 PING 保活,让 NAT/热点不要过早断链。
            if (!send_frame(fd, SB_FRAME_PING, 0, s_seq, NULL, 0)) {
                close(fd);
                fd = -1;
                s_state = SB_UL_ERROR;
                vTaskDelay(pdMS_TO_TICKS(SB_UL_CONNECT_WAIT_MS));
                continue;
            }
            drain_acks(fd);
        }

        if (s_end_requested && uxQueueMessagesWaiting(s_queue) == 0) {
            (void)send_frame(fd, SB_FRAME_END, SB_FLAG_LAST, s_seq, NULL, 0);
            drain_acks(fd);
            close(fd);
            fd = -1;
            s_end_requested = false;
            s_state = SB_UL_IDLE;
        }
    }
    if (fd >= 0) {
        close(fd);
    }
    if (s_queue != NULL) {
        vQueueDelete(s_queue);
        s_queue = NULL;
    }
    s_task = NULL;
    s_running = false;
    s_state = SB_UL_IDLE;
    vTaskDelete(NULL);
}

esp_err_t sb_uplink_start(void)
{
    if (s_running) {
        return ESP_OK;
    }
    s_queue = xQueueCreate(SB_UL_QUEUE_LEN, sizeof(sb_ul_item_t));
    if (s_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }
    s_running = true;
    s_need_hello = true;
    if (xTaskCreate(uplink_task, "sb_uplink", SB_UL_TASK_STACK, NULL, 4, &s_task) != pdPASS) {
        s_running = false;
        vQueueDelete(s_queue);
        s_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void sb_uplink_stop(void)
{
    if (!s_running) {
        return;
    }
    s_running = false;
    // 等待任务自行收尾(它会删队列并把 s_task 清空)。
    for (int i = 0; i < 50 && s_task != NULL; i++) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

sb_uplink_state_t sb_uplink_state(void)
{
    return s_state;
}

uint32_t sb_uplink_frames_sent(void)
{
    return s_sent;
}

uint32_t sb_uplink_frames_dropped(void)
{
    return s_dropped;
}

int sb_uplink_queue_len(void)
{
    return s_queue ? (int)uxQueueMessagesWaiting(s_queue) : 0;
}

void sb_uplink_begin_session(void)
{
    s_need_hello = true;
    s_end_requested = false;
}

void sb_uplink_end_session(void)
{
    s_end_requested = true;
}

bool sb_uplink_push_audio(const uint8_t *data, size_t len, uint8_t flags)
{
    if (s_queue == NULL || data == NULL || len == 0 || len > SB_PROTO_MAX_PAYLOAD) {
        return false;
    }
    sb_ul_item_t item;
    item.len = (uint16_t)len;
    item.flags = flags;
    memcpy(item.data, data, len);
    if (xQueueSend(s_queue, &item, 0) != pdTRUE) {
        s_dropped++;
        return false;
    }
    return true;
}
