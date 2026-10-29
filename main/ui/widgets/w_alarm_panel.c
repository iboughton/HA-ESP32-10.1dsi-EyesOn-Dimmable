/* SPDX-License-Identifier: LicenseRef-FNCL-1.1
 * Copyright (c) 2026 Cpt_Kirk
 *
 * Alarm panel widget for a HA `alarm_control_panel.*` entity.
 *
 * The widget is a single always-visible page (size it to fill a dashboard
 * page) with three columns:
 *   - keypad + arm/disarm buttons (code is typed per action, never stored),
 *   - zone sensors, switch toggles (e.g. chime) and button entities
 *     (e.g. panic) which only fire after a continuous 2 s hold,
 *   - a history list pulled via the `logbook/get_events` WS command and
 *     extended live from the state changes this widget already receives.
 *
 * Layout JSON (besides entity_id / secondary_entity_id = keypad text sensor):
 *   "alarm_zones":   [{"entity_id": "binary_sensor.x", "name": "Front Door"}, ...]
 *   "alarm_actions": [{"entity_id": "switch.chime" | "button.panic", "name": "..."}, ...]
 *   "alarm_log_hours": 24
 */
#include "drivers/display_init.h"
#include "ui/ui_widget_factory.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "app_config.h"
#include "ha/ha_client.h"
#include "ui/fonts/app_text_fonts.h"
#include "ui/theme/theme_default.h"
#include "ui/ui_i18n.h"
#include "ui/ui_memory.h"

#define W_ALARM_TAG "w_alarm"
#define W_ALARM_MAX_ZONES 16
#define W_ALARM_MAX_ACTIONS 8
#define W_ALARM_LOG_MAX 40
#define W_ALARM_NAME_LEN 48
#define W_ALARM_STATE_LEN APP_MAX_STATE_LEN
#define W_ALARM_CODE_MAX 12
#define W_ALARM_HOLD_MS 2000
#define W_ALARM_CODE_CLEAR_MS 20000
#define W_ALARM_TICK_MS 50
#define W_ALARM_LOG_REFRESH_MS (30 * 60 * 1000)
#define W_ALARM_MARK(stage) do { if (display_diag_mark != NULL) { display_diag_mark(stage); } } while (0)
#define W_ALARM_FETCH_RETRY_MS 1500
#define W_ALARM_FETCH_TIMEOUT_MS 15000
#define W_ALARM_DEFAULT_LOG_HOURS 24

/* alarm_control_panel supported_features bits (HA AlarmControlPanelEntityFeature). */
#define W_ALARM_FEAT_ARM_HOME 1U
#define W_ALARM_FEAT_ARM_AWAY 2U
#define W_ALARM_FEAT_ARM_NIGHT 4U
#define W_ALARM_FEAT_ARM_VACATION 32U

#define W_ALARM_COLOR_OK 0x2EB872
#define W_ALARM_COLOR_ARMED 0xE5484D
#define W_ALARM_COLOR_WARN 0xF5A524
#define W_ALARM_COLOR_TRIGGERED 0xFF1F3D
#define W_ALARM_COLOR_UNKNOWN 0x6B7280

typedef enum {
    W_ALARM_TONE_NEUTRAL = 0,
    W_ALARM_TONE_OK,
    W_ALARM_TONE_WARN,
    W_ALARM_TONE_ALERT,
} w_alarm_tone_t;

typedef struct {
    char entity_id[APP_MAX_ENTITY_ID_LEN];
    char name[W_ALARM_NAME_LEN];
    char state[W_ALARM_STATE_LEN];
    char device_class[24];
    bool known;
} w_alarm_slot_t;

typedef struct {
    int64_t when_s;
    char name[W_ALARM_NAME_LEN];
    char text[40];
    uint8_t tone;
} w_alarm_log_entry_t;

typedef struct {
    lv_obj_t *root;
    lv_obj_t *state_pill;
    lv_obj_t *state_label;
    lv_obj_t *sub_label;
    lv_obj_t *code_label;
    lv_obj_t *msg_label;
    lv_obj_t *zone_chips[W_ALARM_MAX_ZONES];
    lv_obj_t *action_btns[W_ALARM_MAX_ACTIONS];
    lv_obj_t *action_bars[W_ALARM_MAX_ACTIONS];
    lv_obj_t *action_labels[W_ALARM_MAX_ACTIONS];
    lv_obj_t *log_list;
    lv_obj_t *log_status;
    lv_timer_t *tick;
} w_alarm_page_t;

typedef struct {
    lv_obj_t *card;

    char title[APP_MAX_NAME_LEN];
    w_alarm_slot_t alarm;
    char keypad_entity_id[APP_MAX_ENTITY_ID_LEN];
    char keypad_text[64];
    char code_format[16];
    bool code_arm_required;
    uint32_t supported_features;

    w_alarm_slot_t zones[W_ALARM_MAX_ZONES];
    size_t zone_count;
    w_alarm_slot_t actions[W_ALARM_MAX_ACTIONS];
    size_t action_count;
    int log_hours;

    /* Built once in create(); page.root is the widget card itself. */
    w_alarm_page_t page;
    char code[W_ALARM_CODE_MAX + 1];
    size_t code_len;
    int64_t last_code_ms;
    int held_action;
    int64_t hold_start_ms;
    bool hold_fired;

    /* History shown in LVGL context. */
    w_alarm_log_entry_t log[W_ALARM_LOG_MAX];
    size_t log_count;
    bool log_loaded;
    bool fetching;
    int64_t fetch_started_ms;
    int64_t next_fetch_ms;

    /* Staging filled by WS RX task callbacks, drained by the page tick. */
    SemaphoreHandle_t staging_mutex;
    w_alarm_log_entry_t *staged_log;
    size_t staged_count;
    bool staged_valid;
    bool staged_failed;
    char staged_cmd_error[64];
    bool staged_cmd_done;
    bool destroyed;
} w_alarm_ctx_t;

static void w_alarm_render_page_state(w_alarm_ctx_t *ctx);
static void w_alarm_render_log(w_alarm_ctx_t *ctx);
static bool w_alarm_action_is_hold(const w_alarm_slot_t *slot);

static int64_t w_alarm_now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static bool w_alarm_entity_in_domain(const char *entity_id, const char *domain)
{
    size_t len = strlen(domain);
    return strncmp(entity_id, domain, len) == 0 && entity_id[len] == '.';
}

/* ----- State text / colors --------------------------------------------- */
static const char *w_alarm_state_text(const char *state)
{
    if (state == NULL || state[0] == '\0') {
        return ui_i18n_get("alarm.state.unknown", "Unknown");
    }
    if (strcmp(state, "disarmed") == 0) return ui_i18n_get("alarm.state.disarmed", "Disarmed");
    if (strcmp(state, "armed_home") == 0) return ui_i18n_get("alarm.state.armed_home", "Armed Home");
    if (strcmp(state, "armed_away") == 0) return ui_i18n_get("alarm.state.armed_away", "Armed Away");
    if (strcmp(state, "armed_night") == 0) return ui_i18n_get("alarm.state.armed_night", "Armed Night");
    if (strcmp(state, "armed_vacation") == 0) return ui_i18n_get("alarm.state.armed_vacation", "Armed Vacation");
    if (strcmp(state, "armed_custom_bypass") == 0) return ui_i18n_get("alarm.state.armed_bypass", "Armed Bypass");
    if (strcmp(state, "arming") == 0) return ui_i18n_get("alarm.state.arming", "Arming");
    if (strcmp(state, "disarming") == 0) return ui_i18n_get("alarm.state.disarming", "Disarming");
    if (strcmp(state, "pending") == 0) return ui_i18n_get("alarm.state.pending", "Pending");
    if (strcmp(state, "triggered") == 0) return ui_i18n_get("alarm.state.triggered", "TRIGGERED");
    if (strcmp(state, "unavailable") == 0) return ui_i18n_get("common.unavailable", "unavailable");
    return state;
}

