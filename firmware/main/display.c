/*
 * On-device live meter for the Waveshare ESP32-C6-Touch-AMOLED-2.16
 * (480x480 CO5300 AMOLED over QSPI, powered through an AXP2101 PMIC).
 *
 * When that board is detected, a task keeps scanning for Thread routers and
 * shows each one with a signal bar, so the probe works with no computer
 * attached. Host commands still work; they share the radio through
 * probe_radio_lock().
 */
#include "sdkconfig.h"

#include "probe.h"

#if CONFIG_OPENTHREAD_ENABLED && CONFIG_IDF_TARGET_ESP32C6

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_sh8601.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "font5x7.h"

#define TAG "display"

/* Board wiring, from Waveshare's ESP-IDF examples for this board. */
#define PIN_I2C_SCL 7
#define PIN_I2C_SDA 8
#define PIN_LCD_CLK 0
#define PIN_LCD_D0 1
#define PIN_LCD_D1 2
#define PIN_LCD_D2 3
#define PIN_LCD_D3 4
#define PIN_LCD_CS 15
#define PMIC_ADDR 0x34
#define PMIC_REG_LDO_ONOFF 0x90
#define PMIC_REG_ALDO3_VOLT 0x94 /* 0.5 V + 0.1 V per step */
#define PMIC_ALDO3_BIT 0x04

#define LCD_W 480
#define LCD_H 480
#define ROW_H 28   /* even: the panel only accepts even-aligned windows */
#define SCALE 3    /* font pixels are drawn 3x3 */
#define CELL_W 18
#define MARGIN_X 24
#define FIRST_ROW_Y 26
#define MAX_ROWS 15

#define RGB(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))
#define WHITE RGB(255, 255, 255)
#define GREY RGB(110, 110, 110)
#define DARK RGB(40, 40, 40)
#define CYAN RGB(80, 200, 255)
#define GREEN RGB(60, 220, 90)
#define YELLOW RGB(250, 200, 40)
#define RED RGB(255, 70, 60)

static esp_lcd_panel_handle_t s_panel;
static SemaphoreHandle_t s_flushed;
static uint16_t *s_strip; /* one row of the screen, LCD_W x ROW_H, big-endian RGB565 */

static bool on_flush_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *data, void *ctx)
{
    (void)io;
    (void)data;
    (void)ctx;
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_flushed, &woken);
    return woken == pdTRUE;
}

/* ---- drawing into the strip ---- */

static void strip_clear(void)
{
    memset(s_strip, 0, LCD_W * ROW_H * sizeof(uint16_t));
}

static void strip_rect(int x, int y, int w, int h, uint16_t colour)
{
    uint16_t be = (uint16_t)((colour << 8) | (colour >> 8));
    for (int yy = y; yy < y + h && yy < ROW_H; yy++) {
        for (int xx = x; xx < x + w && xx < LCD_W; xx++) {
            s_strip[yy * LCD_W + xx] = be;
        }
    }
}

static const glyph_t *glyph_for(char c)
{
    c = (char)toupper((unsigned char)c);
    for (size_t i = 0; i < sizeof(FONT) / sizeof(FONT[0]); i++) {
        if (FONT[i].ch == c) {
            return &FONT[i];
        }
    }
    return c == ' ' ? NULL : &FONT[sizeof(FONT) / sizeof(FONT[0]) - 1];
}

static void strip_text(int x, const char *text, uint16_t colour)
{
    for (; *text; text++, x += CELL_W) {
        const glyph_t *g = glyph_for(*text);
        for (int row = 0; g && row < 7; row++) {
            for (int col = 0; col < 5; col++) {
                if (g->rows[row][col] == '#') {
                    strip_rect(x + col * SCALE, 2 + row * SCALE, SCALE, SCALE, colour);
                }
            }
        }
    }
}

static void strip_flush(int row)
{
    int y = FIRST_ROW_Y + row * ROW_H;
    if (esp_lcd_panel_draw_bitmap(s_panel, 0, y, LCD_W, y + ROW_H, s_strip) == ESP_OK) {
        xSemaphoreTake(s_flushed, pdMS_TO_TICKS(500));
    }
}

/* ---- bring-up ---- */

static esp_err_t pmic_write(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t value)
{
    uint8_t buf[2] = {reg, value};
    return i2c_master_transmit(dev, buf, sizeof(buf), 200);
}

