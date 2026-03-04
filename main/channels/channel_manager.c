#include "channel_manager.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mimi_config.h"
#include <string.h>

static const char *TAG = "channel_manager";

static channel_interface_t *registered_channels[MIMI_MAX_CHANNELS] = {NULL};
static int num_registered_channels = 0;

esp_err_t channel_register(const channel_interface_t *channel_impl) {
    if (!channel_impl || !channel_impl->name) {
        ESP_LOGE(TAG, "Invalid channel implementation");
        return ESP_ERR_INVALID_ARG;
    }
    
    if (num_registered_channels >= MIMI_MAX_CHANNELS) {
        ESP_LOGE(TAG, "Max channels exceeded: %d", MIMI_MAX_CHANNELS);
        return ESP_FAIL;
    }

    // Check for duplicate channel names
    for (int i = 0; i < num_registered_channels; i++) {
        if (strcmp(registered_channels[i]->name, channel_impl->name) == 0) {
            ESP_LOGW(TAG, "Channel %s already registered", channel_impl->name);
            return ESP_FAIL;
        }
    }

    registered_channels[num_registered_channels++] = (channel_interface_t*)channel_impl;
    ESP_LOGI(TAG, "Channel registered: %s", channel_impl->name);
    return ESP_OK;
}

const channel_interface_t *channel_lookup(const char *channel_name) {
    if (!channel_name) {
        return NULL;
    }
    
    for (int i = 0; i < num_registered_channels; i++) {
        if (strcmp(registered_channels[i]->name, channel_name) == 0) {
            return registered_channels[i];
        }
    }
    
    ESP_LOGW(TAG, "Channel not found: %s", channel_name);
    return NULL;
}

esp_err_t channels_manager_init(void) {
    ESP_LOGI(TAG, "Initializing %d channels...", num_registered_channels);
    
    for (int i = 0; i < num_registered_channels; i++) {
        if (registered_channels[i]->init) {
            esp_err_t ret = registered_channels[i]->init();
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to initialize channel %s: %s", 
                        registered_channels[i]->name, esp_err_to_name(ret));
                return ret;
            }
        }
    }
    return ESP_OK;
}

esp_err_t channels_manager_start(void) {
    ESP_LOGI(TAG, "Starting %d channels...", num_registered_channels);
    
    for (int i = 0; i < num_registered_channels; i++) {
        if (registered_channels[i]->start) {
            esp_err_t ret = registered_channels[i]->start();
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to start channel %s: %s", 
                        registered_channels[i]->name, esp_err_to_name(ret));
                return ret;
            }
        }
    }
    return ESP_OK;
}