static uint32_t w_alarm_state_color(const char *state)
{
    if (state == NULL) return W_ALARM_COLOR_UNKNOWN;
    if (strcmp(state, "disarmed") == 0) return W_ALARM_COLOR_OK;
    if (strncmp(state, "armed_", 6) == 0) return W_ALARM_COLOR_ARMED;
    if (strcmp(state, "triggered") == 0) return W_ALARM_COLOR_TRIGGERED;
    if (strcmp(state, "arming") == 0 || strcmp(state, "pending") == 0 || strcmp(state, "disarming") == 0) {
        return W_ALARM_COLOR_WARN;
    }
    return W_ALARM_COLOR_UNKNOWN;
}

static w_alarm_tone_t w_alarm_state_tone(const char *state)
{
    uint32_t color = w_alarm_state_color(state);
    if (color == W_ALARM_COLOR_OK) return W_ALARM_TONE_OK;
    if (color == W_ALARM_COLOR_WARN) return W_ALARM_TONE_WARN;
    if (color == W_ALARM_COLOR_ARMED || color == W_ALARM_COLOR_TRIGGERED) return W_ALARM_TONE_ALERT;
    return W_ALARM_TONE_NEUTRAL;
}

static bool w_alarm_zone_is_hazard(const w_alarm_slot_t *zone)
{
    static const char *const hazard_classes[] = {
        "smoke", "gas", "carbon_monoxide", "heat", "cold", "moisture", "safety", "problem", "tamper", "vibration", "sound",
    };
    for (size_t i = 0; i < sizeof(hazard_classes) / sizeof(hazard_classes[0]); i++) {
        if (strcmp(zone->device_class, hazard_classes[i]) == 0) {
            return true;
        }
    }
    /* Fault/trouble zones (e.g. "*_fault") are hazards even without a device_class. */
    return strstr(zone->entity_id, "fault") != NULL || strstr(zone->entity_id, "trouble") != NULL;
}

static bool w_alarm_zone_active(const w_alarm_slot_t *zone)
{
    return zone->known && strcmp(zone->state, "on") == 0;
}

/* Text for a zone / action / alarm state the way HA's frontend words it. */
static const char *w_alarm_slot_state_text(const w_alarm_slot_t *slot, const char *state, w_alarm_tone_t *out_tone)
{
    w_alarm_tone_t tone = W_ALARM_TONE_NEUTRAL;
    const char *text = state;
    if (state == NULL || state[0] == '\0' || strcmp(state, "unavailable") == 0 || strcmp(state, "unknown") == 0) {
        text = ui_i18n_get("common.unavailable", "unavailable");
    } else if (w_alarm_entity_in_domain(slot->entity_id, "alarm_control_panel")) {
        text = w_alarm_state_text(state);
        tone = w_alarm_state_tone(state);
    } else if (w_alarm_entity_in_domain(slot->entity_id, "button") ||
               w_alarm_entity_in_domain(slot->entity_id, "input_button")) {
        text = ui_i18n_get("alarm.pressed", "Pressed");
        tone = W_ALARM_TONE_WARN;
    } else if (w_alarm_entity_in_domain(slot->entity_id, "binary_sensor")) {
        bool on = strcmp(state, "on") == 0;
        const char *dc = slot->device_class;
        bool opening = strcmp(dc, "door") == 0 || strcmp(dc, "window") == 0 || strcmp(dc, "opening") == 0 ||
                       strcmp(dc, "garage_door") == 0 || dc[0] == '\0';
        bool motion = strcmp(dc, "motion") == 0 || strcmp(dc, "occupancy") == 0 || strcmp(dc, "presence") == 0;
        if (w_alarm_zone_is_hazard(slot)) {
            text = on ? ui_i18n_get("alarm.zone.detected", "Detected") : ui_i18n_get("alarm.zone.ok", "OK");
            tone = on ? W_ALARM_TONE_ALERT : W_ALARM_TONE_OK;
        } else if (motion) {
            text = on ? ui_i18n_get("alarm.zone.motion", "Motion") : ui_i18n_get("alarm.zone.clear", "Clear");
            tone = on ? W_ALARM_TONE_WARN : W_ALARM_TONE_OK;
        } else if (opening) {
            text = on ? ui_i18n_get("alarm.zone.open", "Open") : ui_i18n_get("alarm.zone.closed", "Closed");
            tone = on ? W_ALARM_TONE_WARN : W_ALARM_TONE_OK;
        } else {
            text = on ? ui_i18n_get("alarm.zone.active", "Active") : ui_i18n_get("alarm.zone.ok", "OK");
            tone = on ? W_ALARM_TONE_WARN : W_ALARM_TONE_OK;
        }
    } else if (strcmp(state, "on") == 0) {
        text = ui_i18n_get("common.on", "On");
        tone = W_ALARM_TONE_OK;
    } else if (strcmp(state, "off") == 0) {
        text = ui_i18n_get("common.off", "Off");
    }
    if (out_tone != NULL) {
        *out_tone = tone;
    }
    return text;
}

static uint32_t w_alarm_tone_color(w_alarm_tone_t tone)
{
    switch (tone) {
    case W_ALARM_TONE_OK:
        return W_ALARM_COLOR_OK;
    case W_ALARM_TONE_WARN:
        return W_ALARM_COLOR_WARN;
    case W_ALARM_TONE_ALERT:
        return W_ALARM_COLOR_ARMED;
    default:
        return W_ALARM_COLOR_UNKNOWN;
    }
}

/* ----- Slot lookup ------------------------------------------------------ */
static w_alarm_slot_t *w_alarm_find_slot(w_alarm_ctx_t *ctx, const char *entity_id, int *out_zone, int *out_action)
{
    if (out_zone != NULL) *out_zone = -1;
    if (out_action != NULL) *out_action = -1;
    if (strcmp(ctx->alarm.entity_id, entity_id) == 0) {
        return &ctx->alarm;
    }
    for (size_t i = 0; i < ctx->zone_count; i++) {
        if (strcmp(ctx->zones[i].entity_id, entity_id) == 0) {
            if (out_zone != NULL) *out_zone = (int)i;
            return &ctx->zones[i];
        }
    }
    for (size_t i = 0; i < ctx->action_count; i++) {
        if (strcmp(ctx->actions[i].entity_id, entity_id) == 0) {
            if (out_action != NULL) *out_action = (int)i;
            return &ctx->actions[i];
        }
    }
    return NULL;
}

static void w_alarm_set_slot_name_from_attrs(w_alarm_slot_t *slot, cJSON *attrs)
{
    if (slot->name[0] == '\0') {
        cJSON *friendly = cJSON_GetObjectItemCaseSensitive(attrs, "friendly_name");
        if (cJSON_IsString(friendly) && friendly->valuestring != NULL) {
            snprintf(slot->name, sizeof(slot->name), "%s", friendly->valuestring);
        }
    }
    cJSON *dc = cJSON_GetObjectItemCaseSensitive(attrs, "device_class");
    if (cJSON_IsString(dc) && dc->valuestring != NULL) {
        snprintf(slot->device_class, sizeof(slot->device_class), "%s", dc->valuestring);
    }
}

static const char *w_alarm_slot_display_name(const w_alarm_slot_t *slot)
{
    if (slot->name[0] != '\0') {
        return slot->name;
    }
    const char *dot = strchr(slot->entity_id, '.');
    return dot != NULL ? dot + 1 : slot->entity_id;
}

/* ----- History ---------------------------------------------------------- */
static void w_alarm_log_push_front(w_alarm_ctx_t *ctx, const w_alarm_slot_t *slot, const char *state)
{
    W_ALARM_MARK("alarm:log_push_front");
    if (ctx->log_count == W_ALARM_LOG_MAX) {
        ctx->log_count--;
    }
    memmove(&ctx->log[1], &ctx->log[0], sizeof(ctx->log[0]) * ctx->log_count);
    w_alarm_log_entry_t *e = &ctx->log[0];
    memset(e, 0, sizeof(*e));
    e->when_s = (int64_t)time(NULL);
    snprintf(e->name, sizeof(e->name), "%s", w_alarm_slot_display_name(slot));
    w_alarm_tone_t tone = W_ALARM_TONE_NEUTRAL;
    snprintf(e->text, sizeof(e->text), "%s", w_alarm_slot_state_text(slot, state, &tone));
    e->tone = (uint8_t)tone;
    ctx->log_count++;
}

