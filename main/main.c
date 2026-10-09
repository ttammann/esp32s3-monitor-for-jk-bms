// JK BMS (hw V19) RS485 monitor -> MQTT + Home Assistant discovery.
//
// One protocol, one build. The BMS masters its own bus and pushes 55AA frames;
// this firmware listens and republishes. There is no write path to the BMS and
// no Modbus master -- the UART is opened without a TX pin, so read-only is a
// property of the wiring rather than a promise.
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include "net.h"
#include "listen.h"

static const char *TAG = "jkmon";

// Settings are compiled in. An unset placeholder otherwise shows up as
// "Haven't to connect to a suitable AP" or a DNS failure, which reads like a
// network problem rather than a configuration one.
static void warn_unconfigured(void)
{
    bool ok = true;
    if (strstr(CONFIG_JK_WIFI_SSID, "SET_ME") != NULL) {
        ESP_LOGE(TAG, "Wi-Fi SSID is still the placeholder (\"%s\")",
                 CONFIG_JK_WIFI_SSID);
        ok = false;
    }
    // Checked but never printed. A placeholder is worth naming; the value that
    // eventually replaces it is not something to put in the console log.
    //
    // This one matters more than it looks: a correct SSID with an unset
    // password is the likelier half-configured state -- the SSID is the part
    // you remember to set -- and it produces exactly the association failure
    // this function exists to pre-empt.
    if (strstr(CONFIG_JK_WIFI_PASS, "SET_ME") != NULL) {
        ESP_LOGE(TAG, "Wi-Fi password is still the placeholder");
        ok = false;
    }
    if (strstr(CONFIG_JK_MQTT_URI, "SET_ME") != NULL) {
        ESP_LOGE(TAG, "MQTT broker URI is still the placeholder (\"%s\")",
                 CONFIG_JK_MQTT_URI);
        ok = false;
    }
    // MQTT username and password are deliberately not checked: empty is a
    // valid, documented configuration for an anonymous broker.
    if (!ok) {
        ESP_LOGE(TAG, "these are BUILD-TIME settings: run 'idf.py menuconfig' "
                      "-> JK BMS Monitor Configuration, then rebuild AND "
                      "reflash. Editing them does nothing until you do.");
    }
}

static void listen_task(void *arg)
{
    (void)arg;
    jk_listen_run();     // does not return
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    ESP_LOGI(TAG, "jk-bms-monitor starting, device %s", net_device_id());
    warn_unconfigured();

    // Order matters. The listener must not depend on the network: it has to run
    // for bench bring-up with no Wi-Fi configured at all, and a battery monitor
    // that stops reading the battery because the AP is down is worse than
    // useless. MQTT publishes are no-ops until connected.
    //
    // 8 KB rather than 6: the settings reporter formats a 640-byte JSON payload
    // on the stack, on top of vsnprintf's own double-formatting workspace.
    xTaskCreate(listen_task, "jklisten", 8192, NULL, 5, NULL);

    net_wifi_start();     // returns immediately, associates in background
    net_mqtt_start();     // esp-mqtt retries on its own until the link is up

    if (!net_wifi_wait_ip(20000)) {
        ESP_LOGW(TAG, "no IP after 20s; listening and logging continue locally");
    }
}
