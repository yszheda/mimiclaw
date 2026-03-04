#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

/* Channel identifiers */
#define MIMI_CHAN_TELEGRAM   "telegram"
#define MIMI_CHAN_WEBSOCKET  "websocket"
#define MIMI_CHAN_CLI        "cli"
#define MIMI_CHAN_SYSTEM     "system"

#define MIMI_CHAN_TELEGRAM   "telegram"
#define MIMI_CHAN_WEBSOCKET  "websocket"
#define MIMI_CHAN_CLI        "cli"
#define MIMI_CHAN_SYSTEM     "system"

static esp_err_t telegram_chan_init(void)
{
    return telegram_bot_init();
}

static esp_err_t telegram_chan_start(void)
{
    return telegram_bot_start();
}

static esp_err_t telegram_chan_send_message(const char *target, const char *text)
{
    return telegram_send_message(target, text);
}

static esp_err_t telegram_chan_cleanup(void)
{
    return ESP_OK; 
}

static channel_interface_t telegram_channel_impl = {
    .name = "telegram",
    .init = telegram_chan_init,
    .start = telegram_chan_start,
    .send_message = telegram_chan_send_message,
    .cleanup = telegram_chan_cleanup,
};

// Export the static variable to be available externally
extern const channel_interface_t telegram_channel_impl;

// Additionally add this to allow registration elsewhere if needed
const void* telegram_get_channel_interface(void) {
    return &telegram_channel_impl;
}

/* Message types on the bus */
/* Message types on the bus */
typedef struct {
    char channel[16];       /* "telegram", "websocket", "cli" */
    char chat_id[32];       /* Telegram chat_id or WS client id */
    char *content;          /* Heap-allocated message text (caller must free) */
} mimi_msg_t;

/**
 * Initialize the message bus (inbound + outbound FreeRTOS queues).
 */
esp_err_t message_bus_init(void);

/**
 * Push a message to the inbound queue (towards Agent Loop).
 * The bus takes ownership of msg->content.
 */
esp_err_t message_bus_push_inbound(const mimi_msg_t *msg);

/**
 * Pop a message from the inbound queue (blocking).
 * Caller must free msg->content when done.
 */
esp_err_t message_bus_pop_inbound(mimi_msg_t *msg, uint32_t timeout_ms);

/**
 * Push a message to the outbound queue (towards channels).
 * The bus takes ownership of msg->content.
 */
esp_err_t message_bus_push_outbound(const mimi_msg_t *msg);

/**
 * Pop a message from the outbound queue (blocking).
 * Caller must free msg->content when done.
 */
esp_err_t message_bus_pop_outbound(mimi_msg_t *msg, uint32_t timeout_ms);
/* Channel interface structure */
/* Already defined in channel_manager.h */

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