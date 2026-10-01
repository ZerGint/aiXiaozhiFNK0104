#include "music_bridge_client.h"

#include <cJSON.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_log.h>

#include <algorithm>
#include <cctype>

namespace {

constexpr char kTag[] = "MusicBridge";
constexpr size_t kMaxResponseBytes = 8192;

struct ResponseContext {
    std::string body;
    bool overflow = false;
};

esp_err_t HttpEventHandler(esp_http_client_event_t* event) {
    if (event->event_id != HTTP_EVENT_ON_DATA || event->data == nullptr || event->data_len <= 0) {
        return ESP_OK;
    }

    auto* context = static_cast<ResponseContext*>(event->user_data);
    if (context == nullptr || context->overflow) return ESP_OK;
    if (context->body.size() + static_cast<size_t>(event->data_len) > kMaxResponseBytes) {
        context->overflow = true;
        return ESP_OK;
    }
    context->body.append(static_cast<const char*>(event->data), static_cast<size_t>(event->data_len));
    return ESP_OK;
}

std::string TrimTrailingSlash(std::string value) {
    while (!value.empty() && value.back() == '/') value.pop_back();
    return value;
}

bool IsSafeJobId(const std::string& value) {
    if (value.empty() || value.size() > MusicBridgeClient::kMaxJobIdLength) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char ch) {
        return std::isalnum(ch) || ch == '-' || ch == '_';
    });
}

std::string JsonString(const cJSON* root, const char* name) {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(root, name);
    return cJSON_IsString(item) && item->valuestring != nullptr ? item->valuestring : "";
}

size_t JsonSize(const cJSON* root, const char* name) {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(root, name);
    if (!cJSON_IsNumber(item) || item->valuedouble < 0) return 0;
    return static_cast<size_t>(item->valuedouble);
}

void SetError(std::string& destination, const std::string& value) {
    destination = value.substr(0, 240);
}

} // namespace

bool MusicBridgeClient::Request(const std::string& url,
                                int method,
                                const std::string& post_data,
                                int& status_code,
                                std::string& response_body,
                                std::string& error) const {
    ResponseContext response;
    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.method = static_cast<esp_http_client_method_t>(method);
    config.timeout_ms = kRequestTimeoutMs;
    config.event_handler = HttpEventHandler;
    config.user_data = &response;
    config.buffer_size = 1024;
    config.buffer_size_tx = 2048;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.skip_cert_common_name_check = true;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        error = "http_client_init_failed";
        return false;
    }

    esp_http_client_set_header(client, "Accept", "application/json");
    if (method == HTTP_METHOD_POST) {
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_post_field(client, post_data.c_str(), post_data.size());
    }

    const esp_err_t request_error = esp_http_client_perform(client);
    status_code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (request_error != ESP_OK) {
        error = esp_err_to_name(request_error);
        return false;
    }
    if (response.overflow) {
        error = "response_too_large";
        return false;
    }
    response_body = std::move(response.body);
    if (status_code < 200 || status_code >= 300) {
        error = "http_status_" + std::to_string(status_code);
        return false;
    }
    return true;
}

