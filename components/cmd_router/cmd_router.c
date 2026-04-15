/* The CLI commands of the router

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <inttypes.h>
#include <stdint.h>
#include "esp_log.h"
#include "esp_console.h"
#include "esp_system.h"
#include "esp_sleep.h"
#include "spi_flash_mmap.h"
#include "driver/rtc_io.h"
#include "driver/uart.h"
#include "argtable3/argtable3.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "nvs.h"
#include "esp_wifi.h"

#include "lwip/ip4_addr.h"
#if !IP_NAPT
#error "IP_NAPT must be defined"
#endif
#include "lwip/lwip_napt.h"

#include "mbedtls/sha256.h"
#include "esp_random.h"

#include "driver/gpio.h"
#include "router_globals.h"
#include "cmd_router.h"
#if defined(CONFIG_ETH_DOWNLINK_W5500)
#include "w5500_spi_driver.h"
#endif
#include "pcap_capture.h"
#include "acl.h"
#include "remote_console.h"
/* web UI bind API — avoids pulling in esp_http_server.h */
extern uint8_t web_ui_get_bind(void);
extern void    web_ui_set_bind(uint8_t bind);
#include "syslog_client.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"

#ifdef CONFIG_FREERTOS_USE_STATS_FORMATTING_FUNCTIONS
#define WITH_TASKS_INFO 1
#endif

static const char *TAG = "cmd_router";

static void register_set_hostname(void);
static void register_set_sta(void);
static void register_set_mac(void);
static void register_set_sta_static(void);
static void register_set_ap_ip(void);
static void register_set_ap_dns(void);
static void register_set_eth_nat(void);
static void register_set_eth_dhcps(void);
static void register_show(void);
static void register_portmap(void);
static void register_dhcp_reserve(void);
static void register_set_router_password(void);
static void register_web_ui(void);
static void register_bytes(void);
static void register_pcap(void);
static void register_set_led_gpio(void);
static void register_set_led_lowactive(void);
static void register_set_led_strip(void);
static void register_set_ttl(void);
static void register_set_tx_power(void);
#if defined(CONFIG_IDF_TARGET_ESP32C6)
static void register_set_rf_switch(void);
#endif
#if defined(CONFIG_ETH_DOWNLINK_W5500)
static void register_set_spi_clock(void);
static void register_w5500(void);
#endif
static void register_acl(void);
static void register_remote_console_cmd(void);
static void register_syslog_cmd(void);
#ifdef CONFIG_IDF_TARGET_ESP32C3
#endif
static void register_scan(void);
static void register_set_vpn(void);
static void register_set_tz(void);

/* ACL helper functions (forward declarations) */
static char* acl_format_ip_with_name(uint32_t ip, uint32_t mask, char* buf, size_t buf_len);
static bool acl_parse_ip_or_name(const char* str, uint32_t* ip, uint32_t* mask);
static void acl_print_with_names(uint8_t acl_no);

/* Check if character is a valid hex digit */
static inline int is_hex_digit(char c)
{
    return (c >= '0' && c <= '9') ||
           (c >= 'A' && c <= 'F') ||
           (c >= 'a' && c <= 'f');
}

/* Convert hex digit to value (assumes valid hex digit) */
static inline uint8_t hex_digit_value(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    else
        return toupper((unsigned char)c) - 'A' + 10;
}

/* Check if string represents a boolean true value */
static inline bool parse_bool_true(const char *str)
{
    return (strcasecmp(str, "true") == 0 ||
            strcasecmp(str, "yes") == 0 ||
            strcasecmp(str, "on") == 0 ||
            strcmp(str, "1") == 0);
}

/* Check if string represents a boolean false value */
static inline bool parse_bool_false(const char *str)
{
    return (strcasecmp(str, "false") == 0 ||
            strcasecmp(str, "no") == 0 ||
            strcasecmp(str, "off") == 0 ||
            strcmp(str, "0") == 0);
}

void preprocess_string(char* str)
{
    char *p, *q;

    for (p = q = str; *p != 0; p++)
    {
        if (*(p) == '%' && *(p + 1) != 0 && *(p + 2) != 0 &&
            is_hex_digit(*(p + 1)) && is_hex_digit(*(p + 2)))
        {
            // Valid percent-encoded hex sequence
            p++;
            uint8_t a = hex_digit_value(*p) << 4;
            p++;
            a += hex_digit_value(*p);
            *q++ = a;
        }
        else if (*(p) == '+') {
            *q++ = ' ';
        } else {
            *q++ = *p;
        }
    }
    *q = '\0';
}

esp_err_t get_config_param_str(char* name, char** param)
{
    nvs_handle_t nvs;

    esp_err_t err = nvs_open(PARAM_NAMESPACE, NVS_READONLY, &nvs);
    if (err == ESP_OK) {
        size_t len;
        if ( (err = nvs_get_str(nvs, name, NULL, &len)) == ESP_OK) {
            *param = (char *)malloc(len);
            if (*param == NULL) {
                nvs_close(nvs);
                return ESP_ERR_NO_MEM;
            }
            err = nvs_get_str(nvs, name, *param, &len);
            ESP_LOGI(TAG, "%s %s", name, *param);
        } else {
            return err;
        }
        nvs_close(nvs);
    } else {
        return err;
    }
    return ESP_OK;
}

esp_err_t get_config_param_int(char* name, int* param)
{
    nvs_handle_t nvs;

    esp_err_t err = nvs_open(PARAM_NAMESPACE, NVS_READONLY, &nvs);
    if (err == ESP_OK) {
        if ( (err = nvs_get_i32(nvs, name, (int32_t*)(param))) == ESP_OK) {
            ESP_LOGI(TAG, "%s %d", name, *param);
        } else {
            return err;
        }
        nvs_close(nvs);
    } else {
        return err;
    }
    return ESP_OK;
}

esp_err_t get_config_param_blob(char* name, uint8_t** blob, size_t blob_len)
{
    nvs_handle_t nvs;

    esp_err_t err = nvs_open(PARAM_NAMESPACE, NVS_READONLY, &nvs);
    if (err == ESP_OK) {
        size_t len;
        if ( (err = nvs_get_blob(nvs, name, NULL, &len)) == ESP_OK) {
            if (len != blob_len) {
                nvs_close(nvs);
                return ESP_ERR_NVS_INVALID_LENGTH;
            }
            *blob = (uint8_t *)malloc(len);
            if (*blob == NULL) {
                nvs_close(nvs);
                return ESP_ERR_NO_MEM;
            }
            err = nvs_get_blob(nvs, name, *blob, &len);
            ESP_LOGI(TAG, "%s: %d", name, len);
        } else {
            return err;
        }
        nvs_close(nvs);
    } else {
        return err;
    }
    return ESP_OK;
}

esp_err_t set_config_param_str(const char* name, const char* value)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(PARAM_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;
    err = nvs_set_str(nvs, name, value);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

esp_err_t set_config_param_int(const char* name, int32_t value)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(PARAM_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;
    err = nvs_set_i32(nvs, name, value);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

esp_err_t set_config_param_blob(const char* name, const void* data, size_t len)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(PARAM_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(nvs, name, data, len);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

void register_router(void)
{
    register_show();
    register_set_sta();
    register_set_mac();
    register_scan();
    register_set_sta_static();
    register_set_ap_ip();
    register_set_ap_dns();
    register_set_eth_nat();
    register_set_eth_dhcps();
    register_set_hostname();
    register_dhcp_reserve();
    register_portmap();
    register_acl();
    register_bytes();
    register_pcap();
    register_web_ui();
    register_set_router_password();
    register_set_led_gpio();
    register_set_led_lowactive();
    register_set_led_strip();
    register_set_ttl();
    register_set_tx_power();
#if defined(CONFIG_IDF_TARGET_ESP32C6)
    register_set_rf_switch();
#endif
#if defined(CONFIG_ETH_DOWNLINK_W5500)
    register_set_spi_clock();
    register_w5500();
#endif
    register_remote_console_cmd();
    register_syslog_cmd();
    register_set_tz();
    register_set_vpn();
}

/** Arguments used by 'set_sta' function */
static struct {
    struct arg_str* ssid;
    struct arg_str* password;
    struct arg_str* ent_username;
    struct arg_str* ent_identity;
    struct arg_int* eap_method;
    struct arg_int* ttls_phase2;
    struct arg_int* cert_bundle;
    struct arg_int* no_time_check;
    struct arg_end* end;
} set_sta_arg;

/* 'set_sta' command */
int set_sta(int argc, char **argv)
{
    esp_err_t err;
    nvs_handle_t nvs;

    int nerrors = arg_parse(argc, argv, (void **) &set_sta_arg);
    if (nerrors != 0) {
        arg_print_errors(stderr, set_sta_arg.end, argv[0]);
        return 1;
    }

    preprocess_string((char*)set_sta_arg.ssid->sval[0]);
    preprocess_string((char*)set_sta_arg.password->sval[0]);

    err = nvs_open(PARAM_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_str(nvs, "ssid", set_sta_arg.ssid->sval[0]);
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, "passwd", set_sta_arg.password->sval[0]);
        if (err == ESP_OK) {
            if (set_sta_arg.ent_username->count > 0) {
                err = nvs_set_str(nvs, "ent_username", set_sta_arg.ent_username->sval[0]);
            }
            else {
                err = nvs_set_str(nvs, "ent_username", "");
            }

            if (err == ESP_OK) {
                if (set_sta_arg.ent_identity->count > 0) {
                    err = nvs_set_str(nvs, "ent_identity", set_sta_arg.ent_identity->sval[0]);
                }
                else {
                    err = nvs_set_str(nvs, "ent_identity", "");
                }

                if (err == ESP_OK) {
                    // Save WPA2-Enterprise settings
                    if (set_sta_arg.eap_method->count > 0) {
                        nvs_set_i32(nvs, "eap_method", set_sta_arg.eap_method->ival[0]);
                        eap_method = set_sta_arg.eap_method->ival[0];
                    }
                    if (set_sta_arg.ttls_phase2->count > 0) {
                        nvs_set_i32(nvs, "ttls_phase2", set_sta_arg.ttls_phase2->ival[0]);
                        ttls_phase2 = set_sta_arg.ttls_phase2->ival[0];
                    }
                    if (set_sta_arg.cert_bundle->count > 0) {
                        nvs_set_i32(nvs, "cert_bundle", set_sta_arg.cert_bundle->ival[0]);
                        use_cert_bundle = set_sta_arg.cert_bundle->ival[0];
                    }
                    if (set_sta_arg.no_time_check->count > 0) {
                        nvs_set_i32(nvs, "no_time_chk", set_sta_arg.no_time_check->ival[0]);
                        disable_time_check = set_sta_arg.no_time_check->ival[0];
                    }

                    err = nvs_commit(nvs);
                    if (err == ESP_OK) {
                        ESP_LOGI(TAG, "STA settings %s/%s stored.", set_sta_arg.ssid->sval[0], set_sta_arg.password->sval[0]);
                    }
                }
            }
        }
    }
    nvs_close(nvs);
    return err;
}

static void register_set_sta(void)
{
    set_sta_arg.ssid = arg_str1(NULL, NULL, "<ssid>", "SSID");
    set_sta_arg.password = arg_str1(NULL, NULL, "<passwd>", "Password");
    set_sta_arg.ent_username = arg_str0("-u", "--username", "<ent_username>", "Enterprise username");
    set_sta_arg.ent_identity = arg_str0("-a", "--identity", "<ent_identity>", "Enterprise identity");
    set_sta_arg.eap_method = arg_int0("-e", "--eap", "<0-3>", "EAP method (0=Auto,1=PEAP,2=TTLS,3=TLS)");
    set_sta_arg.ttls_phase2 = arg_int0("-p", "--phase2", "<0-3>", "TTLS phase2 (0=MSCHAPv2,1=MSCHAP,2=PAP,3=CHAP)");
    set_sta_arg.cert_bundle = arg_int0("-c", "--cert-bundle", "<0|1>", "Use CA cert bundle");
    set_sta_arg.no_time_check = arg_int0("-t", "--no-time-check", "<0|1>", "Skip cert time check");
    set_sta_arg.end = arg_end(6);

    const esp_console_cmd_t cmd = {
        .command = "set_sta",
        .help = "Set SSID and password of the STA interface",
        .hint = NULL,
        .func = &set_sta,
        .argtable = &set_sta_arg
    };
    ESP_ERROR_CHECK( esp_console_cmd_register(&cmd) );
}


/** Arguments used by 'set_sta_static' function */
static struct {
    struct arg_str *static_ip;
    struct arg_str *subnet_mask;
    struct arg_str *gateway_addr;
    struct arg_end *end;
} set_sta_static_arg;

/* 'set_sta_static' command */
int set_sta_static(int argc, char **argv)
{
    esp_err_t err;
    nvs_handle_t nvs;

    /* "set_sta_static dhcp" clears static IP and reverts to DHCP */
    if (argc == 2 && strcmp(argv[1], "dhcp") == 0) {
        err = nvs_open(PARAM_NAMESPACE, NVS_READWRITE, &nvs);
        if (err != ESP_OK) {
            return err;
        }
        nvs_erase_key(nvs, "static_ip");
        nvs_erase_key(nvs, "subnet_mask");
        nvs_erase_key(nvs, "gateway_addr");
        err = nvs_commit(nvs);
        nvs_close(nvs);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Static IP cleared. Will use DHCP after reboot.");
        }
        return err;
    }

    int nerrors = arg_parse(argc, argv, (void **) &set_sta_static_arg);
    if (nerrors != 0) {
        arg_print_errors(stderr, set_sta_static_arg.end, argv[0]);
        return 1;
    }

    preprocess_string((char*)set_sta_static_arg.static_ip->sval[0]);
    preprocess_string((char*)set_sta_static_arg.subnet_mask->sval[0]);
    preprocess_string((char*)set_sta_static_arg.gateway_addr->sval[0]);

    err = nvs_open(PARAM_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_str(nvs, "static_ip", set_sta_static_arg.static_ip->sval[0]);
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, "subnet_mask", set_sta_static_arg.subnet_mask->sval[0]);
        if (err == ESP_OK) {
            err = nvs_set_str(nvs, "gateway_addr", set_sta_static_arg.gateway_addr->sval[0]);
            if (err == ESP_OK) {
              err = nvs_commit(nvs);
                if (err == ESP_OK) {
                    ESP_LOGI(TAG, "STA Static IP settings %s/%s/%s stored.", set_sta_static_arg.static_ip->sval[0], set_sta_static_arg.subnet_mask->sval[0], set_sta_static_arg.gateway_addr->sval[0]);
                }
            }
        }
    }
    nvs_close(nvs);
    return err;
}

