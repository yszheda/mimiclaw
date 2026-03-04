#include "feishu_bot.h"
#include "mimi_config.h"
#include "bus/message_bus.h"
#include "channels/channel_manager.h"
#include "proxy/http_proxy.h"

#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "nvs.h"
#include "cjson/cJSON.h"

static const char *TAG = "feishu";

static char s_app_id[MIMI_FEISHU_APP_ID_MAX_LEN] = MIMI_SECRET_FEISHU_APP_ID;
static char s_app_secret[MIMI_FEISHU_APP_SECRET_MAX_LEN] = MIMI_SECRET_FEISHU_APP_SECRET;

#define FEISHU_API_BASE_URL "https://open.feishu.cn/open-apis"
#define FEISHU_ACCESS_TOKEN_URL "/auth/v3/tenant_access_token/internal"
#define FEISHU_SEND_MESSAGE_URL "/im/v1/messages"

#define MIMI_FEISHU_BOT_TOKEN_MAX_LEN 256
#define MIMI_FEISHU_APP_ID_MAX_LEN 128
#define MIMI_FEISHU_APP_SECRET_MAX_LEN 128
#define MIMI_FEISHU_CHAT_MAX_LEN 128
#define MIMI_FEISHU_MAX_MSG_LEN 4000  // Feishu message length limit

static char s_tenant_access_token[512] = "";
static int64_t s_token_expire_time = 0;  // Unix timestamp when token expires

static const char *FEISHU_NVS_KEY_APP_ID = "feishu_app_id";
static const char *FEISHU_NVS_KEY_APP_SECRET = "feishu_app_secret";

/* HTTP response accumulator */
typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} http_resp_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    http_resp_t *resp = (http_resp_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        if (resp->len + evt->data_len >= resp->cap) {
            size_t new_cap = resp->cap * 2;
            if (new_cap < resp->len + evt->data_len + 1) {
                new_cap = resp->len + evt->data_len + 1;
            }
            char *tmp = realloc(resp->buf, new_cap);
            if (!tmp) return ESP_ERR_NO_MEM;
            resp->buf = tmp;
            resp->cap = new_cap;
        }
        memcpy(resp->buf + resp->len, evt->data, evt->data_len);
        resp->len += evt->data_len;
        resp->buf[resp->len] = '\0';
    }
    return ESP_OK;
}

static char *feishu_api_call_direct(const char *endpoint, const char *post_data, bool requires_auth)
{
    char url[512];
    snprintf(url, sizeof(url), "%s%s", FEISHU_API_BASE_URL, endpoint);

    http_resp_t resp = {
        .buf = calloc(1, 4096),
        .len = 0,
        .cap = 4096,
    };
    if (!resp.buf) return NULL;

    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event_handler,
        .user_data = &resp,
        .timeout_ms = 30000,
        .buffer_size = 2048,
        .buffer_size_tx = 2048,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        free(resp.buf);
        return NULL;
    }

    if (requires_auth && s_tenant_access_token[0] != '\0') {
        char auth_header[600];
        snprintf(auth_header, sizeof(auth_header), "Bearer %s", s_tenant_access_token);
        esp_http_client_set_header(client, "Authorization", auth_header);
    }
    esp_http_client_set_header(client, "Content-Type", "application/json");

    if (post_data) {
        esp_http_client_set_method(client, HTTP_METHOD_POST);
        esp_http_client_set_post_field(client, post_data, strlen(post_data));
    }

    esp_err_t err = esp_http_client_perform(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
        free(resp.buf);
        return NULL;
    }

    return resp.buf;
}

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

    char *response = feishu_api_call_direct(FEISHU_ACCESS_TOKEN_URL, request_json, false);

    free(request_json);

    if (!response) {
        ESP_LOGE(TAG, "Failed to get tenant access token");
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(response);
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse access token response");
        free(response);
        return ESP_FAIL;
    }

    cJSON *code = cJSON_GetObjectItem(root, "code");
    if (!cJSON_IsNumber(code) || cJSON_GetNumberValue(code) != 0) {
        ESP_LOGE(TAG, "Failed to get access token, code: %d", (int)cJSON_GetNumberValue(code));
        cJSON_Delete(root);
        free(response);
        return ESP_FAIL;
    }

    cJSON *token_obj = cJSON_GetObjectItem(root, "tenant_access_token");
    if (!cJSON_IsString(token_obj)) {
        ESP_LOGE(TAG, "Access token not found in response");
        cJSON_Delete(root);
        free(response);
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
    free(response);
    return ESP_OK;
}