bool MusicBridgeClient::Submit(const std::string& base_url,
                               const std::string& title,
                               const std::string& style,
                               const std::string& lyrics,
                               const std::string& provider,
                               int duration_seconds,
                               MusicGenerateResult& result) const {
    result = {};
    cJSON* request = cJSON_CreateObject();
    if (request == nullptr) {
        result.error = "out_of_memory";
        return false;
    }
    cJSON_AddStringToObject(request, "title", title.c_str());
    cJSON_AddStringToObject(request, "style", style.c_str());
    cJSON_AddStringToObject(request, "lyrics", lyrics.c_str());
    cJSON_AddStringToObject(request, "provider", provider.c_str());
    if (duration_seconds > 0) {
        cJSON_AddNumberToObject(request, "duration_seconds", duration_seconds);
    }
    char* rendered = cJSON_PrintUnformatted(request);
    cJSON_Delete(request);
    if (rendered == nullptr) {
        result.error = "out_of_memory";
        return false;
    }
    std::string post_data(rendered);
    cJSON_free(rendered);

    int status_code = 0;
    std::string body;
    std::string error;
    const std::string url = TrimTrailingSlash(base_url) + "/generate";
    ESP_LOGI(kTag, "BRIDGE_SUBMIT_START url=%s payload_bytes=%u", url.c_str(),
             static_cast<unsigned>(post_data.size()));
    if (!Request(url, HTTP_METHOD_POST, post_data, status_code, body, error)) {
        SetError(result.error, error);
        ESP_LOGW(kTag, "BRIDGE_SUBMIT_ERROR error=%s status=%d", result.error.c_str(), status_code);
        return false;
    }

    cJSON* response = cJSON_ParseWithLength(body.data(), body.size());
    if (response == nullptr || !cJSON_IsObject(response)) {
        if (response) cJSON_Delete(response);
        result.error = "invalid_json";
        return false;
    }
    const std::string job_id = JsonString(response, "job_id");
    const std::string status = JsonString(response, "status");
    const cJSON* accepted = cJSON_GetObjectItemCaseSensitive(response, "accepted");
    const bool accepted_value = cJSON_IsTrue(accepted);
    if (!accepted_value || !IsSafeJobId(job_id)) {
        result.error = JsonString(response, "error");
        if (result.error.empty()) result.error = "invalid_generate_response";
        cJSON_Delete(response);
        return false;
    }
    result.success = true;
    result.job_id = job_id;
    result.status = status.empty() ? "queued" : status;
    cJSON_Delete(response);
    ESP_LOGI(kTag, "BRIDGE_SUBMIT_OK job_id=%s status=%s", result.job_id.c_str(), result.status.c_str());
    return true;
}

bool MusicBridgeClient::GetJob(const std::string& base_url,
                               const std::string& job_id,
                               MusicJobStatus& result) const {
    result = {};
    if (!IsSafeJobId(job_id)) {
        result.error = "invalid_job_id";
        return false;
    }

    int status_code = 0;
    std::string body;
    std::string error;
    const std::string url = TrimTrailingSlash(base_url) + "/jobs/" + job_id;
    if (!Request(url, HTTP_METHOD_GET, "", status_code, body, error)) {
        SetError(result.error, error);
        return false;
    }

    cJSON* response = cJSON_ParseWithLength(body.data(), body.size());
    if (response == nullptr || !cJSON_IsObject(response)) {
        if (response) cJSON_Delete(response);
        result.error = "invalid_json";
        return false;
    }
    result.job_id = JsonString(response, "job_id");
    result.status = JsonString(response, "status");
    result.title = JsonString(response, "title");
    result.progress = JsonString(response, "progress");
    result.audio_url = JsonString(response, "audio_url");
    result.format = JsonString(response, "format");
    result.size = JsonSize(response, "size");
    result.error = JsonString(response, "error");
    if (!IsSafeJobId(result.job_id) || result.status.empty()) {
        result = {};
        result.error = "invalid_status_response";
        cJSON_Delete(response);
        return false;
    }
    result.success = true;
    cJSON_Delete(response);
    return true;
}

bool MusicBridgeClient::Health(const std::string& base_url, std::string& error) const {
    error.clear();
    int status_code = 0;
    std::string body;
    if (!Request(TrimTrailingSlash(base_url) + "/health", HTTP_METHOD_GET, "", status_code, body, error)) {
        ESP_LOGW(kTag, "BRIDGE_HEALTH_ERROR url=%s error=%s status=%d", base_url.c_str(), error.c_str(), status_code);
        return false;
    }
    cJSON* response = cJSON_ParseWithLength(body.data(), body.size());
    if (response == nullptr || !cJSON_IsObject(response)) {
        if (response) cJSON_Delete(response);
        error = "invalid_health_json";
        return false;
    }
    const cJSON* ok = cJSON_GetObjectItemCaseSensitive(response, "ok");
    const std::string wan_gp = JsonString(response, "wan_gp");
    const bool valid = cJSON_IsTrue(ok) && wan_gp == "connected";
    if (!valid) error = wan_gp.empty() ? "bridge_unhealthy" : "wangp_unavailable";
    cJSON_Delete(response);
    ESP_LOGI(kTag, "BRIDGE_HEALTH %s url=%s", valid ? "OK" : "FAIL", base_url.c_str());
    return valid;
}
