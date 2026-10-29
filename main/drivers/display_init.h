/* SPDX-License-Identifier: LicenseRef-FNCL-1.1
 * Copyright (c) 2026 Cpt_Kirk
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t display_init(void);
bool display_is_ready(void);
bool display_lock(uint32_t timeout_ms);
void display_unlock(void);
esp_err_t display_set_brightness_percent(int percent);

/* Auto-dim and user brightness, shared by all panels (drivers/display_dim.c). */
esp_err_t display_dim_init(void);
void display_note_activity(void);
/* Restores the user brightness and restarts the dim timer. Returns true when the
 * screen was dimmed, so the touch driver can swallow the waking press. */
bool display_wake_from_dim(void);
int display_get_user_brightness(void);
/* Changes the user brightness by delta percent, persists it and returns the new level. */
int display_adjust_user_brightness(int delta);
