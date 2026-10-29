/* SPDX-License-Identifier: LicenseRef-FNCL-1.1
 * Copyright (c) 2026 Cpt_Kirk
 */
#include "net/eth_mgr.h"

#include <stdio.h>

#include "sdkconfig.h"

#if defined(CONFIG_APP_PANEL_VARIANT_10INCH_1280) && CONFIG_APP_PANEL_VARIANT_10INCH_1280

#include "esp_eth.h"
#include "esp_eth_mac_esp.h"
#include "esp_eth_netif_glue.h"
#include "esp_eth_phy_ip101.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include "util/log_tags.h"

#define ETH_GOT_IP_BIT BIT0
#define ETH_LINK_UP_BIT BIT1
#define ETH_PHY_ADDRESS 1
#define ETH_PHY_RESET_GPIO 51

static esp_eth_handle_t s_eth_handle;
static esp_netif_t *s_eth_netif;
static esp_eth_netif_glue_handle_t s_eth_glue;
static EventGroupHandle_t s_eth_event_group;
static esp_event_handler_instance_t s_eth_event_instance;
static esp_event_handler_instance_t s_ip_event_instance;
static bool s_eth_started;

static void eth_mgr_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;

    if (event_base == ETH_EVENT) {
        if (event_id == ETHERNET_EVENT_CONNECTED) {
            ESP_LOGI(TAG_WIFI, "Ethernet link up");
            if (s_eth_event_group != NULL) {
                xEventGroupSetBits(s_eth_event_group, ETH_LINK_UP_BIT);
            }
        } else if (event_id == ETHERNET_EVENT_DISCONNECTED) {
            ESP_LOGW(TAG_WIFI, "Ethernet link down");
            if (s_eth_event_group != NULL) {
                xEventGroupClearBits(s_eth_event_group, ETH_GOT_IP_BIT | ETH_LINK_UP_BIT);
            }
        }
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_ETH_GOT_IP && event_data != NULL) {
        const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG_WIFI, "Ethernet got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        if (s_eth_event_group != NULL) {
            xEventGroupSetBits(s_eth_event_group, ETH_GOT_IP_BIT);
        }
    }
}