static void register_set_sta_static(void)
{
    set_sta_static_arg.static_ip = arg_str1(NULL, NULL, "<ip>", "IP");
    set_sta_static_arg.subnet_mask = arg_str1(NULL, NULL, "<subnet>", "Subnet Mask");
    set_sta_static_arg.gateway_addr = arg_str1(NULL, NULL, "<gw>", "Gateway Address");
    set_sta_static_arg.end = arg_end(3);

    const esp_console_cmd_t cmd = {
        .command = "set_sta_static",
        .help = "Set Static IP for the STA interface, or 'set_sta_static dhcp' to use DHCP",
        .hint = NULL,
        .func = &set_sta_static,
        .argtable = &set_sta_static_arg
    };
    ESP_ERROR_CHECK( esp_console_cmd_register(&cmd) );
}

/** Arguments used by 'set_mac' function */
static struct {
    struct arg_int *mac0;
    struct arg_int *mac1;
    struct arg_int *mac2;
    struct arg_int *mac3;
    struct arg_int *mac4;
    struct arg_int *mac5;
    struct arg_end *end;
} set_mac_arg;

esp_err_t set_mac(const char *key, const char *interface, int argc, char **argv) {
    esp_err_t err;
    nvs_handle_t nvs;

    int nerrors = arg_parse(argc, argv, (void **) &set_mac_arg);
    if (nerrors != 0) {
        arg_print_errors(stderr, set_mac_arg.end, argv[0]);
        return 1;
    }

    err = nvs_open(PARAM_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t mac[] = {set_mac_arg.mac0->ival[0], set_mac_arg.mac1->ival[0], set_mac_arg.mac2->ival[0], set_mac_arg.mac3->ival[0], set_mac_arg.mac4->ival[0], set_mac_arg.mac5->ival[0]};
    err = nvs_set_blob(nvs, key, mac, sizeof(mac));
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "%s mac address %02X:%02X:%02X:%02X:%02X:%02X stored.", interface, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        }
    }
    nvs_close(nvs);
    return err;
}

int set_sta_mac(int argc, char **argv) {
    return set_mac("mac", "STA", argc, argv);
}

static void register_set_mac(void)
{
    set_mac_arg.mac0 = arg_int1(NULL, NULL, "<octet>", "First octet");
    set_mac_arg.mac1 = arg_int1(NULL, NULL, "<octet>", "Second octet");
    set_mac_arg.mac2 = arg_int1(NULL, NULL, "<octet>", "Third octet");
    set_mac_arg.mac3 = arg_int1(NULL, NULL, "<octet>", "Fourth octet");
    set_mac_arg.mac4 = arg_int1(NULL, NULL, "<octet>", "Fifth octet");
    set_mac_arg.mac5 = arg_int1(NULL, NULL, "<octet>", "Sixth octet");
    set_mac_arg.end = arg_end(6);

    const esp_console_cmd_t cmd_sta = {
        .command = "set_sta_mac",
        .help = "Set MAC address of the STA interface",
        .hint = NULL,
        .func = &set_sta_mac,
        .argtable = &set_mac_arg
    };
    ESP_ERROR_CHECK( esp_console_cmd_register(&cmd_sta) );
}

/** Arguments used by 'set_ap_ip' function */
static struct {
    struct arg_str *ap_ip_str;
    struct arg_end *end;
} set_ap_ip_arg;


/* 'set_ap_ip' command */
int set_ap_ip(int argc, char **argv)
{
    esp_err_t err;

    int nerrors = arg_parse(argc, argv, (void **) &set_ap_ip_arg);
    if (nerrors != 0) {
        arg_print_errors(stderr, set_ap_ip_arg.end, argv[0]);
        return 1;
    }

    preprocess_string((char*)set_ap_ip_arg.ap_ip_str->sval[0]);

    // Get current Ethernet IP to check if network is changing
    char* old_ap_ip = NULL;
    get_config_param_str("ap_ip", &old_ap_ip);

    // Parse new IP
    uint32_t new_ip = esp_ip4addr_aton((char*)set_ap_ip_arg.ap_ip_str->sval[0]);

    // Check if we're changing to a different Class C network
    bool clear_config = false;
    if (old_ap_ip != NULL) {
        uint32_t old_ip = esp_ip4addr_aton(old_ap_ip);

        // Compare first 3 octets (Class C network: /24)
        if ((old_ip & 0xFFFFFF00) != (new_ip & 0xFFFFFF00)) {
            clear_config = true;
            ESP_LOGI(TAG, "Ethernet IP network changed from %s to %s - clearing reservations and port mappings",
                     old_ap_ip, set_ap_ip_arg.ap_ip_str->sval[0]);
        }
        free(old_ap_ip);
    }

    err = set_config_param_str("ap_ip", set_ap_ip_arg.ap_ip_str->sval[0]);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Ethernet IP address %s stored.", set_ap_ip_arg.ap_ip_str->sval[0]);
    }

    // Clear DHCP reservations and port mappings if network changed
    if (clear_config && err == ESP_OK) {
        clear_all_dhcp_reservations();
        clear_all_portmaps();
    }

    return err;
}

static void register_set_ap_ip(void)
{
    set_ap_ip_arg.ap_ip_str = arg_str1(NULL, NULL, "<ip>", "IP");
    set_ap_ip_arg.end = arg_end(1);

    const esp_console_cmd_t cmd = {
        .command = "set_ap_ip",
        .help = "Set IP for the Ethernet downlink interface",
        .hint = NULL,
        .func = &set_ap_ip,
        .argtable = &set_ap_ip_arg
    };
    ESP_ERROR_CHECK( esp_console_cmd_register(&cmd) );
}

/** Arguments used by 'set_ap_dns' function */
static struct {
    struct arg_str *dns_str;
    struct arg_end *end;
} set_ap_dns_arg;

/* 'set_ap_dns' command */
static int set_ap_dns(int argc, char **argv)
{
    esp_err_t err;

    int nerrors = arg_parse(argc, argv, (void **) &set_ap_dns_arg);
    if (nerrors != 0) {
        arg_print_errors(stderr, set_ap_dns_arg.end, argv[0]);
        return 1;
    }

    preprocess_string((char*)set_ap_dns_arg.dns_str->sval[0]);

    err = set_config_param_str("ap_dns", set_ap_dns_arg.dns_str->sval[0]);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Ethernet DNS server '%s' stored.", set_ap_dns_arg.dns_str->sval[0]);
        printf("Ethernet DNS set to: %s\n", set_ap_dns_arg.dns_str->sval[0]);
        if (strlen(set_ap_dns_arg.dns_str->sval[0]) == 0) {
            printf("DNS will be learned from upstream (default behavior).\n");
        }
        printf("Restart to apply.\n");
    }

    // Update global
    free(ap_dns);
    ap_dns = strdup(set_ap_dns_arg.dns_str->sval[0]);

    return err;
}

static void register_set_ap_dns(void)
{
    set_ap_dns_arg.dns_str = arg_str1(NULL, NULL, "<dns>", "DNS server IP (empty string to clear)");
    set_ap_dns_arg.end = arg_end(1);

    const esp_console_cmd_t cmd = {
        .command = "set_ap_dns",
        .help = "Set DNS server for Ethernet clients (empty to use upstream)",
        .hint = NULL,
        .func = &set_ap_dns,
        .argtable = &set_ap_dns_arg
    };
    ESP_ERROR_CHECK( esp_console_cmd_register(&cmd) );
}

/* 'set_eth_nat' command */
static struct {
    struct arg_str *mode;
    struct arg_end *end;
} set_eth_nat_arg;

static int set_eth_nat(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **) &set_eth_nat_arg);
    if (nerrors != 0) {
        arg_print_errors(stderr, set_eth_nat_arg.end, argv[0]);
        return 1;
    }

    const char *mode = set_eth_nat_arg.mode->sval[0];
    int val;
    if (strcasecmp(mode, "on") == 0 || strcmp(mode, "1") == 0) {
        val = 1;
    } else if (strcasecmp(mode, "off") == 0 || strcmp(mode, "0") == 0) {
        val = 0;
    } else {
        printf("Usage: set_eth_nat <on|off>\n");
        return 1;
    }

    esp_err_t err = set_config_param_int("eth_nat", val);
    if (err == ESP_OK) {
        eth_nat_enabled = val;
        printf("Ethernet NAT %s. Restart to apply.\n", val ? "enabled" : "disabled (routed mode)");
    }
    return err;
}

static void register_set_eth_nat(void)
{
    set_eth_nat_arg.mode = arg_str1(NULL, NULL, "<on|off>", "Enable or disable NAT on Ethernet");
    set_eth_nat_arg.end = arg_end(1);

    const esp_console_cmd_t cmd = {
        .command = "set_eth_nat",
        .help = "Enable/disable NAT on Ethernet downlink (default: on)",
        .hint = NULL,
        .func = &set_eth_nat,
        .argtable = &set_eth_nat_arg
    };
    ESP_ERROR_CHECK( esp_console_cmd_register(&cmd) );
}

/* 'set_eth_dhcps' command */
static struct {
    struct arg_str *mode;
    struct arg_end *end;
} set_eth_dhcps_arg;

static int set_eth_dhcps(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **) &set_eth_dhcps_arg);
    if (nerrors != 0) {
        arg_print_errors(stderr, set_eth_dhcps_arg.end, argv[0]);
        return 1;
    }

    const char *mode = set_eth_dhcps_arg.mode->sval[0];
    int val;
    if (strcasecmp(mode, "on") == 0 || strcmp(mode, "1") == 0) {
        val = 1;
    } else if (strcasecmp(mode, "off") == 0 || strcmp(mode, "0") == 0) {
        val = 0;
    } else {
        printf("Usage: set_eth_dhcps <on|off>\n");
        return 1;
    }

    esp_err_t err = set_config_param_int("eth_dhcps", val);
    if (err == ESP_OK) {
        eth_dhcps_enabled = val;
        printf("Ethernet DHCP server %s. Restart to apply.\n", val ? "enabled" : "disabled");
    }
    return err;
}

static void register_set_eth_dhcps(void)
{
    set_eth_dhcps_arg.mode = arg_str1(NULL, NULL, "<on|off>", "Enable or disable DHCP server on Ethernet");
    set_eth_dhcps_arg.end = arg_end(1);

    const esp_console_cmd_t cmd = {
        .command = "set_eth_dhcps",
        .help = "Enable/disable DHCP server on Ethernet downlink (default: on)",
        .hint = NULL,
        .func = &set_eth_dhcps,
        .argtable = &set_eth_dhcps_arg
    };
    ESP_ERROR_CHECK( esp_console_cmd_register(&cmd) );
}

/* 'set_hostname' command */
static struct {
    struct arg_str *name;
    struct arg_end *end;
} set_hostname_arg;

static int set_hostname_cmd(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **) &set_hostname_arg);
    if (nerrors != 0) {
        arg_print_errors(stderr, set_hostname_arg.end, argv[0]);
        return 1;
    }

    const char *name = set_hostname_arg.name->sval[0];
    preprocess_string((char*)name);

    // Validate: max 32 chars, only alphanumeric and hyphens (RFC 952)
    size_t len = strlen(name);
    if (len > 32) {
        printf("Hostname too long (max 32 characters).\n");
        return 1;
    }
    for (size_t i = 0; i < len; i++) {
        char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-')) {
            printf("Invalid character '%c'. Use only letters, digits, and hyphens.\n", c);
            return 1;
        }
    }

    esp_err_t err = set_config_param_str("hostname", name);
    if (err != ESP_OK) {
        printf("Error saving hostname: %s\n", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "Hostname set to '%s'", name);
    if (len > 0) {
        printf("Hostname set to: %s\n", name);
    } else {
        printf("Hostname cleared (will use default 'esp32-eth-router').\n");
    }
    printf("Restart to apply.\n");

    free(hostname);
    hostname = strdup(name);

    return err;
}

static void register_set_hostname(void)
{
    set_hostname_arg.name = arg_str1(NULL, NULL, "<name>", "DHCP hostname (empty to clear)");
    set_hostname_arg.end = arg_end(1);

    const esp_console_cmd_t cmd = {
        .command = "set_hostname",
        .help = "Set DHCP client hostname for upstream network (empty to use default)",
        .hint = NULL,
        .func = &set_hostname_cmd,
        .argtable = &set_hostname_arg
    };
    ESP_ERROR_CHECK( esp_console_cmd_register(&cmd) );
}

/* Format web UI bind mask as a string, e.g. "ETH STA " */
static void fmt_web_bind(uint8_t bind, char *buf)
{
    buf[0] = '\0';
    if (bind & RC_BIND_AP)  strcat(buf, "ETH ");
    if (bind & RC_BIND_STA) strcat(buf, "STA ");
    if (bind & RC_BIND_VPN) strcat(buf, "VPN ");
    if (buf[0] == '\0') strcpy(buf, "(none)");
}

