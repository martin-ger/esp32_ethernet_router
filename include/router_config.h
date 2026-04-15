/* Core router configuration constants, byte counters, LED state,
 * TTL/MSS/PMTU tuning, uptime, and netif hooks.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PARAM_NAMESPACE "esp32_nat"

#define PROTO_TCP 6
#define PROTO_UDP 17

// One active connection uses about 5kB RAM
#define AP_MAX_CONNECTIONS 8

// Byte counting variables for STA interface
extern uint64_t sta_bytes_sent;
extern uint64_t sta_bytes_received;

// LED GPIO configuration (-1 means disabled/none)
extern int led_gpio;

// LED low-active mode (0 = active-high, 1 = active-low/inverted)
extern uint8_t led_lowactive;

// Shared LED toggle state (packet-driven flicker)
extern uint8_t led_toggle;

// Addressable LED strip GPIO (-1 = disabled/none)
extern int led_strip_gpio;

// TTL override for STA upstream (0 = disabled/no change, 1-255 = fixed TTL)
extern uint8_t sta_ttl_override;

// MSS clamp for downlink interface (0 = disabled, otherwise max MSS in bytes)
extern uint16_t ap_mss_clamp;

// Path MTU for downlink clients (0 = disabled, otherwise send ICMP Frag Needed when DF packets exceed this)
extern uint16_t ap_pmtu;

// Ethernet NAT mode (1 = NAT enabled (default), 0 = routed/no NAT)
extern uint8_t eth_nat_enabled;

// Ethernet DHCP server (1 = enabled (default), 0 = disabled)
extern uint8_t eth_dhcps_enabled;

// Ethernet link state (true = link up, false = link down)
extern bool eth_link_up;

// Byte counting functions
void init_byte_counter(void);
uint64_t get_sta_bytes_sent(void);
uint64_t get_sta_bytes_received(void);
void reset_sta_byte_counts(void);
void resync_connect_count(void);

// Uptime functions
uint32_t get_uptime_seconds(void);
void format_uptime(uint32_t seconds, char *buf, size_t buf_len);
void format_boot_time(char *buf, size_t buf_len);

// Downlink netif hook functions (Ethernet downlink: ACL, PCAP, MSS/PMTU)
void init_downlink_netif_hooks(void);

#ifdef __cplusplus
}
#endif