esp_err_t eth_mgr_init(void)
{
    if (s_eth_started) {
        return ESP_OK;
    }

    if (s_eth_event_group == NULL) {
        s_eth_event_group = xEventGroupCreate();
        if (s_eth_event_group == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    esp_err_t err = esp_event_handler_instance_register(
        ETH_EVENT, ESP_EVENT_ANY_ID, eth_mgr_event_handler, NULL, &s_eth_event_instance);
    if (err != ESP_OK) {
        ESP_LOGE(TAG_WIFI, "Ethernet ETH_EVENT handler registration failed: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_ETH_GOT_IP, eth_mgr_event_handler, NULL, &s_ip_event_instance);
    if (err != ESP_OK) {
        ESP_LOGE(TAG_WIFI, "Ethernet IP_EVENT handler registration failed: %s", esp_err_to_name(err));
        (void)esp_event_handler_instance_unregister(ETH_EVENT, ESP_EVENT_ANY_ID, s_eth_event_instance);
        s_eth_event_instance = NULL;
        return err;
    }

    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_esp32_emac_config_t emac_config = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr = ETH_PHY_ADDRESS;
    phy_config.reset_gpio_num = ETH_PHY_RESET_GPIO;

    ESP_LOGI(TAG_WIFI, "Ethernet: creating P4 EMAC and IP101 PHY");
    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&emac_config, &mac_config);
    esp_eth_phy_t *phy = esp_eth_phy_new_ip101(&phy_config);
    if (mac == NULL || phy == NULL) {
        if (mac != NULL) {
            (void)mac->del(mac);
        }
        if (phy != NULL) {
            (void)phy->del(phy);
        }
        err = ESP_ERR_NO_MEM;
        goto fail_handlers;
    }

    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
    ESP_LOGI(TAG_WIFI, "Ethernet: installing driver");
    err = esp_eth_driver_install(&eth_config, &s_eth_handle);
    if (err != ESP_OK) {
        (void)mac->del(mac);
        (void)phy->del(phy);
        goto fail_handlers;
    }

    esp_netif_config_t netif_config = ESP_NETIF_DEFAULT_ETH();
    ESP_LOGI(TAG_WIFI, "Ethernet: creating network interface");
    s_eth_netif = esp_netif_new(&netif_config);
    if (s_eth_netif == NULL) {
        err = ESP_ERR_NO_MEM;
        goto fail_driver;
    }

    ESP_LOGI(TAG_WIFI, "Ethernet: attaching netif glue");
    s_eth_glue = esp_eth_new_netif_glue(s_eth_handle);
    if (s_eth_glue == NULL) {
        err = ESP_ERR_NO_MEM;
        goto fail_netif;
    }

    err = esp_netif_attach(s_eth_netif, s_eth_glue);
    if (err != ESP_OK) {
        goto fail_glue;
    }

    xEventGroupClearBits(s_eth_event_group, ETH_GOT_IP_BIT | ETH_LINK_UP_BIT);
    ESP_LOGI(TAG_WIFI, "Ethernet: starting MAC/PHY");
    err = esp_eth_start(s_eth_handle);
    if (err != ESP_OK) {
        goto fail_glue;
    }

    s_eth_started = true;
    ESP_LOGI(TAG_WIFI, "Ethernet initialized (IP101GRI RMII, PHY address %d)", ETH_PHY_ADDRESS);
    return ESP_OK;

fail_glue:
    if (s_eth_glue != NULL) {
        (void)esp_eth_del_netif_glue(s_eth_glue);
        s_eth_glue = NULL;
    }
fail_netif:
    if (s_eth_netif != NULL) {
        esp_netif_destroy(s_eth_netif);
        s_eth_netif = NULL;
    }
fail_driver:
    if (s_eth_handle != NULL) {
        (void)esp_eth_driver_uninstall(s_eth_handle);
        s_eth_handle = NULL;
    }
fail_handlers:
    if (s_ip_event_instance != NULL) {
        (void)esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_ETH_GOT_IP, s_ip_event_instance);
        s_ip_event_instance = NULL;
    }
    if (s_eth_event_instance != NULL) {
        (void)esp_event_handler_instance_unregister(ETH_EVENT, ESP_EVENT_ANY_ID, s_eth_event_instance);
        s_eth_event_instance = NULL;
    }
    ESP_LOGW(TAG_WIFI, "Ethernet initialization failed: %s", esp_err_to_name(err));
    return err;
}

esp_err_t eth_mgr_wait_for_link(uint32_t timeout_ms)
{
    if (!s_eth_started || s_eth_event_group == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    EventBits_t bits = xEventGroupWaitBits(
        s_eth_event_group,
        ETH_LINK_UP_BIT,
        pdFALSE,
        pdTRUE,
        pdMS_TO_TICKS(timeout_ms));
    return (bits & ETH_LINK_UP_BIT) ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t eth_mgr_wait_for_ip(uint32_t timeout_ms)
{
    if (!s_eth_started || s_eth_event_group == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (eth_mgr_has_ip()) {
        return ESP_OK;
    }

    EventBits_t bits = xEventGroupWaitBits(
        s_eth_event_group,
        ETH_GOT_IP_BIT,
        pdFALSE,
        pdTRUE,
        pdMS_TO_TICKS(timeout_ms));
    return (bits & ETH_GOT_IP_BIT) ? ESP_OK : ESP_ERR_TIMEOUT;
}

bool eth_mgr_has_ip(void)
{
    if (!s_eth_started || s_eth_netif == NULL) {
        return false;
    }
    esp_netif_ip_info_t ip_info = {0};
    return esp_netif_get_ip_info(s_eth_netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0;
}

bool eth_mgr_is_active(void)
{
    return s_eth_started;
}

esp_err_t eth_mgr_get_ip(char *out, size_t out_len)
{
    if (out == NULL || out_len == 0 || s_eth_netif == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_netif_ip_info_t ip_info = {0};
    esp_err_t err = esp_netif_get_ip_info(s_eth_netif, &ip_info);
    if (err != ESP_OK) {
        return err;
    }
    if (ip_info.ip.addr == 0) {
        return ESP_ERR_NOT_FOUND;
    }
    if (esp_ip4addr_ntoa(&ip_info.ip, out, out_len) == NULL) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

#else

esp_err_t eth_mgr_init(void) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t eth_mgr_wait_for_link(uint32_t timeout_ms) { (void)timeout_ms; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t eth_mgr_wait_for_ip(uint32_t timeout_ms) { (void)timeout_ms; return ESP_ERR_NOT_SUPPORTED; }
bool eth_mgr_has_ip(void) { return false; }
bool eth_mgr_is_active(void) { return false; }
esp_err_t eth_mgr_get_ip(char *out, size_t out_len) { (void)out; (void)out_len; return ESP_ERR_NOT_SUPPORTED; }

#endif