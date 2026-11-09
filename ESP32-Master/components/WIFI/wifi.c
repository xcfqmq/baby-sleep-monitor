#include "wifi.h"
#include "debug.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include <string.h>
#include <assert.h>

/* ======================= 用户配置 ======================= */
#define WIFI_SSID               "Embedded"
#define WIFI_PASSWORD           "GJXY_130"

/* 断线重连节奏：前 3 次快速重连，之后放慢，避免 AP 长时间不在时刷屏 */
#define RECONNECT_FAST_MS       2000
#define RECONNECT_SLOW_MS       10000
#define RECONNECT_FAST_CNT      3

/* 事件组标志位：表示「已连接并且已拿到 IP」 */
#define WIFI_CONNECTED_BIT      (1 << 0)

static const char *TAG = "WIFI";

static esp_netif_t          *s_netif           = NULL;
static EventGroupHandle_t    s_wifi_events     = NULL;
static esp_timer_handle_t    s_reconnect_timer = NULL;
static int                   s_retry_count     = 0;   /* 连续重连次数，连上后清零 */

/* ---------------- 重连逻辑 ---------------- */

static void schedule_reconnect(void);   /* 前置声明 */

/* 定时器回调：真正发起重连的地方。
 * 为什么不直接在事件回调里 vTaskDelay 后重连？因为事件回调跑在系统事件循环任务里，
 * 阻塞它会拖慢整个系统，所以用 esp_timer 延迟触发。 */
static void reconnect_timer_cb(void *arg)
{
    if (WIFI_is_connected()) {          /* 已经连上了（比如刚好自己恢复了），不用重连 */
        return;
    }
    D_LOGI(TAG, "第 %d 次尝试重连 ...", s_retry_count);
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        D_LOGE(TAG, "esp_wifi_connect 失败: %s", esp_err_to_name(err));
        schedule_reconnect();           /* 连启动都失败，继续排下一次 */
    }
}

/* 安排一次延迟重连（重复调用安全：定时器已在计时则忽略） */
static void schedule_reconnect(void)
{
    s_retry_count++;
    uint32_t delay_ms = (s_retry_count <= RECONNECT_FAST_CNT) ? RECONNECT_FAST_MS : RECONNECT_SLOW_MS;
    D_LOGW(TAG, "%u ms 后进行第 %d 次重连", (unsigned)delay_ms, s_retry_count);

    esp_err_t err = esp_timer_start_once(s_reconnect_timer, (uint64_t)delay_ms * 1000);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        D_LOGE(TAG, "启动重连定时器失败: %s", esp_err_to_name(err));
    }
}

/* ---------------- 断开原因翻译 ---------------- */

/* WIFI_EVENT_STA_DISCONNECTED 只给一个数字 reason，这里翻译成人话，方便看日志定位问题 */
static const char *disconnect_reason_str(uint8_t reason)
{
    switch (reason) {
    case WIFI_REASON_UNSPECIFIED:            return "未指定原因（通常是 AP 主动把你断开/AP 正在关闭）";
    case WIFI_REASON_AUTH_EXPIRE:            return "认证过期";
    case WIFI_REASON_AUTH_LEAVE:             return "AP 要求断开认证";
    case WIFI_REASON_ASSOC_TOOMANY:          return "AP 连接的设备数已满";
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT: return "四次握手超时（最常见：密码错误）";
    case WIFI_REASON_BEACON_TIMEOUT:         return "信标丢失（AP 静默消失了）";
    case WIFI_REASON_NO_AP_FOUND:            return "找不到 AP（热点/路由器关了，或不在范围内）";
    case WIFI_REASON_AUTH_FAIL:              return "认证失败（密码错误或 AP 拒绝）";
    case WIFI_REASON_ASSOC_FAIL:             return "关联失败";
    case WIFI_REASON_HANDSHAKE_TIMEOUT:      return "握手超时";
    case WIFI_REASON_CONNECTION_FAIL:        return "连接失败";
    case WIFI_REASON_ROAMING:                return "正在漫游切换到另一个 AP";
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY: return "找不到安全方式兼容的 AP";
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD: return "AP 加密方式低于阈值要求（阈值被自动设为 WPA2）";
    case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:     return "AP 信号低于 RSSI 阈值";
    default:                                 return "其它原因";
    }
}

