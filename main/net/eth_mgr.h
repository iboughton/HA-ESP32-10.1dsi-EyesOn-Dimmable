/* SPDX-License-Identifier: LicenseRef-FNCL-1.1
 * Copyright (c) 2026 Cpt_Kirk
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

esp_err_t eth_mgr_init(void);
esp_err_t eth_mgr_wait_for_link(uint32_t timeout_ms);
esp_err_t eth_mgr_wait_for_ip(uint32_t timeout_ms);
bool eth_mgr_has_ip(void);
bool eth_mgr_is_active(void);
esp_err_t eth_mgr_get_ip(char *out, size_t out_len);