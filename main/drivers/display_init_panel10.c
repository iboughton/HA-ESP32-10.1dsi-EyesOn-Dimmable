/* SPDX-License-Identifier: LicenseRef-FNCL-1.1
 * Copyright (c) 2026 Cpt_Kirk
 */
#include "drivers/display_init.h"

#include <stdbool.h>

#include "bsp/display.h"
#include "bsp/esp32_p4_nano.h"
#include "esp_err.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

#include "app_config.h"
#include "util/log_tags.h"

#define DISPLAY_FULL_BUFFER_PIXELS ((APP_SCREEN_WIDTH * APP_SCREEN_HEIGHT))
/* DOUBLE_PARTIAL allocates the partial draw buffer from internal RAM (use_psram is
 * ignored in that mode). 800 x 64 x RGB565 = 100 KiB fits; 1/5 height (400 KiB) did not
 * and silently dropped the adapter into its slower fallback path. */
#define DISPLAY_DRAW_BUF_LINES     (BSP_LCD_V_RES / 20U)
#define DISPLAY_ADAPTER_LOG_TAG    "esp_lvgl:adapter"

static bool s_display_ready = false;
static lv_display_t *s_lv_display = NULL;
static int s_display_brightness = -1;

/* Lock-hang diagnostics: last LVGL-side stage plus the app task holding display_lock(). */
static const char *volatile s_diag_stage = "init";
static volatile TaskHandle_t s_lock_owner = NULL;
static volatile int64_t s_lock_owner_since_ms = 0;
static volatile uint32_t s_lock_depth = 0;

void display_diag_mark(const char *stage)
{
    s_diag_stage = stage;
}

static void display_diag_event_cb(lv_event_t *event)
{
    switch (lv_event_get_code(event)) {
    case LV_EVENT_REFR_START: s_diag_stage = "lv:refr_start"; break;
    case LV_EVENT_RENDER_START: s_diag_stage = "lv:render"; break;
    case LV_EVENT_RENDER_READY: s_diag_stage = "lv:render_ready"; break;
    case LV_EVENT_FLUSH_START: s_diag_stage = "lv:flush"; break;
    case LV_EVENT_FLUSH_FINISH: s_diag_stage = "lv:flush_done"; break;
    case LV_EVENT_FLUSH_WAIT_START: s_diag_stage = "lv:flush_wait"; break;
    case LV_EVENT_FLUSH_WAIT_FINISH: s_diag_stage = "lv:flush_wait_done"; break;
    case LV_EVENT_REFR_READY: s_diag_stage = "lv:timers"; break;
    default: break;
    }
}

static void display_diag_task(void *arg)
{
    (void)arg;
    TaskHandle_t lvgl_task = NULL;
    int64_t stuck_since_ms = 0;
    int64_t last_report_ms = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (lvgl_task == NULL) {
            lvgl_task = xTaskGetHandle("lvgl");
        }
        int64_t now_ms = esp_timer_get_time() / 1000;
        if (esp_lv_adapter_lock(0) == ESP_OK) {
            esp_lv_adapter_unlock();
            if (stuck_since_ms != 0) {
                ESP_LOGW(TAG_DISPLAY, "DIAG display lock free again after %lld ms",
                    (long long)(now_ms - stuck_since_ms));
            }
            stuck_since_ms = 0;
            continue;
        }
        if (stuck_since_ms == 0) {
            stuck_since_ms = now_ms;
            continue;
        }
        if ((now_ms - stuck_since_ms) < 5000 || (now_ms - last_report_ms) < 10000) {
            continue;
        }
        last_report_ms = now_ms;
        static const char *const state_names[] = {"running", "ready", "blocked", "suspended", "deleted", "invalid"};
        eTaskState st = (lvgl_task != NULL) ? eTaskGetState(lvgl_task) : eInvalid;
        TaskHandle_t owner = s_lock_owner;
        ESP_LOGE(TAG_DISPLAY,
            "DIAG display lock stuck %lld ms: lvgl task=%s stage=%s app_owner=%s (held %lld ms)",
            (long long)(now_ms - stuck_since_ms), state_names[(st <= eInvalid) ? st : eInvalid],
            s_diag_stage, owner != NULL ? pcTaskGetName(owner) : "none",
            owner != NULL ? (long long)(now_ms - s_lock_owner_since_ms) : 0LL);
    }
}

static esp_lv_adapter_config_t display_adapter_cfg(void)
{
    esp_lv_adapter_config_t cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    cfg.task_priority = 20;
    cfg.task_stack_size = APP_LVGL_TASK_STACK;
    cfg.task_core_id = 1;
    cfg.task_max_delay_ms = 100;
    return cfg;
}

static int display_clamp_brightness(int percent)
{
    if (percent < 0) {
        return 0;
    }
    if (percent > 100) {
        return 100;
    }
    return percent;
}

esp_err_t display_set_brightness_percent(int percent)
{
    const int next = display_clamp_brightness(percent);
    if (s_display_brightness == next) {
        return ESP_OK;
    }

    esp_err_t err = bsp_display_brightness_set(next);
    if (err == ESP_OK) {
        s_display_brightness = next;
        ESP_LOGD(TAG_DISPLAY, "Backlight set to %d%%", next);
    } else {
        ESP_LOGW(TAG_DISPLAY, "Could not set backlight to %d%%: %s", next, esp_err_to_name(err));
    }
    return err;
}

