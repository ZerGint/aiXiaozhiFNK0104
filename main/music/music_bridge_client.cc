#include "music_bridge_client.h"

#include <cJSON.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_heap_caps.h>
#include <esp_log.h>

#include <cstdio>
#include <unistd.h>

#include <algorithm>
#include <cctype>

namespace {

constexpr char kTag[] = "MusicBridge";
constexpr size_t kMaxResponseBytes = 8192;
constexpr size_t kDownloadChunkSize = 16 * 1024;
constexpr uint32_t kDownloadTimeoutMs = 15000;

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

std::string UrlEncode(const std::string& value) {
    constexpr char kHex[] = "0123456789ABCDEF";
    std::string encoded;
    encoded.reserve(value.size());
    for (unsigned char ch : value) {
        if (std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~') {
            encoded.push_back(static_cast<char>(ch));
        } else {
            encoded.push_back('%');
            encoded.push_back(kHex[ch >> 4]);
            encoded.push_back(kHex[ch & 0x0F]);
        }
    }
    return encoded;
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

double JsonDouble(const cJSON* root, const char* name) {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(root, name);
    return cJSON_IsNumber(item) && item->valuedouble >= 0 ? item->valuedouble : 0;
}

bool ParseLibraryTrack(const cJSON* item, MusicLibraryTrack& track) {
    if (item == nullptr || !cJSON_IsObject(item)) return false;
    track = {};
    track.id = JsonString(item, "id");
    track.title = JsonString(item, "title");
    track.filename = JsonString(item, "filename");
    track.provider = JsonString(item, "provider");
    track.created_at = JsonString(item, "created_at");
    track.artist = JsonString(item, "artist");
    track.album = JsonString(item, "album");
    track.size = JsonSize(item, "size");
    track.duration = JsonDouble(item, "duration");
    return IsSafeJobId(track.id) && !track.title.empty() && !track.filename.empty();
}

void SetError(std::string& destination, const std::string& value) {
    destination = value.substr(0, 240);
}

void LogDiagnosticMemory(const char* stage) {
    ESP_LOGI(kTag,
             "MUSIC_DIAG MEM stage=%s internal_free=%u internal_largest=%u internal_min=%u psram_free=%u",
             stage,
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
             static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)));
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
    result.duration = JsonDouble(response, "duration");
    result.filename = JsonString(response, "filename");
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

bool MusicBridgeClient::ListLibrary(const std::string& base_url,
                                    std::vector<MusicLibraryTrack>& result,
                                    std::string& error) const {
    result.clear();
    error.clear();
    int status_code = 0;
    std::string body;
    if (!Request(TrimTrailingSlash(base_url) + "/library", HTTP_METHOD_GET, "", status_code, body, error)) {
        return false;
    }
    cJSON* response = cJSON_ParseWithLength(body.data(), body.size());
    if (response == nullptr || !cJSON_IsArray(response)) {
        if (response) cJSON_Delete(response);
        error = "invalid_library_response";
        return false;
    }
    const cJSON* item = nullptr;
    cJSON_ArrayForEach(item, response) {
        MusicLibraryTrack track;
        if (ParseLibraryTrack(item, track)) result.push_back(std::move(track));
        if (result.size() >= 200) break;
    }
    cJSON_Delete(response);
    return true;
}

bool MusicBridgeClient::ListUserLibrary(const std::string& base_url,
                                        std::vector<MusicLibraryTrack>& result,
                                        std::string& error,
                                        const std::string& query,
                                        size_t limit) const {
    result.clear();
    error.clear();
    int status_code = 0;
    std::string body;
    std::string endpoint = TrimTrailingSlash(base_url) + "/user-library?limit=" +
                           std::to_string(std::max<size_t>(1, std::min<size_t>(limit, 20)));
    if (!query.empty()) endpoint += "&query=" + UrlEncode(query);
    if (!Request(endpoint, HTTP_METHOD_GET, "", status_code, body, error)) {
        return false;
    }
    cJSON* response = cJSON_ParseWithLength(body.data(), body.size());
    if (response == nullptr || !cJSON_IsArray(response)) {
        if (response) cJSON_Delete(response);
        error = "invalid_user_library_response";
        return false;
    }
    const cJSON* item = nullptr;
    cJSON_ArrayForEach(item, response) {
        MusicLibraryTrack track;
        if (ParseLibraryTrack(item, track)) result.push_back(std::move(track));
        if (result.size() >= 200) break;
    }
    cJSON_Delete(response);
    return true;
}

bool MusicBridgeClient::GetLibraryTrack(const std::string& base_url,
                                        const std::string& track_id,
                                        MusicLibraryTrack& result,
                                        std::string& error) const {
    result = {};
    error.clear();
    if (!IsSafeJobId(track_id)) {
        error = "invalid_track_id";
        return false;
    }
    int status_code = 0;
    std::string body;
    if (!Request(TrimTrailingSlash(base_url) + "/library/" + track_id, HTTP_METHOD_GET, "", status_code, body, error)) {
        return false;
    }
    cJSON* response = cJSON_ParseWithLength(body.data(), body.size());
    const bool valid = response != nullptr && ParseLibraryTrack(response, result);
    if (response) cJSON_Delete(response);
    if (!valid) error = "invalid_library_track";
    return valid;
}

bool MusicBridgeClient::GetUserLibraryTrack(const std::string& base_url,
                                            const std::string& track_id,
                                            MusicLibraryTrack& result,
                                            std::string& error) const {
    result = {};
    error.clear();
    if (!IsSafeJobId(track_id)) {
        error = "invalid_track_id";
        return false;
    }
    int status_code = 0;
    std::string body;
    if (!Request(TrimTrailingSlash(base_url) + "/user-library/" + track_id, HTTP_METHOD_GET, "", status_code, body,
                 error)) {
        return false;
    }
    cJSON* response = cJSON_ParseWithLength(body.data(), body.size());
    const bool valid = response != nullptr && ParseLibraryTrack(response, result);
    if (response) cJSON_Delete(response);
    if (!valid) error = "invalid_user_library_track";
    return valid;
}

bool MusicBridgeClient::DownloadAudio(const std::string& base_url,
                                      const std::string& job_id,
                                      size_t expected_size,
                                      const std::string& output_path,
                                      size_t& downloaded_size,
                                      std::string& error,
                                      const std::function<void(size_t, size_t)>& progress) const {
    return DownloadAudioEndpoint(TrimTrailingSlash(base_url) + "/jobs/" + job_id + "/audio", job_id,
                                 expected_size, output_path, downloaded_size, error, progress);
}

bool MusicBridgeClient::DownloadLibraryAudio(const std::string& base_url,
                                             const std::string& track_id,
                                             size_t expected_size,
                                             const std::string& output_path,
                                             size_t& downloaded_size,
                                             std::string& error,
                                             const std::function<void(size_t, size_t)>& progress) const {
    return DownloadAudioEndpoint(TrimTrailingSlash(base_url) + "/library/" + track_id + "/audio", track_id,
                                 expected_size, output_path, downloaded_size, error, progress);
}

bool MusicBridgeClient::DownloadUserLibraryAudio(const std::string& base_url,
                                                 const std::string& track_id,
                                                 size_t expected_size,
                                                 const std::string& output_path,
                                                 size_t& downloaded_size,
                                                 std::string& error,
                                                 const std::function<void(size_t, size_t)>& progress) const {
    return DownloadAudioEndpoint(TrimTrailingSlash(base_url) + "/user-library/" + track_id + "/audio", track_id,
                                 expected_size, output_path, downloaded_size, error, progress);
}

bool MusicBridgeClient::DownloadAudioEndpoint(const std::string& url,
                                              const std::string& id,
                                              size_t expected_size,
                                              const std::string& output_path,
                                              size_t& downloaded_size,
                                              std::string& error,
                                              const std::function<void(size_t, size_t)>& progress) const {
    downloaded_size = 0;
    error.clear();
    if (!IsSafeJobId(id)) {
        error = "invalid_job_id";
        return false;
    }

    FILE* output = fopen(output_path.c_str(), "wb");
    if (output == nullptr) {
        ESP_LOGW(kTag, "MUSIC_DIAG FILE_OPEN_FAIL job_id=%s path=%s", id.c_str(), output_path.c_str());
        error = "open_output_failed";
        return false;
    }

    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.method = HTTP_METHOD_GET;
    config.timeout_ms = kDownloadTimeoutMs;
    config.buffer_size = 4096;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.skip_cert_common_name_check = true;
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        ESP_LOGW(kTag, "MUSIC_DIAG HTTP_FAIL job_id=%s reason=http_client_init_failed", id.c_str());
        fclose(output);
        unlink(output_path.c_str());
        error = "http_client_init_failed";
        return false;
    }

