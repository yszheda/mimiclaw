#include "feishu_inbound.h"
#include "mimi_config.h"
#include "bus/message_bus.h"
#include "proxy/http_proxy.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "nvs.h"
#include "cjson/cJSON.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "feishu_inbound";

static char s_app_id[MIMI_FEISHU_APP_ID_MAX_LEN] = MIMI_SECRET_FEISHU_APP_ID;
static char s_app_secret[MIMI_FEISHU_APP_SECRET_MAX_LEN] = MIMI_SECRET_FEISHU_APP_SECRET;

#define FEISHU_API_BASE_URL "https://open.feishu.cn/open-apis"
#define FEISHU_ACCESS_TOKEN_URL "/auth/v3/tenant_access_token/internal"

static char s_tenant_access_token[512] = "";
static int64_t s_token_expire_time = 0;  // Unix timestamp when token expires

static const char *FEISHU_NVS_KEY_APP_ID = "feishu_app_id";
static const char *FEISHU_NVS_KEY_APP_SECRET = "feishu_app_secret";

/* This is a simplified implementation to handle incoming Feishu messages
 * via a long-polling approach - in reality, Feishu uses webhook model
 * But implementing webhook in embedded environment is more challenging
 * so we might implement a pseudo-webhook by exposing an endpoint or polling events
 */

static esp_err_t refresh_tenant_access_token(void)
{
    if (s_app_id[0] == '\0' || s_app_secret[0] == '\0') {
        ESP_LOGE(TAG, "Missing App ID or App Secret, cannot refresh access token");
        return ESP_ERR_INVALID_STATE;
    }

    int64_t current_time = esp_timer_get_time() / 1000000LL;  // Convert to seconds
    if (s_token_expire_time > current_time + 60) {  // Token still valid for at least 1 minute
        return ESP_OK;
    }

    cJSON *token_request = cJSON_CreateObject();
    cJSON_AddStringToObject(token_request, "app_id", s_app_id);
    cJSON_AddStringToObject(token_request, "app_secret", s_app_secret);

    char *request_json = cJSON_PrintUnformatted(token_request);
    cJSON_Delete(token_request);

    // Use http_client to call the token endpoint
    char url[512];
    snprintf(url, sizeof(url), "%s%s", FEISHU_API_BASE_URL, FEISHU_ACCESS_TOKEN_URL);

    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = 10000,
        .buffer_size = 2048,
        .buffer_size_tx = 2048,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        free(request_json);
        return ESP_FAIL;
    }
    
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, request_json, strlen(request_json));

    esp_err_t err = esp_http_client_perform(client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Token request failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        free(request_json);
        return err;
    }

    int status_code = esp_http_client_get_status_code(client);
    if (status_code != 200) {
        ESP_LOGE(TAG, "Token request returned status: %d", status_code);
        esp_http_client_cleanup(client);
        free(request_json);
        return ESP_FAIL;
    }

    char *response = malloc(4096);
    if (!response) {
        esp_http_client_cleanup(client);
        free(request_json);
        return ESP_ERR_NO_MEM;
    }

    int content_length = esp_http_client_get_content_length(client);
    if (content_length > 4095) {
        ESP_LOGE(TAG, "Response too large: %d", content_length);
        esp_http_client_cleanup(client);
        free(request_json);
        free(response);
        return ESP_FAIL;
    }

    int read_len = esp_http_client_read(client, response, content_length);
    response[read_len] = '\0';

    esp_http_client_cleanup(client);
    free(request_json);

    cJSON *root = cJSON_Parse(response);
    free(response);

    if (!root) {
        ESP_LOGE(TAG, "Failed to parse access token response");
        return ESP_FAIL;
    }

    cJSON *code = cJSON_GetObjectItem(root, "code");
    if (!cJSON_IsNumber(code) || cJSON_GetNumberValue(code) != 0) {
        ESP_LOGE(TAG, "Failed to get access token, code: %d", (int)cJSON_GetNumberValue(code));
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    cJSON *token_obj = cJSON_GetObjectItem(root, "tenant_access_token");
    if (!cJSON_IsString(token_obj)) {
        ESP_LOGE(TAG, "Access token not found in response");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    strncpy(s_tenant_access_token, cJSON_GetStringValue(token_obj), sizeof(s_tenant_access_token) - 1);
    s_tenant_access_token[sizeof(s_tenant_access_token) - 1] = '\0';

    cJSON *expire_obj = cJSON_GetObjectItem(root, "expire");
    if (cJSON_IsNumber(expire_obj)) {
        int expire_seconds = (int)cJSON_GetNumberValue(expire_obj);
        s_token_expire_time = current_time + expire_seconds - 60;  // Refresh 1 minute early
    } else {
        s_token_expire_time = current_time + 7200;  // Default to 2 hours - 60s = 7140s
    }

    ESP_LOGI(TAG, "Access token refreshed, expires in: %" PRId64 " seconds", 
             s_token_expire_time - current_time);

    cJSON_Delete(root);
    return ESP_OK;
}

static bool process_feishu_event(const char *event_json) 
{
    if (!event_json) return false;
    
    cJSON *root = cJSON_Parse(event_json);
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse Feishu event");
        return false;
    }

    // This processes a message received event according to Feishu's event structure
    // In reality for a webhook, we'd receive this as HTTP POST data, 
    // but we'll simplify and make an input that polls for received messages
    
    // Looking for the actual message content in Feishu's JSON
    // For now, this is a conceptual approach to simulate message processing
    cJSON *header = cJSON_GetObjectItem(root, "header");
    cJSON *event = cJSON_GetObjectItem(root, "event");
    
    if (header && event) {
        cJSON *event_type = cJSON_GetObjectItem(header, "event_type");
        if (event_type && cJSON_IsString(event_type) && 
            strcmp(cJSON_GetStringValue(event_type), "im.message.receive_v1") == 0) {
            
            // Process message event
            cJSON *msg_content = cJSON_GetObjectItem(event, "message");
            if (msg_content) {
                cJSON *text_content = cJSON_GetObjectItem(cJSON_GetObjectItem(msg_content, "body"), "text");
                cJSON *chat_id_obj = cJSON_GetObjectItem(cJSON_GetObjectItem(msg_content, "chat_id"));
                
                if (text_content && cJSON_IsString(text_content)) {
                    const char *text = cJSON_GetStringValue(text_content);
                    const char *chat_id = "feishu_default";  // We'd extract chat_id properly in real impl
                    if (chat_id_obj) {
                        chat_id = cJSON_GetStringValue(chat_id_obj);
                    }
                    
                    // Create a message to push to the bus
                    mimi_msg_t msg = {0};
                    strncpy(msg.channel, MIMI_CHAN_FEISHU, sizeof(msg.channel) - 1);
                    strncpy(msg.chat_id, chat_id, sizeof(msg.chat_id) - 1);
                    msg.content = strdup(text);
                    if (msg.content) {
                        if (message_bus_push_inbound(&msg) != ESP_OK) {
                            ESP_LOGW(TAG, "Inbound queue full, drop feishu message");
                            free(msg.content);
                        } else {
                            ESP_LOGI(TAG, "Received Feishu message from %s: %s", chat_id, text);
                        }
                    }
                    
                    cJSON_Delete(root);
                    return true;
                }
            }
        }
    }

    cJSON_Delete(root);
    return false;
}

