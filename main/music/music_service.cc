#include "music_service.h"

#include <cJSON.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <mdns.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>

#include "application.h"
#include "settings.h"

namespace {

constexpr char kTag[] = "MusicService";
constexpr char kNamespace[] = "music";
constexpr char kBridgeUrlKey[] = "bridge_url";
constexpr char kJobsKey[] = "jobs";
constexpr size_t kMaxPersistedJobsBytes = 4096;
constexpr uint32_t kPollTaskStackBytes = 4096;
constexpr char kMdnsServiceType[] = "_fnk-music";
constexpr char kMdnsProtocol[] = "_tcp";

void LogMemory(const char* stage) {
    ESP_LOGI(kTag,
             "MUSIC_MEM stage=%s internal_free=%u internal_largest=%u min=%u psram_free=%u",
             stage,
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
}

std::string Bounded(const std::string& value, size_t limit) {
    return value.substr(0, limit);
}

std::string JsonString(const cJSON* root, const char* name) {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(root, name);
    return cJSON_IsString(item) && item->valuestring != nullptr ? item->valuestring : "";
}

int64_t JsonInt64(const cJSON* root, const char* name) {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(root, name);
    return cJSON_IsNumber(item) ? static_cast<int64_t>(item->valuedouble) : 0;
}

} // namespace

MusicService& MusicService::GetInstance() {
    static MusicService instance;
    return instance;
}

void MusicService::Initialize() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (initialized_) return;
        initialized_ = true;
    }
    LoadState();
    RegisterMcpTools();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (HasPendingJobsLocked()) EnsurePollTask();
    }
    LogMemory("after_initialize");
}

bool MusicService::ValidateText(const std::string& value, size_t max_length) {
    if (value.empty() || value.size() > max_length) return false;
    return std::any_of(value.begin(), value.end(), [](unsigned char ch) { return !std::isspace(ch); });
}

bool MusicService::NormalizeUrl(const std::string& input, std::string& output) {
    output = input;
    while (!output.empty() && std::isspace(static_cast<unsigned char>(output.back()))) output.pop_back();
    size_t first = 0;
    while (first < output.size() && std::isspace(static_cast<unsigned char>(output[first]))) ++first;
    output = output.substr(first);
    if (output.empty() || output.size() > MusicBridgeClient::kMaxUrlLength) return false;
    if (output.rfind("http://", 0) != 0 && output.rfind("https://", 0) != 0) return false;
    if (output.find_first_of("\r\n\t ") != std::string::npos) return false;
    while (output.size() > 1 && output.back() == '/') output.pop_back();
    return true;
}

bool MusicService::SetBridgeUrl(const std::string& url, std::string& error) {
    if (url.empty()) {
        std::lock_guard<std::mutex> lock(mutex_);
        bridge_url_.clear();
        runtime_bridge_url_.clear();
        Settings settings(kNamespace, true);
        settings.SetString(kBridgeUrlKey, "");
        ESP_LOGI(kTag, "MUSIC_BRIDGE_URL_CLEARED automatic_discovery=1");
        return true;
    }
    std::string normalized;
    if (!NormalizeUrl(url, normalized)) {
        error = "invalid_bridge_url";
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        bridge_url_ = normalized;
        runtime_bridge_url_.clear();
        Settings settings(kNamespace, true);
        settings.SetString(kBridgeUrlKey, bridge_url_);
    }
    ESP_LOGI(kTag, "MUSIC_BRIDGE_URL_SAVED length=%u", static_cast<unsigned>(normalized.size()));
    return true;
}

bool MusicService::EnsureMdns() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (mdns_initialized_) return true;
    LogMemory("before_mdns");
    const esp_err_t error = mdns_init();
    if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(kTag, "MUSIC_MDNS_INIT_FAILED error=%s", esp_err_to_name(error));
        return false;
    }
    mdns_initialized_ = true;
    LogMemory("after_mdns");
    ESP_LOGI(kTag, "MUSIC_MDNS_READY service=%s proto=%s", kMdnsServiceType, kMdnsProtocol);
    return true;
}