/* Days since 1970-01-01 for a proleptic Gregorian date (Howard Hinnant). */
static int64_t w_alarm_days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int64_t yoe = y - era * 400;
    const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static double w_alarm_json_when(cJSON *when)
{
    if (cJSON_IsNumber(when)) {
        return when->valuedouble;
    }
    if (cJSON_IsString(when) && when->valuestring != NULL) {
        /* Older HA returns ISO 8601 UTC ("2026-10-26T18:02:11.123+00:00"). */
        int y = 0, mo = 0, d = 0, h = 0, mi = 0, sec = 0;
        if (sscanf(when->valuestring, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &sec) == 6) {
            return (double)(w_alarm_days_from_civil(y, mo, d) * 86400 + h * 3600 + mi * 60 + sec);
        }
    }
    return 0.0;
}

/* WS RX task: parse logbook/get_events into a heap array, stage under mutex. */
static void w_alarm_log_response_cb(bool success, cJSON *result, void *user)
{
    w_alarm_ctx_t *ctx = (w_alarm_ctx_t *)user;
    if (ctx == NULL || ctx->staging_mutex == NULL) {
        return;
    }

    w_alarm_log_entry_t *parsed = NULL;
    size_t parsed_count = 0;
    if (success && cJSON_IsArray(result)) {
        parsed = ui_calloc_prefer_psram(W_ALARM_LOG_MAX, sizeof(*parsed));
        if (parsed != NULL) {
            /* Entries arrive oldest first; walk backwards to keep the newest. */
            for (cJSON *item = cJSON_GetArrayItem(result, cJSON_GetArraySize(result) - 1);
                 item != NULL && parsed_count < W_ALARM_LOG_MAX; item = item->prev) {
                cJSON *entity = cJSON_GetObjectItemCaseSensitive(item, "entity_id");
                cJSON *state = cJSON_GetObjectItemCaseSensitive(item, "state");
                cJSON *name = cJSON_GetObjectItemCaseSensitive(item, "name");
                cJSON *message = cJSON_GetObjectItemCaseSensitive(item, "message");
                const char *entity_id = cJSON_IsString(entity) ? entity->valuestring : "";

                w_alarm_slot_t probe = {0};
                const w_alarm_slot_t *slot = (entity_id[0] != '\0') ? w_alarm_find_slot(ctx, entity_id, NULL, NULL) : NULL;
                if (slot == NULL) {
                    snprintf(probe.entity_id, sizeof(probe.entity_id), "%s", entity_id);
                    if (cJSON_IsString(name)) {
                        snprintf(probe.name, sizeof(probe.name), "%s", name->valuestring);
                    }
                    slot = &probe;
                }

                w_alarm_log_entry_t *e = &parsed[parsed_count];
                e->when_s = (int64_t)w_alarm_json_when(cJSON_GetObjectItemCaseSensitive(item, "when"));
                snprintf(e->name, sizeof(e->name), "%s", w_alarm_slot_display_name(slot));
                w_alarm_tone_t tone = W_ALARM_TONE_NEUTRAL;
                if (cJSON_IsString(state) && state->valuestring != NULL) {
                    snprintf(e->text, sizeof(e->text), "%s", w_alarm_slot_state_text(slot, state->valuestring, &tone));
                } else if (cJSON_IsString(message) && message->valuestring != NULL) {
                    snprintf(e->text, sizeof(e->text), "%s", message->valuestring);
                } else {
                    continue;
                }
                e->tone = (uint8_t)tone;
                parsed_count++;
            }
        }
    }

    xSemaphoreTake(ctx->staging_mutex, portMAX_DELAY);
    if (!ctx->destroyed) {
        free(ctx->staged_log);
        ctx->staged_log = parsed;
        ctx->staged_count = parsed_count;
        ctx->staged_valid = (parsed != NULL);
        ctx->staged_failed = (parsed == NULL);
        parsed = NULL;
    }
    xSemaphoreGive(ctx->staging_mutex);
    free(parsed);
}

static void w_alarm_request_log(w_alarm_ctx_t *ctx)
{
    W_ALARM_MARK("alarm:request_log");
    int64_t now_ms = w_alarm_now_ms();
    time_t now = time(NULL);
    if (now < 1700000000) {
        /* SNTP not synced yet: logbook needs an absolute start time. */
        if (ctx->page.log_status != NULL) {
            lv_label_set_text(ctx->page.log_status, ui_i18n_get("alarm.log.no_clock", "Waiting for clock sync"));
        }
        ctx->next_fetch_ms = now_ms + 5000;
        return;
    }

    cJSON *fields = cJSON_CreateObject();
    cJSON *ids = cJSON_CreateArray();
    if (fields == NULL || ids == NULL) {
        cJSON_Delete(fields);
        cJSON_Delete(ids);
        return;
    }
    time_t start = now - (time_t)ctx->log_hours * 3600;
    struct tm gm = {0};
    gmtime_r(&start, &gm);
    char start_iso[40];
    strftime(start_iso, sizeof(start_iso), "%Y-%m-%dT%H:%M:%S+00:00", &gm);
    cJSON_AddStringToObject(fields, "start_time", start_iso);
    cJSON_AddItemToArray(ids, cJSON_CreateString(ctx->alarm.entity_id));
    for (size_t i = 0; i < ctx->zone_count; i++) {
        cJSON_AddItemToArray(ids, cJSON_CreateString(ctx->zones[i].entity_id));
    }
    for (size_t i = 0; i < ctx->action_count; i++) {
        cJSON_AddItemToArray(ids, cJSON_CreateString(ctx->actions[i].entity_id));
    }
    cJSON_AddItemToObject(fields, "entity_ids", ids);

    ctx->fetching = true;
    ctx->fetch_started_ms = now_ms;
    esp_err_t err = ha_client_send_command_with_response("logbook/get_events", fields, w_alarm_log_response_cb, ctx);
    if (err != ESP_OK) {
        ctx->fetching = false;
        ctx->next_fetch_ms = now_ms + W_ALARM_FETCH_RETRY_MS;
        if (err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(W_ALARM_TAG, "logbook request failed: %s", esp_err_to_name(err));
        }
    }
}

static void w_alarm_drain_staging(w_alarm_ctx_t *ctx)
{
    W_ALARM_MARK("alarm:drain_staging");
    bool got_log = false;
    bool log_failed = false;
    bool cmd_done = false;
    char cmd_error[sizeof(ctx->staged_cmd_error)] = {0};

    xSemaphoreTake(ctx->staging_mutex, portMAX_DELAY);
    if (ctx->staged_valid) {
        ctx->log_count = ctx->staged_count;
        if (ctx->staged_count > 0) {
            memcpy(ctx->log, ctx->staged_log, sizeof(ctx->log[0]) * ctx->staged_count);
        }
        got_log = true;
    }
    log_failed = ctx->staged_failed;
    free(ctx->staged_log);
    ctx->staged_log = NULL;
    ctx->staged_count = 0;
    ctx->staged_valid = false;
    ctx->staged_failed = false;
    if (ctx->staged_cmd_done) {
        cmd_done = true;
        memcpy(cmd_error, ctx->staged_cmd_error, sizeof(cmd_error));
        ctx->staged_cmd_done = false;
        ctx->staged_cmd_error[0] = '\0';
    }
    xSemaphoreGive(ctx->staging_mutex);

    if (got_log || log_failed) {
        ctx->fetching = false;
        ctx->log_loaded = got_log || ctx->log_loaded;
        if (ctx->page.log_status != NULL) {
            lv_label_set_text(ctx->page.log_status,
                log_failed ? ui_i18n_get("alarm.log.failed", "History unavailable") : "");
        }
        if (got_log) {
            w_alarm_render_log(ctx);
        }
    }
    if (cmd_done && cmd_error[0] != '\0' && ctx->page.msg_label != NULL) {
        lv_label_set_text(ctx->page.msg_label, cmd_error);
        lv_obj_set_style_text_color(ctx->page.msg_label, lv_color_hex(W_ALARM_COLOR_ARMED), LV_PART_MAIN);
    }
}

