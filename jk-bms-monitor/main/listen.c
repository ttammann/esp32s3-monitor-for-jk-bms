// Passive RS485 listener for JK's 55AA push stream.
//
// Read-only is structural here, not a policy: the UART is opened with no TX pin
// and DE/RE is held low as a plain GPIO, so this firmware physically cannot put
// a byte on the bus. For a battery management system that is worth having as a
// property of the wiring rather than a promise in a comment.
//
// The BMS masters its own bus. It scans sixteen pack addresses, writing 0x161E
// then 0x1620 to each; the pack at address 0 answers both. One full cycle takes
// ~6.4 s, and that -- not 1 Hz -- is the data cadence every downstream timeout
// has to be sized against.
#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#include "jk55.h"
#include "jk_req.h"
#include "jk_demux.h"
#include "jk_busdiag.h"
#include "jk_fmt.h"
#include "net.h"
#include "listen.h"

#define LS_UART        UART_NUM_1
#define LS_PIN_TX      17    // never routed to the UART in this build
#define LS_PIN_RX      18
#define LS_PIN_DE      21
#define LS_BUF         512   // largest 55AA frame is 308; this is slack
#define LS_RESYNC_MS   150   // no new bytes for this long -> drop a byte
#define LS_REPORT_S    10    // bus-health report interval
#define LS_EVT_QUEUE   24

// Four missed DYNAMIC frames at the measured 6.4 s cadence. Long enough not to
// flap on a single dropped cycle, short enough that Home Assistant is told the
// truth well inside the 30 s expire_after.
#define LS_STALE_AFTER_US  (25 * 1000000LL)

// After a failed bms_status publish on a live connection, wait this long
// before trying again rather than retrying on every 20 ms loop pass.
#define LS_AVAIL_RETRY_US  (1000000LL)

static const char *TAG = "jk";

// The one freshness rule: availability and the cadence diagnostic must agree.
static bool dyn_fresh(int64_t last_dynamic, int64_t now)
{
    return last_dynamic != 0 && now - last_dynamic < LS_STALE_AFTER_US;
}

static void report_dynamic(const jk55_dynamic_t *d, size_t n, jk55_check_t ck)
{
    // Signed power, matching the MQTT payload. The BMS's own value is an
    // unsigned magnitude, so logging it raw printed "-3479 mA  183021 mW" --
    // the two halves of one measurement disagreeing about direction.
    ESP_LOGI(TAG, "DYNAMIC len=%u (%s)  %lu mV  %ld mA  %ld mW  soc %u%% soh %u%%",
             (unsigned)n, ck == JK55_CK_SUM8 ? "sum8" : "crc16",
             (unsigned long)d->pack_mv, (long)d->current_ma,
             (long)jk55_power_mw_signed(d), d->soc_pct, d->soh_pct);
    ESP_LOGI(TAG, "     cells=%u present=0x%08lX avg=%umV delta=%umV "
             "max=#%u min=#%u",
             d->cell_count, (unsigned long)d->cell_present, d->cell_avg_mv,
             d->cell_delta_mv, d->cell_max_idx + 1, d->cell_min_idx + 1);
    ESP_LOGI(TAG, "     mos=%.1fC t1=%.1fC t2=%.1fC bal=%dmA alarms=0x%08lX "
             "chg=%d dsg=%d bal=%d",
             d->mos_temp_dc / 10.0, d->t1_dc / 10.0, d->t2_dc / 10.0,
             d->balance_ma, (unsigned long)d->alarms,
             d->chg_mos, d->dsg_mos, d->bal_mos);
    ESP_LOGI(TAG, "     remain=%ldmAh full=%lumAh cycles=%lu cycle_cap=%lumAh "
             "runtime=%lus",
             (long)d->remain_mah, (unsigned long)d->full_mah,
             (unsigned long)d->cycles, (unsigned long)d->cycle_mah,
             (unsigned long)d->runtime_s);

    const int rows = jk_fmt_cell_rows(d->cell_present);
    for (int r = 0; r < rows; r++) {
        // Sized for the worst case a uint16_t can print, not for plausible
        // cell voltages: eight 5-digit values plus separators. A tighter
        // buffer silently dropped the whole row on one corrupt reading.
        char line[8 * 6 + 1];
        if (jk_fmt_cell_row(line, sizeof(line), d->cell_mv, JK55_MAX_CELLS,
                            r) > 0) {
            printf("        cell %02d-%02d: %s mV\n",
                   r * 8 + 1, r * 8 + 8, line);
        }
    }
}