/* 'web_ui' command */
static int web_ui_cmd(int argc, char **argv)
{

    if (argc < 2) {
        /* Show current status */
        char* lock = NULL;
        get_config_param_str("web_disabled", &lock);
        bool enabled = (lock == NULL || strcmp(lock, "0") == 0);
        int port = 80;
        get_config_param_int("web_port", &port);
        char bind_str[20];
        fmt_web_bind(web_ui_get_bind(), bind_str);
        printf("Web interface: %s (port %d, bind: %s)\n", enabled ? "enabled" : "disabled", port, bind_str);
        printf("\nUsage:\n");
        printf("  web_ui enable                     - Enable web interface (after reboot)\n");
        printf("  web_ui disable                    - Disable web interface (after reboot)\n");
        printf("  web_ui port <port>                - Set web server port (after reboot)\n");
        printf("  web_ui bind <eth|sta|vpn|all>     - Set allowed interfaces (takes effect immediately)\n");
        if (lock != NULL) free(lock);
        return 0;
    }

    const char *action = argv[1];
    esp_err_t err = ESP_OK;

    if (strcmp(action, "enable") == 0) {
        err = set_config_param_str("web_disabled", "0");
        if (err == ESP_OK) {
            ESP_LOGW(TAG, "Web interface enabled via CLI.");
            printf("Web interface will be enabled after reboot.\n");
        }
    } else if (strcmp(action, "disable") == 0) {
        err = set_config_param_str("web_disabled", "1");
        if (err == ESP_OK) {
            ESP_LOGW(TAG, "Web interface disabled via CLI.");
            printf("Web interface will be disabled after reboot.\n");
            printf("Use 'web_ui enable' to re-enable it.\n");
        }
    } else if (strcmp(action, "port") == 0) {
        if (argc < 3) {
            int port = 80;
            get_config_param_int("web_port", &port);
            printf("Current web server port: %d\n", port);
            printf("Usage: web_ui port <port>\n");
            return 0;
        }
        int port = atoi(argv[2]);
        if (port < 1 || port > 65535) {
            printf("Invalid port: %s (must be 1-65535)\n", argv[2]);
            return 1;
        }
        err = set_config_param_int("web_port", port);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Web server port set to %d.", port);
            printf("Web server port set to %d (after reboot).\n", port);
        }
    } else if (strcmp(action, "bind") == 0) {
        if (argc < 3) {
            char bind_str[20];
            fmt_web_bind(web_ui_get_bind(), bind_str);
            printf("Current web UI bind: %s\n", bind_str);
            printf("Usage: web_ui bind <eth|sta|vpn|all>\n");
            printf("  Interfaces may be comma-separated: web_ui bind eth,sta\n");
            return 0;
        }
        /* Parse comma-separated interface list or "all" */
        uint8_t bind = 0;
        char arg[64];
        strlcpy(arg, argv[2], sizeof(arg));
        if (strcmp(arg, "all") == 0) {
            bind = RC_BIND_AP | RC_BIND_STA | RC_BIND_VPN;
        } else {
            char *tok = strtok(arg, ",");
            while (tok) {
                if      (strcmp(tok, "eth") == 0) bind |= RC_BIND_AP;
                else if (strcmp(tok, "sta") == 0) bind |= RC_BIND_STA;
                else if (strcmp(tok, "vpn") == 0) bind |= RC_BIND_VPN;
                else {
                    printf("Unknown interface '%s' (valid: eth, sta, vpn, all)\n", tok);
                    return 1;
                }
                tok = strtok(NULL, ",");
            }
        }
        if (bind == 0) {
            printf("At least one interface required, defaulting to eth.\n");
            bind = RC_BIND_AP;
        }
        web_ui_set_bind(bind);
        char bind_str[20];
        fmt_web_bind(bind, bind_str);
        printf("Web UI access restricted to: %s(takes effect immediately)\n", bind_str);
    } else {
        printf("Unknown action: %s\n", action);
        printf("Usage: web_ui <enable|disable|port|bind>\n");
        return 1;
    }

    return err;
}

static void register_web_ui(void)
{
    const esp_console_cmd_t cmd = {
        .command = "web_ui",
        .help = "Manage the web interface\n"
                "  web_ui                        - Show current status\n"
                "  web_ui enable                 - Enable web interface (after reboot)\n"
                "  web_ui disable                - Disable web interface (after reboot)\n"
                "  web_ui port <port>            - Set web server port (after reboot)\n"
                "  web_ui bind <eth|sta|vpn|all> - Set allowed interfaces (immediate)",
        .hint = " <enable|disable|port|bind>",
        .func = &web_ui_cmd,
    };
    ESP_ERROR_CHECK( esp_console_cmd_register(&cmd) );
}

/* --- Password hashing (SHA-256 + 16-byte salt) --- */
/* NVS key "web_password" stores "salt_hex:hash_hex" (32 + 1 + 64 = 97 chars) */

#define PW_SALT_LEN 16
#define PW_HASH_LEN 32   /* SHA-256 output */

static void pw_bytes_to_hex(const uint8_t *src, size_t len, char *out)
{
    for (size_t i = 0; i < len; i++) {
        sprintf(out + i * 2, "%02x", src[i]);
    }
    out[len * 2] = '\0';
}

static int pw_hex_to_bytes(const char *src, uint8_t *dst, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        unsigned int b;
        if (sscanf(src + i * 2, "%2x", &b) != 1) return -1;
        dst[i] = (uint8_t)b;
    }
    return 0;
}

static void pw_compute_hash(const uint8_t *salt, size_t salt_len,
                            const char *plaintext, uint8_t *hash_out)
{
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);  /* 0 = SHA-256 (not 224) */
    mbedtls_sha256_update(&ctx, salt, salt_len);
    mbedtls_sha256_update(&ctx, (const uint8_t *)plaintext, strlen(plaintext));
    mbedtls_sha256_finish(&ctx, hash_out);
    mbedtls_sha256_free(&ctx);
}

bool is_web_password_set(void)
{
    nvs_handle_t nvs;
    if (nvs_open(PARAM_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) return false;

    size_t len = 0;
    esp_err_t err = nvs_get_str(nvs, "web_password", NULL, &len);
    nvs_close(nvs);
    return (err == ESP_OK && len > 1);  /* len includes null terminator */
}

bool verify_web_password(const char *plaintext)
{
    nvs_handle_t nvs;
    if (nvs_open(PARAM_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) return false;

    size_t stored_len = 0;
    esp_err_t err = nvs_get_str(nvs, "web_password", NULL, &stored_len);
    if (err != ESP_OK || stored_len <= 1) { nvs_close(nvs); return false; }

    char *stored = malloc(stored_len);
    if (!stored) { nvs_close(nvs); return false; }
    nvs_get_str(nvs, "web_password", stored, &stored_len);
    nvs_close(nvs);

    /* New format: "salt_hex:hash_hex" (32 + 1 + 64 = 97 chars + null) */
    char *colon = strchr(stored, ':');
    if (colon && (colon - stored) == PW_SALT_LEN * 2
              && strlen(colon + 1) == PW_HASH_LEN * 2) {
        /* Hashed format */
        uint8_t salt[PW_SALT_LEN], stored_hash[PW_HASH_LEN], computed_hash[PW_HASH_LEN];
        if (pw_hex_to_bytes(stored, salt, PW_SALT_LEN) != 0 ||
            pw_hex_to_bytes(colon + 1, stored_hash, PW_HASH_LEN) != 0) {
            free(stored);
            return false;
        }
        pw_compute_hash(salt, PW_SALT_LEN, plaintext, computed_hash);
        free(stored);

        /* Constant-time comparison */
        volatile int diff = 0;
        for (int i = 0; i < PW_HASH_LEN; i++) {
            diff |= stored_hash[i] ^ computed_hash[i];
        }
        return diff == 0;
    }

    /* Legacy plaintext format - compare directly, then migrate */
    bool match = (strcmp(stored, plaintext) == 0);
    free(stored);
    if (match) {
        /* Silently migrate to hashed format */
        set_web_password_hashed(plaintext);
    }
    return match;
}

esp_err_t set_web_password_hashed(const char *plaintext)
{
    if (plaintext[0] == '\0') {
        /* Empty = disable password */
        return set_config_param_str("web_password", "");
    }

    uint8_t salt[PW_SALT_LEN], hash[PW_HASH_LEN];
    esp_fill_random(salt, PW_SALT_LEN);
    pw_compute_hash(salt, PW_SALT_LEN, plaintext, hash);

    /* Format: "salt_hex:hash_hex" */
    char buf[PW_SALT_LEN * 2 + 1 + PW_HASH_LEN * 2 + 1];
    pw_bytes_to_hex(salt, PW_SALT_LEN, buf);
    buf[PW_SALT_LEN * 2] = ':';
    pw_bytes_to_hex(hash, PW_HASH_LEN, buf + PW_SALT_LEN * 2 + 1);

    return set_config_param_str("web_password", buf);
}

/* 'set_router_password' command */
static int set_router_password_cmd(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: set_router_password <password>\n");
        printf("Use empty string \"\" to disable password protection\n");
        return 1;
    }

    esp_err_t err = set_web_password_hashed(argv[1]);
    if (err == ESP_OK) {
        if (argv[1][0] == '\0') {
            ESP_LOGW(TAG, "Web password protection disabled via CLI.");
            printf("Password protection disabled.\n");
        } else {
            ESP_LOGW(TAG, "Web password changed via CLI.");
            printf("Password updated successfully.\n");
        }
    } else {
        printf("Failed to set password\n");
    }
    return err;
}

static void register_set_router_password(void)
{
    const esp_console_cmd_t cmd = {
        .command = "set_router_password",
        .help = "Set router password for web and remote console (empty string to disable)",
        .hint = NULL,
        .func = &set_router_password_cmd,
    };
    ESP_ERROR_CHECK( esp_console_cmd_register(&cmd) );
}

/** Arguments used by 'portmap' function */
static struct {
    struct arg_str *add_del;
    struct arg_str *TCP_UDP;
    struct arg_int *ext_port;
    struct arg_str *int_ip;
    struct arg_int *int_port;
    struct arg_str *iface;
    struct arg_end *end;
} portmap_args;

/* 'portmap' command */
int portmap(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **) &portmap_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, portmap_args.end, argv[0]);
        return 1;
    }

    bool add;
    if (strcmp((char *)portmap_args.add_del->sval[0], "add")== 0) {
        add = true;
    } else if (strcmp((char *)portmap_args.add_del->sval[0], "del")== 0) {
        add = false;
    } else {
        printf("Must be 'add' or 'del'\n");
        return 1;
    }

    uint8_t tcp_udp;
    if (strcmp((char *)portmap_args.TCP_UDP->sval[0], "TCP")== 0) {
        tcp_udp = PROTO_TCP;
    } else if (strcmp((char *)portmap_args.TCP_UDP->sval[0], "UDP")== 0) {
        tcp_udp = PROTO_UDP;
    } else {
        printf("Must be 'TCP' or 'UDP'\n");
        return 1;
    }

    uint16_t ext_port = portmap_args.ext_port->ival[0];
    const char *ip_str = (char *)portmap_args.int_ip->sval[0];
    uint32_t int_ip = esp_ip4addr_aton(ip_str);

    /* If IP parsing failed (returns IPADDR_NONE), try device name */
    if (int_ip == IPADDR_NONE) {
        if (!resolve_device_name_to_ip(ip_str, &int_ip)) {
            printf("Invalid IP address or device name: %s\n", ip_str);
            return 1;
        }
    }
    uint16_t int_port = portmap_args.int_port->ival[0];

    if (add) {
        /* Validate internal IP is in same /24 network as downlink interface */
        if ((int_ip & 0x00FFFFFF) != (my_ap_ip & 0x00FFFFFF)) {
            ip4_addr_t ap_addr, int_addr;
            ap_addr.addr = my_ap_ip;
            int_addr.addr = int_ip;
            printf("Internal IP " IPSTR " must be in same network as downlink (" IPSTR "/24)\n",
                   IP2STR(&int_addr), IP2STR(&ap_addr));
            return 1;
        }

        /* Check if external port is already in use for this protocol */
        for (int i = 0; i < IP_PORTMAP_MAX; i++) {
            if (portmap_tab[i].valid &&
                portmap_tab[i].proto == tcp_udp &&
                portmap_tab[i].mport == ext_port) {
                printf("External port %d/%s is already mapped\n",
                       ext_port, tcp_udp == PROTO_TCP ? "TCP" : "UDP");
                return 1;
            }
        }

        uint8_t iface = 0;  // Default: STA (uplink)
        if (portmap_args.iface->count > 0) {
            if (strcasecmp(portmap_args.iface->sval[0], "VPN") == 0) {
                iface = 1;
            } else if (strcasecmp(portmap_args.iface->sval[0], "STA") != 0) {
                printf("Interface must be 'STA' or 'VPN'\n");
                return 1;
            }
        }

        add_portmap(tcp_udp, ext_port, int_ip, int_port, iface);
    } else {
        del_portmap(tcp_udp, ext_port);
    }

    return ESP_OK;
}

static void register_portmap(void)
{
    portmap_args.add_del = arg_str1(NULL, NULL, "[add|del]", "add or delete portmapping");
    portmap_args.TCP_UDP = arg_str1(NULL, NULL, "[TCP|UDP]", "TCP or UDP port");
    portmap_args.ext_port = arg_int1(NULL, NULL, "<ext_portno>", "external port number");
    portmap_args.int_ip = arg_str1(NULL, NULL, "<int_ip>", "internal IP or device name");
    portmap_args.int_port = arg_int1(NULL, NULL, "<int_portno>", "internal port number");
    portmap_args.iface = arg_str0(NULL, NULL, "[STA|VPN]", "interface (default: STA)");
    portmap_args.end = arg_end(6);

    const esp_console_cmd_t cmd = {
        .command = "portmap",
        .help = "Add or delete a portmapping to the router",
        .hint = NULL,
        .func = &portmap,
        .argtable = &portmap_args
    };
    ESP_ERROR_CHECK( esp_console_cmd_register(&cmd) );
}

/* 'show' command arguments */
static struct {
    struct arg_str *type;
    struct arg_end *end;
} show_args;

/* 'show' command implementation */
static int show(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **) &show_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, show_args.end, argv[0]);
        return 1;
    }

    if (show_args.type->count == 0) {
        printf("Usage: show <status|config|mappings|acl|vpn|ota>\n");
        printf("  status   - Show router status (connection, clients, memory)\n");
        printf("  config   - Show router configuration (STA/downlink settings)\n");
        printf("  mappings - Show DHCP pool, reservations and port mappings\n");
        printf("  acl      - Show firewall ACL rules\n");
        printf("  vpn      - Show WireGuard VPN status and config\n");
        return 1;
    }

    const char *type = show_args.type->sval[0];

    if (strcmp(type, "status") == 0) {
        // Show status
        printf("Router Status:\n");
        printf("==============\n");

        // Uptime
        char uptime_str[32];
        format_uptime(get_uptime_seconds(), uptime_str, sizeof(uptime_str));
        char boot_time_str[32];
        format_boot_time(boot_time_str, sizeof(boot_time_str));
        printf("Uptime: %s (since %s)\n", uptime_str, boot_time_str);

        // Connection status
        if (ap_connect) {
            wifi_ap_record_t ap_info;
            if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
                printf("Uplink WiFi: connected (ch %d, %d dBm)\n", ap_info.primary, ap_info.rssi);
            } else {
                printf("Uplink WiFi: connected\n");
            }
        } else {
            printf("Uplink WiFi: not connected\n");
        }
        if (ap_connect) {
            ip4_addr_t addr;
            addr.addr = my_ip;
            printf("Uplink IP: " IPSTR "\n", IP2STR(&addr));
        } else {
            printf("Uplink IP: none\n");
        }
        {
            ip4_addr_t addr;
            addr.addr = my_ap_ip;
            printf("Ethernet IP: " IPSTR "\n", IP2STR(&addr));
        }
        printf("Ethernet: %s\n", eth_link_up ? "link up" : "link down");

        // Byte counts
        printf("Bytes sent/received: %" PRIu64 " / %" PRIu64 " bytes\n", get_sta_bytes_sent(), get_sta_bytes_received());

        // Free heap
        printf("Free heap: %lu bytes\n", (unsigned long)esp_get_free_heap_size());

