/*
 * Thread survey over the 802.15.4 radio, without joining any network:
 *  - thread_discover: MLE discovery, lists every Thread router in earshot
 *    with its network name, extended PAN ID, channel and signal strength.
 *  - thread_energy: peak energy per 802.15.4 channel (11-26), which also
 *    picks up Wi-Fi and other 2.4 GHz interference.
 */
#include "sdkconfig.h"

#include "probe.h"

#if CONFIG_OPENTHREAD_ENABLED

#include <stdio.h>

#include "esp_netif.h"
#include "esp_openthread.h"
#include "esp_openthread_lock.h"
#include "esp_openthread_netif_glue.h"
#include "esp_openthread_types.h"
#include "esp_vfs_eventfd.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hal/ieee802154_ll.h"
#include "openthread/ip6.h"
#include "openthread/link.h"
#include "openthread/thread.h"

static SemaphoreHandle_t s_ready;
static SemaphoreHandle_t s_done;
static cJSON *s_results;
static uint32_t s_cca_duration; /* hardware's clear-channel-assessment time at boot */

static void ot_task(void *arg)
{
    (void)arg;
    esp_openthread_platform_config_t config = {
        .radio_config = {.radio_mode = RADIO_MODE_NATIVE},
        .host_config = {.host_connection_mode = HOST_CONNECTION_MODE_NONE},
        .port_config = {.storage_partition_name = "nvs", .netif_queue_size = 10, .task_queue_size = 10},
    };
    ESP_ERROR_CHECK(esp_openthread_init(&config));

    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_OPENTHREAD();
    esp_netif_t *netif = esp_netif_new(&netif_cfg);
    assert(netif);
    ESP_ERROR_CHECK(esp_netif_attach(netif, esp_openthread_netif_glue_init(&config)));

    s_cca_duration = IEEE802154.ed_duration.duration;
    xSemaphoreGive(s_ready);
    esp_openthread_launch_mainloop();

    esp_openthread_netif_glue_deinit();
    esp_netif_destroy(netif);
    esp_vfs_eventfd_unregister();
    vTaskDelete(NULL);
}

void thread_init(void)
{
    /* eventfds used by: netif, OpenThread task queue, radio driver */
    esp_vfs_eventfd_config_t eventfd_config = {.max_fds = 3};
    ESP_ERROR_CHECK(esp_vfs_eventfd_register(&eventfd_config));
    s_ready = xSemaphoreCreateBinary();
    s_done = xSemaphoreCreateBinary();
    xTaskCreate(ot_task, "ot_main", 10240, NULL, 5, NULL);
    xSemaphoreTake(s_ready, portMAX_DELAY);
}

static void hex(char *out, const uint8_t *in, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        sprintf(out + 2 * i, "%02x", in[i]);
    }
}

/* Both callbacks run on the OpenThread task; a NULL result marks the end. */
static void on_discover(otActiveScanResult *r, void *ctx)
{
    (void)ctx;
    if (!r) {
        xSemaphoreGive(s_done);
        return;
    }
    char ext_addr[17], ext_pan[17], pan[7];
    hex(ext_addr, r->mExtAddress.m8, 8);
    hex(ext_pan, r->mExtendedPanId.m8, 8);
    snprintf(pan, sizeof(pan), "0x%04x", r->mPanId);

    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "network_name", r->mNetworkName.m8);
    cJSON_AddStringToObject(o, "ext_pan_id", ext_pan);
    cJSON_AddStringToObject(o, "pan_id", pan);
    cJSON_AddStringToObject(o, "ext_addr", ext_addr);
    cJSON_AddNumberToObject(o, "channel", r->mChannel);
    cJSON_AddNumberToObject(o, "rssi", r->mRssi);
    cJSON_AddNumberToObject(o, "lqi", r->mLqi);
    cJSON_AddNumberToObject(o, "thread_version", r->mVersion);
    cJSON_AddItemToArray(s_results, o);
}

static void on_energy(otEnergyScanResult *r, void *ctx)
{
    (void)ctx;
    if (!r) {
        xSemaphoreGive(s_done);
        return;
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "channel", r->mChannel);
    cJSON_AddNumberToObject(o, "max_rssi", r->mMaxRssi);
    cJSON_AddItemToArray(s_results, o);
}

/* Starts a scan under the OpenThread lock and waits for its end marker. */
static cJSON *run_scan(bool energy, uint32_t channels, uint16_t duration_ms, TickType_t timeout, const char **err)
{
    s_results = cJSON_CreateArray();
    xSemaphoreTake(s_done, 0);

    esp_openthread_lock_acquire(portMAX_DELAY);
    otInstance *ot = esp_openthread_get_instance();
    otError rc = OT_ERROR_NONE;
    if (!otIp6IsEnabled(ot)) {
        rc = otIp6SetEnabled(ot, true);
    }
    if (rc == OT_ERROR_NONE) {
        rc = energy ? otLinkEnergyScan(ot, channels, duration_ms, on_energy, NULL)
                    : otThreadDiscover(ot, channels, OT_PANID_BROADCAST, false, false, on_discover, NULL);
    }
    esp_openthread_lock_release();

    if (rc != OT_ERROR_NONE) {
        *err = rc == OT_ERROR_BUSY ? "802.15.4 radio busy" : "thread scan failed to start";
    } else if (xSemaphoreTake(s_done, timeout) != pdTRUE) {
        *err = "thread scan timed out";
    } else {
        /* Take the 802.15.4 interface down again so its receiver does not
         * hold the shared 2.4 GHz radio while Wi-Fi or BLE scan. */
        esp_openthread_lock_acquire(portMAX_DELAY);
        otIp6SetEnabled(ot, false);
        if (energy) {
            /* The ESP-IDF 5.4 driver leaves the energy-scan duration in the
             * register that also times the clear-channel check before every
             * transmit, which makes later transmits take that long and
             * discovery time out. Put the original value back. */
            ieee802154_ll_set_ed_duration(s_cca_duration);
        }
        esp_openthread_lock_release();
        return s_results;
    }
    /* On timeout the scan may still be running, so keep the callbacks'
     * target alive rather than freeing it under them. */
    if (rc != OT_ERROR_NONE) {
        cJSON_Delete(s_results);
    }
    return NULL;
}

cJSON *thread_discover(const cJSON *args, const char **err)
{
    /* "channel": 11-26 limits the scan to one channel (about 0.3 s); 0 scans all. */
    int channel = probe_arg_int(args, "channel", 0, 0, 26);
    if (channel != 0 && channel < 11) {
        *err = "channel must be 11-26";
        return NULL;
    }
    return run_scan(false, channel ? 1u << channel : 0, 0, pdMS_TO_TICKS(30000), err);
}

cJSON *thread_energy(const cJSON *args, const char **err)
{
    int per_channel_ms = probe_arg_int(args, "duration_ms", 200, 20, 2000);
    return run_scan(true, 0, per_channel_ms, pdMS_TO_TICKS(16 * per_channel_ms + 5000), err);
}

#else /* no 802.15.4 radio on this chip */

void thread_init(void)
{
}

cJSON *thread_discover(const cJSON *args, const char **err)
{
    (void)args;
    *err = "this chip has no 802.15.4 radio (use an ESP32-C6)";
    return NULL;
}

cJSON *thread_energy(const cJSON *args, const char **err)
{
    return thread_discover(args, err);
}

#endif