/* The panel's supply (ALDO3) doubles as its reset: switch it off and on. */
static esp_err_t panel_power_cycle(i2c_master_bus_handle_t bus)
{
    i2c_device_config_t cfg = {.dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = PMIC_ADDR, .scl_speed_hz = 100000};
    i2c_master_dev_handle_t pmic;
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &cfg, &pmic), TAG, "pmic");

    uint8_t reg = PMIC_REG_LDO_ONOFF, onoff = 0;
    ESP_RETURN_ON_ERROR(i2c_master_transmit_receive(pmic, &reg, 1, &onoff, 1, 200), TAG, "pmic read");
    ESP_RETURN_ON_ERROR(pmic_write(pmic, PMIC_REG_ALDO3_VOLT, (3300 - 500) / 100), TAG, "aldo3 volt");
    ESP_RETURN_ON_ERROR(pmic_write(pmic, PMIC_REG_LDO_ONOFF, onoff & ~PMIC_ALDO3_BIT), TAG, "aldo3 off");
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_RETURN_ON_ERROR(pmic_write(pmic, PMIC_REG_LDO_ONOFF, onoff | PMIC_ALDO3_BIT), TAG, "aldo3 on");
    vTaskDelay(pdMS_TO_TICKS(100));
    return ESP_OK;
}

static const sh8601_lcd_init_cmd_t s_init_cmds[] = {
    {0x11, (uint8_t[]){0x00}, 0, 600},
    {0xFE, (uint8_t[]){0x20}, 1, 0},
    {0x19, (uint8_t[]){0x10}, 1, 0},
    {0x1C, (uint8_t[]){0xA0}, 1, 0},
    {0xFE, (uint8_t[]){0x00}, 1, 0},
    {0xC4, (uint8_t[]){0x80}, 1, 0},
    {0x3A, (uint8_t[]){0x55}, 1, 0},
    {0x35, (uint8_t[]){0x00}, 1, 0},
    {0x36, (uint8_t[]){0x30}, 1, 0},
    {0x53, (uint8_t[]){0x20}, 1, 0},
    {0x51, (uint8_t[]){0xFF}, 1, 0},
    {0x63, (uint8_t[]){0xFF}, 1, 0},
    {0x2A, (uint8_t[]){0x00, 0x00, 0x01, 0xDF}, 4, 0},
    {0x2B, (uint8_t[]){0x00, 0x00, 0x01, 0xDF}, 4, 0},
    {0x29, (uint8_t[]){0x00}, 0, 100},
};

static esp_err_t panel_init(void)
{
    i2c_master_bus_config_t i2c_cfg = {
        .i2c_port = I2C_NUM_0,
        .scl_io_num = PIN_I2C_SCL,
        .sda_io_num = PIN_I2C_SDA,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus;
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&i2c_cfg, &bus), TAG, "i2c");
    if (i2c_master_probe(bus, PMIC_ADDR, 100) != ESP_OK) {
        i2c_del_master_bus(bus); /* some other ESP32-C6 board: no screen */
        return ESP_ERR_NOT_FOUND;
    }
    ESP_RETURN_ON_ERROR(panel_power_cycle(bus), TAG, "panel power");

    spi_bus_config_t spi_cfg = {
        .sclk_io_num = PIN_LCD_CLK,
        .data0_io_num = PIN_LCD_D0,
        .data1_io_num = PIN_LCD_D1,
        .data2_io_num = PIN_LCD_D2,
        .data3_io_num = PIN_LCD_D3,
        .max_transfer_sz = LCD_W * ROW_H * sizeof(uint16_t),
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(SPI2_HOST, &spi_cfg, SPI_DMA_CH_AUTO), TAG, "spi");

    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = PIN_LCD_CS,
        .dc_gpio_num = -1,
        .spi_mode = 0,
        .pclk_hz = 40 * 1000 * 1000,
        .trans_queue_depth = 1,
        .on_color_trans_done = on_flush_done,
        .lcd_cmd_bits = 32,
        .lcd_param_bits = 8,
        .flags.quad_mode = true,
    };
    esp_lcd_panel_io_handle_t io;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_cfg, &io), TAG, "panel io");

    sh8601_vendor_config_t vendor = {
        .init_cmds = s_init_cmds,
        .init_cmds_size = sizeof(s_init_cmds) / sizeof(s_init_cmds[0]),
        .flags.use_qspi_interface = 1,
    };
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = -1,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_sh8601(io, &panel_cfg, &s_panel), TAG, "panel");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "panel init");
    return ESP_OK;
}

/* ---- the live meter ---- */

#define MAX_ROUTERS 24
#define FORGET_AFTER 12 /* scans without an answer before a router is dropped */

typedef struct {
    char addr[17];
    char net[17];
    int channel;
    int rssi;
    int missed;
} router_t;

static router_t s_routers[MAX_ROUTERS];
static int s_router_count;

