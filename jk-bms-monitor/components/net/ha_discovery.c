// Home Assistant MQTT discovery.
//
// Two state topics. Most entities point at the per-cycle state JSON (one per
// ~6.4 s scan cycle, not 1 Hz -- the BMS sets the cadence, not us) with a
// value_template selecting their field. A few point at the retained settings
// topic instead, which publishes only when the pack's configuration actually
// changes.
//
// That difference drives availability: a state entity carries expire_after, so
// a wedged publisher shows as unavailable. A settings entity must not, because
// "unchanged for an hour" is its normal condition and expiry would blank it.
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "sdkconfig.h"
#include "net.h"
#include "strbuf.h"

static const char *TAG = "ha";

typedef struct {
    const char *key;      // JSON field in the payload
    const char *name;
    const char *unit;
    const char *dev_cla;  // HA device_class, or NULL
    const char *stat_cla; // HA state_class, or NULL to omit
    bool diagnostic;      // entity_category: diagnostic
    bool settings;        // reads the settings topic instead of state
    // Icon, or NULL to let HA choose. Only worth setting where there is no
    // device_class to infer one from: HA falls back to a generic eye for those,
    // which tells the reader nothing. Anything with a voltage/current/
    // temperature/duration class already picks a sensible icon on its own.
    const char *icon;
    // Complete value template, or NULL for the default "{{ value_json.<key> }}".
    // Spelled out in full rather than as a format string, so no non-literal
    // ever reaches sb_addf's printf format checking.
    const char *tpl;
} ent_t;

#define MEAS "measurement"
#define TOTI "total_increasing"

static const ent_t ENTS[] = {
    // --- live pack state ---
    { "pack_v",       "Pack voltage",    "V",   "voltage",         MEAS, false, false, .icon = NULL },
    { "current_a",    "Current",         "A",   "current",         MEAS, false, false, .icon = NULL },
    { "power_w",      "Power",           "W",   "power",           MEAS, false, false, .icon = NULL },
    { "soc",          "State of charge", "%",   "battery",         MEAS, false, false, .icon = NULL },
    { "soh",          "State of health", "%",   NULL,              MEAS, true,  false,
      .icon = "mdi:heart-pulse" },
    { "cell_min_v",   "Cell min",        "V",   "voltage",         MEAS, false, false, .icon = NULL },
    { "cell_max_v",   "Cell max",        "V",   "voltage",         MEAS, false, false, .icon = NULL },
    { "cell_delta_v", "Cell delta",      "V",   "voltage",         MEAS, false, false, .icon = NULL },
    { "cell_avg_v",   "Cell average",    "V",   "voltage",         MEAS, true,  false, .icon = NULL },
    { "temp_mos",     "MOSFET temp",     "°C",  "temperature",     MEAS, false, false, .icon = NULL },
    { "temp1",        "Temperature 1",   "°C",  "temperature",     MEAS, false, false, .icon = NULL },
    { "temp2",        "Temperature 2",   "°C",  "temperature",     MEAS, false, false, .icon = NULL },
    { "balance_a",    "Balance current", "A",   "current",         MEAS, true,  false, .icon = NULL },
    // --- capacity and cycle life, from the 55AA frame ---
    { "remain_ah",    "Remaining",       "Ah",  NULL,              MEAS, false, false,
      .icon = "mdi:battery-50" },
    { "full_ah",      "Full capacity",   "Ah",  NULL,              MEAS, true,  false,
      .icon = "mdi:battery" },
    { "cycles",       "Cycle count",     NULL,  NULL,              TOTI, true,  false,
      .icon = "mdi:counter" },
    { "cycle_ah",     "Total cycled",    "Ah",  NULL,              TOTI, true,  false,
      .icon = "mdi:battery-sync" },
    // Key stays runtime_s so uniq_id -- and therefore the existing HA
    // entity -- survives; only the presentation changes. 6,337,363 s is
    // not a number anyone reads.
    { "runtime_s",    "BMS runtime",     "d",   "duration",        TOTI, true,  false,
      .icon = "mdi:timer-sand",
      .tpl = "{{ (value_json.runtime_s | float(0) / 86400) | round(2) }}" },
    // Which cell is the outlier matters when chasing a weak cell.
    { "cell_min_idx", "Cell min index",  NULL,  NULL,              MEAS, true,  false,
      .icon = "mdi:arrow-down-bold-circle-outline" },
    { "cell_max_idx", "Cell max index",  NULL,  NULL,              MEAS, true,  false,
      .icon = "mdi:arrow-up-bold-circle-outline" },
    { "alarms",       "Alarm bits",      NULL,  NULL,              MEAS, true,  false,
      .icon = "mdi:alert-circle-outline" },
    // Rises when a crimp or busbar is going bad, which is the failure this
    // pack can actually develop. One worst-case number, not sixteen flat ones.
    { "wire_res_max_mohm", "Worst wire resistance", "mΩ", NULL,    MEAS, true,  false,
      .icon = "mdi:resistor" },
    // --- link diagnostics ---
    { "rssi",         "WiFi RSSI",       "dBm", "signal_strength", MEAS, true,  false, .icon = NULL },
    // Both reset to zero on reboot. total_increasing tells HA's statistics
    // engine that a drop is a restart, not a negative delta; "measurement"
    // would corrupt long-term history at every reboot.
    { "uptime_s",     "Uptime",          "s",   "duration",        TOTI, true,  false, .icon = NULL },
    { "frames",       "Frames decoded",  NULL,  NULL,              TOTI, true,  false,
      .icon = "mdi:package-down" },

    // --- pack configuration, from the SETTINGS frame ---
    { "cell_count",       "Cells configured",   NULL, NULL,      NULL, true, true,
      .icon = "mdi:format-list-numbered" },
    { "design_ah",        "Design capacity",    "Ah", NULL,      NULL, true, true,
      .icon = "mdi:battery-outline" },
    { "cell_ovp_v",       "Cell OVP",           "V",  "voltage", NULL, true, true, .icon = NULL },
    { "cell_uvp_v",       "Cell UVP",           "V",  "voltage", NULL, true, true, .icon = NULL },
    { "balance_trigger_v","Balance trigger",    "V",  "voltage", NULL, true, true, .icon = NULL },
    { "charge_limit_a",   "Charge limit",       "A",  "current", NULL, true, true, .icon = NULL },
    { "discharge_limit_a","Discharge limit",    "A",  "current", NULL, true, true, .icon = NULL },
    { "float_v",          "Float voltage",      "V",  "voltage", NULL, true, true, .icon = NULL },
    { "chg_otp_c",        "Charge over-temp",   "°C", "temperature", NULL, true, true, .icon = NULL },
    { "mos_otp_c",        "MOSFET over-temp",   "°C", "temperature", NULL, true, true, .icon = NULL },
};

