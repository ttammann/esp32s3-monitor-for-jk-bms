#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "mqtt_client.h"
#include "sdkconfig.h"
#include "net.h"

static const char *TAG = "mqtt";
static esp_mqtt_client_handle_t s_client;
static volatile bool s_connected;
static volatile uint32_t s_epoch;
static char s_t_state[48], s_t_status[48], s_t_bms[48], s_t_settings[48];

const char *net_topic_state(void)      { return s_t_state; }
const char *net_topic_status(void)     { return s_t_status; }
const char *net_topic_bms_status(void) { return s_t_bms; }
const char *net_topic_settings(void)   { return s_t_settings; }

static void on_mqtt(void *arg, esp_event_base_t base, int32_t ev, void *data)
{
    switch ((esp_mqtt_event_id_t)ev) {
    case MQTT_EVENT_CONNECTED:
        s_connected = true;
        s_epoch++;
        ESP_LOGI(TAG, "connected to %s", CONFIG_JK_MQTT_URI);
        esp_mqtt_client_publish(s_client, s_t_status, "online", 0, 1, 1);
        ha_discovery_publish_all();
        break;
    case MQTT_EVENT_DISCONNECTED:
        s_connected = false;
        ESP_LOGW(TAG, "disconnected");
        break;
    case MQTT_EVENT_ERROR: {
        // "error event" told you nothing. The usual causes here are a refused
        // connection (bad credentials) and a dead socket; they need different
        // fixes, so say which.
        const esp_mqtt_event_handle_t e = (esp_mqtt_event_handle_t)data;
        if (e->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
            ESP_LOGW(TAG, "transport error: sock_errno=%d",
                     e->error_handle->esp_transport_sock_errno);
        } else if (e->error_handle->error_type ==
                   MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
            ESP_LOGE(TAG, "connection refused (0x%x) - check user/password",
                     e->error_handle->connect_return_code);
        } else {
            ESP_LOGW(TAG, "error type %d", e->error_handle->error_type);
        }
        break;
    }
    default:
        break;
    }
}

void net_mqtt_start(void)
{
    const char *id = net_device_id();
    snprintf(s_t_state,  sizeof(s_t_state),  "jkbms/%s/state", id);
    snprintf(s_t_status, sizeof(s_t_status), "jkbms/%s/status", id);
    snprintf(s_t_bms,    sizeof(s_t_bms),    "jkbms/%s/bms_status", id);
    snprintf(s_t_settings, sizeof(s_t_settings), "jkbms/%s/settings", id);

    const esp_mqtt_client_config_t cfg = {
        .broker.address.uri = CONFIG_JK_MQTT_URI,
        // Empty strings must mean "no credentials". Passing "" makes the
        // client authenticate as an empty user, which a broker rejects with a
        // confusing "not authorized" rather than connecting anonymously.
        .credentials.username =
            CONFIG_JK_MQTT_USER[0] ? CONFIG_JK_MQTT_USER : NULL,
        .credentials.authentication.password =
            CONFIG_JK_MQTT_PASS[0] ? CONFIG_JK_MQTT_PASS : NULL,
        .credentials.client_id = id,
        .session.keepalive = 30,
        .session.last_will = {
            .topic = s_t_status,
            .msg = "offline",
            .msg_len = 7,
            .qos = 1,
            .retain = 1,
        },
    };
    s_client = esp_mqtt_client_init(&cfg);
    ESP_ERROR_CHECK(esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID,
                                                   on_mqtt, NULL));
    ESP_ERROR_CHECK(esp_mqtt_client_start(s_client));
}

bool net_mqtt_connected(void)
{
    return s_connected;
}

uint32_t net_mqtt_epoch(void)
{
    return s_epoch;
}

int net_mqtt_publish(const char *topic, const char *payload,
                     int qos, int retain)
{
    if (!s_connected) {
        // Expected before the broker is up and again after every disconnect,
        // so this is DEBUG rather than a warning. The listener deliberately
        // starts before the network and publishes into the void until MQTT
        // catches up.
        ESP_LOGD(TAG, "not connected, dropped publish to %s", topic);
        return -1;
    }
    const int id = esp_mqtt_client_publish(s_client, topic, payload, 0, qos,
                                           retain);
    if (id < 0) {
        // Connected but the write failed: that is worth hearing about, because
        // no caller can distinguish it from "not connected" by return alone.
        ESP_LOGW(TAG, "publish to %s failed", topic);
    }
    return id;
}
