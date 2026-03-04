#pragma once

#include "esp_err.h"

/**
 * Initialize the Feishu bot.
 */
esp_err_t feishu_bot_init(void);

/**
 * Start the Feishu polling task (long polling on Core 0 or webhook-based).
 */
esp_err_t feishu_bot_start(void);

/**
 * Send a text message to a Feishu chat/group.
 * @param chat_id  Feishu chat ID or group ID
 * @param text     Message text
 */
esp_err_t feishu_send_message(const char *chat_id, const char *text);

/**
 * Save the Feishu bot token to NVS.
 */
esp_err_t feishu_set_token(const char *token);

/**
 * Get the channel interface for Feishu.
 */
const void* feishu_get_channel_interface(void);