#if defined(CONFIG_ETH_DOWNLINK_W5500)
        // SPI clock speed (read from NVS, same logic as init)
        int spi_mhz = CONFIG_ETH_SPI_CLOCK_MHZ;
        get_config_param_int("spi_clk_mhz", &spi_mhz);
        if (spi_mhz < 1 || spi_mhz > 40) spi_mhz = CONFIG_ETH_SPI_CLOCK_MHZ;
        printf("SPI clock: %d MHz\n", spi_mhz);

        // SPI error counters
        w5500_spi_stats_t spi_stats = w5500_spi_get_stats();
        if (spi_stats.read_spi_fail || spi_stats.read_timeout ||
            spi_stats.write_spi_fail || spi_stats.write_timeout) {
            printf("SPI errors: rd_fail=%"PRIu32" rd_timeout=%"PRIu32
                   " wr_fail=%"PRIu32" wr_timeout=%"PRIu32"\n",
                   spi_stats.read_spi_fail, spi_stats.read_timeout,
                   spi_stats.write_spi_fail, spi_stats.write_timeout);
        } else {
            printf("SPI errors: none\n");
        }
#endif

        // Connected clients
        resync_connect_count();
        printf("Connected clients: %u\n", connect_count);
        if (connect_count > 0) {
            connected_client_t clients[8];
            int count = get_connected_clients(clients, 8);

            if (count > 0) {
                printf("\nClient Details:\n");
                printf("MAC Address       IP Address       Device Name\n");
                printf("----------------  ---------------  -------------------\n");

                for (int i = 0; i < count; i++) {
                    char mac_str[18];
                    sprintf(mac_str, "%02X:%02X:%02X:%02X:%02X:%02X",
                            clients[i].mac[0], clients[i].mac[1], clients[i].mac[2],
                            clients[i].mac[3], clients[i].mac[4], clients[i].mac[5]);

                    char ip_str[16] = "N/A";
                    if (clients[i].has_ip) {
                        ip4_addr_t addr;
                        addr.addr = clients[i].ip;
                        sprintf(ip_str, IPSTR, IP2STR(&addr));
                    }

                    printf("%-17s  %-15s  %s\n", mac_str, ip_str, clients[i].name);
                }
            }
        }
        
    } else if (strcmp(type, "config") == 0) {
        // Show config
        char* static_ip = NULL;
        char* subnet_mask = NULL;
        char* gateway_addr = NULL;

        get_config_param_str("static_ip", &static_ip);
        get_config_param_str("subnet_mask", &subnet_mask);
        get_config_param_str("gateway_addr", &gateway_addr);

        printf("Router Configuration:\n");
        printf("====================\n");

        bool hide_pw = remote_console_is_capturing();
        char* ssid = NULL;
        char* ent_username = NULL;
        char* ent_identity = NULL;
        char* passwd = NULL;
        get_config_param_str("ssid", &ssid);
        get_config_param_str("ent_username", &ent_username);
        get_config_param_str("ent_identity", &ent_identity);
        get_config_param_str("passwd", &passwd);

        printf("STA Settings:\n");
        printf("  SSID: %s\n", ssid != NULL ? ssid : "<undef>");
        printf("  Password: %s\n", passwd == NULL ? "<undef>" : hide_pw ? "***" : passwd);
        if ((ent_username != NULL) && (strlen(ent_username) > 0)) {
            printf("  Enterprise Username: %s\n", ent_username);
            if ((ent_identity != NULL) && (strlen(ent_identity) > 0)) {
                printf("  Enterprise Identity: %s\n", ent_identity);
            }
            const char* eap_names[] = {"Auto", "PEAP", "TTLS", "TLS"};
            const char* phase2_names[] = {"MSCHAPv2", "MSCHAP", "PAP", "CHAP"};
            printf("  EAP Method: %s\n", (eap_method >= 0 && eap_method <= 3) ? eap_names[eap_method] : "Unknown");
            printf("  TTLS Phase 2: %s\n", (ttls_phase2 >= 0 && ttls_phase2 <= 3) ? phase2_names[ttls_phase2] : "Unknown");
            printf("  CA Cert Bundle: %s\n", use_cert_bundle ? "enabled" : "disabled");
            printf("  Time Check: %s\n", disable_time_check ? "disabled" : "enabled");
        } else {
            printf("  Enterprise: <not active>\n");
        }

        if (ssid != NULL) free(ssid);
        if (ent_username != NULL) free(ent_username);
        if (ent_identity != NULL) free(ent_identity);
        if (passwd != NULL) free(passwd);

        if (static_ip != NULL && strlen(static_ip) > 0) {
            printf("  Static IP: %s\n", static_ip);
            printf("  Subnet Mask: %s\n", subnet_mask != NULL ? subnet_mask : "<undef>");
            printf("  Gateway: %s\n", gateway_addr != NULL ? gateway_addr : "<undef>");
        } else {
            printf("  Static IP: <not configured>\n");
        }
        printf("  Hostname: %s\n", hostname);

        printf("\nEthernet Downlink Settings:\n");
        ip4_addr_t addr;
        addr.addr = my_ap_ip;
        printf("  IP Address: " IPSTR "\n", IP2STR(&addr));
        printf("  DNS Server: %s\n", (ap_dns && ap_dns[0]) ? ap_dns : "(upstream)");
        printf("  NAT: %s\n", eth_nat_enabled ? "enabled" : "disabled (routed)");
        printf("  DHCP Server: %s\n", eth_dhcps_enabled ? "enabled" : "disabled");

        char* web_lock = NULL;
        get_config_param_str("web_disabled", &web_lock);
        bool web_enabled = (web_lock == NULL || strcmp(web_lock, "0") == 0);
        int web_port = 80;
        get_config_param_int("web_port", &web_port);
        char web_bind_buf[20];
        fmt_web_bind(web_ui_get_bind(), web_bind_buf);
        printf("\nWeb Interface: %s (port %d, bind: %s)\n", web_enabled ? "enabled" : "disabled", web_port, web_bind_buf);
        if (web_lock != NULL) free(web_lock);

        remote_console_config_t rc_cfg;
        remote_console_get_config(&rc_cfg);
        char rc_bind_buf[20];
        fmt_web_bind(rc_cfg.bind, rc_bind_buf);
        printf("Remote Console: %s (port %d, bind: %s)\n", rc_cfg.enabled ? "enabled" : "disabled", rc_cfg.port, rc_bind_buf);

        int8_t tx_power = 0;
        if (esp_wifi_get_max_tx_power(&tx_power) == ESP_OK) {
            printf("TX Power: %.1f dBm\n", tx_power * 0.25);
        }

        // Cleanup
        if (static_ip != NULL) free(static_ip);
        if (subnet_mask != NULL) free(subnet_mask);
        if (gateway_addr != NULL) free(gateway_addr);
        
    } else if (strcmp(type, "mappings") == 0) {
        // Show mappings
        printf("Network Mappings:\n");
        printf("=================\n");

        printf("\nDHCP Pool:\n");
        print_dhcp_pool();

        printf("\nDHCP Reservations:\n");
        print_dhcp_reservations();

        printf("\nPort Mappings:\n");
        print_portmap_tab();

    } else if (strcmp(type, "acl") == 0) {
        // Show ACL rules with device names
        printf("Firewall ACL Rules:\n");
        printf("===================\n");

        for (int i = 0; i < MAX_ACL_LISTS; i++) {
            acl_print_with_names(i);
        }

    } else if (strcmp(type, "vpn") == 0) {
        printf("WireGuard VPN:\n");
        printf("==============\n");
        printf("Enabled: %s\n", vpn_enabled ? "yes" : "no");
        if (vpn_enabled) {
            const char *state;
            if (vpn_is_connected()) {
                state = "peer up";
            } else if (vpn_connected) {
                state = "handshake pending";
            } else {
                state = "disconnected";
            }
            printf("Status: %s\n", state);
        } else {
            printf("Status: disabled\n");
        }
        printf("Tunnel IP: %s\n", (vpn_address && vpn_address[0]) ? vpn_address : "<not set>");
        printf("Netmask: %s\n", (vpn_netmask && vpn_netmask[0]) ? vpn_netmask : "255.255.255.0");
        printf("Endpoint: %s:%ld\n", (vpn_endpoint && vpn_endpoint[0]) ? vpn_endpoint : "<not set>", (long)vpn_port);
        printf("Keepalive: %ld sec\n", (long)vpn_keepalive);
        printf("Private Key: %s\n", (vpn_private_key && vpn_private_key[0]) ? "<set>" : "<not set>");
        printf("Public Key: %s\n", (vpn_public_key && vpn_public_key[0]) ? vpn_public_key : "<not set>");
        printf("Preshared Key: %s\n", (vpn_preshared_key && vpn_preshared_key[0]) ? "<set>" : "<not set>");
        printf("MSS Clamp: %u\n", ap_mss_clamp);
        printf("Path MTU: %u\n", ap_pmtu);
        printf("Kill Switch: %s\n", vpn_killswitch ? "on" : "off");
        printf("Route All: %s\n", vpn_route_all ? "yes (all traffic)" : "no (split tunnel)");

    } else if (strcmp(type, "ota") == 0) {
        const esp_partition_t *running = esp_ota_get_running_partition();
        const esp_app_desc_t *app_desc = esp_app_get_description();
        const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
        const esp_partition_t *last = esp_ota_get_last_invalid_partition();

        printf("Running partition: %s (0x%lx, %luK)\n",
            running ? running->label : "unknown",
            running ? (unsigned long)running->address : 0,
            running ? (unsigned long)running->size / 1024 : 0);
        printf("Firmware version: %s\n", app_desc ? app_desc->version : "unknown");
        printf("Built: %s %s\n",
            app_desc ? app_desc->date : "unknown",
            app_desc ? app_desc->time : "");
        printf("IDF version: %s\n", app_desc ? app_desc->idf_ver : "unknown");
        printf("Next OTA partition: %s (0x%lx, %luK)\n",
            next ? next->label : "none",
            next ? (unsigned long)next->address : 0,
            next ? (unsigned long)next->size / 1024 : 0);

        if (last) {
            printf("Last invalid partition: %s\n", last->label);
        }

        esp_ota_img_states_t ota_state;
        if (running && esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
            const char *state_str = "unknown";
            switch (ota_state) {
                case ESP_OTA_IMG_NEW:             state_str = "new (first boot pending)"; break;
                case ESP_OTA_IMG_PENDING_VERIFY:  state_str = "pending verify"; break;
                case ESP_OTA_IMG_VALID:           state_str = "valid"; break;
                case ESP_OTA_IMG_INVALID:         state_str = "invalid"; break;
                case ESP_OTA_IMG_ABORTED:         state_str = "aborted"; break;
                default: break;
            }
            printf("Image state: %s\n", state_str);
        }

    } else {
        printf("Invalid parameter. Use: show <status|config|mappings|acl|vpn|ota>\n");
        return 1;
    }

    return 0;
}

static void register_show(void)
{
    show_args.type = arg_str1(NULL, NULL, "[status|config|mappings|acl|vpn|ota]", "Type of information");
    show_args.end = arg_end(1);

    const esp_console_cmd_t cmd = {
        .command = "show",
        .help = "Show router status, config, mappings, ACL rules, VPN or OTA info",
        .hint = NULL,
        .func = &show,
        .argtable = &show_args
    };
    ESP_ERROR_CHECK( esp_console_cmd_register(&cmd) );
}

/** Arguments used by 'dhcp_reserve' function */
static struct {
    struct arg_str *add_del;
    struct arg_str *mac_addr;
    struct arg_str *ip_addr;
    struct arg_str *name;
    struct arg_end *end;
} dhcp_reserve_args;

/* 'dhcp_reserve' command */
int dhcp_reserve(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **) &dhcp_reserve_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, dhcp_reserve_args.end, argv[0]);
        return 1;
    }

    int action; // 0 = add, 1 = del
    if (strcmp((char *)dhcp_reserve_args.add_del->sval[0], "add") == 0) {
        action = 0;
    } else if (strcmp((char *)dhcp_reserve_args.add_del->sval[0], "del") == 0) {
        action = 1;
    } else {
        printf("Must be 'add' or 'del'\n");
        return 1;
    }

    // Parse MAC address (AA:BB:CC:DD:EE:FF or AA-BB-CC-DD-EE-FF)
    unsigned int mac[6];
    const char *mac_str = dhcp_reserve_args.mac_addr->sval[0];
    if (sscanf(mac_str, "%02x:%02x:%02x:%02x:%02x:%02x",
               &mac[0], &mac[1], &mac[2], &mac[3], &mac[4], &mac[5]) != 6 &&
        sscanf(mac_str, "%02x-%02x-%02x-%02x-%02x-%02x",
               &mac[0], &mac[1], &mac[2], &mac[3], &mac[4], &mac[5]) != 6) {
        printf("Invalid MAC address format. Use AA:BB:CC:DD:EE:FF\n");
        return 1;
    }

    uint8_t mac_bytes[6];
    for (int i = 0; i < 6; i++) {
        mac_bytes[i] = (uint8_t)mac[i];
    }

    if (action == 0) {
        // Parse IP address
        uint32_t ip = esp_ip4addr_aton((char *)dhcp_reserve_args.ip_addr->sval[0]);
        if (ip == IPADDR_NONE) {
            printf("Invalid IP address\n");
            return 1;
        }

        // Validate IP is in same /24 network as downlink interface
        if (ip != 0 && (ip & 0x00FFFFFF) != (my_ap_ip & 0x00FFFFFF)) {
            ip4_addr_t ap_addr, res_addr;
            ap_addr.addr = my_ap_ip;
            res_addr.addr = ip;
            printf("IP " IPSTR " must be in same network as downlink (" IPSTR "/24)\n",
                   IP2STR(&res_addr), IP2STR(&ap_addr));
            return 1;
        }

        // Get optional name
        const char *name = NULL;
        if (dhcp_reserve_args.name->count > 0) {
            name = dhcp_reserve_args.name->sval[0];
        }

        esp_err_t err = add_dhcp_reservation(mac_bytes, ip, name);
        if (err == ESP_OK) {
            printf("DHCP reservation added\n");
        } else if (err == ESP_ERR_NO_MEM) {
            printf("No more slots available for DHCP reservations\n");
            return 1;
        } else {
            printf("Failed to add DHCP reservation\n");
            return 1;
        }
    } else {
        esp_err_t err = del_dhcp_reservation(mac_bytes);
        if (err == ESP_OK) {
            printf("DHCP reservation deleted\n");
        } else {
            printf("Failed to delete DHCP reservation\n");
            return 1;
        }
    }

    return ESP_OK;
}

