#include "mqtt.h"
#include "broker_disc.h"
#include "debug.h"
#include "mqtt_client.h"
#include <stdio.h>
#include <string.h>

/* ---- broker 地址：优先 UDP 广播自动发现，失败则用备用地址 ---- */
#define MQTT_FALLBACK_IP     "192.168.1.101"   /* ← 发现失败时的兜底：改成你电脑的 IP */
#define MQTT_BROKER_PORT     1883
#define MQTT_DISC_TIMEOUT_MS 10000             /* 电脑每 2 秒广播一次，等 10 秒足够 */

static char s_broker_uri[64];   /* 必须在 mqtt 客户端生命周期内持续有效 */

#define MQTT_CLIENT_ID     "esp32s3_vibration"
#define MQTT_RECONNECT_MS  5000   /* 断线后重连间隔，esp-mqtt 默认会自动重连 */

static const char *TAG = "MQTT";

static esp_mqtt_client_handle_t s_client    = NULL;
static bool                     s_connected = false;

/* MQTT 事件回调：连接/断开/出错都在这里，用来维护 s_connected 状态 */
static void mqtt_event_handler(void *arg, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_mqtt_event_t *ev = (esp_mqtt_event_t *)event_data;

    switch ((esp_mqtt_event_id_t)event_id)
    {
        case MQTT_EVENT_CONNECTED:
          s_connected = true;
          D_LOGI(TAG, "已连接到 broker");
          break;

        case MQTT_EVENT_DISCONNECTED:
          s_connected = false;
          D_LOGW(TAG, "与 broker 断开，等待自动重连 ...");
          break;

       case MQTT_EVENT_ERROR:
        {
          s_connected = false;
          /* error_type 说明错误来源：TCP 传输层 / 连接被拒绝 / 订阅失败 */
          int err_type = (ev->error_handle) ? (int)ev->error_handle->error_type : -1;
          D_LOGE(TAG, "MQTT 错误 (error_type=%d，检查 broker 地址是否正确)", err_type);
          break;
         }

       default:
         break;
    }
}

void MQTT_init(void)
{
    char     ip[32] = {0};
    uint16_t port   = MQTT_BROKER_PORT;

    if (broker_disc_find(ip, sizeof(ip), &port, MQTT_DISC_TIMEOUT_MS) == ESP_OK) {
        D_LOGI(TAG, "自动发现 broker: %s:%u", ip, (unsigned)port);
    } else {
        D_LOGW(TAG, "自动发现失败，使用备用 broker: %s:%u",
                 MQTT_FALLBACK_IP, (unsigned)MQTT_BROKER_PORT);
        strncpy(ip, MQTT_FALLBACK_IP, sizeof(ip) - 1);
        port = MQTT_BROKER_PORT;
    }
    snprintf(s_broker_uri, sizeof(s_broker_uri), "mqtt://%s:%u", ip, (unsigned)port);

    const esp_mqtt_client_config_t cfg = {
        .broker.address.uri            = s_broker_uri,
        .credentials.client_id         = MQTT_CLIENT_ID,
        .session.keepalive             = 60,
        .network.reconnect_timeout_ms  = MQTT_RECONNECT_MS,
    };

    s_client = esp_mqtt_client_init(&cfg);
    ESP_ERROR_CHECK(s_client ? ESP_OK : ESP_ERR_NO_MEM);

    ESP_ERROR_CHECK(esp_mqtt_client_register_event(s_client, MQTT_EVENT_CONNECTED,    &mqtt_event_handler, NULL));
    ESP_ERROR_CHECK(esp_mqtt_client_register_event(s_client, MQTT_EVENT_DISCONNECTED, &mqtt_event_handler, NULL));
    ESP_ERROR_CHECK(esp_mqtt_client_register_event(s_client, MQTT_EVENT_ERROR,        &mqtt_event_handler, NULL));

    ESP_ERROR_CHECK(esp_mqtt_client_start(s_client));
}

bool MQTT_is_connected(void)
{
    return s_connected;
}

int MQTT_publish(const char *topic, const char *payload)
{
    if (s_client == NULL || !s_connected) {
        return -1;
    }
    return esp_mqtt_client_publish(s_client, topic, payload, (int)strlen(payload), 0, 0);
}