    esp_err_t request_error = esp_http_client_open(client, 0);
    if (request_error == ESP_OK) request_error = esp_http_client_fetch_headers(client) < 0 ? ESP_FAIL : ESP_OK;
    const int status_code = esp_http_client_get_status_code(client);
    const int64_t content_length = esp_http_client_get_content_length(client);
    if (request_error == ESP_OK && status_code == 200) {
        LogDiagnosticMemory("after_http_open");
        ESP_LOGI(kTag, "MUSIC_DIAG HTTP_HEADERS job_id=%s content_length=%lld expected_size=%u", id.c_str(),
                 static_cast<long long>(content_length), static_cast<unsigned>(expected_size));
    }
    if (request_error != ESP_OK || status_code != 200) {
        error = request_error != ESP_OK ? esp_err_to_name(request_error)
                                        : "http_status_" + std::to_string(status_code);
        ESP_LOGW(kTag, "MUSIC_DIAG HTTP_FAIL job_id=%s reason=%s", id.c_str(), error.c_str());
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        fclose(output);
        unlink(output_path.c_str());
        return false;
    }
    if (content_length > 0 && expected_size > 0 && static_cast<size_t>(content_length) != expected_size) {
        ESP_LOGW(kTag, "MUSIC_DIAG SIZE_MISMATCH job_id=%s content_length=%lld expected_size=%u", id.c_str(),
                 static_cast<long long>(content_length), static_cast<unsigned>(expected_size));
        error = "content_length_mismatch";
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        fclose(output);
        unlink(output_path.c_str());
        return false;
    }

