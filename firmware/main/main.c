/*
 * matter-probe: a walk-around radio probe for debugging Matter/Thread setups.
 *
 * Protocol: one JSON object per line over the USB serial port.
 *   request:  {"id": 1, "cmd": "wifi_scan", "args": {...}}
 *   response: {"id": 1, "ok": true, "data": ...}
 *             {"id": 1, "ok": false, "error": "..."}
 * Lines that do not start with '{' are log output and can be ignored.
 */
#include <stdio.h>
#include <string.h>

#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_chip_info.h"
#include "esp_event.h"
#include "esp_idf_version.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include "probe.h"

#define LINE_MAX_LEN 512

static SemaphoreHandle_t s_radio_lock;

void probe_radio_lock(void)
{
    xSemaphoreTake(s_radio_lock, portMAX_DELAY);
}

void probe_radio_unlock(void)
{
    xSemaphoreGive(s_radio_lock);
}

int probe_arg_int(const cJSON *args, const char *name, int def, int min, int max)
{
    const cJSON *v = cJSON_GetObjectItem(args, name);
    if (!cJSON_IsNumber(v)) {
        return def;
    }
    int n = v->valueint;
    return n < min ? min : (n > max ? max : n);
}

static cJSON *cmd_info(const cJSON *args, const char **err)
{
    (void)args;
    (void)err;
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_BASE);
    char mac_str[18];
    snprintf(mac_str, sizeof(mac_str), "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "firmware", "matter-probe");
    cJSON_AddStringToObject(o, "version", PROBE_FW_VERSION);
    cJSON_AddStringToObject(o, "chip", CONFIG_IDF_TARGET);
    cJSON_AddNumberToObject(o, "chip_revision", chip.revision);
    cJSON_AddStringToObject(o, "idf", esp_get_idf_version());
    cJSON_AddStringToObject(o, "mac", mac_str);
    cJSON_AddNumberToObject(o, "free_heap", esp_get_free_heap_size());
    cJSON *caps = cJSON_AddArrayToObject(o, "capabilities");
    cJSON_AddItemToArray(caps, cJSON_CreateString("wifi"));
    cJSON_AddItemToArray(caps, cJSON_CreateString("ble"));
#if CONFIG_OPENTHREAD_ENABLED
    cJSON_AddItemToArray(caps, cJSON_CreateString("thread"));
#endif
    return o;
}

typedef cJSON *(*cmd_fn)(const cJSON *args, const char **err);

static const struct {
    const char *name;
    cmd_fn fn;
} s_commands[] = {
    {"info", cmd_info},
    {"wifi_scan", wifi_scan},
    {"ble_scan", ble_scan},
    {"thread_discover", thread_discover},
    {"thread_energy", thread_energy},
};

static void send_json(cJSON *msg)
{
    char *text = cJSON_PrintUnformatted(msg);
    if (text) {
        fputs(text, stdout);
        fputc('\n', stdout);
        fflush(stdout);
        cJSON_free(text);
    }
    cJSON_Delete(msg);
}

static void handle_line(const char *line)
{
    cJSON *resp = cJSON_CreateObject();
    cJSON *req = cJSON_Parse(line);
    const char *err = NULL;
    cJSON *data = NULL;

    const cJSON *id = cJSON_GetObjectItem(req, "id");
    if (id) {
        cJSON_AddItemToObject(resp, "id", cJSON_Duplicate(id, true));
    }

    const cJSON *cmd = cJSON_GetObjectItem(req, "cmd");
    if (!req) {
        err = "invalid JSON";
    } else if (!cJSON_IsString(cmd)) {
        err = "missing \"cmd\"";
    } else {
        err = "unknown command";
        for (size_t i = 0; i < sizeof(s_commands) / sizeof(s_commands[0]); i++) {
            if (strcmp(cmd->valuestring, s_commands[i].name) == 0) {
                err = NULL;
                bool scan = s_commands[i].fn != cmd_info;
                if (scan) {
                    probe_radio_lock();
                }
                data = s_commands[i].fn(cJSON_GetObjectItem(req, "args"), &err);
                if (scan) {
                    probe_radio_unlock();
                }
                if (!data && !err) {
                    err = "command failed";
                }
                break;
            }
        }
    }

    cJSON_AddBoolToObject(resp, "ok", err == NULL);
    if (err) {
        cJSON_AddStringToObject(resp, "error", err);
        cJSON_Delete(data);
    } else {
        cJSON_AddItemToObject(resp, "data", data);
    }
    cJSON_Delete(req);
    send_json(resp);
}

void app_main(void)
{
    esp_err_t nvs = nvs_flash_init();
    if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs);
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_netif_init());

    usb_serial_jtag_driver_config_t usb_cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    usb_cfg.rx_buffer_size = 1024;
    usb_cfg.tx_buffer_size = 4096;
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&usb_cfg));
    usb_serial_jtag_vfs_use_driver();

    s_radio_lock = xSemaphoreCreateMutex();
    thread_init();
    display_start();

    static char line[LINE_MAX_LEN];
    size_t len = 0;
    bool overflow = false;
    for (;;) {
        char c;
        if (usb_serial_jtag_read_bytes(&c, 1, portMAX_DELAY) != 1) {
            continue;
        }
        if (c == '\n' || c == '\r') {
            if (overflow) {
                handle_line("");
            } else if (len > 0) {
                line[len] = '\0';
                handle_line(line);
            }
            len = 0;
            overflow = false;
        } else if (len < LINE_MAX_LEN - 1) {
            line[len++] = c;
        } else {
            overflow = true;
        }
    }
}