/* ----- Commands --------------------------------------------------------- */
/* WS RX task: surface HA's error message (e.g. "Invalid alarm code"). */
static void w_alarm_command_response_cb(bool success, cJSON *result, void *user)
{
    (void)result;
    w_alarm_ctx_t *ctx = (w_alarm_ctx_t *)user;
    if (ctx == NULL || ctx->staging_mutex == NULL) {
        return;
    }
    xSemaphoreTake(ctx->staging_mutex, portMAX_DELAY);
    if (!ctx->destroyed) {
        ctx->staged_cmd_done = true;
        snprintf(ctx->staged_cmd_error, sizeof(ctx->staged_cmd_error), "%s",
            success ? "" : ui_i18n_get("alarm.msg.rejected", "Rejected by alarm (wrong code?)"));
    }
    xSemaphoreGive(ctx->staging_mutex);
}

static void w_alarm_set_message(w_alarm_ctx_t *ctx, const char *text, lv_color_t color)
{
    if (ctx->page.msg_label != NULL) {
        lv_label_set_text(ctx->page.msg_label, text);
        lv_obj_set_style_text_color(ctx->page.msg_label, color, LV_PART_MAIN);
    }
}

static bool w_alarm_call(w_alarm_ctx_t *ctx, const char *domain, const char *service, const char *entity_id,
    const char *code)
{
    cJSON *data = cJSON_CreateObject();
    if (data == NULL) {
        return false;
    }
    cJSON_AddStringToObject(data, "entity_id", entity_id);
    if (code != NULL && code[0] != '\0') {
        cJSON_AddStringToObject(data, "code", code);
    }

    /* Prefer a plain call_service with a reply so a wrong code is reported;
     * fall back to fire-and-forget when the heavy-send gate is busy. */
    cJSON *fields = cJSON_CreateObject();
    if (fields != NULL) {
        cJSON_AddStringToObject(fields, "domain", domain);
        cJSON_AddStringToObject(fields, "service", service);
        cJSON_AddItemToObject(fields, "service_data", cJSON_Duplicate(data, true));
        if (ha_client_send_command_with_response("call_service", fields, w_alarm_command_response_cb, ctx) == ESP_OK) {
            cJSON_Delete(data);
            return true;
        }
    }

    char *payload = cJSON_PrintUnformatted(data);
    cJSON_Delete(data);
    if (payload == NULL) {
        return false;
    }
    esp_err_t err = ha_client_call_service(domain, service, payload);
    cJSON_free(payload);
    return err == ESP_OK;
}

static void w_alarm_clear_code(w_alarm_ctx_t *ctx)
{
    memset(ctx->code, 0, sizeof(ctx->code));
    ctx->code_len = 0;
}

static bool w_alarm_code_needed(const w_alarm_ctx_t *ctx, bool arming)
{
    if (ctx->code_format[0] == '\0') {
        return false;
    }
    return arming ? ctx->code_arm_required : true;
}

static void w_alarm_run_alarm_service(w_alarm_ctx_t *ctx, const char *service, bool arming, const char *label)
{
    if (strcmp(ctx->alarm.state, "unavailable") == 0) {
        w_alarm_set_message(ctx, ui_i18n_get("common.unavailable", "unavailable"), lv_color_hex(W_ALARM_COLOR_ARMED));
        return;
    }
    if (w_alarm_code_needed(ctx, arming) && ctx->code_len == 0) {
        w_alarm_set_message(ctx, ui_i18n_get("alarm.msg.enter_code", "Enter code first"), lv_color_hex(W_ALARM_COLOR_WARN));
        return;
    }
    bool ok = w_alarm_call(ctx, "alarm_control_panel", service, ctx->alarm.entity_id, ctx->code);
    w_alarm_clear_code(ctx);
    char msg[64];
    snprintf(msg, sizeof(msg), "%s: %s", ok ? ui_i18n_get("alarm.msg.sent", "Sent") : ui_i18n_get("alarm.msg.failed", "Failed"),
        label);
    w_alarm_set_message(ctx, msg, ok ? theme_default_color_text_muted() : lv_color_hex(W_ALARM_COLOR_ARMED));
    w_alarm_render_page_state(ctx);
}

/* ----- Page rendering --------------------------------------------------- */
static void w_alarm_style_pill(lv_obj_t *pill, uint32_t color)
{
    lv_obj_set_style_bg_color(pill, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, LV_PART_MAIN);
}

static void w_alarm_render_summary(w_alarm_ctx_t *ctx)
{
    if (ctx->page.sub_label == NULL) {
        return;
    }
    char sub[96];
    size_t open = 0;
    const char *first_open = NULL;
    for (size_t i = 0; i < ctx->zone_count; i++) {
        if (w_alarm_zone_active(&ctx->zones[i])) {
            if (open == 0) {
                first_open = w_alarm_slot_display_name(&ctx->zones[i]);
            }
            open++;
        }
    }
    if (ctx->keypad_text[0] != '\0') {
        snprintf(sub, sizeof(sub), "%s", ctx->keypad_text);
    } else if (open == 1) {
        snprintf(sub, sizeof(sub), "%s: %s", ui_i18n_get("alarm.zone.not_ready", "Not ready"), first_open);
    } else if (open > 1) {
        snprintf(sub, sizeof(sub), "%u %s", (unsigned)open, ui_i18n_get("alarm.zones_active", "zones active"));
    } else if (ctx->zone_count > 0) {
        snprintf(sub, sizeof(sub), "%s", ui_i18n_get("alarm.zones_ready", "Ready - all zones clear"));
    } else {
        sub[0] = '\0';
    }
    lv_label_set_text(ctx->page.sub_label, sub);
}

static void w_alarm_render_code(w_alarm_ctx_t *ctx)
{
    if (ctx->page.code_label == NULL) {
        return;
    }
    if (ctx->code_len == 0) {
        lv_label_set_text(ctx->page.code_label,
            ctx->code_format[0] != '\0' ? ui_i18n_get("alarm.enter_code", "Enter code")
                                        : ui_i18n_get("alarm.no_code", "No code required"));
        lv_obj_set_style_text_color(ctx->page.code_label, theme_default_color_text_muted(), LV_PART_MAIN);
        return;
    }
    char masked[W_ALARM_CODE_MAX * 2 + 1] = {0};
    for (size_t i = 0; i < ctx->code_len; i++) {
        masked[i * 2] = '*';
        masked[i * 2 + 1] = ' ';
    }
    lv_label_set_text(ctx->page.code_label, masked);
    lv_obj_set_style_text_color(ctx->page.code_label, theme_default_color_text_primary(), LV_PART_MAIN);
}

