#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "esp_attr.h"
#include "lwip/pbuf.h"

#define NF_DIR_INGRESS 0x01   /* ETH client → router (pre-NAT source IPs) */
#define NF_DIR_EGRESS  0x02   /* router → ETH client */

esp_err_t  netflow_init(void);
esp_err_t  netflow_enable(const char *collector_ip, uint16_t port, uint8_t directions);
esp_err_t  netflow_set_directions(uint8_t directions);
esp_err_t  netflow_disable(void);
void       netflow_notify_connected(void);
bool       netflow_is_enabled(void);
void       netflow_set_timeouts(uint32_t idle_sec, uint32_t active_sec);
void       netflow_get_config(bool *enabled, char *ip_out, size_t ip_len,
                               uint16_t *port_out,
                               uint32_t *idle_sec_out, uint32_t *active_sec_out,
                               uint8_t *directions_out);
uint32_t   netflow_get_active_flows(void);
uint32_t   netflow_get_exported_flows(void);
void       netflow_print_flows(void);

IRAM_ATTR void netflow_account(struct pbuf *p, uint8_t dir);