static void register_dhcp_reserve(void)
{
    dhcp_reserve_args.add_del = arg_str1(NULL, NULL, "[add|del]", "add or delete");
    dhcp_reserve_args.mac_addr = arg_str1(NULL, NULL, "<mac>", "MAC address (AA:BB:CC:DD:EE:FF)");
    dhcp_reserve_args.ip_addr = arg_str0(NULL, NULL, "<ip>", "IP address (required for add)");
    dhcp_reserve_args.name = arg_str0("-n", "--name", "<name>", "optional device name");
    dhcp_reserve_args.end = arg_end(4);

    const esp_console_cmd_t cmd = {
        .command = "dhcp_reserve",
        .help = "Add/delete DHCP reservation by MAC",
        .hint = NULL,
        .func = &dhcp_reserve,
        .argtable = &dhcp_reserve_args
    };
    ESP_ERROR_CHECK( esp_console_cmd_register(&cmd) );
}

/* 'bytes' command */
static struct {
    struct arg_str* action;
    struct arg_end* end;
} bytes_args;

static int bytes(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **) &bytes_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, bytes_args.end, argv[0]);
        return 1;
    }

    if (bytes_args.action->count == 0) {
        // Show current byte counts
        printf("STA Interface Byte Counts:\n");
        printf("  Sent:     %" PRIu64 " bytes\n", get_sta_bytes_sent());
        printf("  Received: %" PRIu64 " bytes\n", get_sta_bytes_received());
        return 0;
    }

    const char *action = bytes_args.action->sval[0];
    if (strcmp(action, "reset") == 0) {
        reset_sta_byte_counts();
        printf("Byte counts reset to zero\n");
    } else {
        printf("Usage: bytes [reset]\n");
        printf("  bytes     - Show current byte counts\n");
        printf("  bytes reset - Reset byte counts to zero\n");
        return 1;
    }

    return 0;
}

static void register_bytes(void)
{
    bytes_args.action = arg_str0(NULL, NULL, "[reset]", "reset byte counts or show current counts");
    bytes_args.end = arg_end(1);

    const esp_console_cmd_t cmd = {
        .command = "bytes",
        .help = "Show or reset STA interface byte counts",
        .hint = NULL,
        .func = &bytes,
        .argtable = &bytes_args
    };
    ESP_ERROR_CHECK( esp_console_cmd_register(&cmd) );
}

/* 'pcap' command arguments */
static struct {
    struct arg_str* action;
    struct arg_str* mode;
    struct arg_int* snaplen;
    struct arg_end* end;
} pcap_args;

/* 'pcap' command implementation */
static int pcap(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **) &pcap_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, pcap_args.end, argv[0]);
        return 1;
    }

    if (pcap_args.action->count == 0) {
        printf("Usage: pcap <action> [args]\n");
        printf("  mode [off|acl|promisc] - Get or set capture mode\n");
        printf("    off      - Capture disabled\n");
        printf("    acl      - Capture ACL_MONITOR flagged packets (any interface)\n");
        printf("    promisc  - Capture all downlink client traffic (not STA)\n");
        printf("  status     - Show capture status\n");
        printf("  snaplen [n]- Get or set max capture bytes (64-1600)\n");
        printf("  start      - Legacy: enable promiscuous mode\n");
        printf("  stop       - Legacy: disable capture\n");
        return 1;
    }

    const char *action = pcap_args.action->sval[0];

    if (strcmp(action, "mode") == 0) {
        if (pcap_args.mode->count > 0) {
            const char *mode_str = pcap_args.mode->sval[0];
            if (strcmp(mode_str, "off") == 0) {
                pcap_set_mode(PCAP_MODE_OFF);
                printf("Capture mode: off\n");
            } else if (strcmp(mode_str, "acl") == 0) {
                pcap_set_mode(PCAP_MODE_ACL_MONITOR);
                printf("Capture mode: acl-monitor\n");
                printf("Only packets matching ACL rules with +M flag will be captured (any interface)\n");
            } else if (strcmp(mode_str, "promisc") == 0 || strcmp(mode_str, "promiscuous") == 0) {
                pcap_set_mode(PCAP_MODE_PROMISCUOUS);
                printf("Capture mode: promiscuous\n");
                printf("All downlink client traffic will be captured (STA excluded)\n");
            } else {
                printf("Invalid mode. Use: off, acl, or promisc\n");
                return 1;
            }
            printf("Connect Wireshark to TCP port 19000\n");
        } else {
            printf("Current mode: %s\n", pcap_mode_to_string(pcap_get_mode()));
        }
    } else if (strcmp(action, "start") == 0) {
        // Legacy: start = promiscuous mode
        pcap_set_mode(PCAP_MODE_PROMISCUOUS);
        printf("PCAP capture started in promiscuous mode (snaplen=%d)\n", pcap_get_snaplen());
        printf("Connect Wireshark to TCP port 19000\n");
    } else if (strcmp(action, "stop") == 0) {
        // Legacy: stop = off mode
        pcap_set_mode(PCAP_MODE_OFF);
        printf("PCAP capture stopped\n");
    } else if (strcmp(action, "snaplen") == 0) {
        int val = 0;
        bool has_value = false;

        if (pcap_args.snaplen->count > 0) {
            val = pcap_args.snaplen->ival[0];
            has_value = true;
        } else if (pcap_args.mode->count > 0) {
            // argtable may have put the number in mode slot (string before int)
            val = atoi(pcap_args.mode->sval[0]);
            if (val > 0) has_value = true;
        }

        if (has_value) {
            if (pcap_set_snaplen((uint16_t)val)) {
                printf("Snaplen set to %d bytes\n", pcap_get_snaplen());
            } else {
                printf("Error: snaplen must be between 64 and 1600\n");
                return 1;
            }
        } else {
            printf("Current snaplen: %d bytes\n", pcap_get_snaplen());
        }
    } else if (strcmp(action, "status") == 0) {
        printf("PCAP Capture Status:\n");
        printf("====================\n");
        printf("Mode:     %s\n", pcap_mode_to_string(pcap_get_mode()));
        printf("Client:   %s\n", pcap_client_connected() ? "connected" : "not connected");
        printf("Snaplen:  %d bytes\n", pcap_get_snaplen());

        size_t used, total;
        pcap_get_buffer_usage(&used, &total);
        printf("Buffer:   %u / %u bytes (%.1f%%)\n",
               (unsigned)used, (unsigned)total,
               total > 0 ? (100.0f * used / total) : 0.0f);

        printf("Captured: %lu packets\n", (unsigned long)pcap_get_captured_count());
        printf("Dropped:  %lu packets\n", (unsigned long)pcap_get_dropped_count());
        printf("\nConnection: nc <esp32_ip> 19000 | wireshark -k -i -\n");
    } else {
        printf("Invalid action. Use: pcap <mode|status|snaplen|start|stop>\n");
        return 1;
    }

    return 0;
}

static void register_pcap(void)
{
    pcap_args.action = arg_str1(NULL, NULL, "<action>", "mode|status|snaplen|start|stop");
    pcap_args.mode = arg_str0(NULL, NULL, "<mode>", "off|acl|promisc");
    pcap_args.snaplen = arg_int0(NULL, NULL, "<bytes>", "snaplen value (64-1600)");
    pcap_args.end = arg_end(3);

    const esp_console_cmd_t cmd = {
        .command = "pcap",
        .help = "Control PCAP packet capture (TCP port 19000)",
        .hint = NULL,
        .func = &pcap,
        .argtable = &pcap_args
    };
    ESP_ERROR_CHECK( esp_console_cmd_register(&cmd) );
}

/* 'set_led_gpio' command */
static int set_led_gpio_cmd(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: set_led_gpio <gpio_number|none>\n");
        printf("  gpio_number: GPIO pin number (0-48)\n");
        printf("  none: disable LED status blinking\n");
        printf("\nCurrent setting: ");
        if (led_gpio < 0) {
            printf("none (disabled)\n");
        } else {
            printf("GPIO %d\n", led_gpio);
        }
        return 0;
    }

    esp_err_t err;
    int gpio_num;

    // Parse argument
    if (strcasecmp(argv[1], "none") == 0 || strcmp(argv[1], "-1") == 0) {
        gpio_num = -1;
    } else {
        char *endptr;
        gpio_num = strtol(argv[1], &endptr, 10);
        if (*endptr != '\0' || gpio_num < 0 || gpio_num > 48) {
            printf("Invalid GPIO number. Use 0-48 or 'none'.\n");
            return 1;
        }
    }

    err = set_config_param_int("led_gpio", gpio_num);
    if (err == ESP_OK) {
        if (gpio_num < 0) {
            ESP_LOGI(TAG, "LED GPIO disabled.");
            printf("LED status blinking disabled.\n");
        } else {
            ESP_LOGI(TAG, "LED GPIO set to %d.", gpio_num);
            printf("LED status blinking set to GPIO %d.\n", gpio_num);
        }
        printf("Restart the device for changes to take effect.\n");
    } else {
        printf("Failed to save setting\n");
    }
    return err;
}

static void register_set_led_gpio(void)
{
    const esp_console_cmd_t cmd = {
        .command = "set_led_gpio",
        .help = "Set GPIO for status LED blinking (use 'none' to disable)",
        .hint = NULL,
        .func = &set_led_gpio_cmd,
    };
    ESP_ERROR_CHECK( esp_console_cmd_register(&cmd) );
}

/* 'set_led_lowactive' command - set LED low-active (inverted) mode */
static int set_led_lowactive_cmd(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: set_led_lowactive <true|false>\n");
        printf("  true: LED is active-low (inverted, common for onboard LEDs)\n");
        printf("  false: LED is active-high (default)\n");
        printf("\nCurrent setting: %s\n", led_lowactive ? "true (low-active)" : "false (active-high)");
        return 0;
    }

    esp_err_t err;
    int value;

    // Parse boolean argument
    if (parse_bool_true(argv[1])) {
        value = 1;
    } else if (parse_bool_false(argv[1])) {
        value = 0;
    } else {
        printf("Invalid value. Use true/false.\n");
        return 1;
    }

    err = set_config_param_int("led_low", value);
    if (err == ESP_OK) {
        led_lowactive = value;
        if (value) {
            ESP_LOGI(TAG, "LED set to low-active (inverted) mode.");
            printf("LED set to low-active (inverted) mode.\n");
        } else {
            ESP_LOGI(TAG, "LED set to active-high (normal) mode.");
            printf("LED set to active-high (normal) mode.\n");
        }
        printf("Change takes effect immediately.\n");
    } else {
        printf("Failed to save setting\n");
    }
    return err;
}

static void register_set_led_lowactive(void)
{
    const esp_console_cmd_t cmd = {
        .command = "set_led_lowactive",
        .help = "Set LED to low-active (inverted) mode for active-low LEDs",
        .hint = NULL,
        .func = &set_led_lowactive_cmd,
    };
    ESP_ERROR_CHECK( esp_console_cmd_register(&cmd) );
}

/* 'set_led_strip' command - set GPIO for addressable LED strip (WS2812) */
static int set_led_strip_cmd(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: set_led_strip <gpio_number|none>\n");
        printf("  gpio_number: GPIO pin for WS2812 data line (0-48)\n");
        printf("  none: disable addressable LED strip\n");
        printf("\nCurrent setting: ");
        if (led_strip_gpio < 0) {
            printf("none (disabled)\n");
        } else {
            printf("GPIO %d\n", led_strip_gpio);
        }
        return 0;
    }

    int gpio_num;
    if (strcasecmp(argv[1], "none") == 0 || strcmp(argv[1], "-1") == 0) {
        gpio_num = -1;
    } else {
        char *endptr;
        gpio_num = strtol(argv[1], &endptr, 10);
        if (*endptr != '\0' || gpio_num < 0 || gpio_num > 48) {
            printf("Invalid GPIO number. Use 0-48 or 'none'.\n");
            return 1;
        }
    }

    esp_err_t err = set_config_param_int("ls_gpio", gpio_num);
    if (err == ESP_OK) {
        if (gpio_num < 0) {
            printf("LED strip disabled.\n");
        } else {
            printf("LED strip set to GPIO %d.\n", gpio_num);
        }
        printf("Restart the device for changes to take effect.\n");
    } else {
        printf("Failed to save setting\n");
    }
    return err;
}

static void register_set_led_strip(void)
{
    const esp_console_cmd_t cmd = {
        .command = "set_led_strip",
        .help = "Set GPIO for addressable LED strip (WS2812/SK6812, 'none' to disable, requires restart)",
        .hint = NULL,
        .func = &set_led_strip_cmd,
    };
    ESP_ERROR_CHECK( esp_console_cmd_register(&cmd) );
}

/* 'set_ttl' command - set TTL override for upstream packets */
static int set_ttl_cmd(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: set_ttl <value>\n");
        printf("  value: 0-255 (0 = disabled, no TTL change)\n");
        printf("\nCurrent setting: %d", sta_ttl_override);
        if (sta_ttl_override == 0) {
            printf(" (disabled)\n");
        } else {
            printf("\n");
        }
        return 0;
    }

    esp_err_t err;

    // Parse argument
    char *endptr;
    long ttl_val = strtol(argv[1], &endptr, 10);
    if (*endptr != '\0' || ttl_val < 0 || ttl_val > 255) {
        printf("Invalid TTL value. Use 0-255 (0 = disabled).\n");
        return 1;
    }

    err = set_config_param_int("sta_ttl", (int32_t)ttl_val);
    if (err == ESP_OK) {
        sta_ttl_override = (uint8_t)ttl_val;
        if (ttl_val == 0) {
            ESP_LOGI(TAG, "TTL override disabled.");
            printf("TTL override disabled (no change to packets).\n");
        } else {
            ESP_LOGI(TAG, "TTL override set to %ld.", ttl_val);
            printf("TTL override set to %ld for upstream packets.\n", ttl_val);
        }
    } else {
        printf("Failed to save setting\n");
    }
    return err;
}