/* ---------------- 事件回调 ---------------- */

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_STA_START:
            D_LOGI(TAG, "STA 已启动，开始连接 \"%s\" ...", WIFI_SSID);
            if (esp_wifi_connect() != ESP_OK) {
                schedule_reconnect();
            }
            break;

        case WIFI_EVENT_STA_CONNECTED:
            D_LOGI(TAG, "链路连接成功，等待 DHCP 分配 IP ...");
            break;

        case WIFI_EVENT_STA_DISCONNECTED: {
            wifi_event_sta_disconnected_t *d = (wifi_event_sta_disconnected_t *)event_data;
            xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);
            D_LOGW(TAG, "连接断开 (reason=%d: %s)，进入自动重连", d->reason, disconnect_reason_str(d->reason));
            schedule_reconnect();
            break;
        }

        default:
            break;
        }
    } else if (event_base == IP_EVENT) {
        if (event_id == IP_EVENT_STA_GOT_IP) {
            ip_event_got_ip_t *e = (ip_event_got_ip_t *)event_data;
            char ip[16];
            esp_ip4addr_ntoa(&e->ip_info.ip, ip, sizeof(ip));
            s_retry_count = 0;                                  /* 连上了，重连计数清零 */
            xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
            D_LOGI(TAG, "已获取 IP: %s，网络就绪", ip);
        } else if (event_id == IP_EVENT_STA_LOST_IP) {
            xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);
            D_LOGW(TAG, "IP 已丢失，等待重新获取");
        }
    }
}

/* ---------------- 对外接口 ---------------- */

void WIFI_INIT(void)
{
    /* 1. 事件组：用于通知其他任务「网络是否就绪」 */
    s_wifi_events = xEventGroupCreate();
    ESP_ERROR_CHECK(s_wifi_events ? ESP_OK : ESP_ERR_NO_MEM);

    /* 2. 重连定时器（一次性） */
    const esp_timer_create_args_t timer_args = {
        .callback = reconnect_timer_cb,
        .name     = "wifi_reconnect",
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_reconnect_timer));

    /* 3. 网络接口 + 事件循环 */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_netif = esp_netif_create_default_wifi_sta();   /* IDF 5.x 必须显式创建，否则没有 DHCP */
    assert(s_netif);

    /* 4. WiFi 驱动 */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    /* 配置只放内存、不写 NVS，减少 flash 擦写 */
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    /* 按中国区规范设置，否则用不了 12/13 信道（路由器在这两个信道时会找不到 AP） */
    esp_err_t err = esp_wifi_set_country_code("CN", true);
    if (err != ESP_OK) {
        D_LOGW(TAG, "设置国家代码失败: %s（不影响使用）", esp_err_to_name(err));
    }

    /* 5. 事件回调：WIFI_EVENT 收全部；IP 事件分别收「拿到 IP」和「丢失 IP」 */
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_LOST_IP, &wifi_event_handler, NULL));

    /* 6. 配置并启动 */
    wifi_config_t wifi_config = {
        .sta = {
            .ssid     = WIFI_SSID,
            .password = WIFI_PASSWORD,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

    /* 关闭省电模式：响应更及时、丢包更少（电池供电项目可改回 WIFI_PS_MIN_MODEM） */
    err = esp_wifi_set_ps(WIFI_PS_NONE);
    if (err != ESP_OK) {
        D_LOGW(TAG, "设置省电模式失败: %s", esp_err_to_name(err));
    }

    ESP_ERROR_CHECK(esp_wifi_start());

    /* 信标丢失判定时间：AP 静默消失时，多久判定为掉线。
     * 注意：必须在 esp_wifi_start() 之后调用，否则返回 ESP_ERR_WIFI_NOT_STARTED。
     * IDF 文档里 STA 默认为 6 秒，这里设为 5 秒（STA 最小允许 3 秒），发现得更快一点。 */
    err = esp_wifi_set_inactive_time(WIFI_IF_STA, 5);
    if (err != ESP_OK) {
        D_LOGW(TAG, "设置信标丢失超时失败: %s", esp_err_to_name(err));
    }
}

bool WIFI_wait_ip(uint32_t timeout_ms)
{
    if (s_wifi_events == NULL) {
        return false;
    }
    TickType_t ticks = (timeout_ms == 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    EventBits_t bits = xEventGroupWaitBits(s_wifi_events, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, ticks);
    return (bits & WIFI_CONNECTED_BIT) != 0;
}

bool WIFI_is_connected(void)
{
    return (s_wifi_events != NULL) &&
           ((xEventGroupGetBits(s_wifi_events) & WIFI_CONNECTED_BIT) != 0);
}

const char *WIFI_get_ip_str(void)
{
    static char buf[16];
    esp_netif_ip_info_t ip_info;

    if (s_netif != NULL &&
        esp_netif_get_ip_info(s_netif, &ip_info) == ESP_OK &&
        ip_info.ip.addr != 0) {
        esp_ip4addr_ntoa(&ip_info.ip, buf, sizeof(buf));
    } else {
        strcpy(buf, "0.0.0.0");
    }
    return buf;
}
