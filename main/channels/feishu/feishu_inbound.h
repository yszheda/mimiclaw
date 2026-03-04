#pragma once

#include "esp_err.h"

#define MIMI_CHAN_FEISHU "feishu"
#define MIMI_FEISHU_APP_ID_MAX_LEN 128
#define MIMI_FEISHU_APP_SECRET_MAX_LEN 128

/**
 * Initialize the Feishu inbound message processor.
 */
esp_err_t feishu_inbound_init(void);

/**
 * Start handling incoming Feishu messages.
 */
esp_err_t feishu_inbound_start(void);