void MusicService::InvalidateRuntimeUrl(const std::string& url) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (bridge_url_.empty() && runtime_bridge_url_ == url) {
        runtime_bridge_url_.clear();
        ESP_LOGW(kTag, "MUSIC_BRIDGE_RUNTIME_INVALIDATED");
    }
}

std::string MusicService::ResolveBridgeUrl() {
    std::string manual;
    std::string cached;
    int64_t last_attempt = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        manual = bridge_url_;
        cached = runtime_bridge_url_;
        last_attempt = last_discovery_attempt_ms_;
    }

    MusicBridgeClient client;
    if (!manual.empty()) {
        std::string error;
        if (client.Health(manual, error)) return manual;
        ESP_LOGW(kTag, "MUSIC_BRIDGE_MANUAL_UNAVAILABLE error=%s", error.c_str());
    } else if (!cached.empty()) {
        ESP_LOGI(kTag, "MUSIC_BRIDGE_RUNTIME_CACHE_HIT");
        return cached;
    }

    const int64_t now_ms = esp_timer_get_time() / 1000;
    if (last_attempt != 0 && now_ms - last_attempt < kDiscoveryRetryIntervalMs) {
        ESP_LOGW(kTag, "MUSIC_MDNS_RATE_LIMIT remaining_ms=%lld",
                 static_cast<long long>(kDiscoveryRetryIntervalMs - (now_ms - last_attempt)));
        return {};
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        last_discovery_attempt_ms_ = now_ms;
    }
    if (!EnsureMdns()) return {};

    mdns_result_t* results = nullptr;
    const esp_err_t query_error = mdns_query_ptr(kMdnsServiceType, kMdnsProtocol, 2500, 4, &results);
    if (query_error != ESP_OK) {
        if (results != nullptr) mdns_query_results_free(results);
        ESP_LOGW(kTag, "MUSIC_MDNS_QUERY_FAILED error=%s", esp_err_to_name(query_error));
        return {};
    }

    std::string discovered;
    for (mdns_result_t* result = results; result != nullptr && discovered.empty(); result = result->next) {
        for (mdns_ip_addr_t* address = result->addr; address != nullptr; address = address->next) {
            if (address->addr.type != ESP_IPADDR_TYPE_V4) continue;
            const auto* ip4 = &address->addr.u_addr.ip4;
            char base[64] = {};
            snprintf(base, sizeof(base), "http://" IPSTR ":%u", IP2STR(ip4), result->port);
            std::string candidate(base);
            std::string error;
            ESP_LOGI(kTag, "MUSIC_MDNS_CANDIDATE endpoint=%s", candidate.c_str());
            if (client.Health(candidate, error)) {
                discovered = std::move(candidate);
                break;
            }
            ESP_LOGW(kTag, "MUSIC_MDNS_CANDIDATE_REJECTED error=%s", error.c_str());
        }
    }
    mdns_query_results_free(results);
    if (discovered.empty()) {
        ESP_LOGW(kTag, "MUSIC_MDNS_NOT_FOUND");
        return {};
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (bridge_url_.empty()) runtime_bridge_url_ = discovered;
    }
    ESP_LOGI(kTag, "MUSIC_MDNS_DISCOVERED endpoint=%s", discovered.c_str());
    return discovered;
}

std::string MusicService::GetBridgeUrl() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return bridge_url_;
}

void MusicService::LoadState() {
    Settings settings(kNamespace, false);
    std::string url;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        url = settings.GetString(kBridgeUrlKey, "");
        if (!NormalizeUrl(url, bridge_url_)) bridge_url_.clear();
    }

    const std::string persisted = settings.GetString(kJobsKey, "");
    if (persisted.empty() || persisted.size() > kMaxPersistedJobsBytes) return;
    cJSON* root = cJSON_ParseWithLength(persisted.data(), persisted.size());
    if (!root || !cJSON_IsArray(root)) {
        if (root) cJSON_Delete(root);
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    size_t index = 0;
    cJSON* item = nullptr;
    cJSON_ArrayForEach(item, root) {
        if (index >= jobs_.size() || !cJSON_IsObject(item)) break;
        PendingJob job;
        job.job_id = Bounded(JsonString(item, "job_id"), MusicBridgeClient::kMaxJobIdLength);
        job.title = Bounded(JsonString(item, "title"), kMaxTitleLength);
        job.status = Bounded(JsonString(item, "status"), 32);
        job.progress = Bounded(JsonString(item, "progress"), 160);
        job.audio_url = Bounded(JsonString(item, "audio_url"), MusicBridgeClient::kMaxUrlLength);
        job.format = Bounded(JsonString(item, "format"), 16);
        job.size = static_cast<size_t>(std::max<int64_t>(0, JsonInt64(item, "size")));
        job.error = Bounded(JsonString(item, "error"), 240);
        job.updated_at = JsonInt64(item, "updated_at");
        job.active = !job.job_id.empty() && !job.status.empty();
        if (job.active) jobs_[index++] = std::move(job);
    }
    cJSON_Delete(root);
}

