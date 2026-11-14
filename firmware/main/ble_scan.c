/*
 * BLE survey: finds Matter devices that are advertising for commissioning
 * (service data for UUID 0xFFF6) and counts all other advertisers as a
 * rough measure of how busy the band is.
 */
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"

#include "probe.h"

#define MATTER_UUID16 0xFFF6
#define MAX_SEEN 128

typedef struct {
    uint8_t addr[6];
    int8_t rssi;
    bool matter;
    bool has_ids;
    uint8_t opcode;
    uint16_t discriminator;
    uint16_t vid;
    uint16_t pid;
} seen_t;

static seen_t s_seen[MAX_SEEN];
static int s_seen_count;
static SemaphoreHandle_t s_synced;
static SemaphoreHandle_t s_done;
static bool s_inited;

static seen_t *lookup(const uint8_t *addr)
{
    for (int i = 0; i < s_seen_count; i++) {
        if (memcmp(s_seen[i].addr, addr, 6) == 0) {
            return &s_seen[i];
        }
    }
    if (s_seen_count == MAX_SEEN) {
        return NULL;
    }
    seen_t *s = &s_seen[s_seen_count++];
    memset(s, 0, sizeof(*s));
    memcpy(s->addr, addr, 6);
    s->rssi = -127;
    return s;
}

static int on_gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    if (event->type == BLE_GAP_EVENT_DISC_COMPLETE) {
        xSemaphoreGive(s_done);
        return 0;
    }
    if (event->type != BLE_GAP_EVENT_DISC) {
        return 0;
    }

    seen_t *s = lookup(event->disc.addr.val);
    if (!s) {
        return 0;
    }
    if (event->disc.rssi > s->rssi) {
        s->rssi = event->disc.rssi;
    }

    struct ble_hs_adv_fields fields;
    if (ble_hs_adv_parse_fields(&fields, event->disc.data, event->disc.length_data) != 0) {
        return 0;
    }
    /* Service data: uuid16 (LE), opcode, discriminator+version (LE), VID, PID, flags */
    const uint8_t *d = fields.svc_data_uuid16;
    uint8_t n = fields.svc_data_uuid16_len;
    if (d && n >= 5 && (d[0] | (d[1] << 8)) == MATTER_UUID16) {
        s->matter = true;
        s->opcode = d[2];
        s->discriminator = (d[3] | (d[4] << 8)) & 0x0FFF;
        if (n >= 9) {
            s->has_ids = true;
            s->vid = d[5] | (d[6] << 8);
            s->pid = d[7] | (d[8] << 8);
        }
    }
    return 0;
}

static void on_sync(void)
{
    xSemaphoreGive(s_synced);
}

static void host_task(void *arg)
{
    (void)arg;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

cJSON *ble_scan(const cJSON *args, const char **err)
{
    int duration_ms = probe_arg_int(args, "duration_ms", 5000, 500, 60000);

    if (!s_inited) {
        s_synced = xSemaphoreCreateBinary();
        s_done = xSemaphoreCreateBinary();
        if (nimble_port_init() != ESP_OK) {
            *err = "ble init failed";
            return NULL;
        }
        ble_hs_cfg.sync_cb = on_sync;
        nimble_port_freertos_init(host_task);
        if (xSemaphoreTake(s_synced, pdMS_TO_TICKS(5000)) != pdTRUE) {
            *err = "ble host did not start";
            return NULL;
        }
        s_inited = true;
    }

    uint8_t own_addr_type;
    if (ble_hs_util_ensure_addr(0) != 0 || ble_hs_id_infer_auto(0, &own_addr_type) != 0) {
        *err = "ble address setup failed";
        return NULL;
    }

    s_seen_count = 0;
    xSemaphoreTake(s_done, 0);
    struct ble_gap_disc_params params = {.passive = 1, .filter_duplicates = 0};
    if (ble_gap_disc(own_addr_type, duration_ms, &params, on_gap_event, NULL) != 0) {
        *err = "ble scan failed to start";
        return NULL;
    }
    if (xSemaphoreTake(s_done, pdMS_TO_TICKS(duration_ms + 3000)) != pdTRUE) {
        ble_gap_disc_cancel();
    }

    cJSON *out = cJSON_CreateObject();
    cJSON_AddNumberToObject(out, "duration_ms", duration_ms);
    cJSON_AddNumberToObject(out, "advertisers", s_seen_count);
    cJSON *list = cJSON_AddArrayToObject(out, "matter");
    for (int i = 0; i < s_seen_count; i++) {
        const seen_t *s = &s_seen[i];
        if (!s->matter) {
            continue;
        }
        char addr[18];
        snprintf(addr, sizeof(addr), "%02x:%02x:%02x:%02x:%02x:%02x", s->addr[5], s->addr[4],
                 s->addr[3], s->addr[2], s->addr[1], s->addr[0]);
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "addr", addr);
        cJSON_AddNumberToObject(o, "rssi", s->rssi);
        cJSON_AddBoolToObject(o, "commissionable", s->opcode == 0);
        cJSON_AddNumberToObject(o, "discriminator", s->discriminator);
        if (s->has_ids) {
            cJSON_AddNumberToObject(o, "vendor_id", s->vid);
            cJSON_AddNumberToObject(o, "product_id", s->pid);
        }
        cJSON_AddItemToArray(list, o);
    }
    return out;
}