static void log_settings(const jk55_settings_t *s, bool changed)
{
    ESP_LOGI(TAG, "SETTINGS%s: %luS design=%lumAh addr=%lu",
             changed ? " changed" : "",
             (unsigned long)s->cell_count,
             (unsigned long)s->design_capacity_mah,
             (unsigned long)s->device_addr);
    ESP_LOGI(TAG, "     cell ovp=%lumV (rec %lumV)  uvp=%lumV (rec %lumV)",
             (unsigned long)s->cell_ovp_mv,
             (unsigned long)s->cell_ovp_recover_mv,
             (unsigned long)s->cell_uvp_mv,
             (unsigned long)s->cell_uvp_recover_mv);
    ESP_LOGI(TAG, "     chg limit=%ldmA  dsg limit=%ldmA  bal max=%lumA",
             (long)s->charge_current_ma, (long)s->discharge_current_ma,
             (unsigned long)s->max_balance_current_ma);
    ESP_LOGI(TAG, "     balance: start=%lumV trigger-diff=%lumV  "
             "chg=%d dsg=%d bal=%d",
             (unsigned long)s->balance_start_mv,
             (unsigned long)s->balance_trigger_mv,
             s->charge_enabled, s->discharge_enabled, s->balance_enabled);
    ESP_LOGI(TAG, "     temps: chg otp=%.1fC utp=%.1fC  dsg otp=%.1fC  "
             "mos otp=%.1fC",
             s->charge_otp_dc / 10.0, s->charge_utp_dc / 10.0,
             s->discharge_otp_dc / 10.0, s->mos_otp_dc / 10.0);
}

// Settings arrive once per scan cycle and almost never change, so logging every
// one would double the console traffic for no information. Report the first,
// then only what actually changed.
//
// Publishing is tracked separately from logging, and it has to be. The console
// dedupe is about noise; the broker needs the payload to have actually landed.
// Publishing while MQTT is down silently drops the message, and the listener
// deliberately starts ~6.4 s before Wi-Fi and MQTT are up -- so the first
// settings frame is both the most likely to be dropped and, under a log-only
// dedupe, the only one that would ever have been sent. The settings entities
// carry no expire_after (see ha_discovery.c), so nothing recovers on its own:
// they would simply stay blank until the pack configuration changed.
//
// Retry until a publish succeeds, and again on every reconnect, because a
// broker that restarted has lost the retained copy.
static void report_settings(const jk55_settings_t *s)
{
    static jk55_settings_t prev;
    static bool have_prev;
    static uint32_t published_epoch;    // 0 = not yet on the broker
    static bool formattable = true;     // false once this content failed to fit

    if (!have_prev || memcmp(&prev, s, sizeof(*s)) != 0) {
        // have_prev is still the *previous* state here, so it doubles as
        // "we have seen settings before", i.e. this is a change rather than
        // the first report.
        log_settings(s, have_prev);
        prev = *s;          // the decoder zeroes padding, so memcmp is stable
        have_prev = true;
        published_epoch = 0;            // content moved on; must be resent
        formattable = true;             // new content deserves a fresh attempt
    }

    const uint32_t epoch = net_mqtt_epoch();
    if (!formattable || epoch == 0 || published_epoch == epoch) {
        return;                         // hopeless, never connected, or sent
    }

    char json[640];
    if (jk_fmt_settings_json(json, sizeof(json), s) <= 0) {
        // Deterministic for a given payload, so retrying it every 6.4 s would
        // only produce the same error line forever.
        ESP_LOGE(TAG, "settings JSON truncated, not publishing");
        formattable = false;
        return;
    }
    // QoS 1, unlike the state payload's QoS 0: this is published roughly once
    // per boot, so there is no next message to paper over a loss.
    if (net_mqtt_publish(net_topic_settings(), json, 1, 1) >= 0) {
        published_epoch = epoch;
    }
}