cJSON* MusicService::JobToJson(const PendingJob& job) {
    cJSON* item = cJSON_CreateObject();
    if (!item) return nullptr;
    cJSON_AddStringToObject(item, "job_id", job.job_id.c_str());
    cJSON_AddStringToObject(item, "title", job.title.c_str());
    cJSON_AddStringToObject(item, "status", job.status.c_str());
    cJSON_AddStringToObject(item, "progress", job.progress.c_str());
    cJSON_AddStringToObject(item, "audio_url", job.audio_url.c_str());
    cJSON_AddStringToObject(item, "format", job.format.c_str());
    cJSON_AddNumberToObject(item, "size", static_cast<double>(job.size));
    cJSON_AddStringToObject(item, "error", job.error.c_str());
    cJSON_AddNumberToObject(item, "updated_at", static_cast<double>(job.updated_at));
    return item;
}

void MusicService::PersistLocked() {
    cJSON* root = cJSON_CreateArray();
    if (!root) return;
    for (const auto& job : jobs_) {
        if (!job.active) continue;
        cJSON* item = JobToJson(job);
        if (!item) continue;
        cJSON_AddItemToArray(root, item);
    }
    char* rendered = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!rendered) return;
    std::string value(rendered);
    cJSON_free(rendered);
    if (value.size() <= kMaxPersistedJobsBytes) {
        Settings settings(kNamespace, true);
        settings.SetString(kJobsKey, value);
    } else {
        ESP_LOGW(kTag, "MUSIC_STATE_NOT_PERSISTED bytes=%u", static_cast<unsigned>(value.size()));
    }
}

bool MusicService::IsTerminal(const std::string& status) {
    return status == "ready" || status == "failed";
}

bool MusicService::HasPendingJobsLocked() const {
    for (const auto& job : jobs_) {
        if (job.active && !IsTerminal(job.status)) return true;
    }
    return false;
}

bool MusicService::AddPendingJob(const MusicGenerateResult& generated, const std::string& title) {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t target = jobs_.size();
    int64_t oldest = INT64_MAX;
    for (size_t i = 0; i < jobs_.size(); ++i) {
        if (!jobs_[i].active) { target = i; break; }
        if (IsTerminal(jobs_[i].status) && jobs_[i].updated_at < oldest) {
            oldest = jobs_[i].updated_at;
            target = i;
        }
    }
    if (target == jobs_.size()) {
        target = 0;
        for (size_t i = 1; i < jobs_.size(); ++i) {
            if (jobs_[i].updated_at < jobs_[target].updated_at) target = i;
        }
    }
    PendingJob& job = jobs_[target];
    job = {};
    job.active = true;
    job.job_id = generated.job_id;
    job.title = Bounded(title, kMaxTitleLength);
    job.status = Bounded(generated.status.empty() ? "queued" : generated.status, 32);
    job.updated_at = static_cast<int64_t>(time(nullptr));
    PersistLocked();
    return true;
}

void MusicService::ApplyStatus(const MusicJobStatus& status) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& job : jobs_) {
        if (!job.active || job.job_id != status.job_id) continue;
        job.status = Bounded(status.status, 32);
        if (!status.title.empty()) job.title = Bounded(status.title, kMaxTitleLength);
        job.progress = Bounded(status.progress, 160);
        job.audio_url = Bounded(status.audio_url, MusicBridgeClient::kMaxUrlLength);
        job.format = Bounded(status.format, 16);
        job.size = status.size;
        job.error = Bounded(status.error, 240);
        job.updated_at = static_cast<int64_t>(time(nullptr));
        PersistLocked();
        ESP_LOGI(kTag, "MUSIC_JOB_UPDATE job_id=%s status=%s", job.job_id.c_str(), job.status.c_str());
        return;
    }
}