typedef struct {
    const char *key;
    const char *name;
    const char *dev_cla;      // may be NULL
    bool        avail_on_bms; // gate on bms_status as well as status?
    bool        settings;
} bin_t;

static const bin_t BINS[] = {
    { "chg_mos",  "Charge MOSFET",     NULL,           true,  false },
    { "dsg_mos",  "Discharge MOSFET",  NULL,           true,  false },
    { "bal_mos",  "Balancing",         NULL,           true,  false },
    { "charge_enabled",    "Charge enabled",    NULL,  false, true },
    { "discharge_enabled", "Discharge enabled", NULL,  false, true },
    { "balance_enabled",   "Balancer enabled",  NULL,  false, true },
};

// Shared tail: expiry + availability + device block.
static int common_tail(char *buf, size_t n, int w, const char *id,
                       bool on_bms, bool expires)
{
#if CONFIG_JK_EXPIRE_S > 0
    // Availability topics only catch a clean disconnect or the LWT. A task
    // that wedges mid-loop keeps the broker's last state message, and HA would
    // show it as current indefinitely. expire_after is what bounds that.
    //
    // Settings entities are exempt: they republish only on change, so an
    // unchanged pack would expire and read as unavailable.
    if (expires) {
        w = sb_addf(buf, n, w, "\"exp_aft\":%d,", CONFIG_JK_EXPIRE_S);
    }
#else
    (void)expires;
#endif
    if (on_bms) {
        w = sb_addf(buf, n, w,
                 "\"avty\":[{\"t\":\"%s\"},{\"t\":\"%s\"}],\"avty_mode\":\"all\",",
                 net_topic_status(), net_topic_bms_status());
    } else {
        w = sb_addf(buf, n, w, "\"avty\":[{\"t\":\"%s\"}],", net_topic_status());
    }
    return sb_addf(buf, n, w,
        "\"dev\":{\"ids\":[\"%s\"],\"name\":\"JK BMS\","
        "\"mf\":\"JK\",\"mdl\":\"PB series (hw V19)\"}}", id);
}

