#ifndef SOFTPLC_ETHERNET_IP_CONNECTION_MANAGER_H
#define SOFTPLC_ETHERNET_IP_CONNECTION_MANAGER_H

#define CIP_MAX_CONNECTIONS 16

int cip_connection_manager_init(const char *config_path);
void cip_connection_manager_sample_inputs(void);
void cip_connection_manager_sync_outputs(void);
void cip_connection_manager_shutdown(void);

#endif