static void register_set_ttl(void)
{
    const esp_console_cmd_t cmd = {
        .command = "set_ttl",
        .help = "Set TTL override for upstream STA packets (0 = disabled)",
        .hint = NULL,
        .func = &set_ttl_cmd,
    };
    ESP_ERROR_CHECK( esp_console_cmd_register(&cmd) );
}

/* 'set_tx_power' command - set WiFi TX power */
static int set_tx_power_cmd(int argc, char **argv)
{
    int8_t current_power = 0;
    esp_err_t ret = esp_wifi_get_max_tx_power(&current_power);

    if (argc < 2) {
        printf("Usage: set_tx_power <dBm>\n");
        printf("  dBm: 2-20 (0 = max/default)\n");
        printf("  Actual steps: 2, 5, 7, 8, 11, 13, 14, 15, 16, 18, 20\n");
        if (ret == ESP_OK) {
            printf("\nCurrent TX power: %.1f dBm (raw: %d)\n", current_power * 0.25, current_power);
        }
        int saved = 0;
        get_config_param_int("tx_power", &saved);
        if (saved > 0) {
            printf("Saved setting: %d dBm\n", saved);
        } else {
            printf("Saved setting: max (default)\n");
        }
        return 0;
    }

    char *endptr;
    long dbm = strtol(argv[1], &endptr, 10);
    if (*endptr != '\0' || (dbm != 0 && (dbm < 2 || dbm > 20))) {
        printf("Invalid value. Use 2-20 dBm (0 = max/default).\n");
        return 1;
    }

    esp_err_t err = set_config_param_int("tx_power", (int32_t)dbm);

    if (err != ESP_OK) {
        printf("Failed to save setting\n");
        return err;
    }

    if (dbm == 0) {
        printf("TX power set to max (default). Restart to apply.\n");
    } else {
        int8_t power_qdbm = (int8_t)(dbm * 4);
        ret = esp_wifi_set_max_tx_power(power_qdbm);
        if (ret == ESP_OK) {
            esp_wifi_get_max_tx_power(&current_power);
            printf("TX power set to %.1f dBm (applied immediately, saved for reboot).\n", current_power * 0.25);
        } else {
            printf("Saved for next reboot. Could not apply now: %s\n", esp_err_to_name(ret));
        }
    }
    return 0;
}

static void register_set_tx_power(void)
{
    const esp_console_cmd_t cmd = {
        .command = "set_tx_power",
        .help = "Set WiFi TX power in dBm (2-20, 0 = max/default)",
        .hint = NULL,
        .func = &set_tx_power_cmd,
    };
    ESP_ERROR_CHECK( esp_console_cmd_register(&cmd) );
}

#if defined(CONFIG_IDF_TARGET_ESP32C6)
/* 'set_rf_switch_XIAO' command - XIAO ESP32-C6 antenna selection */
static int set_rf_switch_cmd(int argc, char **argv)
{
    if (argc < 2) {
        int current = 0;
        get_config_param_int("rf_switch", &current);
        printf("XIAO ESP32-C6 RF switch: %s\n", current ? "external antenna" : "built-in antenna (default)");
        printf("Usage: set_rf_switch_XIAO <0|1>\n");
        printf("  0 = built-in ceramic antenna (default)\n");
        printf("  1 = external antenna\n");
        return 0;
    }

    int value;
    if (strcmp(argv[1], "1") == 0) {
        value = 1;
    } else if (strcmp(argv[1], "0") == 0) {
        value = 0;
    } else {
        printf("Invalid value. Use 0 (built-in) or 1 (external).\n");
        return 1;
    }

    esp_err_t err = set_config_param_int("rf_switch", value);
    if (err == ESP_OK) {
        // Apply immediately
        gpio_reset_pin(GPIO_NUM_3);
        gpio_set_direction(GPIO_NUM_3, GPIO_MODE_OUTPUT);
        gpio_set_level(GPIO_NUM_3, 0);  // Activate RF switch control
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_reset_pin(GPIO_NUM_14);
        gpio_set_direction(GPIO_NUM_14, GPIO_MODE_OUTPUT);
        gpio_set_level(GPIO_NUM_14, value); // 0 = built-in, 1 = external

        if (value) {
            printf("Switched to external antenna.\n");
        } else {
            printf("Switched to built-in ceramic antenna.\n");
        }
        printf("Setting saved (persists across reboots).\n");
    } else {
        printf("Failed to save setting\n");
    }
    return err;
}

static void register_set_rf_switch(void)
{
    const esp_console_cmd_t cmd = {
        .command = "set_rf_switch_XIAO",
        .help = "XIAO ESP32-C6: switch between built-in (0) and external (1) antenna",
        .hint = NULL,
        .func = &set_rf_switch_cmd,
    };
    ESP_ERROR_CHECK( esp_console_cmd_register(&cmd) );
}
#endif

#if defined(CONFIG_ETH_DOWNLINK_W5500)
#define _STRINGIFY(x) #x
#define STRINGIFY(x)  _STRINGIFY(x)
/* 'set_spi_clock' command — W5500 SPI clock speed (applied after restart) */
static struct {
    struct arg_int *mhz;
    struct arg_end *end;
} set_spi_clock_arg;

static int set_spi_clock_cmd(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **) &set_spi_clock_arg);
    if (nerrors != 0) {
        arg_print_errors(stderr, set_spi_clock_arg.end, argv[0]);
        return 1;
    }

    int mhz = set_spi_clock_arg.mhz->ival[0];
    if (mhz < 1 || mhz > 40) {
        printf("SPI clock must be between 1 and 40 MHz.\n");
        return 1;
    }

    esp_err_t err = set_config_param_int("spi_clk_mhz", mhz);
    if (err == ESP_OK) {
        printf("W5500 SPI clock set to %d MHz. Restart to apply\n", mhz);
    }
    return err;
}

static void register_set_spi_clock(void)
{
    set_spi_clock_arg.mhz = arg_int1(NULL, NULL, "<MHz>", "SPI clock speed in MHz (1-40, default: " STRINGIFY(CONFIG_ETH_SPI_CLOCK_MHZ) ")");
    set_spi_clock_arg.end = arg_end(1);

    const esp_console_cmd_t cmd = {
        .command = "set_spi_clock",
        .help = "Set W5500 SPI clock speed in MHz (1-40). Saved to NVS, applied after restart.",
        .hint = NULL,
        .func = &set_spi_clock_cmd,
        .argtable = &set_spi_clock_arg
    };
    ESP_ERROR_CHECK( esp_console_cmd_register(&cmd) );
}

/* --- w5500 status / reset command --- */

static struct {
    struct arg_str *action;
    struct arg_end *end;
} w5500_arg;

static int w5500_cmd(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&w5500_arg);
    if (nerrors != 0) {
        arg_print_errors(stderr, w5500_arg.end, argv[0]);
        return 1;
    }

    const char *action = w5500_arg.action->sval[0];

    if (strcmp(action, "status") == 0) {
        w5500_diag_t d;
        if (w5500_get_diag(&d) != ESP_OK) {
            printf("W5500 not available (MAC not initialised)\n");
            return 1;
        }

        /* Chip version */
        printf("W5500 Diagnostics:\n");
        printf("  Chip version : 0x%02X%s\n", d.version,
               d.version == 0x04 ? " (OK)" : " (!!! unexpected, expect 0x04)");

        /* PHY */
        bool link  = (d.phycfgr & 0x01) != 0;
        bool spd   = (d.phycfgr & 0x02) != 0;
        bool duplex= (d.phycfgr & 0x04) != 0;
        printf("  PHY          : link=%-4s  speed=%-6s  duplex=%s  PHYCFGR=0x%02X\n",
               link ? "UP" : "DOWN",
               spd  ? "100M" : "10M",
               duplex ? "FULL" : "HALF",
               d.phycfgr);

        /* Socket 0 mode and status
         * Expected mode: 0xA4 = MAC_RAW(0x04)|MAC_FILTER(0x80)|BLOCK_MCAST(0x20)
         * Expected status: 0x42 = SOCK_MACRAW (open and active) */
        bool mode_ok   = (d.sock_mr == 0xA4);
        bool status_ok = (d.sock_sr == 0x42);
        const char *mode_str   = mode_ok   ? "MAC_RAW+FILTER" : "UNEXPECTED";
        const char *status_str = status_ok ? "SOCK_MACRAW"
                               : (d.sock_sr == 0x00) ? "SOCK_CLOSED"
                               : "UNKNOWN";
        printf("  Socket 0     : mode=0x%02X(%s)  status=0x%02X(%s)\n",
               d.sock_mr, mode_str, d.sock_sr, status_str);
        if (!mode_ok) {
            printf("  *** Mode is wrong (expected 0xA4) — W5500 may have reset; run 'w5500 reset'\n");
        } else if (!status_ok) {
            /* mode is correct but socket is not open */
            bool link_up = (d.phycfgr & 0x01) != 0;
            if (link_up) {
                printf("  *** Socket CLOSED with link UP — link event may not have fired yet, or run 'w5500 reset'\n");
            } else {
                printf("  Socket CLOSED (link is down — will open when link comes up)\n");
            }
        }

        /* TX buffer */
        bool tx_stuck = (d.tx_fsr == 0);
        printf("  TX buffer    : free=%5u/16384  RD=0x%04X  WR=0x%04X%s\n",
               d.tx_fsr, d.tx_rd, d.tx_wr,
               tx_stuck ? "  *** STUCK — free=0" : "");

        /* RX buffer */
        printf("  RX buffer    : pending=%5u      RD=0x%04X  WR=0x%04X\n",
               d.rx_rsr, d.rx_rd, d.rx_wr);

        /* Interrupts — expected: SIMR=0x01, SOCK0_IMR bits[4:0]=0x04 (RECV only).
         * Bits [7:5] of Sn_IMR are reserved and always read back as 1 (0xE0),
         * so the normal read-back value is 0xE4, not 0x04. */
        printf("  Interrupts   : SIMR=0x%02X  SOCK0_IR=0x%02X  SOCK0_IMR=0x%02X\n",
               d.simr, d.sock_ir, d.sock_imr);
        if (d.simr != 0x01)   printf("  *** SIMR is 0x%02X, expected 0x01 (SOCK0 only)\n", d.simr);
        if ((d.sock_imr & 0x1F) != 0x04) printf("  *** SOCK0_IMR is 0x%02X, effective 0x%02X, expected 0x04 (RECV only)\n", d.sock_imr, d.sock_imr & 0x1F);
        if (d.sock_ir & 0x10) printf("  *** SEND-done IRQ still pending (SIR_SEND) — TX may have timed out\n");
        if (d.sock_ir & 0x04) printf("  *** RECV IRQ pending (SIR_RECV) — unprocessed RX data\n");

        /* SPI error counters */
        w5500_spi_stats_t spi = w5500_spi_get_stats();
        printf("  SPI errors   : rd_fail=%"PRIu32"  rd_timeout=%"PRIu32
               "  wr_fail=%"PRIu32"  wr_timeout=%"PRIu32"\n",
               spi.read_spi_fail, spi.read_timeout,
               spi.write_spi_fail, spi.write_timeout);

    } else if (strcmp(action, "reset") == 0) {
        // Soft-reset: reopen the W5500 socket without disturbing lwIP/NAT state.
        // esp_eth_stop/start would fire link-down/up events and tear down DHCP
        // and NAT, breaking connectivity even when the uplink is healthy.
        esp_err_t err = w5500_reset_socket();
        if (err == ESP_ERR_INVALID_STATE) {
            printf("W5500 MAC not initialised\n");
            return 1;
        } else if (err != ESP_OK) {
            printf("Socket reset failed: %s\n", esp_err_to_name(err));
            return 1;
        }
        printf("W5500 socket reset complete.\n");

    } else {
        printf("Unknown action '%s'. Use: w5500 status | w5500 reset\n", action);
        return 1;
    }

    return 0;
}

