/* SPDX-License-Identifier: LicenseRef-FNCL-1.1
 * Copyright (c) 2026 Cpt_Kirk
 *
 * Panel-independent auto-dim and user brightness. The per-panel display_init_*.c
 * files only provide display_set_brightness_percent(); the dim timer, the user's
 * chosen brightness (persisted in NVS) and the wake-touch guard live here.
 */
#include "drivers/display_init.h"

#include <stdbool.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"

#include "app_config.h"
#include "util/log_tags.h"

#define DISPLAY_DIM_NVS_NAMESPACE "display"
#define DISPLAY_DIM_NVS_KEY_LEVEL "bright"

static esp_timer_handle_t s_dim_timer = NULL;
static volatile bool s_dimmed = false;
static int s_user_brightness = APP_DISPLAY_ACTIVE_BRIGHTNESS_PERCENT;

static int display_dim_clamp_user(int percent)
{
    if (percent < APP_DISPLAY_MIN_USER_BRIGHTNESS_PERCENT) {
        return APP_DISPLAY_MIN_USER_BRIGHTNESS_PERCENT;
    }
    if (percent > 100) {
        return 100;
    }
    return percent;
}

static void display_dim_load_user_brightness(void)
{
    nvs_handle_t handle;
    if (nvs_open(DISPLAY_DIM_NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return;
    }
    uint8_t level = 0;
    if (nvs_get_u8(handle, DISPLAY_DIM_NVS_KEY_LEVEL, &level) == ESP_OK) {
        s_user_brightness = display_dim_clamp_user(level);
    }
    nvs_close(handle);
}

static void display_dim_save_user_brightness(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(DISPLAY_DIM_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, DISPLAY_DIM_NVS_KEY_LEVEL, (uint8_t)s_user_brightness);
        if (err == ESP_OK) {
            err = nvs_commit(handle);
        }
        nvs_close(handle);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG_DISPLAY, "Could not save brightness: %s", esp_err_to_name(err));
    }
}

static void display_dim_timer_cb(void *arg)
{
    (void)arg;
    if (!display_is_ready()) {
        return;
    }
    /* Dimming only counts when it actually lowers the backlight; otherwise the
     * wake-touch guard would swallow a tap for no visible reason. */
    if (s_user_brightness <= APP_DISPLAY_DIM_BRIGHTNESS_PERCENT) {
        return;
    }
    ESP_LOGD(TAG_DISPLAY, "Idle timeout: dimming backlight");
    s_dimmed = true;
    (void)display_set_brightness_percent(APP_DISPLAY_DIM_BRIGHTNESS_PERCENT);
}

static void display_dim_restart_timer(void)
{
    if (s_dim_timer == NULL) {
        return;
    }
    if (esp_timer_is_active(s_dim_timer)) {
        (void)esp_timer_stop(s_dim_timer);
    }
    const uint64_t timeout_us = (uint64_t)APP_DISPLAY_DIM_TIMEOUT_MS * 1000ULL;
    esp_err_t err = esp_timer_start_once(s_dim_timer, timeout_us);
    if (err != ESP_OK) {
        ESP_LOGW(TAG_DISPLAY, "Could not start display dim timer: %s", esp_err_to_name(err));
    }
}

esp_err_t display_dim_init(void)
{
    if (s_dim_timer != NULL) {
        return ESP_OK;
    }

    display_dim_load_user_brightness();

    const esp_timer_create_args_t timer_args = {
        .callback = display_dim_timer_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "display_dim",
        .skip_unhandled_events = true,
    };
    esp_err_t err = esp_timer_create(&timer_args, &s_dim_timer);
    ESP_LOGI(TAG_DISPLAY, "Auto-dim: user brightness %d%%, dim to %d%% after %d s",
        s_user_brightness, APP_DISPLAY_DIM_BRIGHTNESS_PERCENT, APP_DISPLAY_DIM_TIMEOUT_MS / 1000);
    if (err != ESP_OK) {
        ESP_LOGW(TAG_DISPLAY, "Could not create display dim timer: %s", esp_err_to_name(err));
    }
    return err;
}

void display_note_activity(void)
{
    (void)display_wake_from_dim();
}

bool display_wake_from_dim(void)
{
    if (!display_is_ready()) {
        return false;
    }
    const bool was_dimmed = s_dimmed;
    s_dimmed = false;
    (void)display_set_brightness_percent(s_user_brightness);
    display_dim_restart_timer();
    return was_dimmed;
}

int display_get_user_brightness(void)
{
    return s_user_brightness;
}

int display_adjust_user_brightness(int delta)
{
    const int next = display_dim_clamp_user(s_user_brightness + delta);
    ESP_LOGD(TAG_DISPLAY, "Brightness button %+d: %d%% -> %d%%", delta, s_user_brightness, next);
    if (next != s_user_brightness) {
        s_user_brightness = next;
        display_dim_save_user_brightness();
    }
    display_note_activity();
    return s_user_brightness;
}