MusicService::PendingJob MusicService::FindJobLocked(const std::string& job_id) const {
    for (const auto& job : jobs_) {
        if (job.active && job.job_id == job_id) return job;
    }
    return {};
}

MusicService::PendingJob MusicService::FindLatestJobLocked() const {
    PendingJob latest;
    for (const auto& job : jobs_) {
        if (job.active && job.updated_at >= latest.updated_at) latest = job;
    }
    return latest;
}

void MusicService::EnsurePollTask() {
    if (poll_task_ != nullptr) return;
    BaseType_t created = xTaskCreate(PollTaskEntry, "music_poll", kPollTaskStackBytes, this, 2, &poll_task_);
    if (created != pdPASS) {
        poll_task_ = nullptr;
        ESP_LOGE(kTag, "MUSIC_POLL_TASK_CREATE_FAILED");
    }
}

void MusicService::PollTaskEntry(void* arg) {
    static_cast<MusicService*>(arg)->PollTask();
}

void MusicService::PollTask() {
    for (;;) {
        PollPendingJobs();
        bool should_exit = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            should_exit = !HasPendingJobsLocked();
            if (should_exit) poll_task_ = nullptr;
        }
        if (should_exit) {
            vTaskDelete(nullptr);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(kPollIntervalMs));
    }
}

void MusicService::PollPendingJobs() {
    std::string url;
    std::array<std::string, kMaxTrackedJobs> ids;
    size_t count = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& job : jobs_) {
            if (job.active && !IsTerminal(job.status)) ids[count++] = job.job_id;
        }
    }
    if (count > 0) url = ResolveBridgeUrl();
    if (url.empty() || count == 0) return;
    LogMemory("before_poll");
    MusicBridgeClient client;
    for (size_t i = 0; i < count; ++i) {
        MusicJobStatus status;
        if (client.GetJob(url, ids[i], status)) {
            ApplyStatus(status);
        } else {
            ESP_LOGW(kTag, "MUSIC_POLL_ERROR job_id=%s error=%s", ids[i].c_str(), status.error.c_str());
            InvalidateRuntimeUrl(url);
            break;
        }
    }
    LogMemory("after_poll");
}

std::string MusicService::ErrorResult(const std::string& error) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "accepted", false);
    cJSON_AddStringToObject(root, "error", error.substr(0, 240).c_str());
    char* rendered = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!rendered) return "{\"accepted\":false,\"error\":\"out_of_memory\"}";
    std::string result(rendered);
    cJSON_free(rendered);
    return result;
}

std::string MusicService::GenerateResultJson(const MusicGenerateResult& result) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "accepted", result.success);
    if (result.success) {
        cJSON_AddStringToObject(root, "job_id", result.job_id.c_str());
        cJSON_AddStringToObject(root, "status", result.status.c_str());
        cJSON_AddStringToObject(root, "message", "Song generation started.");
    } else {
        cJSON_AddStringToObject(root, "error", result.error.c_str());
    }
    char* rendered = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!rendered) return "{\"accepted\":false,\"error\":\"out_of_memory\"}";
    std::string result_json(rendered);
    cJSON_free(rendered);
    return result_json;
}

std::string MusicService::StatusResultJson(const PendingJob& job) {
    if (!job.active) return "{\"success\":false,\"error\":\"no_pending_job\"}";
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "success", true);
    cJSON_AddStringToObject(root, "job_id", job.job_id.c_str());
    cJSON_AddStringToObject(root, "status", job.status.c_str());
    cJSON_AddStringToObject(root, "title", job.title.c_str());
    if (!job.progress.empty()) cJSON_AddStringToObject(root, "progress", job.progress.c_str());
    if (!job.audio_url.empty()) cJSON_AddStringToObject(root, "audio_url", job.audio_url.c_str());
    if (!job.format.empty()) cJSON_AddStringToObject(root, "format", job.format.c_str());
    if (job.size > 0) cJSON_AddNumberToObject(root, "size", static_cast<double>(job.size));
    if (!job.error.empty()) cJSON_AddStringToObject(root, "error", job.error.c_str());
    if (job.status == "ready") cJSON_AddBoolToObject(root, "downloaded", false);
    char* rendered = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!rendered) return "{\"success\":false,\"error\":\"out_of_memory\"}";
    std::string result(rendered);
    cJSON_free(rendered);
    return result;
}

