/* WiFi credential externs, STA config, NVS helpers, and set_sta.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

extern char* ssid;
extern char* ent_username;
extern char* ent_identity;
extern char* passwd;
extern char* static_ip;
extern char* subnet_mask;
extern char* gateway_addr;
extern char* ap_ssid;
extern char* ap_passwd;
extern char* ap_dns;
extern char* hostname;

extern uint16_t connect_count;
extern bool ap_connect;
extern bool wifi_scan_active;

#define DEFAULT_AP_IP "192.168.4.1"

extern uint32_t my_ip;
extern uint32_t my_ap_ip;

// WPA2-Enterprise settings
extern int32_t eap_method;          // 0=Auto, 1=PEAP, 2=TTLS, 3=TLS
extern int32_t ttls_phase2;         // 0=MSCHAPv2, 1=MSCHAP, 2=PAP, 3=CHAP
extern int32_t use_cert_bundle;     // 0=off, 1=on
extern int32_t disable_time_check;  // 0=off, 1=on

void preprocess_string(char* str);
int set_sta(int argc, char **argv);
int set_sta_mac(int argc, char **argv);
int set_sta_static(int argc, char **argv);
int set_ap_ip(int argc, char **argv);

esp_err_t get_config_param_blob(char* name, uint8_t** blob, size_t blob_len);
esp_err_t get_config_param_int(char* name, int* param);
esp_err_t get_config_param_str(char* name, char** param);

esp_err_t set_config_param_str(const char* name, const char* value);
esp_err_t set_config_param_int(const char* name, int32_t value);
esp_err_t set_config_param_blob(const char* name, const void* data, size_t len);

#ifdef __cplusplus
}
#endif
