/* 2.4 GHz Wi-Fi survey: which access points share spectrum with Thread. */
#include <stdio.h>
#include <stdlib.h>

#include "esp_netif.h"
#include "esp_wifi.h"

#include "probe.h"

#define MAX_APS 64

static bool s_inited;

cJSON *wifi_scan(const cJSON *args, const char **err)
{
    (void)args;
    if (!s_inited) {
        esp_netif_create_default_wifi_sta();
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        if (esp_wifi_init(&cfg) != ESP_OK || esp_wifi_set_storage(WIFI_STORAGE_RAM) != ESP_OK ||
            esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK) {
            *err = "wifi init failed";
            return NULL;
        }
        s_inited = true;
    }

    /* The radio is only held by Wi-Fi for the duration of the scan, so it
     * does not compete with 802.15.4 or BLE the rest of the time. */
    if (esp_wifi_start() != ESP_OK) {
        *err = "wifi start failed";
        return NULL;
    }
    wifi_scan_config_t scan = {.show_hidden = true};
    esp_err_t rc = esp_wifi_scan_start(&scan, true);

    uint16_t count = MAX_APS;
    wifi_ap_record_t *aps = calloc(MAX_APS, sizeof(*aps));
    if (rc == ESP_OK && aps) {
        rc = esp_wifi_scan_get_ap_records(&count, aps);
    }
    esp_wifi_stop();
    if (rc != ESP_OK || !aps) {
        free(aps);
        *err = "wifi scan failed";
        return NULL;
    }

    cJSON *list = cJSON_CreateArray();
    for (int i = 0; i < count; i++) {
        const wifi_ap_record_t *ap = &aps[i];
        char bssid[18];
        snprintf(bssid, sizeof(bssid), "%02x:%02x:%02x:%02x:%02x:%02x", ap->bssid[0], ap->bssid[1],
                 ap->bssid[2], ap->bssid[3], ap->bssid[4], ap->bssid[5]);
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "ssid", (const char *)ap->ssid);
        cJSON_AddStringToObject(o, "bssid", bssid);
        cJSON_AddNumberToObject(o, "channel", ap->primary);
        cJSON_AddStringToObject(o, "secondary",
                                ap->second == WIFI_SECOND_CHAN_ABOVE   ? "above"
                                : ap->second == WIFI_SECOND_CHAN_BELOW ? "below"
                                                                       : "none");
        cJSON_AddNumberToObject(o, "rssi", ap->rssi);
        cJSON_AddItemToArray(list, o);
    }
    free(aps);
    return list;
}