esp_err_t display_init(void)
{
    if (s_display_ready) {
        return ESP_OK;
    }

    esp_lv_adapter_config_t adapter_cfg = display_adapter_cfg();
    esp_err_t err = esp_lv_adapter_init(&adapter_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG_DISPLAY, "esp_lv_adapter_init failed: %s", esp_err_to_name(err));
        return err;
    }

    bsp_lcd_handles_t lcd = {0};
    err = bsp_display_new_with_handles(NULL, &lcd);
    if (err != ESP_OK || lcd.panel == NULL) {
        ESP_LOGE(TAG_DISPLAY, "bsp_display_new_with_handles failed: %s", esp_err_to_name(err));
        return (err == ESP_OK) ? ESP_FAIL : err;
    }

    err = esp_lcd_panel_disp_on_off(lcd.panel, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG_DISPLAY, "Could not enable LCD panel output: %s", esp_err_to_name(err));
    }

    /* Loads the saved user brightness, so do it before lighting the backlight. */
    (void)display_dim_init();

    err = display_set_brightness_percent(display_get_user_brightness());
    if (err != ESP_OK) {
        ESP_LOGW(TAG_DISPLAY, "Could not enable backlight: %s", esp_err_to_name(err));
    }

    const esp_lv_adapter_display_config_t disp_cfg = {
        .panel = lcd.panel,
        .panel_io = lcd.io,
        .profile = {
            .interface = ESP_LV_ADAPTER_PANEL_IF_MIPI_DSI,
            .rotation = ESP_LV_ADAPTER_ROTATE_90,
            .hor_res = BSP_LCD_H_RES,
            .ver_res = BSP_LCD_V_RES,
            .buffer_height = DISPLAY_DRAW_BUF_LINES,
            .use_psram = true,
            .enable_ppa_accel = false,
            .require_double_buffer = false,
            .mono_layout = ESP_LV_ADAPTER_MONO_LAYOUT_NONE,
        },
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_DOUBLE_PARTIAL,
        .te_sync = ESP_LV_ADAPTER_TE_SYNC_DISABLED(),
    };
    s_lv_display = esp_lv_adapter_register_display(&disp_cfg);

    if (s_lv_display == NULL) {
        ESP_LOGE(TAG_DISPLAY, "esp_lv_adapter_register_display failed");
        return ESP_FAIL;
    }

    err = esp_lv_adapter_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG_DISPLAY, "esp_lv_adapter_start failed: %s", esp_err_to_name(err));
        return err;
    }

    /* esp_lv_adapter_lock() logs an error on every timeout, but the UI deliberately
     * uses short try-locks (display_lock(0..50)) and defers work on failure. Silence the
     * core adapter tag once init has succeeded; display/touch/bridge tags are unaffected. */
    esp_log_level_set(DISPLAY_ADAPTER_LOG_TAG, ESP_LOG_NONE);

    if (esp_lv_adapter_lock(2000) == ESP_OK) {
        lv_display_add_event_cb(s_lv_display, display_diag_event_cb, LV_EVENT_ALL, NULL);
        esp_lv_adapter_unlock();
    }
    xTaskCreatePinnedToCore(display_diag_task, "disp_diag", 3072, NULL, 5, NULL, 0);

    if (esp_lv_adapter_lock(2000) == ESP_OK) {
        lv_display_set_antialiasing(s_lv_display, APP_LVGL_ANTIALIASING != 0);
        esp_lv_adapter_unlock();
    } else {
        ESP_LOGW(TAG_DISPLAY, "Could not lock LVGL for rotation setup");
        lv_display_set_antialiasing(s_lv_display, APP_LVGL_ANTIALIASING != 0);
    }
    ESP_LOGI(TAG_DISPLAY, "LVGL antialiasing: %s", (APP_LVGL_ANTIALIASING != 0) ? "on" : "off");

    s_display_ready = true;
    ESP_LOGI(TAG_DISPLAY,
        "Display initialized (esp_lvgl_adapter + DSI, tear_avoid=double_partial, buffer_height=%u, %u px)",
        (unsigned)DISPLAY_DRAW_BUF_LINES,
        (unsigned)(BSP_LCD_H_RES * DISPLAY_DRAW_BUF_LINES));
    display_note_activity();
    return ESP_OK;
}

bool display_is_ready(void)
{
    return s_display_ready;
}

bool display_lock(uint32_t timeout_ms)
{
    if (esp_lv_adapter_lock((int32_t)timeout_ms) != ESP_OK) {
        return false;
    }
    if (s_lock_depth++ == 0) {
        s_lock_owner = xTaskGetCurrentTaskHandle();
        s_lock_owner_since_ms = esp_timer_get_time() / 1000;
    }
    return true;
}

void display_unlock(void)
{
    if (s_lock_depth > 0 && --s_lock_depth == 0) {
        s_lock_owner = NULL;
    }
    esp_lv_adapter_unlock();
}