esp_err_t feishu_send_message(const char *chat_id, const char *text)
{
    if (s_app_id[0] == '\0' || s_app_secret[0] == '\0') {
        ESP_LOGE(TAG, "Missing Feishu credentials for sending message");
        return ESP_ERR_INVALID_STATE;
    }

    if (refresh_tenant_access_token() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to refresh access token");
        return ESP_FAIL;
    }

    // Split long messages if needed
    size_t text_len = strlen(text);
    size_t offset = 0;
    esp_err_t all_ok = ESP_OK;

    while (offset < text_len) {
        size_t chunk = text_len - offset;
        if (chunk > MIMI_FEISHU_MAX_MSG_LEN) {
            chunk = MIMI_FEISHU_MAX_MSG_LEN;
        }

        // Create a temporary piece for this chunk
        char *segment = malloc(chunk + 1);
        if (!segment) {
            return ESP_ERR_NO_MEM;
        }
        memcpy(segment, text + offset, chunk);
        segment[chunk] = '\0';

        // Prepare the message in Feishu format    
        cJSON *msg_root = cJSON_CreateObject();
        cJSON_AddStringToObject(msg_root, "receive_id", chat_id);
        cJSON_AddStringToObject(msg_root, "msg_type", "text");

        cJSON *content = cJSON_CreateObject();
        cJSON_AddStringToObject(content, "text", segment);
        cJSON_AddItemToObject(msg_root, "content", content);

        char *json_str = cJSON_PrintUnformatted(msg_root);
        cJSON_Delete(msg_root);
        free(segment);

        if (!json_str) {
            all_ok = ESP_ERR_NO_MEM;
            offset += chunk;
            continue;
        }

        ESP_LOGI(TAG, "Sending feishu message to %s (%d bytes)", chat_id, (int)chunk);

        char *resp = feishu_api_call_direct(FEISHU_SEND_MESSAGE_URL, json_str, true);
        free(json_str);

        if (resp) {
            cJSON *res_obj = cJSON_Parse(resp);
            if (res_obj) {
                cJSON *code = cJSON_GetObjectItem(res_obj, "code");
                if (cJSON_IsNumber(code) && cJSON_GetNumberValue(code) == 0) {
                    ESP_LOGI(TAG, "Feishu send success to %s (%d bytes)", chat_id, (int)chunk);
                } else {
                    ESP_LOGE(TAG, "Send message failed: code %d", (int)cJSON_GetNumberValue(code));
                    all_ok = ESP_FAIL;
                }
                cJSON_Delete(res_obj);
            } else {
                ESP_LOGE(TAG, "Failed to parse response");
                all_ok = ESP_FAIL;
            }
            free(resp);
        } else {
            ESP_LOGE(TAG, "Send message failed: no response");
            all_ok = ESP_FAIL;
        }

        offset += chunk;
    }

    return all_ok;
}


// Feishu channel implementation for the interface
static esp_err_t feishu_chan_init(void)
{
    // Initialize NVS namespace for our parameters if not exists
    ESP_LOGI(TAG, "Feishu channel initialization");
   
    // Load stored keys if available
    nvs_handle_t nvs;
    if (nvs_open(MIMI_NVS_FEISHU, NVS_READONLY, &nvs) == ESP_OK) {
        char tmp[MIMI_FEISHU_APP_ID_MAX_LEN] = {0};
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
        ESP_LOGI(TAG, "Feishu credentials loaded");
    } else {
        ESP_LOGW(TAG, "Missing Feishu credentials. Use CLI to configure them.");
    }
    return ESP_OK;
}

static esp_err_t feishu_chan_start(void)
{
    ESP_LOGI(TAG, "Feishu channel is ready for message sending");
    return ESP_OK;
}

static esp_err_t feishu_chan_cleanup(void)
{
    // Clear sensitive tokens
    s_app_id[0] = '\0';
    s_app_secret[0] = '\0';
    s_tenant_access_token[0] = '\0';
    s_token_expire_time = 0;
    return ESP_OK;
}

static channel_interface_t feishu_channel_impl = {
    .name = "feishu",
    .init = feishu_chan_init,
    .start = feishu_chan_start,
    .send_message = feishu_send_message,
    .cleanup = feishu_chan_cleanup,
};

esp_err_t feishu_bot_init(void)
{
    return feishu_chan_init();
}

esp_err_t feishu_bot_start(void)
{
    return feishu_chan_start();
}

esp_err_t feishu_set_app_id(const char *app_id)
{
    nvs_handle_t nvs;
    ESP_ERROR_CHECK(nvs_open(MIMI_NVS_FEISHU, NVS_READWRITE, &nvs));
    ESP_ERROR_CHECK(nvs_set_str(nvs, FEISHU_NVS_KEY_APP_ID, app_id));
    ESP_ERROR_CHECK(nvs_commit(nvs));
    nvs_close(nvs);

    strncpy(s_app_id, app_id, sizeof(s_app_id) - 1);
    ESP_LOGI(TAG, "Feishu app ID saved");
    return ESP_OK;
}

esp_err_t feishu_set_app_secret(const char *app_secret)
{
    nvs_handle_t nvs;
    ESP_ERROR_CHECK(nvs_open(MIMI_NVS_FEISHU, NVS_READWRITE, &nvs));
    ESP_ERROR_CHECK(nvs_set_str(nvs, FEISHU_NVS_KEY_APP_SECRET, app_secret));
    ESP_ERROR_CHECK(nvs_commit(nvs));
    nvs_close(nvs);

    strncpy(s_app_secret, app_secret, sizeof(s_app_secret) - 1);
    ESP_LOGI(TAG, "Feishu app secret saved");
    return ESP_OK;
}

const void* feishu_get_channel_interface(void)
{
    return &feishu_channel_impl;
}