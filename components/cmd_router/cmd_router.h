/* Console example — various router commands

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void register_router(void);

/**
 * @brief Send a Wake-on-LAN magic packet
 * @param mac      Target MAC address (6 bytes)
 * @param bind_ip  Source interface IP to bind to (network byte order)
 * @param dest_ip_str  Destination IP string (e.g. "255.255.255.255")
 * @param dest_port    UDP destination port (typically 9)
 * @return true if at least one of three sends succeeded
 */
bool wol_send_mac(const uint8_t mac[6], uint32_t bind_ip, const char *dest_ip_str, int dest_port);

#ifdef __cplusplus
}
#endif