static void publish(const char *topic, const char *pl, int w, size_t cap)
{
    if (sb_overflowed(cap, w)) {
        // Truncated JSON would leave a broken retained config in the broker.
        ESP_LOGE(TAG, "discovery payload truncated for %s, not publishing",
                 topic);
        return;
    }
    // Checked, unlike the state payload: discovery is published once per
    // connect and nothing reissues it, so a silent failure leaves an entity
    // missing from Home Assistant until the next reconnect.
    if (net_mqtt_publish(topic, pl, 1, 1) < 0) {
        ESP_LOGE(TAG, "discovery publish failed for %s", topic);
    }
}

// Publishes ~58 retained QoS-1 configs synchronously, from the MQTT event
// handler, i.e. on the esp-mqtt task.
//
// That looks like something to move off the event handler, and it is not.
// esp_mqtt_client_publish() takes a *recursive* mutex and does not wait for a
// PUBACK -- it writes the socket and returns -- so there is no deadlock and no
// stall beyond the writes themselves.
//
// The obvious alternative, esp_mqtt_client_enqueue(), is actively wrong here.
// The esp-mqtt task drains exactly one QUEUED outbox item per loop iteration,
// and each iteration polls the socket for up to MQTT_POLL_READ_TIMEOUT_MS
// (1000 ms, unconditional while connected). 58 enqueued messages would take
// ~58 s to drain, past OUTBOX_EXPIRED_TIMEOUT_MS (30 s), after which the
// remainder is silently discarded -- roughly half the entities would simply
// never appear in Home Assistant. Checked against esp-mqtt in ESP-IDF
// v5.5.5 (mqtt_client.c: the outbox dequeue at the top of MQTT_STATE_CONNECTED,
// and the poll after the state switch).
//
// So: publish synchronously, and keep the payloads small enough that the burst
// is a few tens of KB written straight to the socket.
void ha_discovery_publish_all(void)
{
    const char *id = net_device_id();
    static char topic[96];
    static char pl[1024];

    for (size_t i = 0; i < sizeof(ENTS) / sizeof(ENTS[0]); i++) {
        const ent_t *e = &ENTS[i];
        const char *stat = e->settings ? net_topic_settings()
                                       : net_topic_state();
        snprintf(topic, sizeof(topic),
                 "homeassistant/sensor/%s_%s/config", id, e->key);
        int w = sb_addf(pl, sizeof(pl), 0,
            "{\"name\":\"%s\",\"uniq_id\":\"%s_%s\",\"has_entity_name\":true,"
            "\"stat_t\":\"%s\",",
            e->name, id, e->key, stat);
        // The override carries the whole template rather than a format string,
        // so no non-literal ever reaches sb_addf's printf checking.
        if (e->tpl) {
            w = sb_addf(pl, sizeof(pl), w, "\"val_tpl\":\"%s\",", e->tpl);
        } else {
            w = sb_addf(pl, sizeof(pl), w,
                        "\"val_tpl\":\"{{ value_json.%s }}\",", e->key);
        }
        if (e->icon) {
            w = sb_addf(pl, sizeof(pl), w, "\"ic\":\"%s\",", e->icon);
        }
        if (e->stat_cla) {
            w = sb_addf(pl, sizeof(pl), w, "\"stat_cla\":\"%s\",", e->stat_cla);
        }
        if (e->unit) {
            w = sb_addf(pl, sizeof(pl), w, "\"unit_of_meas\":\"%s\",", e->unit);
        }
        if (e->dev_cla) {
            w = sb_addf(pl, sizeof(pl), w, "\"dev_cla\":\"%s\",", e->dev_cla);
        }
        if (e->diagnostic) {
            w = sb_addf(pl, sizeof(pl), w, "\"ent_cat\":\"diagnostic\",");
        }
        w = common_tail(pl, sizeof(pl), w, id, !e->settings, !e->settings);
        publish(topic, pl, w, sizeof(pl));
    }

    for (size_t i = 0; i < sizeof(BINS) / sizeof(BINS[0]); i++) {
        const bin_t *b = &BINS[i];
        const char *stat = b->settings ? net_topic_settings()
                                       : net_topic_state();
        snprintf(topic, sizeof(topic),
                 "homeassistant/binary_sensor/%s_%s/config", id, b->key);
        // Render the JSON bool to explicit ON/OFF rather than relying on how
        // Jinja stringifies true/false.
        int w = sb_addf(pl, sizeof(pl), 0,
            "{\"name\":\"%s\",\"uniq_id\":\"%s_%s\",\"has_entity_name\":true,"
            "\"stat_t\":\"%s\","
            "\"val_tpl\":\"{{ 'ON' if value_json.%s else 'OFF' }}\","
            "\"pl_on\":\"ON\",\"pl_off\":\"OFF\",\"ent_cat\":\"diagnostic\",",
            b->name, id, b->key, stat, b->key);
        if (b->dev_cla) {
            w = sb_addf(pl, sizeof(pl), w, "\"dev_cla\":\"%s\",", b->dev_cla);
        }
        w = common_tail(pl, sizeof(pl), w, id, b->avail_on_bms, !b->settings);
        publish(topic, pl, w, sizeof(pl));
    }

    // "BMS comms" reads the bms_status topic directly rather than a field in
    // the state JSON, which is why it is not in BINS[] above.
    //
    // It used to read a `comms_ok` JSON field whose only producer passed a
    // literal true, so the entity could never once report a disconnection --
    // the single thing it exists for. A freshness flag computed at state-build
    // time cannot say otherwise: a state payload only exists because a frame
    // just arrived. The retained online/offline payload on bms_status already
    // carries exactly this, is event-driven, and survives a reconnect, so it
    // needs no expire_after.
    //
    // Availability is this firmware's own status topic only. Gating it on
    // bms_status would make the entity unavailable in precisely the case it is
    // meant to report.
    snprintf(topic, sizeof(topic),
             "homeassistant/binary_sensor/%s_comms_ok/config", id);
    int wc = sb_addf(pl, sizeof(pl), 0,
        "{\"name\":\"BMS comms\",\"uniq_id\":\"%s_comms_ok\","
        "\"has_entity_name\":true,\"stat_t\":\"%s\","
        "\"pl_on\":\"online\",\"pl_off\":\"offline\","
        "\"dev_cla\":\"connectivity\",\"ent_cat\":\"diagnostic\",",
        id, net_topic_bms_status());
    wc = common_tail(pl, sizeof(pl), wc, id, false, false);
    publish(topic, pl, wc, sizeof(pl));

    // Per-cell voltages: present but disabled by default in HA. `cells` is
    // positional (see jk_fmt_state_json) and may be shorter than
    // CONFIG_JK_CELL_COUNT or hold nulls; the template yields `none` (entity
    // shows unknown) for both, rather than a Jinja index error.
    for (int c = 0; c < CONFIG_JK_CELL_COUNT; c++) {
        snprintf(topic, sizeof(topic),
                 "homeassistant/sensor/%s_cell%02d/config", id, c + 1);
        int w = sb_addf(pl, sizeof(pl), 0,
            "{\"name\":\"Cell %02d\",\"uniq_id\":\"%s_cell%02d\","
            "\"has_entity_name\":true,\"stat_t\":\"%s\","
            "\"val_tpl\":\"{{ value_json.cells[%d] "
            "if value_json.cells | count > %d else none }}\","
            "\"unit_of_meas\":\"V\",\"dev_cla\":\"voltage\","
            "\"stat_cla\":\"measurement\",\"en\":false,",
            c + 1, id, c + 1, net_topic_state(), c, c);
        w = common_tail(pl, sizeof(pl), w, id, true, true);
        publish(topic, pl, w, sizeof(pl));
    }

    ESP_LOGI(TAG, "discovery published for %s (%d sensors, %d binary, %d cells)",
             id, (int)(sizeof(ENTS) / sizeof(ENTS[0])),
             (int)(sizeof(BINS) / sizeof(BINS[0])) + 1,   // +1: BMS comms
             CONFIG_JK_CELL_COUNT);
}