static void w_alarm_render_page_state(w_alarm_ctx_t *ctx)
{
    W_ALARM_MARK("alarm:render_page_state");
    w_alarm_page_t *page = &ctx->page;
    if (page->root == NULL) {
        return;
    }
    const char *state = ctx->alarm.known ? ctx->alarm.state : "";
    lv_label_set_text(page->state_label, w_alarm_state_text(state));
    w_alarm_style_pill(page->state_pill, w_alarm_state_color(state));
    w_alarm_render_summary(ctx);
    w_alarm_render_code(ctx);

    for (size_t i = 0; i < ctx->zone_count; i++) {
        if (page->zone_chips[i] == NULL) {
            continue;
        }
        w_alarm_tone_t tone = W_ALARM_TONE_NEUTRAL;
        const char *text = w_alarm_slot_state_text(&ctx->zones[i], ctx->zones[i].known ? ctx->zones[i].state : "", &tone);
        lv_label_set_text(page->zone_chips[i], text);
        bool active = w_alarm_zone_active(&ctx->zones[i]);
        lv_obj_set_style_bg_opa(page->zone_chips[i], active ? LV_OPA_COVER : LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_bg_color(page->zone_chips[i], lv_color_hex(w_alarm_tone_color(tone)), LV_PART_MAIN);
        lv_obj_set_style_text_color(page->zone_chips[i],
            active ? lv_color_hex(0xFFFFFF) : lv_color_hex(w_alarm_tone_color(tone)), LV_PART_MAIN);
    }

    for (size_t i = 0; i < ctx->action_count; i++) {
        if (page->action_labels[i] == NULL || w_alarm_action_is_hold(&ctx->actions[i])) {
            continue;
        }
        bool on = ctx->actions[i].known && strcmp(ctx->actions[i].state, "on") == 0;
        char text[80];
        snprintf(text, sizeof(text), "%s: %s", w_alarm_slot_display_name(&ctx->actions[i]),
            ctx->actions[i].known ? (on ? ui_i18n_get("common.on", "On") : ui_i18n_get("common.off", "Off"))
                                  : ui_i18n_get("common.unavailable", "unavailable"));
        lv_label_set_text(page->action_labels[i], text);
        lv_obj_set_style_bg_color(page->action_btns[i],
            on ? lv_color_hex(W_ALARM_COLOR_OK) : lv_color_hex(APP_UI_COLOR_TOPBAR_CHIP_BG), LV_PART_MAIN);
    }
}

static void w_alarm_render_log(w_alarm_ctx_t *ctx)
{
    W_ALARM_MARK("alarm:render_log");
    w_alarm_page_t *page = &ctx->page;
    if (page->log_list == NULL) {
        return;
    }
    lv_obj_clean(page->log_list);
    if (ctx->log_count == 0) {
        lv_obj_t *empty = lv_label_create(page->log_list);
        lv_label_set_text(empty, ctx->log_loaded ? ui_i18n_get("alarm.log.empty", "No events") : "...");
        lv_obj_set_style_text_color(empty, theme_default_color_text_muted(), LV_PART_MAIN);
        lv_obj_set_style_text_font(empty, APP_FONT_TEXT_18, LV_PART_MAIN);
        return;
    }

    time_t now = time(NULL);
    struct tm today = {0};
    localtime_r(&now, &today);
    for (size_t i = 0; i < ctx->log_count; i++) {
        const w_alarm_log_entry_t *e = &ctx->log[i];
        lv_obj_t *row = lv_obj_create(page->log_list);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_style_pad_ver(row, 6, LV_PART_MAIN);
        lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
        lv_obj_set_style_border_width(row, 1, LV_PART_MAIN);
        lv_obj_set_style_border_color(row, lv_color_hex(APP_UI_COLOR_CARD_BORDER), LV_PART_MAIN);
        lv_obj_set_layout(row, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, 10, LV_PART_MAIN);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);

        char when_text[24] = "--:--";
        if (e->when_s > 0) {
            time_t t = (time_t)e->when_s;
            struct tm lt = {0};
            localtime_r(&t, &lt);
            bool same_day = lt.tm_yday == today.tm_yday && lt.tm_year == today.tm_year;
            strftime(when_text, sizeof(when_text), same_day ? "%H:%M" : "%a %H:%M", &lt);
        }
        lv_obj_t *when = lv_label_create(row);
        lv_label_set_text(when, when_text);
        lv_obj_set_width(when, 96);
        lv_obj_set_style_text_font(when, APP_FONT_TEXT_16, LV_PART_MAIN);
        lv_obj_set_style_text_color(when, theme_default_color_text_muted(), LV_PART_MAIN);

        lv_obj_t *name = lv_label_create(row);
        lv_label_set_text(name, e->name);
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        lv_obj_set_flex_grow(name, 1);
        lv_obj_set_style_text_font(name, APP_FONT_TEXT_18, LV_PART_MAIN);
        lv_obj_set_style_text_color(name, theme_default_color_text_primary(), LV_PART_MAIN);

        lv_obj_t *text = lv_label_create(row);
        lv_label_set_text(text, e->text);
        lv_obj_set_style_text_font(text, APP_FONT_TEXT_18, LV_PART_MAIN);
        lv_obj_set_style_text_color(text, lv_color_hex(w_alarm_tone_color((w_alarm_tone_t)e->tone)), LV_PART_MAIN);
    }
}

/* ----- Page events ------------------------------------------------------ */
static void w_alarm_keypad_event_cb(lv_event_t *event)
{
    W_ALARM_MARK("alarm:keypad_event_cb");
    w_alarm_ctx_t *ctx = (w_alarm_ctx_t *)lv_event_get_user_data(event);
    lv_obj_t *btnm = lv_event_get_target(event);
    uint32_t idx = lv_buttonmatrix_get_selected_button(btnm);
    if (ctx == NULL || idx == LV_BUTTONMATRIX_BUTTON_NONE) {
        return;
    }
    const char *txt = lv_buttonmatrix_get_button_text(btnm, idx);
    if (txt == NULL) {
        return;
    }
    ctx->last_code_ms = w_alarm_now_ms();
    if (strcmp(txt, "C") == 0) {
        w_alarm_clear_code(ctx);
    } else if (strcmp(txt, "<") == 0) {
        if (ctx->code_len > 0) {
            ctx->code[--ctx->code_len] = '\0';
        }
    } else if (txt[0] >= '0' && txt[0] <= '9' && txt[1] == '\0' && ctx->code_len < W_ALARM_CODE_MAX) {
        ctx->code[ctx->code_len++] = txt[0];
        ctx->code[ctx->code_len] = '\0';
    }
    w_alarm_set_message(ctx, "", theme_default_color_text_muted());
    w_alarm_render_code(ctx);
}

typedef struct {
    const char *service;
    const char *label_key;
    const char *label_fallback;
    bool arming;
} w_alarm_mode_t;

static const w_alarm_mode_t s_alarm_modes[] = {
    {"alarm_disarm", "alarm.btn.disarm", "Disarm", false},
    {"alarm_arm_away", "alarm.btn.away", "Arm Away", true},
    {"alarm_arm_home", "alarm.btn.home", "Arm Home", true},
    {"alarm_arm_night", "alarm.btn.night", "Arm Night", true},
    {"alarm_arm_vacation", "alarm.btn.vacation", "Vacation", true},
};

static void w_alarm_mode_event_cb(lv_event_t *event)
{
    W_ALARM_MARK("alarm:mode_event_cb");
    w_alarm_ctx_t *ctx = (w_alarm_ctx_t *)lv_event_get_user_data(event);
    lv_obj_t *btn = lv_event_get_target(event);
    if (ctx == NULL) {
        return;
    }
    uintptr_t mode = (uintptr_t)lv_obj_get_user_data(btn);
    if (mode >= sizeof(s_alarm_modes) / sizeof(s_alarm_modes[0])) {
        return;
    }
    const w_alarm_mode_t *m = &s_alarm_modes[mode];
    w_alarm_run_alarm_service(ctx, m->service, m->arming, ui_i18n_get(m->label_key, m->label_fallback));
}

static bool w_alarm_action_is_hold(const w_alarm_slot_t *slot)
{
    return w_alarm_entity_in_domain(slot->entity_id, "button") || w_alarm_entity_in_domain(slot->entity_id, "input_button");
}

static void w_alarm_action_event_cb(lv_event_t *event)
{
    W_ALARM_MARK("alarm:action_event_cb");
    w_alarm_ctx_t *ctx = (w_alarm_ctx_t *)lv_event_get_user_data(event);
    lv_obj_t *btn = lv_event_get_current_target(event);
    if (ctx == NULL) {
        return;
    }
    int idx = (int)(uintptr_t)lv_obj_get_user_data(btn);
    if (idx < 0 || (size_t)idx >= ctx->action_count) {
        return;
    }
    w_alarm_slot_t *slot = &ctx->actions[idx];
    lv_event_code_t code = lv_event_get_code(event);

    if (!w_alarm_action_is_hold(slot)) {
        if (code == LV_EVENT_CLICKED) {
            const char *domain = w_alarm_entity_in_domain(slot->entity_id, "input_boolean") ? "input_boolean" : "switch";
            char payload[160];
            snprintf(payload, sizeof(payload), "{\"entity_id\":\"%s\"}", slot->entity_id);
            if (ha_client_call_service(domain, "toggle", payload) != ESP_OK) {
                w_alarm_set_message(ctx, ui_i18n_get("alarm.msg.failed", "Failed"), lv_color_hex(W_ALARM_COLOR_ARMED));
            }
        }
        return;
    }

    if (code == LV_EVENT_PRESSED) {
        ctx->held_action = idx;
        ctx->hold_start_ms = w_alarm_now_ms();
        ctx->hold_fired = false;
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        if (ctx->held_action == idx) {
            ctx->held_action = -1;
            if (ctx->page.action_bars[idx] != NULL) {
                lv_bar_set_value(ctx->page.action_bars[idx], 0, LV_ANIM_OFF);
            }
        }
    }
}