/*
 * The actual Feishu integration would use webhooks - this is a simulation
 * In a real implementation we would need to set up a web server to receive
 * webhook notifications, or use long-polling approach which is more
 * suitable for embedded systems
 */
esp_err_t feishu_inbound_start(void)
{
    ESP_LOGI(TAG, "Starting Feishu inbound processor");
    // Since Feishu typically uses webhooks, a minimal embedded implementation
    // may need to simulate webhook processing or implement long polling
    
    // For now, we'll do minimal setup as full integration would require 
    // a web server to be running
    return ESP_OK;
}

esp_err_t feishu_inbound_init(void)
{
    ESP_LOGI(TAG, "Feishu inbound initializing...");
    
    // Load stored keys if available
    nvs_handle_t nvs;
    if (nvs_open(MIMI_NVS_FEISHU, NVS_READONLY, &nvs) == ESP_OK) {
        char tmp[256] = {0};
        size_t len = sizeof(tmp);
        if (nvs_get_str(nvs, FEISHU_NVS_KEY_APP_ID, tmp, &len) == ESP_OK && tmp[0]) {
            strncpy(s_app_id, tmp, sizeof(s_app_id) - 1);
        }

        len = sizeof(tmp);
        if (nvs_get_str(nvs, FEISHU_NVS_KEY_APP_SECRET, tmp, &len) == ESP_OK && tmp[0]) {
            strncpy(s_app_secret, tmp, sizeof(s_app_secret) - 1);
        }

        nvs_close(nvs);
    }

    if (s_app_id[0] || s_app_secret[0]) {
        ESP_LOGI(TAG, "Feishu inbound configured with app details");
    } else {
        ESP_LOGW(TAG, "Missing Feishu credentials. Will be unable to authenticate API calls.");
    }
    
    return ESP_OK;
}