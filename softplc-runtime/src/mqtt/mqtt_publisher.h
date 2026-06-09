#ifndef SOFTPLC_MQTT_PUBLISHER_H
#define SOFTPLC_MQTT_PUBLISHER_H

#define MQTT_MAX_PAYLOAD 512

int mqtt_publisher_init(const char *config_path);
void mqtt_publisher_tick(void);
void mqtt_publisher_shutdown(void);

#endif