    void* buffer = heap_caps_malloc(kDownloadChunkSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buffer == nullptr) buffer = heap_caps_malloc(kDownloadChunkSize, MALLOC_CAP_8BIT);
    if (buffer == nullptr) {
        ESP_LOGW(kTag, "MUSIC_DIAG DOWNLOAD_FAIL job_id=%s reason=buffer_alloc", id.c_str());
        error = "download_buffer_alloc_failed";
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        fclose(output);
        unlink(output_path.c_str());
        return false;
    }
    ESP_LOGI(kTag, "MUSIC_DIAG BUFFER_ALLOC size=%u ptr=%p location=%s", static_cast<unsigned>(kDownloadChunkSize),
             buffer, esp_ptr_external_ram(buffer) ? "PSRAM" : "INTERNAL");
    LogDiagnosticMemory("after_download_buffer_alloc");

    bool success = true;
    bool midpoint_logged = false;
    size_t next_progress = 2 * 1024 * 1024;
    for (;;) {
        const int read = esp_http_client_read(client, static_cast<char*>(buffer), kDownloadChunkSize);
        if (read < 0) {
            error = "http_read_failed";
            success = false;
            break;
        }
        if (read == 0) break;
        const size_t written = fwrite(buffer, 1, static_cast<size_t>(read), output);
        downloaded_size += written;
        if (written != static_cast<size_t>(read)) {
            error = "sd_write_failed";
            success = false;
            break;
        }
        if (progress) {
            const size_t total = content_length > 0 ? static_cast<size_t>(content_length) : expected_size;
            progress(downloaded_size, total);
        }
        if (!midpoint_logged && expected_size > 0 && downloaded_size * 2 >= expected_size) {
            midpoint_logged = true;
            LogDiagnosticMemory("mid_download");
        }
        if (downloaded_size >= next_progress) {
            ESP_LOGI(kTag, "MUSIC_DOWNLOAD_PROGRESS id=%s bytes=%u", id.c_str(),
                     static_cast<unsigned>(downloaded_size));
            next_progress += 2 * 1024 * 1024;
        }
    }
    LogDiagnosticMemory("after_download_complete");
    ESP_LOGI(kTag, "MUSIC_DIAG DOWNLOAD_BYTES job_id=%s downloaded_bytes=%u content_length=%lld expected_size=%u",
             id.c_str(), static_cast<unsigned>(downloaded_size), static_cast<long long>(content_length),
             static_cast<unsigned>(expected_size));
    ESP_LOGI(kTag, "MUSIC_DIAG BUFFER_FREE ptr=%p", buffer);
    free(buffer);
    LogDiagnosticMemory("after_download_buffer_free");
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    const bool flushed = fflush(output) == 0;
    const bool closed = fclose(output) == 0;
    const bool size_match = downloaded_size > 0 &&
                            (content_length <= 0 || downloaded_size == static_cast<size_t>(content_length)) &&
                            (expected_size == 0 || downloaded_size == expected_size);
    ESP_LOGI(kTag, "MUSIC_DIAG SIZE_VALIDATION job_id=%s result=%s final_downloaded=%u content_length=%lld expected_size=%u",
             id.c_str(), success && flushed && closed && size_match ? "PASS" : "FAIL",
             static_cast<unsigned>(downloaded_size), static_cast<long long>(content_length),
             static_cast<unsigned>(expected_size));
    LogDiagnosticMemory("after_file_close");
    if (!success || !flushed || !closed || !size_match) {
        if (!success && error == "http_read_failed") {
            ESP_LOGW(kTag, "MUSIC_DIAG HTTP_FAIL job_id=%s reason=%s", id.c_str(), error.c_str());
        }
        if (!size_match) ESP_LOGW(kTag, "MUSIC_DIAG SIZE_MISMATCH job_id=%s", id.c_str());
        if (error.empty()) error = "download_size_mismatch";
        unlink(output_path.c_str());
        return false;
    }
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

bool MusicBridgeClient::Reachable(const std::string& base_url, std::string& error) const {
    error.clear();
    int status_code = 0;
    std::string body;
    if (!Request(TrimTrailingSlash(base_url) + "/health", HTTP_METHOD_GET, "", status_code, body, error)) {
        ESP_LOGW(kTag, "BRIDGE_REACHABILITY_ERROR url=%s error=%s status=%d", base_url.c_str(), error.c_str(),
                 status_code);
        return false;
    }
    cJSON* response = cJSON_ParseWithLength(body.data(), body.size());
    if (response == nullptr || !cJSON_IsObject(response)) {
        if (response) cJSON_Delete(response);
        error = "invalid_health_json";
        return false;
    }
    const cJSON* ok = cJSON_GetObjectItemCaseSensitive(response, "ok");
    const std::string bridge = JsonString(response, "bridge");
    const bool reachable = cJSON_IsTrue(ok) || bridge == "ready" || bridge == "no_provider";
    if (!reachable) error = "bridge_unhealthy";
    cJSON_Delete(response);
    ESP_LOGI(kTag, "BRIDGE_REACHABLE %s url=%s", reachable ? "OK" : "FAIL", base_url.c_str());
    return reachable;
}
