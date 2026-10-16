// main/sb_net.h —— WiFi 连接与 SoftAP 配网。
#pragma once

#include "esp_err.h"
#include <stdbool.h>

typedef enum {
    SB_NET_DOWN = 0,   // 未连接
    SB_NET_CONNECTING,
    SB_NET_UP,         // 已连上热点并拿到 IP
} sb_net_state_t;

// 初始化 netif/事件/默认事件循环(幂等)。
esp_err_t sb_net_init(void);

// 以 STA 连接指定热点。已连接则先断开。失败返回错误。
esp_err_t sb_net_sta_connect(const char *ssid, const char *pass);

// 确保正在连接或已连接(幂等)。若无凭据或正在配网,返回 ESP_ERR_INVALID_STATE。
// 调用方(应用主循环)只需周期性调用它,不必自己实现重试:断开后的重连由本模块
// 用指数退避在后台完成。
esp_err_t sb_net_ensure_connected(void);

// 从已保存的配置载入 WiFi 凭据,之后 sb_net_ensure_connected() 才能工作。
// 必须在 sb_net_init() 之后、进入应用循环之前调用一次。
//
// 之前遗漏了这一步:ensure_connected() 只看内存里的 s_ssid,而 s_ssid 只在
// 主动调用 sb_net_sta_connect() 时才被赋值。结果每次重启后设备都不会使用 NVS
// 里已保存的凭据,看起来就像"又要重新配网"。
esp_err_t sb_net_load_saved(void);

// 是否正在后台等待重连(退避计时中)。用于界面/日志区分"闲置"与"重连中"。
bool sb_net_reconnecting(void);

void sb_net_sta_disconnect(void);
sb_net_state_t sb_net_state(void);
const char *sb_net_ip(void);   // 未连接返回 ""

// 进入配网模式:开 SoftAP + 本地配置网页。
// 手机连上 SoftAP 后访问 http://192.168.4.1/ ,网页里选择热点并输入密码;
// 设备验证能连上后才写入 NVS,然后关闭 SoftAP/网页。
// 返回 ESP_OK 表示配网服务已启动(不代表已配网成功)。
esp_err_t sb_net_provision_start(void);

// 关闭配网服务(SoftAP + HTTP)。
esp_err_t sb_net_provision_stop(void);

// 配网结果:0=进行中,1=成功(凭据已保存),-1=用户取消/超时。
int sb_net_provision_result(void);