static void register_w5500(void)
{
    w5500_arg.action = arg_str1(NULL, NULL, "<status|reset>",
                                "status: read W5500 registers and show diagnostics\n"
                                "        reset:  stop/start the Ethernet driver (closes and reopens SOCK0)");
    w5500_arg.end = arg_end(1);

    const esp_console_cmd_t cmd = {
        .command = "w5500",
        .help = "W5500 diagnostics and recovery. Usage: w5500 <status|reset>",
        .hint = NULL,
        .func = &w5500_cmd,
        .argtable = &w5500_arg
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}
#endif // CONFIG_ETH_DOWNLINK_W5500

/**
 * @brief Format IP address with device name for /32 addresses
 * If the IP has a full /32 mask and a matching DHCP reservation with a name,
 * returns the device name. Otherwise returns the formatted IP/mask.
 */
static char* acl_format_ip_with_name(uint32_t ip, uint32_t mask, char* buf, size_t buf_len)
{
    /* Check for "any" (0.0.0.0/0) */
    if (ip == 0 && mask == 0) {
        snprintf(buf, buf_len, "any");
        return buf;
    }

    /* For /32 addresses, try to look up device name */
    if (mask == 0xFFFFFFFF) {
        const char* name = lookup_device_name_by_ip(ip);
        if (name != NULL) {
            snprintf(buf, buf_len, "%s", name);
            return buf;
        }
    }

    /* Fall back to standard IP formatting */
    return acl_format_ip(ip, mask, buf, buf_len);
}

/**
 * @brief Parse IP address or device name for ACL rules
 * First tries to parse as IP/CIDR, then tries to resolve as device name.
 * Device names are resolved to /32 addresses.
 */
static bool acl_parse_ip_or_name(const char* str, uint32_t* ip, uint32_t* mask)
{
    /* First try standard IP parsing */
    if (acl_parse_ip(str, ip, mask)) {
        return true;
    }

    /* Try to resolve as device name (case-insensitive) */
    if (resolve_device_name_to_ip(str, ip)) {
        *mask = 0xFFFFFFFF;  /* /32 for device names */
        return true;
    }

    return false;
}

/**
 * @brief Print ACL rules with device names for /32 addresses
 */
static void acl_print_with_names(uint8_t acl_no)
{
    if (acl_no >= MAX_ACL_LISTS) {
        printf("Invalid ACL list number\n");
        return;
    }

    printf("\nACL: %s\n", acl_get_name(acl_no));
    printf("==========\n");

    acl_stats_t *stats = acl_get_stats(acl_no);
    printf("Stats: allowed=%lu, denied=%lu, no_match=%lu\n",
           (unsigned long)stats->packets_allowed,
           (unsigned long)stats->packets_denied,
           (unsigned long)stats->packets_nomatch);

    if (acl_is_empty(acl_no)) {
        printf("No rules configured (all packets allowed)\n");
        return;
    }

    printf("\n%3s  %-6s  %-20s  %-20s  %-6s  %-6s  %-8s  %s\n",
           "Idx", "Proto", "Source", "Destination", "SPort", "DPort", "Action", "Hits");
    printf("---  ------  --------------------  --------------------  ------  ------  --------  ----\n");

    /* Snapshot rules under the lock, then print outside to avoid
     * blocking packet processing (printf can block via remote console). */
    acl_entry_t rules_copy[MAX_ACL_ENTRIES];
    acl_lock();
    acl_entry_t *rules = acl_get_rules(acl_no);
    memcpy(rules_copy, rules, sizeof(rules_copy));
    acl_unlock();

    for (int i = 0; i < MAX_ACL_ENTRIES; i++) {
        acl_entry_t *rule = &rules_copy[i];
        if (!rule->valid) {
            continue;
        }

        /* Format protocol */
        const char *proto_str;
        switch (rule->proto) {
            case 0:  proto_str = "IP"; break;
            case 1:  proto_str = "ICMP"; break;
            case 6:  proto_str = "TCP"; break;
            case 17: proto_str = "UDP"; break;
            default: proto_str = "?"; break;
        }

        /* Format IP addresses with device names */
        char src_str[24], dest_str[24];
        acl_format_ip_with_name(rule->src, rule->s_mask, src_str, sizeof(src_str));
        acl_format_ip_with_name(rule->dest, rule->d_mask, dest_str, sizeof(dest_str));

        /* Format ports */
        char s_port_str[8], d_port_str[8];
        if (rule->s_port == 0) {
            strcpy(s_port_str, "*");
        } else {
            snprintf(s_port_str, sizeof(s_port_str), "%d", rule->s_port);
        }
        if (rule->d_port == 0) {
            strcpy(d_port_str, "*");
        } else {
            snprintf(d_port_str, sizeof(d_port_str), "%d", rule->d_port);
        }

        /* Format action */
        const char *action_str;
        uint8_t action = rule->allow & 0x01;
        uint8_t monitor = rule->allow & ACL_MONITOR;
        if (action == ACL_ALLOW) {
            action_str = monitor ? "allow+M" : "allow";
        } else {
            action_str = monitor ? "deny+M" : "deny";
        }

        printf("%3d  %-6s  %-20s  %-20s  %-6s  %-6s  %-8s  %lu\n",
               i, proto_str, src_str, dest_str, s_port_str, d_port_str,
               action_str, (unsigned long)rule->hit_count);
    }
}

/* 'acl' command implementation */
static int acl_cmd(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: acl <list> <action> [params...]\n");
        printf("Lists: from_esp, to_esp, from_eth, to_eth\n");
        printf("\nActions:\n");
        printf("  acl <list> clear              - Clear all rules from list\n");
        printf("  acl <list> clear_stats        - Clear statistics for list\n");
        printf("  acl <list> del <idx>          - Delete rule at index\n");
        printf("  acl <list> <proto> <src> [<s_port>] <dst> [<d_port>] <action>\n");
        printf("\nProtocols: IP, TCP, UDP, ICMP\n");
        printf("Addresses: IP/mask, 'any', or device name from DHCP reservations\n");
        printf("Ports:     Port number or '*' for any (TCP/UDP only)\n");
        printf("Actions:   allow, deny, allow_monitor, deny_monitor\n");
        printf("\nExamples:\n");
        printf("  acl from_esp clear\n");
        printf("  acl from_esp IP any 255.255.255.255 allow\n");
        printf("  acl from_esp UDP any any any 53 allow\n");
        printf("  acl from_esp TCP any 22 192.168.4.0/24 * deny\n");
        printf("  acl from_esp IP any MyPhone deny      # Use device name\n");
        printf("  acl from_esp IP any any deny          # Block all at end\n");
        printf("  acl from_esp del 0                    # Delete first rule\n");
        return 0;
    }

    /* Parse list name */
    int list_no = acl_parse_name(argv[1]);
    if (list_no < 0) {
        printf("Invalid ACL list: %s\n", argv[1]);
        printf("Use: from_esp, to_esp, from_eth, to_eth\n");
        return 1;
    }

    if (argc < 3) {
        printf("Missing action. Use: clear, clear_stats, del, or <proto> <src> <dst> <action>\n");
        return 1;
    }

    /* Handle 'clear' action */
    if (strcmp(argv[2], "clear") == 0) {
        acl_clear(list_no);
        save_acl_rules();
        ESP_LOGW(TAG, "ACL list %s cleared via CLI.", acl_get_name(list_no));
        printf("ACL list %s cleared.\n", acl_get_name(list_no));
        return 0;
    }

    /* Handle 'clear_stats' action */
    if (strcmp(argv[2], "clear_stats") == 0) {
        acl_clear_stats(list_no);
        printf("ACL statistics for %s cleared.\n", acl_get_name(list_no));
        return 0;
    }

    /* Handle 'del' action */
    if (strcmp(argv[2], "del") == 0) {
        if (argc < 4) {
            printf("Usage: acl <list> del <index>\n");
            return 1;
        }
        int idx = atoi(argv[3]);
        if (idx < 0 || idx >= MAX_ACL_ENTRIES) {
            printf("Invalid rule index: %d (0-%d)\n", idx, MAX_ACL_ENTRIES - 1);
            return 1;
        }
        if (acl_delete(list_no, idx)) {
            save_acl_rules();
            ESP_LOGW(TAG, "ACL rule %d deleted from %s via CLI.", idx, acl_get_name(list_no));
            printf("Deleted rule %d from %s\n", idx, acl_get_name(list_no));
        } else {
            printf("No rule at index %d\n", idx);
        }
        return 0;
    }

    /* Parse as add rule: <proto> <src> [<s_port>] <dst> [<d_port>] <action> */
    const char *proto_str = argv[2];
    uint8_t proto;

    if (strcasecmp(proto_str, "IP") == 0) {
        proto = 0;
    } else if (strcasecmp(proto_str, "ICMP") == 0) {
        proto = 1;
    } else if (strcasecmp(proto_str, "TCP") == 0) {
        proto = 6;
    } else if (strcasecmp(proto_str, "UDP") == 0) {
        proto = 17;
    } else {
        printf("Invalid protocol: %s (use IP, ICMP, TCP, UDP)\n", proto_str);
        return 1;
    }

    /* For TCP/UDP, we expect: proto src s_port dst d_port action (7 args total)
       For IP/ICMP, we expect: proto src dst action (5 args total)
       But we also support: proto src dst action (no ports) for TCP/UDP */

    uint32_t src_ip, src_mask, dst_ip, dst_mask;
    uint16_t s_port = 0, d_port = 0;
    uint8_t allow;
    int arg_idx = 3;

    /* Parse source (IP/CIDR, 'any', or device name) */
    if (arg_idx >= argc) {
        printf("Missing source address\n");
        return 1;
    }
    if (!acl_parse_ip_or_name(argv[arg_idx], &src_ip, &src_mask)) {
        printf("Invalid source address or device name: %s\n", argv[arg_idx]);
        return 1;
    }
    arg_idx++;

    /* For TCP/UDP, check if next arg is a port */
    if ((proto == 6 || proto == 17) && arg_idx < argc) {
        if (strcmp(argv[arg_idx], "*") == 0 || strcmp(argv[arg_idx], "any") == 0) {
            s_port = 0;
            arg_idx++;
        } else if (argv[arg_idx][0] >= '0' && argv[arg_idx][0] <= '9') {
            s_port = atoi(argv[arg_idx]);
            arg_idx++;
        }
        /* If not a port-like value, treat as destination */
    }

    /* Parse destination (IP/CIDR, 'any', or device name) */
    if (arg_idx >= argc) {
        printf("Missing destination address\n");
        return 1;
    }
    if (!acl_parse_ip_or_name(argv[arg_idx], &dst_ip, &dst_mask)) {
        printf("Invalid destination address or device name: %s\n", argv[arg_idx]);
        return 1;
    }
    arg_idx++;

    /* For TCP/UDP, check if next arg is a port */
    if ((proto == 6 || proto == 17) && arg_idx < argc) {
        if (strcmp(argv[arg_idx], "*") == 0 || strcmp(argv[arg_idx], "any") == 0) {
            d_port = 0;
            arg_idx++;
        } else if (argv[arg_idx][0] >= '0' && argv[arg_idx][0] <= '9') {
            d_port = atoi(argv[arg_idx]);
            arg_idx++;
        }
        /* If not a port-like value, treat as action */
    }

    /* Parse action */
    if (arg_idx >= argc) {
        printf("Missing action (allow, deny, allow_monitor, deny_monitor)\n");
        return 1;
    }
    const char *action_str = argv[arg_idx];

    if (strcasecmp(action_str, "allow") == 0) {
        allow = ACL_ALLOW;
    } else if (strcasecmp(action_str, "deny") == 0) {
        allow = ACL_DENY;
    } else if (strcasecmp(action_str, "allow_monitor") == 0) {
        allow = ACL_ALLOW | ACL_MONITOR;
    } else if (strcasecmp(action_str, "deny_monitor") == 0) {
        allow = ACL_DENY | ACL_MONITOR;
    } else {
        printf("Invalid action: %s (use allow, deny, allow_monitor, deny_monitor)\n", action_str);
        return 1;
    }

    /* Add the rule */
    if (acl_add(list_no, src_ip, src_mask, dst_ip, dst_mask, proto, s_port, d_port, allow)) {
        save_acl_rules();
        ESP_LOGW(TAG, "ACL rule added to %s via CLI.", acl_get_name(list_no));
        printf("Rule added to %s\n", acl_get_name(list_no));
    } else {
        printf("Failed to add rule (list may be full)\n");
        return 1;
    }

    return 0;
}

static void register_acl(void)
{
    const esp_console_cmd_t cmd = {
        .command = "acl",
        .help = "Manage firewall ACL rules\n"
                "  acl <list> <proto> <src> [<s_port>] <dst> [<d_port>] <action>  - Add rule\n"
                "  acl <list> del <index>       - Delete rule at index\n"
                "  acl <list> clear             - Clear all rules from list\n"
                "  acl <list> clear_stats       - Clear statistics for list\n"
                "  Lists: to_esp, from_esp, from_eth, to_eth\n"
                "  Protocols: IP, TCP, UDP, ICMP\n"
                "  Actions: allow, deny, allow_monitor, deny_monitor",
        .hint = " <list> <proto> <src> [<s_port>] <dst> [<d_port>] <action>",
        .func = &acl_cmd,
    };
    ESP_ERROR_CHECK( esp_console_cmd_register(&cmd) );
}

/* 'remote_console' command implementation */
static int remote_console_cmd(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: remote_console <action> [args]\n");
        printf("  status              - Show remote console status\n");
        printf("  enable              - Enable remote console\n");
        printf("  disable             - Disable remote console\n");
        printf("  port <port>         - Set TCP port (requires restart)\n");
        printf("  bind <both|ap|sta>  - Set interface binding\n");
        printf("  timeout <seconds>   - Set idle timeout (0=none)\n");
        printf("  kick                - Disconnect current session\n");
        return 0;
    }

    const char *action = argv[1];

    if (strcmp(action, "status") == 0) {
        remote_console_config_t config;
        remote_console_status_t status;
        remote_console_get_config(&config);
        remote_console_get_status(&status);

        printf("Remote Console Status:\n");
        printf("======================\n");
        printf("Enabled:        %s\n", config.enabled ? "yes" : "no");
        printf("Port:           %d\n", config.port);

        { char _bs[20]; fmt_web_bind(config.bind, _bs); printf("Interface:      %s\n", _bs); }
        printf("Idle timeout:   %lu sec\n", (unsigned long)config.idle_timeout_sec);

        const char *state_str[] = {"disabled", "listening", "auth wait", "active"};
        printf("State:          %s\n", state_str[status.state]);

        if (status.state == RC_STATE_ACTIVE) {
            printf("Client IP:      %s\n", status.client_ip);
            printf("Session time:   %lu sec\n", (unsigned long)status.session_duration_sec);
            printf("Idle:           %lu sec\n", (unsigned long)status.idle_sec);
        }

        printf("Connections:    %lu total\n", (unsigned long)status.total_connections);
        printf("Auth failures:  %lu\n", (unsigned long)status.failed_auths);

        printf("\nWARNING: Currently uses plain TCP (not encrypted).\n");

    } else if (strcmp(action, "enable") == 0) {
        esp_err_t err = remote_console_enable();
        if (err == ESP_OK) {
            ESP_LOGW(TAG, "Remote console enabled via CLI.");
            printf("Remote console enabled.\n");
        } else {
            printf("Error: %s\n", esp_err_to_name(err));
        }

    } else if (strcmp(action, "disable") == 0) {
        remote_console_disable();
        ESP_LOGW(TAG, "Remote console disabled via CLI.");
        printf("Remote console disabled.\n");

    } else if (strcmp(action, "port") == 0) {
        if (argc < 3) {
            printf("Usage: remote_console port <port>\n");
            return 1;
        }
        int port = atoi(argv[2]);
        if (port < 1 || port > 65535) {
            printf("Invalid port number (1-65535)\n");
            return 1;
        }
        remote_console_set_port((uint16_t)port);
        ESP_LOGW(TAG, "Remote console port changed to %d via CLI.", port);
        printf("Port set to %d. Restart or disable/enable to apply.\n", port);

    } else if (strcmp(action, "bind") == 0) {
        if (argc < 3) {
            printf("Usage: remote_console bind <ap,sta,vpn>\n");
            printf("  Comma-separated list, e.g.: ap,sta or ap,vpn\n");
            return 1;
        }
        uint8_t bind = 0;
        char arg_copy[64];
        strncpy(arg_copy, argv[2], sizeof(arg_copy) - 1);
        arg_copy[sizeof(arg_copy) - 1] = '\0';
        char *token = strtok(arg_copy, ",");
        while (token) {
            if ((strcmp(token, "eth") == 0)||(strcmp(token, "ETH") == 0)||(strcmp(token, "ap") == 0)||(strcmp(token, "AP")) == 0) bind |= RC_BIND_AP;
            else if ((strcmp(token, "sta") == 0)||(strcmp(token, "STA") == 0)) bind |= RC_BIND_STA;
            else if ((strcmp(token, "vpn") == 0)||(strcmp(token, "VPN")== 0)) bind |= RC_BIND_VPN;
            else {
                printf("Unknown interface: %s. Use: ap, sta, vpn\n", token);
                return 1;
            }
            token = strtok(NULL, ",");
        }
        if (bind == 0) {
            printf("Must specify at least one interface\n");
            return 1;
        }
        remote_console_set_bind(bind);
        ESP_LOGW(TAG, "Remote console bind changed to %s via CLI.", argv[2]);
        printf("Bind set. Restart or disable/enable to apply.\n");

    } else if (strcmp(action, "timeout") == 0) {
        if (argc < 3) {
            printf("Usage: remote_console timeout <seconds>\n");
            return 1;
        }
        uint32_t timeout = (uint32_t)atoi(argv[2]);
        remote_console_set_timeout(timeout);
        ESP_LOGI(TAG, "Remote console timeout changed to %lu sec via CLI.", (unsigned long)timeout);
        printf("Timeout set to %lu seconds.\n", (unsigned long)timeout);

    } else if (strcmp(action, "kick") == 0) {
        esp_err_t err = remote_console_kick();
        if (err == ESP_OK) {
            ESP_LOGW(TAG, "Remote console session kicked via CLI.");
            printf("Kick request sent.\n");
        } else {
            printf("No active session to kick.\n");
        }

    } else {
        printf("Unknown action: %s\n", action);
        return 1;
    }

    return 0;
}