static void w_alarm_update_hold(w_alarm_ctx_t *ctx, int64_t now)
{
    int idx = ctx->held_action;
    if (idx < 0 || (size_t)idx >= ctx->action_count || ctx->page.action_bars[idx] == NULL) {
        return;
    }
    int64_t elapsed = now - ctx->hold_start_ms;
    int pct = (int)((elapsed * 100) / W_ALARM_HOLD_MS);
    lv_bar_set_value(ctx->page.action_bars[idx], pct > 100 ? 100 : pct, LV_ANIM_OFF);
    if (elapsed < W_ALARM_HOLD_MS || ctx->hold_fired) {
        return;
    }
    ctx->hold_fired = true;
    w_alarm_slot_t *slot = &ctx->actions[idx];
    const char *domain = w_alarm_entity_in_domain(slot->entity_id, "input_button") ? "input_button" : "button";
    char payload[160];
    snprintf(payload, sizeof(payload), "{\"entity_id\":\"%s\"}", slot->entity_id);
    esp_err_t err = ha_client_call_service(domain, "press", payload);
    char msg[80];
    snprintf(msg, sizeof(msg), "%s: %s", err == ESP_OK ? ui_i18n_get("alarm.msg.sent", "Sent") : ui_i18n_get("alarm.msg.failed", "Failed"),
        w_alarm_slot_display_name(slot));
    w_alarm_set_message(ctx, msg, lv_color_hex(err == ESP_OK ? W_ALARM_COLOR_WARN : W_ALARM_COLOR_ARMED));
    ESP_LOGW(W_ALARM_TAG, "Held action fired: %s (%s)", slot->entity_id, esp_err_to_name(err));
}

static void w_alarm_page_tick_cb(lv_timer_t *timer)
{
    W_ALARM_MARK("alarm:page_tick_cb");
    w_alarm_ctx_t *ctx = (w_alarm_ctx_t *)lv_timer_get_user_data(timer);
    if (ctx == NULL || ctx->page.root == NULL) {
        return;
    }
    int64_t now = w_alarm_now_ms();

    w_alarm_update_hold(ctx, now);
    w_alarm_drain_staging(ctx);

    if (ctx->fetching && (now - ctx->fetch_started_ms) > W_ALARM_FETCH_TIMEOUT_MS) {
        ctx->fetching = false;
        ctx->next_fetch_ms = now + W_ALARM_FETCH_RETRY_MS * 10;
    }
    /* Live state changes extend the list; a periodic re-sync picks up anything
     * missed while HA was disconnected. Wait for the initial subscribe burst. */
    if (!ctx->fetching && now >= ctx->next_fetch_ms && ha_client_is_connected() &&
        ha_client_is_initial_sync_done()) {
        ctx->next_fetch_ms = now + W_ALARM_LOG_REFRESH_MS;
        w_alarm_request_log(ctx);
    }

    if (ctx->code_len > 0 && (now - ctx->last_code_ms) > W_ALARM_CODE_CLEAR_MS) {
        w_alarm_clear_code(ctx);
        w_alarm_render_code(ctx);
    }
    W_ALARM_MARK("alarm:tick_done");
}

static void w_alarm_refresh_log_event_cb(lv_event_t *event)
{
    w_alarm_ctx_t *ctx = (w_alarm_ctx_t *)lv_event_get_user_data(event);
    if (ctx == NULL || ctx->fetching) {
        return;
    }
    ctx->next_fetch_ms = 0;
    if (ctx->page.log_status != NULL) {
        lv_label_set_text(ctx->page.log_status, ui_i18n_get("alarm.log.loading", "Loading..."));
    }
}

/* ----- Page construction ------------------------------------------------ */
static lv_obj_t *w_alarm_column(lv_obj_t *parent, int grow)
{
    lv_obj_t *col = lv_obj_create(parent);
    theme_default_style_card(col);
    lv_obj_set_height(col, LV_PCT(100));
    lv_obj_set_flex_grow(col, grow);
    lv_obj_set_style_pad_all(col, 16, LV_PART_MAIN);
    lv_obj_set_layout(col, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, 10, LV_PART_MAIN);
    lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);
    return col;
}

static lv_obj_t *w_alarm_heading(lv_obj_t *parent, const char *text)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, APP_FONT_TEXT_22, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, theme_default_color_text_muted(), LV_PART_MAIN);
    return label;
}

static lv_obj_t *w_alarm_button(lv_obj_t *parent, const char *text, uint32_t bg, const lv_font_t *font)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_height(btn, 64);
    lv_obj_set_style_radius(btn, 12, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, lv_color_hex(bg), LV_PART_MAIN);
    lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN);
    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_center(label);
    return btn;
}

