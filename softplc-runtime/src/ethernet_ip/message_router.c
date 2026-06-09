#include "message_router.h"

#include <string.h>

int cip_message_route(const uint8_t *request, size_t req_len,
                      uint8_t *response, size_t *resp_len) {
    /* TODO: dispatch unconnected explicit messages to CIP object handlers */
    (void)request;
    (void)req_len;
    if (resp_len) *resp_len = 0;
    if (response) memset(response, 0, 0);
    return 0;
}
