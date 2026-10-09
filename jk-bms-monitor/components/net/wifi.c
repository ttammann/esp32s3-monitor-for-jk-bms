#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#include "net.h"

static const char *TAG = "wifi";
static EventGroupHandle_t s_evt;
#define GOT_IP_BIT BIT0

const char *net_device_id(void)
{
    static char id[16];
    if (!id[0]) {
        uint8_t mac[6];
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        snprintf(id, sizeof(id), "jkbms-%02x%02x%02x", mac[3], mac[4], mac[5]);
    }
    return id;
}

// Reconnect backoff. This must NOT be a vTaskDelay inside the handler: event
// handlers run on the shared default-loop task, so sleeping here stalls every
// other event in the system -- including the IP event we are waiting for.
//
// All retry state is owned by the default event loop. The timer callback
// only posts JK_WIFI_RETRY back to it, so the connect, the re-arm and the
// backoff all run on one task, ordered against GOT_IP, with no lock.
ESP_EVENT_DEFINE_BASE(JK_WIFI_RETRY);
static esp_timer_handle_t s_retry;
#define RETRY_MS_MIN 2000
#define RETRY_MS_MAX 30000
static uint32_t s_retry_ms = RETRY_MS_MIN;

static void on_retry_timer(void *arg)
{
    (void)arg;
    if (esp_event_post(JK_WIFI_RETRY, 0, NULL, 0, 0) != ESP_OK) {
        // Event queue full. Dropping this would end the retry chain, so try
        // again later; esp_timer allows re-arming from its own callback.
        esp_timer_start_once(s_retry, (uint64_t)RETRY_MS_MAX * 1000);
    }
}

// Nothing re-arms the timer except this, so every path that leaves the board
// unconnected must come through here or the board stays offline until a
// power cycle.
static void schedule_retry(const char *why)
{
    const esp_err_t err = esp_timer_start_once(s_retry,
                                               (uint64_t)s_retry_ms * 1000);
    if (err == ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "%s, retry already pending", why);
        return;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s, and could not arm reconnect timer (%s); Wi-Fi "
                      "will not retry until reboot", why, esp_err_to_name(err));
        return;
    }
    ESP_LOGW(TAG, "%s, retrying in %lums", why, (unsigned long)s_retry_ms);
    // Back off so a wrong password doesn't hammer the radio forever. Only
    // when a new shot was armed: a pending one must not be doubled again.
    s_retry_ms = (s_retry_ms * 2 > RETRY_MS_MAX) ? RETRY_MS_MAX
                                                 : s_retry_ms * 2;
}

// Every failure re-arms, ESP_ERR_WIFI_CONN included: IDF documents it as an
// internal control-block error, with no promise of a disconnect event to
// follow.
static void try_connect(void)
{
    // A retry queued before GOT_IP was handled must not poke a working
    // station.
    if (xEventGroupGetBits(s_evt) & GOT_IP_BIT) {
        return;
    }
    const esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        char why[64];
        snprintf(why, sizeof(why), "connect failed (%s)",
                 esp_err_to_name(err));
        schedule_retry(why);
    }
}

static void on_wifi(void *arg, esp_event_base_t base, int32_t ev, void *data)
{
    (void)arg;
    if (base == JK_WIFI_RETRY ||
        (base == WIFI_EVENT && ev == WIFI_EVENT_STA_START)) {
        try_connect();
    } else if (base == WIFI_EVENT && ev == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_evt, GOT_IP_BIT);
        schedule_retry("disconnected");
    } else if (base == IP_EVENT && ev == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_evt, GOT_IP_BIT);
        esp_timer_stop(s_retry);        // not running is fine; nothing to stop
        s_retry_ms = RETRY_MS_MIN;
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "got ip " IPSTR, IP2STR(&e->ip_info.ip));
    }
}

void net_wifi_start(void)
{
    s_evt = xEventGroupCreate();
    const esp_timer_create_args_t targs = {
        .callback = on_retry_timer,
        .name     = "wifi_retry",
    };
    ESP_ERROR_CHECK(esp_timer_create(&targs, &s_retry));
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               on_wifi, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               on_wifi, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(JK_WIFI_RETRY, ESP_EVENT_ANY_ID,
                                               on_wifi, NULL));

    wifi_config_t cfg = { 0 };
    strlcpy((char *)cfg.sta.ssid, CONFIG_JK_WIFI_SSID, sizeof(cfg.sta.ssid));
    strlcpy((char *)cfg.sta.password, CONFIG_JK_WIFI_PASS,
            sizeof(cfg.sta.password));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Power save off, deliberately.
    //
    // IDF defaults to WIFI_PS_MIN_MODEM, which sleeps between beacons and uses
    // the beacon's TIM element to learn when the AP has buffered traffic. The
    // AP serving this SSID does not provide one -- observed on hardware
    // 2026-08-31, the driver logs "There is no TIM IE in Beacon frame" and
    // "Beacon interval IE of Beacon frame is 0", then a bcn_timeout. Sleeping
    // against a beacon we cannot read is how an unattended monitor silently
    // drops off the network for hours.
    //
    // The cost is idle current, which does not matter: this board is mains
    // powered off the same pack it is watching.
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    ESP_LOGI(TAG, "connecting to \"%s\" as %s (non-blocking, power save off)",
             CONFIG_JK_WIFI_SSID, net_device_id());
}

bool net_wifi_wait_ip(uint32_t timeout_ms)
{
    EventBits_t b = xEventGroupWaitBits(s_evt, GOT_IP_BIT, pdFALSE, pdTRUE,
                                        pdMS_TO_TICKS(timeout_ms));
    return (b & GOT_IP_BIT) != 0;
}

int net_wifi_rssi(void)
{
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        return ap.rssi;
    }
    return 0;
}
