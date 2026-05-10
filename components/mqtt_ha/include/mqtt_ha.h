/* MQTT Home Assistant auto-discovery integration.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "esp_err.h"

#ifdef CONFIG_MQTT_HOMEASSISTANT

#ifdef __cplusplus
extern "C" {
#endif

void mqtt_ha_init(void);
esp_err_t mqtt_ha_start(void);
esp_err_t mqtt_ha_stop(void);
void mqtt_ha_rediscover(void);
const char *mqtt_ha_get_status(void);

#ifdef __cplusplus
}
#endif

#endif /* CONFIG_MQTT_HOMEASSISTANT */