static void w_alarm_build_keypad_column(w_alarm_ctx_t *ctx, lv_obj_t *col)
{
    w_alarm_page_t *page = &ctx->page;

    lv_obj_t *header = lv_obj_create(col);
    lv_obj_remove_style_all(header);
    lv_obj_set_size(header, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_layout(header, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(header, 12, LV_PART_MAIN);

    page->state_pill = lv_obj_create(header);
    lv_obj_remove_style_all(page->state_pill);
    lv_obj_set_size(page->state_pill, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(page->state_pill, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(page->state_pill, 16, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(page->state_pill, 6, LV_PART_MAIN);
    page->state_label = lv_label_create(page->state_pill);
    lv_obj_set_style_text_font(page->state_label, APP_FONT_TEXT_22, LV_PART_MAIN);
    lv_obj_set_style_text_color(page->state_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);

    page->sub_label = lv_label_create(header);
    lv_label_set_long_mode(page->sub_label, LV_LABEL_LONG_DOT);
    lv_obj_set_flex_grow(page->sub_label, 1);
    lv_obj_set_style_text_font(page->sub_label, APP_FONT_TEXT_18, LV_PART_MAIN);
    lv_obj_set_style_text_color(page->sub_label, theme_default_color_text_muted(), LV_PART_MAIN);

    page->code_label = lv_label_create(col);
    lv_obj_set_width(page->code_label, LV_PCT(100));
    lv_obj_set_style_text_align(page->code_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_font(page->code_label, APP_FONT_DISPLAY_34, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(page->code_label, 6, LV_PART_MAIN);

    static const char *const keypad_map[] = {
        "1", "2", "3", "\n",
        "4", "5", "6", "\n",
        "7", "8", "9", "\n",
        "C", "0", "<", "",
    };
    lv_obj_t *keypad = lv_buttonmatrix_create(col);
    lv_buttonmatrix_set_map(keypad, keypad_map);
    lv_obj_set_width(keypad, LV_PCT(100));
    lv_obj_set_flex_grow(keypad, 1);
    lv_obj_set_style_bg_opa(keypad, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(keypad, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(keypad, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_gap(keypad, 10, LV_PART_MAIN);
    lv_obj_set_style_radius(keypad, 12, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(keypad, lv_color_hex(APP_UI_COLOR_TOPBAR_CHIP_BG), LV_PART_ITEMS);
    lv_obj_set_style_text_color(keypad, theme_default_color_text_primary(), LV_PART_ITEMS);
    lv_obj_set_style_text_font(keypad, APP_FONT_DISPLAY_34, LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(keypad, 0, LV_PART_ITEMS);
    lv_obj_add_event_cb(keypad, w_alarm_keypad_event_cb, LV_EVENT_VALUE_CHANGED, ctx);

    lv_obj_t *modes = lv_obj_create(col);
    lv_obj_remove_style_all(modes);
    lv_obj_set_size(modes, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_layout(modes, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(modes, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(modes, 10, LV_PART_MAIN);

    uint32_t features = ctx->supported_features;
    if (features == 0) {
        features = W_ALARM_FEAT_ARM_HOME | W_ALARM_FEAT_ARM_AWAY;
    }
    for (size_t i = 0; i < sizeof(s_alarm_modes) / sizeof(s_alarm_modes[0]); i++) {
        bool show = (i == 0) ||
                    (i == 1 && (features & W_ALARM_FEAT_ARM_AWAY)) ||
                    (i == 2 && (features & W_ALARM_FEAT_ARM_HOME)) ||
                    (i == 3 && (features & W_ALARM_FEAT_ARM_NIGHT)) ||
                    (i == 4 && (features & W_ALARM_FEAT_ARM_VACATION));
        if (!show) {
            continue;
        }
        lv_obj_t *btn = w_alarm_button(modes, ui_i18n_get(s_alarm_modes[i].label_key, s_alarm_modes[i].label_fallback),
            i == 0 ? W_ALARM_COLOR_OK : W_ALARM_COLOR_ARMED, i == 0 ? APP_FONT_TEXT_22 : APP_FONT_TEXT_16);
        /* Disarm spans the row; arm buttons share the next one. */
        lv_obj_set_width(btn, i == 0 ? LV_PCT(100) : LV_PCT(31));
        lv_obj_set_flex_grow(btn, i == 0 ? 0 : 1);
        lv_obj_set_user_data(btn, (void *)(uintptr_t)i);
        lv_obj_add_event_cb(btn, w_alarm_mode_event_cb, LV_EVENT_CLICKED, ctx);
    }

    page->msg_label = lv_label_create(col);
    lv_label_set_text(page->msg_label, "");
    lv_obj_set_width(page->msg_label, LV_PCT(100));
    lv_obj_set_style_text_align(page->msg_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_font(page->msg_label, APP_FONT_TEXT_18, LV_PART_MAIN);
}

static void w_alarm_build_zone_column(w_alarm_ctx_t *ctx, lv_obj_t *col)
{
    w_alarm_page_t *page = &ctx->page;
    w_alarm_heading(col, ui_i18n_get("alarm.zones", "Zones"));

    lv_obj_t *list = lv_obj_create(col);
    lv_obj_remove_style_all(list);
    lv_obj_set_width(list, LV_PCT(100));
    lv_obj_set_flex_grow(list, 1);
    lv_obj_set_layout(list, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, 4, LV_PART_MAIN);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);

    for (size_t i = 0; i < ctx->zone_count; i++) {
        lv_obj_t *row = lv_obj_create(list);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, LV_PCT(100), 44);
        lv_obj_set_layout(row, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t *name = lv_label_create(row);
        lv_label_set_text(name, w_alarm_slot_display_name(&ctx->zones[i]));
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        lv_obj_set_flex_grow(name, 1);
        lv_obj_set_style_text_font(name, APP_FONT_TEXT_20, LV_PART_MAIN);
        lv_obj_set_style_text_color(name, theme_default_color_text_primary(), LV_PART_MAIN);

        lv_obj_t *chip = lv_label_create(row);
        lv_obj_set_style_text_font(chip, APP_FONT_TEXT_18, LV_PART_MAIN);
        lv_obj_set_style_radius(chip, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_pad_hor(chip, 12, LV_PART_MAIN);
        lv_obj_set_style_pad_ver(chip, 4, LV_PART_MAIN);
        page->zone_chips[i] = chip;
    }

    if (ctx->action_count == 0) {
        return;
    }
    w_alarm_heading(col, ui_i18n_get("alarm.controls", "Controls"));
    lv_obj_t *actions = lv_obj_create(col);
    lv_obj_remove_style_all(actions);
    lv_obj_set_size(actions, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_layout(actions, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(actions, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(actions, 8, LV_PART_MAIN);
    lv_obj_clear_flag(actions, LV_OBJ_FLAG_SCROLLABLE);

    for (size_t i = 0; i < ctx->action_count; i++) {
        bool hold = w_alarm_action_is_hold(&ctx->actions[i]);
        lv_obj_t *btn = lv_button_create(actions);
        lv_obj_set_size(btn, hold ? LV_PCT(31) : LV_PCT(100), hold ? 72 : 52);
        lv_obj_set_flex_grow(btn, hold ? 1 : 0);
        lv_obj_set_style_radius(btn, 12, LV_PART_MAIN);
        lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(btn, 6, LV_PART_MAIN);
        lv_obj_set_style_bg_color(btn, lv_color_hex(hold ? 0x8A1C2B : APP_UI_COLOR_TOPBAR_CHIP_BG), LV_PART_MAIN);
        lv_obj_set_user_data(btn, (void *)(uintptr_t)i);
        lv_obj_add_event_cb(btn, w_alarm_action_event_cb, LV_EVENT_ALL, ctx);

        lv_obj_t *label = lv_label_create(btn);
        lv_obj_set_style_text_font(label, APP_FONT_TEXT_18, LV_PART_MAIN);
        lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_clear_flag(label, LV_OBJ_FLAG_CLICKABLE);
        if (hold) {
            char text[80];
            snprintf(text, sizeof(text), "%s\n%s", w_alarm_slot_display_name(&ctx->actions[i]),
                ui_i18n_get("alarm.hold", "hold 2 s"));
            lv_label_set_text(label, text);
            lv_obj_align(label, LV_ALIGN_TOP_MID, 0, 0);
            lv_obj_t *bar = lv_bar_create(btn);
            lv_obj_set_size(bar, LV_PCT(100), 6);
            lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, 0);
            lv_bar_set_range(bar, 0, 100);
            lv_bar_set_value(bar, 0, LV_ANIM_OFF);
            lv_obj_set_style_bg_color(bar, lv_color_hex(0x000000), LV_PART_MAIN);
            lv_obj_set_style_bg_opa(bar, LV_OPA_30, LV_PART_MAIN);
            lv_obj_set_style_bg_color(bar, lv_color_hex(0xFFFFFF), LV_PART_INDICATOR);
            lv_obj_clear_flag(bar, LV_OBJ_FLAG_CLICKABLE);
            page->action_bars[i] = bar;
        } else {
            lv_obj_center(label);
        }
        page->action_btns[i] = btn;
        page->action_labels[i] = label;
    }
}

static void w_alarm_build_log_column(w_alarm_ctx_t *ctx, lv_obj_t *col)
{
    w_alarm_page_t *page = &ctx->page;

    lv_obj_t *header = lv_obj_create(col);
    lv_obj_remove_style_all(header);
    lv_obj_set_size(header, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_layout(header, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    char heading[48];
    snprintf(heading, sizeof(heading), "%s (%dh)", ui_i18n_get("alarm.history", "History"), ctx->log_hours);
    w_alarm_heading(header, heading);

    lv_obj_t *refresh = w_alarm_button(header, ui_i18n_get("alarm.refresh", "Refresh"), APP_UI_COLOR_TOPBAR_CHIP_BG,
        APP_FONT_TEXT_18);
    lv_obj_set_size(refresh, 120, 48);
    lv_obj_add_event_cb(refresh, w_alarm_refresh_log_event_cb, LV_EVENT_CLICKED, ctx);

    page->log_status = lv_label_create(col);
    lv_label_set_text(page->log_status, ui_i18n_get("alarm.log.loading", "Loading..."));
    lv_obj_set_style_text_font(page->log_status, APP_FONT_TEXT_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(page->log_status, theme_default_color_text_muted(), LV_PART_MAIN);

    page->log_list = lv_obj_create(col);
    lv_obj_remove_style_all(page->log_list);
    lv_obj_set_width(page->log_list, LV_PCT(100));
    lv_obj_set_flex_grow(page->log_list, 1);
    lv_obj_set_layout(page->log_list, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(page->log_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(page->log_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(page->log_list, LV_SCROLLBAR_MODE_AUTO);
}

/* ----- Lifecycle -------------------------------------------------------- */
static void w_alarm_card_event_cb(lv_event_t *event)
{
    w_alarm_ctx_t *ctx = (w_alarm_ctx_t *)lv_event_get_user_data(event);
    if (ctx == NULL || lv_event_get_code(event) != LV_EVENT_DELETE) {
        return;
    }
    ha_client_cancel_pending_responses_for_user(ctx);
    if (ctx->page.tick != NULL) {
        lv_timer_del(ctx->page.tick);
    }
    memset(&ctx->page, 0, sizeof(ctx->page));
    if (ctx->staging_mutex != NULL) {
        xSemaphoreTake(ctx->staging_mutex, portMAX_DELAY);
        ctx->destroyed = true;
        free(ctx->staged_log);
        ctx->staged_log = NULL;
        xSemaphoreGive(ctx->staging_mutex);
        vSemaphoreDelete(ctx->staging_mutex);
        ctx->staging_mutex = NULL;
    }
    free(ctx);
}

static size_t w_alarm_parse_slots(const cJSON *json, const char *key, w_alarm_slot_t *slots, size_t max_slots)
{
    const cJSON *list = (json != NULL) ? cJSON_GetObjectItemCaseSensitive(json, key) : NULL;
    if (!cJSON_IsArray(list)) {
        return 0;
    }
    size_t count = 0;
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, list) {
        if (count >= max_slots) {
            break;
        }
        const cJSON *entity = cJSON_GetObjectItemCaseSensitive(item, "entity_id");
        const cJSON *name = cJSON_GetObjectItemCaseSensitive(item, "name");
        if (!cJSON_IsString(entity) || entity->valuestring == NULL || strchr(entity->valuestring, '.') == NULL) {
            continue;
        }
        w_alarm_slot_t *slot = &slots[count++];
        snprintf(slot->entity_id, sizeof(slot->entity_id), "%s", entity->valuestring);
        if (cJSON_IsString(name) && name->valuestring != NULL) {
            snprintf(slot->name, sizeof(slot->name), "%s", name->valuestring);
        }
    }
    return count;
}

esp_err_t w_alarm_panel_create(const ui_widget_def_t *def, lv_obj_t *parent, ui_widget_instance_t *out_instance)
{
    if (def == NULL || parent == NULL || out_instance == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    w_alarm_ctx_t *ctx = ui_calloc_prefer_psram(1, sizeof(w_alarm_ctx_t));
    if (ctx == NULL) {
        return ESP_ERR_NO_MEM;
    }
    ctx->staging_mutex = xSemaphoreCreateMutex();
    if (ctx->staging_mutex == NULL) {
        free(ctx);
        return ESP_ERR_NO_MEM;
    }
    snprintf(ctx->title, sizeof(ctx->title), "%s", def->title[0] ? def->title : def->id);
    snprintf(ctx->alarm.entity_id, sizeof(ctx->alarm.entity_id), "%s", def->entity_id);
    snprintf(ctx->keypad_entity_id, sizeof(ctx->keypad_entity_id), "%s", def->secondary_entity_id);
    ctx->zone_count = w_alarm_parse_slots(def->json, "alarm_zones", ctx->zones, W_ALARM_MAX_ZONES);
    ctx->action_count = w_alarm_parse_slots(def->json, "alarm_actions", ctx->actions, W_ALARM_MAX_ACTIONS);
    const cJSON *hours = (def->json != NULL) ? cJSON_GetObjectItemCaseSensitive(def->json, "alarm_log_hours") : NULL;
    ctx->log_hours = (cJSON_IsNumber(hours) && hours->valueint >= 1 && hours->valueint <= 168) ? hours->valueint
                                                                                              : W_ALARM_DEFAULT_LOG_HOURS;
    ctx->held_action = -1;

    /* Transparent container; each column is its own card. */
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_set_pos(card, def->x, def->y);
    lv_obj_set_size(card, def->w, def->h);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_column(card, 12, LV_PART_MAIN);
    lv_obj_set_layout(card, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);

    ctx->card = card;
    ctx->page.root = card;
    lv_obj_add_event_cb(card, w_alarm_card_event_cb, LV_EVENT_DELETE, ctx);

    w_alarm_build_keypad_column(ctx, w_alarm_column(card, 4));
    w_alarm_build_zone_column(ctx, w_alarm_column(card, 4));
    w_alarm_build_log_column(ctx, w_alarm_column(card, 5));
    ctx->page.tick = lv_timer_create(w_alarm_page_tick_cb, W_ALARM_TICK_MS, ctx);
    /* First history fetch waits for the HA initial sync (see tick). */
    ctx->next_fetch_ms = w_alarm_now_ms() + 3000;

    w_alarm_render_page_state(ctx);
    w_alarm_render_log(ctx);
    out_instance->obj = card;
    out_instance->ctx = ctx;
    return ESP_OK;
}

static void w_alarm_refresh_extra_slots(w_alarm_ctx_t *ctx);

void w_alarm_panel_apply_state(ui_widget_instance_t *instance, const ha_state_t *state)
{
    W_ALARM_MARK("alarm:panel_apply_state");
    if (instance == NULL || instance->ctx == NULL || state == NULL) {
        return;
    }
    w_alarm_ctx_t *ctx = (w_alarm_ctx_t *)instance->ctx;

    if (ctx->keypad_entity_id[0] != '\0' && strcmp(state->entity_id, ctx->keypad_entity_id) == 0) {
        snprintf(ctx->keypad_text, sizeof(ctx->keypad_text), "%s",
            strcmp(state->state, "unavailable") == 0 ? "" : state->state);
        w_alarm_render_summary(ctx);
        return;
    }

    int zone_idx = -1;
    int action_idx = -1;
    w_alarm_slot_t *slot = w_alarm_find_slot(ctx, state->entity_id, &zone_idx, &action_idx);
    if (slot == NULL) {
        return;
    }

    cJSON *attrs = (state->attributes_json[0] != '\0') ? cJSON_Parse(state->attributes_json) : NULL;
    if (attrs != NULL) {
        w_alarm_set_slot_name_from_attrs(slot, attrs);
        if (slot == &ctx->alarm) {
            cJSON *code_format = cJSON_GetObjectItemCaseSensitive(attrs, "code_format");
            snprintf(ctx->code_format, sizeof(ctx->code_format), "%s",
                cJSON_IsString(code_format) && code_format->valuestring != NULL ? code_format->valuestring : "");
            cJSON *arm_required = cJSON_GetObjectItemCaseSensitive(attrs, "code_arm_required");
            ctx->code_arm_required = cJSON_IsBool(arm_required) ? cJSON_IsTrue(arm_required) : true;
            cJSON *features = cJSON_GetObjectItemCaseSensitive(attrs, "supported_features");
            if (cJSON_IsNumber(features)) {
                ctx->supported_features = (uint32_t)features->valueint;
            }
        }
        cJSON_Delete(attrs);
    }

    bool changed = slot->known && strcmp(slot->state, state->state) != 0;
    snprintf(slot->state, sizeof(slot->state), "%s", state->state);
    slot->known = true;
    if (changed && ctx->log_loaded) {
        w_alarm_log_push_front(ctx, slot, state->state);
        w_alarm_render_log(ctx);
    }

    /* Primary refreshes (layout load / reconcile) also pull zone states, since
     * ui_runtime only replays entity_id and secondary_entity_id per widget. */
    if (slot == &ctx->alarm) {
        w_alarm_refresh_extra_slots(ctx);
    }
    w_alarm_render_page_state(ctx);
}

static void w_alarm_refresh_extra_slots(w_alarm_ctx_t *ctx)
{
    W_ALARM_MARK("alarm:refresh_extra_slots");
    static ha_state_t scratch;
    w_alarm_slot_t *groups[] = {ctx->zones, ctx->actions};
    size_t counts[] = {ctx->zone_count, ctx->action_count};
    for (size_t g = 0; g < 2; g++) {
        for (size_t i = 0; i < counts[g]; i++) {
            w_alarm_slot_t *slot = &groups[g][i];
            if (!ha_model_get_state(slot->entity_id, &scratch)) {
                continue;
            }
            if (!slot->known || slot->device_class[0] == '\0' || slot->name[0] == '\0') {
                cJSON *attrs = (scratch.attributes_json[0] != '\0') ? cJSON_Parse(scratch.attributes_json) : NULL;
                if (attrs != NULL) {
                    w_alarm_set_slot_name_from_attrs(slot, attrs);
                    cJSON_Delete(attrs);
                }
            }
            snprintf(slot->state, sizeof(slot->state), "%s", scratch.state);
            slot->known = true;
        }
    }
}

void w_alarm_panel_mark_unavailable(ui_widget_instance_t *instance)
{
    if (instance == NULL || instance->ctx == NULL) {
        return;
    }
    w_alarm_ctx_t *ctx = (w_alarm_ctx_t *)instance->ctx;
    snprintf(ctx->alarm.state, sizeof(ctx->alarm.state), "unavailable");
    ctx->alarm.known = true;
    w_alarm_render_page_state(ctx);
}

bool w_alarm_panel_tracks_entity(const ui_widget_instance_t *instance, const char *entity_id)
{
    if (instance == NULL || instance->ctx == NULL) {
        return false;
    }
    w_alarm_ctx_t *ctx = (w_alarm_ctx_t *)instance->ctx;
    return w_alarm_find_slot(ctx, entity_id, NULL, NULL) != NULL;
}
