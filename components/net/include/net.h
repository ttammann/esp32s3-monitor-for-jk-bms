#pragma once

#include <stdbool.h>
#include <stdint.h>

// Starts the station and returns immediately. Association and reconnection
// happen in the background; nothing else in the firmware may depend on the
// network being up. The 55AA listener in particular must run without it --
// that is the whole bench-test story.
void net_wifi_start(void);

// Wait up to timeout_ms for an IP. Returns false on timeout; the station keeps
// trying regardless. Pass 0 to poll.
bool net_wifi_wait_ip(uint32_t timeout_ms);

int  net_wifi_rssi(void);   // 0 if not associated

// Device identity, derived from the STA MAC: "jkbms-a1b2c3"
const char *net_device_id(void);

// MQTT topics (built once from the device id)
const char *net_topic_state(void);
const char *net_topic_status(void);      // this firmware's LWT topic
const char *net_topic_bms_status(void);  // BMS data-freshness availability
const char *net_topic_settings(void);    // retained pack configuration

void net_mqtt_start(void);
bool net_mqtt_connected(void);

// Counts completed MQTT connections: 0 before the first, then incrementing on
// every (re)connect. It exists so a caller that publishes something *once* can
// tell "already sent" from "sent to a connection that no longer exists".
//
// The broker's retained store is not ours to assume. A broker that restarted
// has forgotten every retained message, and a payload published only on change
// -- the settings topic -- would then never be republished. Comparing this
// against the epoch a publish succeeded in makes that recoverable.
uint32_t net_mqtt_epoch(void);

// Returns msg_id >= 0, or -1 if the client isn't connected or the write failed.
//
// A dropped publish is logged here, so callers whose message is self-healing
// (the per-cycle state payload) may ignore the return. Callers whose message is
// published once must check it and retry -- see report_settings() in listen.c.
int  net_mqtt_publish(const char *topic, const char *payload,
                      int qos, int retain);

// Publishes the retained HA discovery configs. Called on every MQTT connect.
void ha_discovery_publish_all(void);
