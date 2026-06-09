#include "mqtt_publisher.h"

#include <mosquitto.h>
#include <stdio.h>
#include <string.h>

static struct mosquitto *g_mosq;
static char g_topic_base[128];

int mqtt_publisher_init(const char *config_path) {
    (void)config_path;
    mosquitto_lib_init();
    g_mosq = mosquitto_new("softplc-runtime", true, NULL);
    if (!g_mosq) {
        fprintf(stderr, "[mqtt] mosquitto_new failed\n");
        return -1;
    }
    strncpy(g_topic_base, "softplc/devices", sizeof(g_topic_base) - 1);
    /* TODO: read broker address from config; connect async with reconnect */
    fprintf(stderr, "[mqtt] publisher initialized (broker not connected in scaffold)\n");
    return 0;
}

void mqtt_publisher_tick(void) {
    if (!g_mosq) return;
    char topic[160];
    char payload[MQTT_MAX_PAYLOAD];
    snprintf(topic, sizeof(topic), "%s/status", g_topic_base);
    int n = snprintf(payload, sizeof(payload),
                     "{\"timestamp\":0,\"status\":\"ok\"}");
    if (n < 0 || n >= (int)sizeof(payload)) return;
    /* mosquitto_publish(g_mosq, NULL, topic, n, payload, 0, false); */
}

void mqtt_publisher_shutdown(void) {
    if (g_mosq) {
        mosquitto_destroy(g_mosq);
        g_mosq = NULL;
    }
    mosquitto_lib_cleanup();
}
