#pragma once

#include "esp_err.h"
#include "bus/message_bus.h"
#include "mimi_config.h"

/* Channel interface structure */
typedef struct channel_interface {
    const char *name;  // Channel identifier like "telegram", "feishu", etc.
    
    // Initialization and start functions
    esp_err_t (*init)(void);
    esp_err_t (*start)(void);
    
    // Message sending function
    esp_err_t (*send_message)(const char *target, const char *text);
    
    // Optional: cleanup when stopping
    esp_err_t (*cleanup)(void);
} channel_interface_t;

/* Interface for outbound dispatcher */

/** Register a channel implementation in the channel registry */
esp_err_t channel_register(const channel_interface_t *channel_impl);

/** Lookup a channel implementation */
const channel_interface_t *channel_lookup(const char *channel_name);

/** Initialize all channels - call this after individual channel inits */
esp_err_t channels_manager_init(void);

/** Start all enabled channels - call this after individual channel starts */
esp_err_t channels_manager_start(void);

/* Helper utility */
static inline esp_err_t channel_send_message(const char *channel_name, const char *target, const char *text) {
    const channel_interface_t *chan = channel_lookup(channel_name);
    if (!chan || !chan->send_message) {
        return ESP_ERR_INVALID_ARG;
    }
    return chan->send_message(target, text);
}