void MusicService::RegisterMcpTools() {
    auto& mcp = McpServer::GetInstance();
    PropertyList generate_properties({
        Property("title", kPropertyTypeString),
        Property("style", kPropertyTypeString),
        Property("lyrics", kPropertyTypeString),
        Property("provider", kPropertyTypeString, std::string("yue2")),
        Property("duration_seconds", kPropertyTypeInteger, 0, 0, 600),
    });
    mcp.AddTool(
        "music.generate",
        "Start one asynchronous song generation. The device automatically discovers and checks the local music service; do not ask the user for network details. Write the title, concise style, and complete lyrics yourself, call this once, then tell the user generation has started. Omit duration_seconds for the standard 320-second maximum, or set it only when the user explicitly requests another duration. The returned job_id can be checked with music.generation_status; do not call generation again while it is running.",
        generate_properties,
        [this](const PropertyList& properties) -> ReturnValue {
            const std::string title = properties["title"].value<std::string>();
            const std::string style = properties["style"].value<std::string>();
            const std::string lyrics = properties["lyrics"].value<std::string>();
            const std::string provider = properties["provider"].value<std::string>();
            const int duration_seconds = properties["duration_seconds"].value<int>();
            if (!ValidateText(title, kMaxTitleLength) || !ValidateText(style, kMaxStyleLength) ||
                !ValidateText(lyrics, kMaxLyricsLength)) {
                return ErrorResult("invalid_title_style_or_lyrics_length");
            }
            if (provider != "yue2") return ErrorResult("unsupported_provider");
            const std::string url = ResolveBridgeUrl();
            if (url.empty()) return ErrorResult("generator_unavailable");

            LogMemory("before_generate");
            MusicGenerateResult generated;
            MusicBridgeClient client;
            if (!client.Submit(url, title, style, lyrics, provider, duration_seconds, generated)) {
                LogMemory("after_generate_error");
                return ErrorResult(generated.error.empty() ? "bridge_request_failed" : generated.error);
            }
            AddPendingJob(generated, title);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                EnsurePollTask();
            }
            LogMemory("after_generate");
            return GenerateResultJson(generated);
        });

    PropertyList status_properties({Property("job_id", kPropertyTypeString, std::string(""))});
    mcp.AddTool(
        "music.generation_status",
        "Report the status of a previously requested song. Use the optional job_id from music.generate when the user asks whether it is ready; do not regenerate a song because it is still generating.",
        status_properties,
        [this](const PropertyList& properties) -> ReturnValue {
            const std::string requested = properties["job_id"].value<std::string>();
            std::lock_guard<std::mutex> lock(mutex_);
            const PendingJob job = requested.empty() ? FindLatestJobLocked() : FindJobLocked(requested);
            return StatusResultJson(job);
        });

    PropertyList config_properties({Property("url", kPropertyTypeString)});
    auto* config_tool = new McpTool(
        "music.set_bridge_url",
        "Manual Music Bridge URL override for user-only diagnostics. Leave url empty to clear the override and return to automatic local service discovery; ordinary song generation does not require network details.",
        config_properties,
        [this](const PropertyList& properties) -> ReturnValue {
            std::string error;
            if (!SetBridgeUrl(properties["url"].value<std::string>(), error)) return ErrorResult(error);
            cJSON* result = cJSON_CreateObject();
            cJSON_AddBoolToObject(result, "success", true);
            cJSON_AddStringToObject(result, "message", properties["url"].value<std::string>().empty() ? "Manual override cleared; automatic discovery enabled." : "Music Bridge manual override saved.");
            return result;
        });
    config_tool->set_user_only(true);
    mcp.AddTool(config_tool);
}
