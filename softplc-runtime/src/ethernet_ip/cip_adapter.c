#include "cip_adapter.h"

#include <stdio.h>
#include <string.h>

int cip_adapter_open(cip_connection_t *conn) {
    /* TODO: implement EtherNet/IP CIP forward_open */
    conn->connected = 1;
    fprintf(stderr, "[cip] open instance=0x%04x ip=%s rpi=%u\n",
            conn->instance_id, conn->device_ip, conn->rpi_ms);
    return 0;
}

int cip_adapter_read(cip_connection_t *conn) {
    /* TODO: read produced data from device into conn->consumed */
    if (!conn->connected) return -1;
    return 0;
}

int cip_adapter_write(cip_connection_t *conn) {
    /* TODO: write conn->produced to device */
    if (!conn->connected) return -1;
    return 0;
}

void cip_adapter_close(cip_connection_t *conn) {
    if (!conn->connected) return;
    conn->connected = 0;
    fprintf(stderr, "[cip] close instance=0x%04x\n", conn->instance_id);
}
