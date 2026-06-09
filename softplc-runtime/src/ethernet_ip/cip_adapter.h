#ifndef SOFTPLC_ETHERNET_IP_CIP_ADAPTER_H
#define SOFTPLC_ETHERNET_IP_CIP_ADAPTER_H

#include <stdint.h>
#include <stddef.h>

#define CIP_MAX_DATA 64

typedef struct {
    uint16_t instance_id;
    char device_ip[32];
    uint16_t rpi_ms;
    uint16_t timeout_ms;
    uint8_t produced[CIP_MAX_DATA];
    uint8_t consumed[CIP_MAX_DATA];
    size_t produced_size;
    size_t consumed_size;
    int connected;
} cip_connection_t;

int cip_adapter_open(cip_connection_t *conn);
int cip_adapter_read(cip_connection_t *conn);
int cip_adapter_write(cip_connection_t *conn);
void cip_adapter_close(cip_connection_t *conn);

#endif