void jk_listen_run(void)
{
    const uart_config_t cfg = {
        .baud_rate  = CONFIG_JK_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    // Hold the transceiver in receive for as long as this firmware runs.
    gpio_config_t de = {
        .pin_bit_mask = 1ULL << LS_PIN_DE,
        .mode         = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&de));
    ESP_ERROR_CHECK(gpio_set_level(LS_PIN_DE, 0));

    // And park the driver's data input high-impedance, so the transceiver
    // input pin is never driven by us even if DE were to float.
    gpio_config_t tx = {
        .pin_bit_mask = 1ULL << LS_PIN_TX,
        .mode         = GPIO_MODE_INPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&tx));

    // An event queue is what makes framing errors, breaks and overflows
    // visible. Without it the driver counts them and throws them away, and
    // a wrong baud looks identical to an unplugged cable.
    static QueueHandle_t evtq;
    ESP_ERROR_CHECK(uart_driver_install(LS_UART, 4096, 0, LS_EVT_QUEUE,
                                        &evtq, 0));
    ESP_ERROR_CHECK(uart_param_config(LS_UART, &cfg));
    // TX deliberately unassigned: the UART has no pin to transmit on.
    ESP_ERROR_CHECK(uart_set_pin(LS_UART, UART_PIN_NO_CHANGE, LS_PIN_RX,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    ESP_LOGI(TAG, "listening: uart1 rx=%d de=%d(low) %d baud, tx disabled",
             LS_PIN_RX, LS_PIN_DE, CONFIG_JK_BAUD);

    static uint8_t buf[LS_BUF];
    static jk55_dynamic_t dyn;
    static jk55_settings_t set;
    static char json[1536];

    size_t n = 0;
    int64_t last_rx = esp_timer_get_time();
    // Seed from "now", not 0. A dead bus is exactly the case the warning below
    // exists for, and a zero seed made it unreachable until the first byte
    // arrived -- i.e. it could only fire once traffic had already been seen.
    int64_t last_report = last_rx;
    int64_t last_dynamic = 0;
    uint32_t dyn_interval_ms = 0;
    uint32_t total_frames = 0;
    bool bms_avail = false;
    uint32_t bms_avail_epoch = 0;        // MQTT session it was last sent in;
                                         // 0 forces the first publish
    int64_t bms_avail_retry = 0;         // no publish attempt before this
    static jk_busstats_t stats;

    for (;;) {
        int got = uart_read_bytes(LS_UART, buf + n, LS_BUF - n,
                                  pdMS_TO_TICKS(20));
        if (got > 0) {
            n += (size_t)got;
            stats.bytes += (uint32_t)got;
            last_rx = esp_timer_get_time();
        }

        bool draining = true;
        while (draining && n > 0) {
            const bool stalled = (n == LS_BUF) ||
                (esp_timer_get_time() - last_rx > LS_RESYNC_MS * 1000);
            jk_demux_out_t out;

            switch (jk_demux(buf, n, stalled, &out)) {
            case JK_DEMUX_JK55:
                stats.frames_jk55++;
                total_frames++;
                // The decoders own the frame-type check; each rejects any
                // frame that is not its own type.
                if (jk55_decode_dynamic(buf, out.len, &dyn)) {
                    const int64_t now = esp_timer_get_time();
                    // A gap that spans a stale period is the outage length,
                    // not the cadence; report no interval until the next one.
                    dyn_interval_ms = dyn_fresh(last_dynamic, now)
                        ? (uint32_t)((now - last_dynamic) / 1000) : 0;
                    last_dynamic = now;
                    report_dynamic(&dyn, out.len, out.ck);
                    if (jk_fmt_state_json(json, sizeof(json), &dyn,
                                          net_wifi_rssi(),
                                          esp_timer_get_time() / 1000000,
                                          total_frames) > 0) {
                        // Return deliberately unchecked, unlike the settings
                        // and bms_status publishes: this payload is reissued
                        // every scan cycle, so a drop costs ~6.4 s of latency
                        // and repairs itself. net_mqtt_publish logs it.
                        net_mqtt_publish(net_topic_state(), json, 0, 0);
                    } else {
                        ESP_LOGE(TAG, "state JSON truncated, not publishing");
                    }
                } else if (jk55_decode_settings(buf, out.len, &set)) {
                    report_settings(&set);
                } else {
                    ESP_LOGW(TAG, "55AA frame byte4=0x%02X len=%u did not "
                             "decode", buf[4], (unsigned)out.len);
                }
                break;
            case JK_DEMUX_REQ:
                stats.frames_req++;
                // On a self-polling bus these arrive ~32 times per cycle, in
                // both request and echo form; at INFO they would bury the data
                // they exist to produce.
                ESP_LOGD(TAG, "pack %02X frame request reg=0x%04X len=%u",
                         out.req_pack, out.req_reg, (unsigned)out.len);
                break;
            case JK_DEMUX_DROP1:
                ESP_LOGD(TAG, "resync, dropping %02X", buf[0]);
                stats.dropped++;
                out.len = 1;
                break;
            case JK_DEMUX_NEED_MORE:
            default:
                draining = false;
                continue;
            }
            // jk_demux's contract makes this unreachable: every positive length
            // it reports has already been bounds-checked against n by
            // jk55_frame_len or jk_req_len. It is guarded anyway because the
            // cost of being wrong is not a bad log line -- len 0 spins here
            // forever, and len > n underflows a size_t straight into memmove.
            // The host harness that mirrors this loop carries the same guard
            // (test/test_util.h); production had been the one without it.
            if (out.len == 0 || out.len > n) {
                ESP_LOGE(TAG, "demux returned len=%u for n=%u, dropping buffer",
                         (unsigned)out.len, (unsigned)n);
                n = 0;
                break;
            }
            memmove(buf, buf + out.len, n - out.len);
            n -= out.len;
        }

        // Drain UART error events. UART_DATA is ignored: the bytes themselves
        // come from uart_read_bytes above.
        uart_event_t ev;
        while (xQueueReceive(evtq, &ev, 0) == pdTRUE) {
            switch (ev.type) {
            case UART_FRAME_ERR:   stats.frame_err++;  break;
            case UART_PARITY_ERR:  stats.parity_err++; break;
            case UART_BREAK:       stats.breaks++;     break;
            case UART_FIFO_OVF:    stats.fifo_ovf++;   break;
            case UART_BUFFER_FULL: stats.buf_full++;   break;
            default: break;
            }
        }

        // Availability follows the data, not the link: a listener with a live
        // UART and a silent BMS has nothing to publish and must say so. It is
        // resent on every reconnect as well as on change, for the same reason
        // as report_settings(): a broker that restarted has lost the retained
        // copy, and every state entity is gated on this topic.
        const int64_t now = esp_timer_get_time();
        const bool fresh = dyn_fresh(last_dynamic, now);
        const uint32_t epoch = net_mqtt_epoch();
        if ((fresh != bms_avail || epoch != bms_avail_epoch) &&
            epoch != 0 && net_mqtt_connected() && now >= bms_avail_retry) {
            if (net_mqtt_publish(net_topic_bms_status(),
                                 fresh ? "online" : "offline", 1, 1) >= 0) {
                bms_avail = fresh;
                bms_avail_epoch = epoch;
            } else {
                bms_avail_retry = now + LS_AVAIL_RETRY_US;
            }
        }

        if (now - last_report > LS_REPORT_S * 1000000LL) {
            last_report = now;
            const jk_bus_verdict_t v = jk_bus_diagnose(&stats);
            ESP_LOGI(TAG, "%ds: %lu bytes, %lu req, %lu 55AA, %lu dropped | "
                     "errs: %lu frame, %lu parity, %lu break, %lu ovf, %lu full",
                     LS_REPORT_S, (unsigned long)stats.bytes,
                     (unsigned long)stats.frames_req,
                     (unsigned long)stats.frames_jk55,
                     (unsigned long)stats.dropped,
                     (unsigned long)stats.frame_err,
                     (unsigned long)stats.parity_err,
                     (unsigned long)stats.breaks,
                     (unsigned long)stats.fifo_ovf,
                     (unsigned long)stats.buf_full);
            if (v == JK_BUS_OK) {
                // Only meaningful if a dynamic frame arrived in this window;
                // otherwise it is the age of a measurement nobody should trust.
                if (dyn_interval_ms && fresh) {
                    ESP_LOGI(TAG, "  -> %s; DYNAMIC every %lu.%02lus",
                             jk_bus_advice(v),
                             (unsigned long)(dyn_interval_ms / 1000),
                             (unsigned long)((dyn_interval_ms % 1000) / 10));
                } else {
                    ESP_LOGI(TAG, "  -> %s", jk_bus_advice(v));
                }
            } else {
                ESP_LOGW(TAG, "  -> %s", jk_bus_advice(v));
            }
            stats = (jk_busstats_t){ 0 };   // per-interval, not cumulative
        }
    }
}
