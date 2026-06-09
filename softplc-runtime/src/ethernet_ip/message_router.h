#ifndef SOFTPLC_ETHERNET_IP_MESSAGE_ROUTER_H
#define SOFTPLC_ETHERNET_IP_MESSAGE_ROUTER_H

#include <stddef.h>
#include <stdint.h>

int cip_message_route(const uint8_t *request, size_t req_len,
                      uint8_t *response, size_t *resp_len);

#endif