static void register_remote_console_cmd(void)
{
    const esp_console_cmd_t cmd = {
        .command = "remote_console",
        .help = "Manage remote console (network CLI access)\n"
                "  remote_console status              - Show status and connection info\n"
                "  remote_console enable               - Enable remote console\n"
                "  remote_console disable              - Disable remote console\n"
                "  remote_console port <port>          - Set TCP port (default: 2323)\n"
                "  remote_console bind <ap,sta,vpn>    - Set interface binding\n"
                "  remote_console timeout <seconds>    - Set idle timeout (0=none)\n"
                "  remote_console kick                 - Disconnect current session",
        .hint = " <action> [<args>]",
        .func = &remote_console_cmd,
    };
    ESP_ERROR_CHECK( esp_console_cmd_register(&cmd) );
}

/* 'syslog' command - configure remote syslog forwarding */
static int syslog_cmd(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: syslog <action> [args]\n");
        printf("  status                    - Show syslog configuration\n");
        printf("  enable <server> [<port>]  - Enable syslog (default port 514)\n");
        printf("  disable                   - Disable syslog forwarding\n");
        return 0;
    }

    const char *action = argv[1];

    if (strcmp(action, "status") == 0) {
        bool enabled;
        char server[SYSLOG_MAX_SERVER_LEN];
        uint16_t port;
        syslog_get_config(&enabled, server, sizeof(server), &port);

        printf("Syslog Status:\n");
        printf("==============\n");
        printf("Enabled:  %s\n", enabled ? "yes" : "no");
        printf("Server:   %s\n", server[0] ? server : "(not set)");
        printf("Port:     %u\n", port);

    } else if (strcmp(action, "enable") == 0) {
        if (argc < 3) {
            printf("Usage: syslog enable <server> [<port>]\n");
            return 1;
        }
        const char *server = argv[2];
        uint16_t port = SYSLOG_DEFAULT_PORT;
        if (argc >= 4) {
            int p = atoi(argv[3]);
            if (p < 1 || p > 65535) {
                printf("Invalid port number (1-65535)\n");
                return 1;
            }
            port = (uint16_t)p;
        }
        esp_err_t err = syslog_enable(server, port);
        if (err == ESP_OK) {
            ESP_LOGW(TAG, "Syslog enabled: %s:%u via CLI.", server, port);
            printf("Syslog enabled: %s:%u\n", server, port);
        } else {
            printf("Error: %s\n", esp_err_to_name(err));
        }

    } else if (strcmp(action, "disable") == 0) {
        syslog_disable();
        ESP_LOGW(TAG, "Syslog disabled via CLI.");
        printf("Syslog disabled.\n");

    } else {
        printf("Unknown action: %s\n", action);
        return 1;
    }

    return 0;
}

static void register_syslog_cmd(void)
{
    const esp_console_cmd_t cmd = {
        .command = "syslog",
        .help = "Manage remote syslog forwarding\n"
                "  syslog status                    - Show syslog configuration\n"
                "  syslog enable <server> [<port>]  - Enable syslog (default port 514)\n"
                "  syslog disable                   - Disable syslog forwarding",
        .hint = " <action> [<args>]",
        .func = &syslog_cmd,
    };
    ESP_ERROR_CHECK( esp_console_cmd_register(&cmd) );
}

/* 'set_tz' command - set timezone */
static int set_tz_cmd(int argc, char **argv)
{
    if (argc < 2) {
        const char *tz = getenv("TZ");
        printf("Timezone: %s\n", tz ? tz : "(not set, using UTC)");
        printf("\nUsage: set_tz <POSIX TZ string>\n");
        printf("Examples:\n");
        printf("  set_tz UTC               - UTC\n");
        printf("  set_tz CET-1CEST,M3.5.0,M10.5.0/3  - Central Europe\n");
        printf("  set_tz EST5EDT,M3.2.0,M11.1.0       - US Eastern\n");
        printf("  set_tz PST8PDT,M3.2.0,M11.1.0       - US Pacific\n");
        printf("  set_tz clear             - Clear timezone (revert to UTC)\n");
        return 0;
    }

    if (strcmp(argv[1], "clear") == 0) {
        unsetenv("TZ");
        tzset();
        set_config_param_str("tz", "");
        printf("Timezone cleared (using UTC).\n");
        return 0;
    }

    setenv("TZ", argv[1], 1);
    tzset();
    set_config_param_str("tz", argv[1]);

    /* Show current time to confirm */
    time_t now = time(NULL);
    struct tm tm_info;
    localtime_r(&now, &tm_info);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S %Z", &tm_info);
    printf("Timezone set to: %s\n", argv[1]);
    printf("Current time:    %s\n", buf);

    return 0;
}

static void register_set_tz(void)
{
    const esp_console_cmd_t cmd = {
        .command = "set_tz",
        .help = "Set timezone (POSIX TZ string)\n"
                "  set_tz                   - Show current timezone\n"
                "  set_tz <TZ string>       - Set timezone\n"
                "  set_tz clear             - Clear timezone (revert to UTC)",
        .hint = " <TZ string>",
        .func = &set_tz_cmd,
    };
    ESP_ERROR_CHECK( esp_console_cmd_register(&cmd) );
}

/* Helper function to convert auth mode to string */
static const char* auth_mode_to_str(wifi_auth_mode_t authmode)
{
    switch (authmode) {
        case WIFI_AUTH_OPEN:            return "Open";
        case WIFI_AUTH_WEP:             return "WEP";
        case WIFI_AUTH_WPA_PSK:         return "WPA";
        case WIFI_AUTH_WPA2_PSK:        return "WPA2";
        case WIFI_AUTH_WPA_WPA2_PSK:    return "WPA/WPA2";
        case WIFI_AUTH_WPA3_PSK:        return "WPA3";
        case WIFI_AUTH_WPA2_WPA3_PSK:   return "WPA2/WPA3";
        case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2-Ent";
        default:                        return "Unknown";
    }
}

/* 'scan' command implementation */
static int scan_cmd(int argc, char **argv)
{
    wifi_scan_config_t scan_config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
    };

    /* Stop connection attempts so they don't interfere with scanning */
    if (!ap_connect) {
        wifi_scan_active = true;
        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(100));  /* Let disconnect complete */
    }

    printf("Scanning for WiFi networks...\n");
    esp_err_t err = esp_wifi_scan_start(&scan_config, true);  /* Blocking scan */

    /* Read results BEFORE clearing flag or reconnecting, so nothing
     * can interfere with the scan result buffer. */
    uint16_t ap_count = 0;
    wifi_ap_record_t *ap_list = NULL;

    if (err == ESP_OK) {
        esp_wifi_scan_get_ap_num(&ap_count);
        if (ap_count > 0) {
            ap_list = malloc(sizeof(wifi_ap_record_t) * ap_count);
            if (ap_list != NULL) {
                esp_wifi_scan_get_ap_records(&ap_count, ap_list);
            } else {
                ap_count = 0;
            }
        }
    }

    /* Now safe to resume connection attempts */
    wifi_scan_active = false;
    if (!ap_connect) {
        esp_wifi_connect();
    }

    if (err != ESP_OK) {
        printf("Scan failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    if (ap_count == 0) {
        printf("No networks found.\n");
        return 0;
    }

    /* Print header */
    printf("\nFound %d networks:\n", ap_count);
    printf("%-32s  %3s  %4s  %-12s\n", "SSID", "Ch", "RSSI", "Security");
    printf("--------------------------------  ---  ----  ------------\n");

    for (int i = 0; i < ap_count; i++) {
        const char *auth = auth_mode_to_str(ap_list[i].authmode);
        printf("%-32s  %3d  %4d  %s\n", ap_list[i].ssid, ap_list[i].primary, ap_list[i].rssi, auth);
    }

    free(ap_list);
    return 0;
}

static void register_scan(void)
{
    const esp_console_cmd_t cmd = {
        .command = "scan",
        .help = "Scan for available WiFi networks",
        .hint = NULL,
        .func = &scan_cmd,
    };
    ESP_ERROR_CHECK( esp_console_cmd_register(&cmd) );
}

/** Arguments used by 'set_vpn' function */
static struct {
    struct arg_str *privkey;
    struct arg_str *pubkey;
    struct arg_str *endpoint;
    struct arg_str *address;
    struct arg_str *psk;
    struct arg_str *mask;
    struct arg_int *port;
    struct arg_int *keepalive;
    struct arg_int *enable;
    struct arg_int *killswitch;
    struct arg_int *route_all;
    struct arg_end *end;
} set_vpn_args;

static int set_vpn_cmd(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **) &set_vpn_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, set_vpn_args.end, argv[0]);
        return 1;
    }

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(PARAM_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        printf("Error opening NVS: %s\n", esp_err_to_name(err));
        return 1;
    }

    if (set_vpn_args.privkey->count > 0) {
        nvs_set_str(nvs, "vpn_privkey", set_vpn_args.privkey->sval[0]);
    }
    if (set_vpn_args.pubkey->count > 0) {
        nvs_set_str(nvs, "vpn_pubkey", set_vpn_args.pubkey->sval[0]);
    }
    if (set_vpn_args.endpoint->count > 0) {
        nvs_set_str(nvs, "vpn_endpoint", set_vpn_args.endpoint->sval[0]);
    }
    if (set_vpn_args.address->count > 0) {
        nvs_set_str(nvs, "vpn_ip", set_vpn_args.address->sval[0]);
    }
    if (set_vpn_args.psk->count > 0) {
        nvs_set_str(nvs, "vpn_psk", set_vpn_args.psk->sval[0]);
    }
    if (set_vpn_args.mask->count > 0) {
        nvs_set_str(nvs, "vpn_mask", set_vpn_args.mask->sval[0]);
    }
    if (set_vpn_args.port->count > 0) {
        nvs_set_i32(nvs, "vpn_port", set_vpn_args.port->ival[0]);
    }
    if (set_vpn_args.keepalive->count > 0) {
        nvs_set_i32(nvs, "vpn_ka", set_vpn_args.keepalive->ival[0]);
    }
    if (set_vpn_args.enable->count > 0) {
        nvs_set_i32(nvs, "vpn_enabled", set_vpn_args.enable->ival[0]);
    }
    if (set_vpn_args.killswitch->count > 0) {
        nvs_set_i32(nvs, "vpn_ks", set_vpn_args.killswitch->ival[0]);
    }
    if (set_vpn_args.route_all->count > 0) {
        nvs_set_i32(nvs, "vpn_rall", set_vpn_args.route_all->ival[0]);
    }

    nvs_commit(nvs);
    nvs_close(nvs);
    printf("VPN settings saved. Restart to apply.\n");
    return 0;
}

static void register_set_vpn(void)
{
    set_vpn_args.privkey   = arg_str0(NULL, NULL, "<private_key>", "WireGuard private key (base64)");
    set_vpn_args.pubkey    = arg_str0(NULL, NULL, "<public_key>", "Peer public key (base64)");
    set_vpn_args.endpoint  = arg_str0(NULL, NULL, "<endpoint>", "Peer endpoint host/IP");
    set_vpn_args.address   = arg_str0(NULL, NULL, "<address>", "Tunnel IP (e.g. 10.0.0.2)");
    set_vpn_args.psk       = arg_str0("k", "psk", "<preshared_key>", "Preshared key (base64)");
    set_vpn_args.mask      = arg_str0("m", "mask", "<netmask>", "Tunnel netmask (default 255.255.255.0)");
    set_vpn_args.port      = arg_int0("p", "port", "<port>", "Peer UDP port (default 51820)");
    set_vpn_args.keepalive = arg_int0("a", "keepalive", "<seconds>", "Persistent keepalive (0=disabled)");
    set_vpn_args.enable    = arg_int0("e", "enable", "<0|1>", "Enable/disable VPN");
    set_vpn_args.killswitch = arg_int0("K", "killswitch", "<0|1>", "Kill switch: block internet when VPN down (default on)");
    set_vpn_args.route_all = arg_int0("R", "route-all", "<0|1>", "Route all traffic through VPN (0=split tunnel)");
    set_vpn_args.end       = arg_end(4);

    const esp_console_cmd_t cmd = {
        .command = "set_vpn",
        .help = "Configure WireGuard VPN (restart to apply)",
        .hint = NULL,
        .func = &set_vpn_cmd,
        .argtable = &set_vpn_args
    };
    ESP_ERROR_CHECK( esp_console_cmd_register(&cmd) );
}