static void note_router(const cJSON *item)
{
    const char *addr = cJSON_GetStringValue(cJSON_GetObjectItem(item, "ext_addr"));
    const char *net = cJSON_GetStringValue(cJSON_GetObjectItem(item, "network_name"));
    if (!addr || !net) {
        return;
    }
    router_t *r = NULL;
    for (int i = 0; i < s_router_count && !r; i++) {
        if (strcmp(s_routers[i].addr, addr) == 0) {
            r = &s_routers[i];
        }
    }
    if (!r) {
        if (s_router_count == MAX_ROUTERS) {
            return;
        }
        r = &s_routers[s_router_count++];
        strlcpy(r->addr, addr, sizeof(r->addr));
    }
    strlcpy(r->net, net, sizeof(r->net));
    r->channel = cJSON_GetObjectItem(item, "channel")->valueint;
    r->rssi = cJSON_GetObjectItem(item, "rssi")->valueint;
    r->missed = -1; /* becomes 0 in age_routers() */
}

static int by_network_then_signal(const void *a, const void *b)
{
    const router_t *x = a, *y = b;
    int net = strcmp(x->net, y->net);
    return net ? net : y->rssi - x->rssi;
}

static void age_routers(void)
{
    int kept = 0;
    for (int i = 0; i < s_router_count; i++) {
        if (++s_routers[i].missed <= FORGET_AFTER) {
            s_routers[kept++] = s_routers[i];
        }
    }
    s_router_count = kept;
    qsort(s_routers, s_router_count, sizeof(router_t), by_network_then_signal);
}

static void draw(const char *status)
{
    int row = 0;
    strip_clear();
    strip_text(MARGIN_X, "THREAD ROUTERS", WHITE);
    strip_text(LCD_W - MARGIN_X - CELL_W * (int)strlen(status), status, GREY);
    strip_flush(row++);

    const char *net = "";
    for (int i = 0; i < s_router_count && row < MAX_ROWS; i++) {
        const router_t *r = &s_routers[i];
        char text[32];
        if (strcmp(net, r->net) != 0 && row < MAX_ROWS - 1) {
            net = r->net;
            snprintf(text, sizeof(text), "%s CH%d", r->net, r->channel);
            strip_clear();
            strip_text(MARGIN_X, text, CYAN);
            strip_flush(row++);
        }
        bool stale = r->missed > 2;
        uint16_t colour = stale ? GREY : r->rssi >= -70 ? GREEN : r->rssi >= -80 ? YELLOW : RED;
        int bar_x = MARGIN_X + 5 * CELL_W, bar_w = 250;
        int level = (r->rssi + 95) * bar_w / 60; /* -95 dBm empty, -35 dBm full */
        level = level < 0 ? 0 : level > bar_w ? bar_w : level;

        strip_clear();
        strip_text(MARGIN_X, r->addr + 12, stale ? GREY : WHITE);
        strip_rect(bar_x, 4, bar_w, 16, DARK);
        strip_rect(bar_x, 4, level, 16, colour);
        snprintf(text, sizeof(text), "%4d", r->rssi);
        strip_text(LCD_W - MARGIN_X - 4 * CELL_W, text, stale ? GREY : WHITE);
        strip_flush(row++);
    }
    strip_clear();
    if (s_router_count == 0) {
        strip_text(MARGIN_X, "NO THREAD IN RANGE", RED);
        strip_flush(row++);
        strip_clear();
    }
    while (row < MAX_ROWS) {
        strip_flush(row++);
    }
}

static void scan_into_table(int channel)
{
    const char *err = NULL;
    probe_radio_lock();
    cJSON *found = thread_discover_channel(channel, &err);
    probe_radio_unlock();
    const cJSON *item;
    cJSON_ArrayForEach(item, found) {
        note_router(item);
    }
    cJSON_Delete(found);
}

static void display_task(void *arg)
{
    (void)arg;
    draw("SCAN");
    for (unsigned round = 0;; round++) {
        /* A full sweep now and then finds new networks; in between, only the
         * channels already known to carry Thread are scanned, which is fast. */
        uint32_t channels = 0;
        for (int i = 0; i < s_router_count; i++) {
            channels |= 1u << s_routers[i].channel;
        }
        if (round % 20 == 0 || channels == 0) {
            scan_into_table(0);
        } else {
            for (int ch = 11; ch <= 26; ch++) {
                if (channels & (1u << ch)) {
                    scan_into_table(ch);
                }
            }
        }
        age_routers();
        draw(round % 2 ? "LIVE" : "LIVE.");
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void display_start(void)
{
    s_flushed = xSemaphoreCreateBinary();
    s_strip = heap_caps_malloc(LCD_W * ROW_H * sizeof(uint16_t), MALLOC_CAP_DMA);
    esp_err_t rc = s_strip ? panel_init() : ESP_ERR_NO_MEM;
    if (rc != ESP_OK) {
        if (rc != ESP_ERR_NOT_FOUND) {
            ESP_LOGW(TAG, "screen not started: %s", esp_err_to_name(rc));
        }
        return;
    }
    xTaskCreate(display_task, "display", 6144, NULL, 4, NULL);
}

#else

void display_start(void)
{
}

#endif
