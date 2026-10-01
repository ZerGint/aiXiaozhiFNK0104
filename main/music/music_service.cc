#include "music_service.h"

#include <cJSON.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <mdns.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <dirent.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <cerrno>
#include <cstring>
#include <vector>

#include "application.h"
#include "display.h"
#include "generated_music_job_storage.h"
#include "generated_music_storage.h"
#include "managers/storage_manager.h"
#include "media/internet_radio_player.h"
#include "media/media_player.h"
#include "settings.h"
#include "board.h"

namespace {

constexpr char kTag[] = "MusicService";
constexpr char kNamespace[] = "music";
constexpr char kBridgeUrlKey[] = "bridge_url";
constexpr uint32_t kPollTaskStackBytes = 4096;
constexpr char kGeneratedMusicDir[] = "/sdcard/generated_music";
constexpr char kGeneratedMusicIndex[] = "/sdcard/generated_music/library.dat";
constexpr char kGeneratedMusicIndexTmp[] = "/sdcard/generated_music/library.tmp";
constexpr char kGeneratedMusicJobs[] = "/sdcard/generated_music/jobs.dat";
constexpr char kGeneratedMusicJobsTmp[] = "/sdcard/generated_music/jobs.tmp";
constexpr size_t kDownloadSafetyMargin = 64 * 1024;
constexpr uint32_t kDownloadRetryBaseSec = 30;
constexpr uint32_t kDownloadRetryMaxSec = 600;
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

void LogDiagnosticMemory(const char* stage) {
    ESP_LOGI(kTag,
             "MUSIC_DIAG MEM stage=%s internal_free=%u internal_largest=%u internal_min=%u psram_free=%u",
             stage,
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
             static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)));
}

std::string Bounded(const std::string& value, size_t limit) {
    return value.substr(0, limit);
}

std::string ShortId(const std::string& id) {
    return id.substr(0, std::min<size_t>(8, id.size()));
}

bool IsSafeStoredFilename(const std::string& value) {
    if (value.empty() || value.size() > GeneratedMusicStorage::kMaxFilenameLength ||
        value.size() < 4 || value.substr(value.size() - 4) != ".mp3" ||
        value.find("..") != std::string::npos || value.find('/') != std::string::npos ||
        value.find('\\') != std::string::npos) {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](unsigned char ch) {
        return ch >= 0x20 && ch != '"' && ch != '<' && ch != '>' && ch != '|' && ch != '?' && ch != '*';
    });
}

} // namespace

MusicService& MusicService::GetInstance() {
    static MusicService instance;
    return instance;
}

void MusicService::BeginVoiceConversation() {
    std::lock_guard<std::mutex> lock(mutex_);
    generation_followup_blocked_ = false;
}

bool MusicService::IsGenerationFollowupBlocked() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return generation_followup_blocked_;
}

void MusicService::BlockGenerationFollowups() {
    std::lock_guard<std::mutex> lock(mutex_);
    generation_followup_blocked_ = true;
}

void MusicService::Initialize() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (initialized_) return;
        initialized_ = true;
    }
    LoadState();
    LoadLocalIndex();
    RegisterMcpTools();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (HasPendingJobsLocked()) EnsurePollTask();
    }
    LogMemory("after_initialize");
    LogDiagnosticMemory("after_reboot_stable");
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
    GeneratedMusicJobRecord persisted;
    bool exists = false;
    std::string error;
    const bool valid = StorageManager::GetInstance().IsSdCardMounted() &&
                       GeneratedMusicJobStorage::Load(kGeneratedMusicJobs, persisted, exists, error);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        job_ = {};
        if (valid && exists) {
            job_.active = true;
            job_.job_id = Bounded(persisted.job_id, MusicBridgeClient::kMaxJobIdLength);
            job_.title = Bounded(persisted.title, kMaxTitleLength);
            job_.status = Bounded(persisted.status, 32);
            job_.size = static_cast<size_t>(persisted.size);
            job_.duration = persisted.duration_seconds;
            job_.filename = Bounded(persisted.filename, 160);
            job_.download_status = Bounded(persisted.download_status, 24);
            job_.local_filename = Bounded(persisted.local_filename, 160);
            job_.downloaded_size = static_cast<size_t>(persisted.downloaded_size);
            job_.download_attempts = persisted.download_attempts;
            job_.next_download_at = persisted.next_download_at;
            job_.error = Bounded(persisted.error, 240);
            if (job_.status == "ready" && job_.download_status == "local") job_.status = "local";
            if (job_.status == "downloading") {
                job_.status = "ready";
                job_.download_status = "pending";
                job_.next_download_at = 0;
            }
        }
    }
    if (!valid && !error.empty()) {
        ESP_LOGW(kTag, "MUSIC_DIAG JOBS_BOOT valid=0 status=none job_id= error=%s", error.c_str());
    } else {
        std::lock_guard<std::mutex> lock(mutex_);
        ESP_LOGI(kTag, "MUSIC_DIAG JOBS_BOOT valid=%d status=%s job_id=%s", exists ? 1 : 0,
                 job_.active ? job_.status.c_str() : "none", job_.active ? job_.job_id.c_str() : "");
    }
    ESP_LOGI(kTag, "MUSIC_DIAG OLD_NVS_JOBS_IGNORED");
}

bool MusicService::PersistLocked() {
    if (!job_.active) return false;
    GeneratedMusicJobRecord record;
    record.job_id = job_.job_id;
    record.title = job_.title;
    record.status = job_.status;
    record.filename = job_.filename;
    record.download_status = job_.download_status;
    record.local_filename = job_.local_filename;
    record.error = job_.error;
    record.size = job_.size;
    record.duration_seconds = static_cast<uint32_t>(std::max(0.0, job_.duration));
    record.downloaded_size = job_.downloaded_size;
    record.download_attempts = job_.download_attempts;
    record.next_download_at = job_.next_download_at;
    std::string error;
    if (!GeneratedMusicJobStorage::SaveAtomic(kGeneratedMusicJobs, kGeneratedMusicJobsTmp, record, error)) {
        ESP_LOGW(kTag, "MUSIC_DIAG JOBS_WRITE job_id=%s status=%s result=FAIL error=%s", job_.job_id.c_str(),
                 job_.status.c_str(), error.c_str());
        return false;
    }
    ESP_LOGI(kTag, "MUSIC_DIAG JOBS_WRITE job_id=%s status=%s result=PASS", job_.job_id.c_str(),
             job_.status.c_str());
    return true;
}

bool MusicService::IsTerminal(const std::string& status) {
    return status == "local" || status == "complete" || status == "failed" || status == "cancelled";
}

bool MusicService::HasPendingJobsLocked() const {
    if (!job_.active || IsTerminal(job_.status)) return false;
    return job_.status == "queued" || job_.status == "submitting" || job_.status == "generating" ||
           job_.status == "ready" || job_.status == "downloading";
}

bool MusicService::AddPendingJob(const MusicGenerateResult& generated, const std::string& title) {
    std::lock_guard<std::mutex> lock(mutex_);
    job_ = {};
    job_.active = true;
    job_.job_id = generated.job_id;
    job_.title = Bounded(title, kMaxTitleLength);
    job_.status = Bounded(generated.status.empty() ? "queued" : generated.status, 32);
    if (!PersistLocked()) {
        job_ = {};
        return false;
    }
    return true;
}

void MusicService::ApplyStatus(const MusicJobStatus& status) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!job_.active || job_.job_id != status.job_id) return;
    const std::string old_status = job_.status;
    job_.status = Bounded(status.status, 32);
    if (!status.title.empty()) job_.title = Bounded(status.title, kMaxTitleLength);
    job_.size = status.size;
    job_.duration = status.duration;
    job_.filename = Bounded(status.filename, 160);
    job_.error = Bounded(status.error, 240);
    if (job_.status == "ready" && job_.download_status.empty()) job_.download_status = "pending";
    PersistLocked();
    ESP_LOGI(kTag, "MUSIC_JOB_UPDATE job_id=%s status=%s", job_.job_id.c_str(), job_.status.c_str());
    if (old_status != job_.status) {
        ESP_LOGI(kTag, "MUSIC_DIAG JOB_STATE job_id=%s old=%s new=%s", job_.job_id.c_str(),
                 old_status.c_str(), job_.status.c_str());
        if (IsTerminal(job_.status)) {
            ESP_LOGI(kTag, "MUSIC_DIAG JOBS_TERMINAL job_id=%s status=%s", job_.job_id.c_str(),
                     job_.status.c_str());
        }
    }
    if (job_.status == "ready" && job_.download_status != "local") {
        ESP_LOGI(kTag, "MUSIC_DOWNLOAD_PENDING id=%s", job_.job_id.c_str());
    }
}

bool MusicService::SafeFilename(const std::string& remote,
                                const std::string& job_id,
                                std::string& output) {
    std::string value = remote;
    if (value.empty()) value = "Generated Track [" + ShortId(job_id) + "].mp3";
    for (char& ch : value) {
        const unsigned char byte = static_cast<unsigned char>(ch);
        if (byte < 0x20 || ch == '/' || ch == '\\' || ch == ':' || ch == '"' || ch == '<' ||
            ch == '>' || ch == '|' || ch == '?' || ch == '*') {
            ch = '_';
        }
    }
    if (value.find("..") != std::string::npos) value.replace(value.find(".."), 2, "__");
    while (!value.empty() && (value.back() == ' ' || value.back() == '.')) value.pop_back();
    if (value.empty()) value = "Generated Track [" + ShortId(job_id) + "].mp3";
    if (value.size() > 150) value.resize(150);
    if (value.size() < 4 || value.substr(value.size() - 4) != ".mp3") value += ".mp3";
    output = value;
    return output.size() <= 160 && output.find("..") == std::string::npos &&
           output.find('/') == std::string::npos && output.find('\\') == std::string::npos;
}

void MusicService::LoadLocalIndex() {
    if (!StorageManager::GetInstance().IsSdCardMounted()) return;
    mkdir(kGeneratedMusicDir, 0775);
    struct stat temp_stat = {};
    if (stat(kGeneratedMusicIndexTmp, &temp_stat) == 0) unlink(kGeneratedMusicIndexTmp);

    std::vector<GeneratedMusicRecord> records;
    std::string load_error;
    const bool valid = GeneratedMusicStorage::Load(kGeneratedMusicIndex, records, load_error);
    if (!valid && !load_error.empty()) ESP_LOGW(kTag, "MUSIC_INDEX_CORRUPT format=GMDB error=%s start_empty=1", load_error.c_str());

    if (valid) {
        const size_t original_count = records.size();
        for (size_t i = 0; i < records.size();) {
            const auto& record = records[i];
            struct stat audio_stat = {};
            const bool missing_or_unsafe = !IsSafeStoredFilename(record.filename) ||
                                           stat((std::string(kGeneratedMusicDir) + "/" + record.filename).c_str(), &audio_stat) != 0 ||
                                           audio_stat.st_size <= 0;
            const bool duplicate = std::any_of(records.begin(), records.begin() + i, [&](const auto& item) {
                return item.filename == record.filename;
            });
            if (missing_or_unsafe || duplicate) {
                records.erase(records.begin() + i);
            } else {
                ++i;
            }
        }
        if (records.size() != original_count) {
            std::string save_error;
            if (!GeneratedMusicStorage::SaveAtomic(kGeneratedMusicIndex, kGeneratedMusicIndexTmp,
                                                   records, save_error)) {
                ESP_LOGW(kTag, "MUSIC_INDEX_RECONCILE_FAILED error=%s", save_error.c_str());
            }
        }
    } else {
        records.clear();
    }

    DIR* dir = opendir(kGeneratedMusicDir);
    if (dir) {
        struct dirent* entry = nullptr;
        while ((entry = readdir(dir)) != nullptr) {
            const std::string name = entry->d_name;
            if (name.size() > 5 && name.substr(name.size() - 5) == ".part") {
                unlink((std::string(kGeneratedMusicDir) + "/" + name).c_str());
            }
        }
        closedir(dir);
    }
    ESP_LOGI(kTag, "MUSIC_DIAG GMDB_BOOT valid=%d count=%u", valid ? 1 : 0,
             static_cast<unsigned>(records.size()));
    ESP_LOGI(kTag, "MUSIC_INDEX_LOADED format=GMDB tracks=%u valid=%d", static_cast<unsigned>(records.size()), valid ? 1 : 0);
}

bool MusicService::NetworkSafeForDownload() const {
    return CanStartGeneratedDownload();
}

const char* MusicService::DownloadBlockReason(bool include_gate) const {
    if (!StorageManager::GetInstance().IsSdCardMounted()) return "sd";
    const DeviceState state = Application::GetInstance().GetDeviceState();
    if (state == kDeviceStateUpgrading) return "ota";
    if (state != kDeviceStateIdle) return "ai";
    if (InternetRadioPlayer::GetInstance().IsActive()) return "radio";
    if (MediaPlayer::GetInstance().IsPlaying()) return "media";
    if (include_gate && IsGeneratedDownloadBusy()) return "gate";
    return nullptr;
}

bool MusicService::CanStartGeneratedDownload() const {
    return DownloadBlockReason() == nullptr;
}

bool MusicService::TryBeginGeneratedDownload() {
    LogDiagnosticMemory("before_gate_claim");
    bool expected = false;
    if (!generated_download_busy_.compare_exchange_strong(expected, true, std::memory_order_acq_rel,
                                                          std::memory_order_relaxed)) {
        return false;
    }
    ESP_LOGI(kTag, "MUSIC_GATE CLAIM");
    return true;
}

void MusicService::EndGeneratedDownload() {
    bool expected = true;
    if (!generated_download_busy_.compare_exchange_strong(expected, false, std::memory_order_acq_rel,
                                                          std::memory_order_relaxed)) {
        return;
    }
    LogDiagnosticMemory("after_gate_release");
    LogDiagnosticMemory("stable_idle");
    ESP_LOGI(kTag, "MUSIC_GATE RELEASE");
}

void MusicService::ShowDownloadOverlay(const char* title, int percent) {
    auto display = Board::GetInstance().GetDisplay();
    if (display != nullptr) {
        display->ShowGeneratedDownloadProgress(title, percent);
        ESP_LOGI(kTag, "MUSIC_UI DOWNLOAD_OVERLAY_SHOW percent=%d", percent);
        LogDiagnosticMemory("after_overlay_show");
    }
}

void MusicService::HideDownloadOverlay() {
    auto display = Board::GetInstance().GetDisplay();
    if (display != nullptr) {
        display->HideGeneratedDownloadProgress();
        ESP_LOGI(kTag, "MUSIC_UI DOWNLOAD_OVERLAY_HIDE");
        LogDiagnosticMemory("after_overlay_hide");
    }
}

void MusicService::RecordDownloadFailure(const std::string& job_id, const std::string& reason) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!job_.active || job_.job_id != job_id) return;
    ++job_.download_attempts;
    const uint32_t shift = std::min<uint32_t>(job_.download_attempts - 1, 4);
    const uint32_t delay = std::min<uint32_t>(kDownloadRetryMaxSec, kDownloadRetryBaseSec << shift);
    job_.download_status = "failed";
    job_.next_download_at = static_cast<int64_t>(time(nullptr)) + delay;
    job_.error = Bounded(reason, 240);
    PersistLocked();
    ESP_LOGW(kTag, "MUSIC_DOWNLOAD_FAIL id=%s reason=%s retry_sec=%u", job_id.c_str(),
             reason.c_str(), static_cast<unsigned>(delay));
}

bool MusicService::DownloadReadyJob(const PendingJob& snapshot) {
    if (!CanStartGeneratedDownload()) {
        const char* reason = DownloadBlockReason();
        ESP_LOGI(kTag, "MUSIC_DOWNLOAD_DEFERRED reason=%s job_id=%s", reason != nullptr ? reason : "state_changed",
                 snapshot.job_id.c_str());
        return false;
    }
    const std::string url = ResolveBridgeUrl();
    if (url.empty()) {
        ESP_LOGI(kTag, "MUSIC_DIAG DOWNLOAD_DECISION job_id=%s action=error", snapshot.job_id.c_str());
        ESP_LOGW(kTag, "MUSIC_DIAG DOWNLOAD_FAIL reason=bridge_unavailable job_id=%s", snapshot.job_id.c_str());
        RecordDownloadFailure(snapshot.job_id, "bridge_unavailable");
        return false;
    }

    std::string filename;
    if (!SafeFilename(snapshot.filename, snapshot.job_id, filename)) {
        ESP_LOGI(kTag, "MUSIC_DIAG DOWNLOAD_DECISION job_id=%s action=error", snapshot.job_id.c_str());
        ESP_LOGW(kTag, "MUSIC_DIAG DOWNLOAD_FAIL reason=unsafe_filename job_id=%s", snapshot.job_id.c_str());
        RecordDownloadFailure(snapshot.job_id, "unsafe_filename");
        return false;
    }
    const std::string final_path = std::string(kGeneratedMusicDir) + "/" + filename;
    const std::string temp_path = final_path + ".part";

    struct stat final_check = {};
    struct stat part_check = {};
    const bool final_file_exists = stat(final_path.c_str(), &final_check) == 0 && final_check.st_size > 0;
    const bool part_file_exists = stat(temp_path.c_str(), &part_check) == 0 && part_check.st_size > 0;
    bool gmdb_contains = false;
    {
        std::vector<GeneratedMusicRecord> diagnostic_records;
        std::string diagnostic_error;
        if (GeneratedMusicStorage::Load(kGeneratedMusicIndex, diagnostic_records, diagnostic_error)) {
            gmdb_contains = std::any_of(diagnostic_records.begin(), diagnostic_records.end(), [&](const auto& record) {
                return record.filename == filename;
            });
        }
    }
    ESP_LOGI(kTag,
             "MUSIC_DIAG DOWNLOAD_CHECK job_id=%s expected_filename=%s already_local=%d gmdb_contains=%d final_file_exists=%d part_file_exists=%d",
             snapshot.job_id.c_str(), filename.c_str(), final_file_exists ? 1 : 0, gmdb_contains ? 1 : 0,
             final_file_exists ? 1 : 0, part_file_exists ? 1 : 0);

    auto mark_local = [&](size_t size) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!job_.active || job_.job_id != snapshot.job_id) return;
        job_.status = "local";
        job_.download_status = "local";
        job_.local_filename = filename;
        job_.downloaded_size = size;
        job_.download_attempts = 0;
        job_.next_download_at = 0;
        job_.error.clear();
        PersistLocked();
        ESP_LOGI(kTag, "MUSIC_DIAG JOBS_TERMINAL job_id=%s status=local", job_.job_id.c_str());
    };

    bool existing_local = false;
    size_t existing_size = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        struct stat existing_stat = {};
        if (stat(final_path.c_str(), &existing_stat) == 0 && existing_stat.st_size > 0) {
            existing_local = true;
            existing_size = static_cast<size_t>(existing_stat.st_size);
        } else {
            struct statvfs space = {};
            if (statvfs(kGeneratedMusicDir, &space) == 0) {
                const uint64_t available = static_cast<uint64_t>(space.f_bavail) * space.f_frsize;
                if (available < static_cast<uint64_t>(snapshot.size) + kDownloadSafetyMargin) {
                    ESP_LOGI(kTag, "MUSIC_DIAG DOWNLOAD_DECISION job_id=%s action=error", snapshot.job_id.c_str());
                    ESP_LOGW(kTag, "MUSIC_DIAG DOWNLOAD_FAIL reason=no_space job_id=%s", snapshot.job_id.c_str());
                    ESP_LOGW(kTag, "MUSIC_DOWNLOAD_NO_SPACE id=%s available=%llu required=%llu",
                             snapshot.job_id.c_str(), static_cast<unsigned long long>(available),
                             static_cast<unsigned long long>(snapshot.size) + kDownloadSafetyMargin);
                    return false;
                }
            }
            mkdir(kGeneratedMusicDir, 0775);
            unlink(temp_path.c_str());
        }
    }
    if (existing_local) {
        ESP_LOGI(kTag, "MUSIC_DIAG DOWNLOAD_DECISION job_id=%s action=skip_local", snapshot.job_id.c_str());
        ESP_LOGI(kTag, "MUSIC_DIAG READY_ALREADY_LOCAL job_id=%s filename=%s", snapshot.job_id.c_str(), filename.c_str());
        mark_local(existing_size);
        ESP_LOGI(kTag, "MUSIC_DIAG LOCAL_READY job_id=%s filename=%s file_exists=1 file_size=%u gmdb_contains=%d",
                 snapshot.job_id.c_str(), filename.c_str(), static_cast<unsigned>(existing_size), gmdb_contains ? 1 : 0);
        ESP_LOGI(kTag, "MUSIC_DIAG STATUS_LOCAL job_id=%s status=%s download_status=local downloaded=%u local_filename=%s",
                 snapshot.job_id.c_str(), snapshot.status.c_str(), static_cast<unsigned>(existing_size), filename.c_str());
        LogDiagnosticMemory("after_download_return");
        return true;
    }

    ESP_LOGI(kTag, "MUSIC_DIAG DOWNLOAD_DECISION job_id=%s action=download", snapshot.job_id.c_str());
    if (!CanStartGeneratedDownload() || !TryBeginGeneratedDownload()) {
        const char* reason = DownloadBlockReason();
        ESP_LOGI(kTag, "MUSIC_DOWNLOAD_DEFERRED reason=%s", reason != nullptr ? reason : "gate");
        return false;
    }
    bool gate_claimed = true;
    auto release_gate = [&]() {
        if (gate_claimed) {
            HideDownloadOverlay();
            EndGeneratedDownload();
            gate_claimed = false;
        }
    };
    // The download gate is already claimed above. Re-check only external
    // blockers here; including the gate would always reject our own claim.
    const char* external_block_reason = DownloadBlockReason(false);
    if (external_block_reason != nullptr) {
        const char* reason = external_block_reason;
        ESP_LOGI(kTag, "MUSIC_DOWNLOAD_DEFERRED reason=%s", reason != nullptr ? reason : "state_changed");
        release_gate();
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (job_.active && job_.job_id == snapshot.job_id) {
            job_.status = "downloading";
            job_.download_status = "downloading";
            job_.local_filename = filename;
            job_.filename = filename;
            PersistLocked();
        }
    }
    ShowDownloadOverlay("Скачивание песни", 0);
    ESP_LOGI(kTag, "MUSIC_DOWNLOAD_START id=%s expected_bytes=%u", snapshot.job_id.c_str(), static_cast<unsigned>(snapshot.size));
    ESP_LOGI(kTag,
             "MUSIC_DIAG DOWNLOAD_START job_id=%s endpoint=%s expected_size=%u final_path=%s part_path=%s",
             snapshot.job_id.c_str(), url.c_str(), static_cast<unsigned>(snapshot.size), final_path.c_str(),
             temp_path.c_str());
    LogDiagnosticMemory("before_download");
    size_t downloaded = 0;
    std::string error;
    MusicBridgeClient client;
    int last_percent = -1;
    int64_t last_ui_ms = 0;
    bool mid_download_logged = false;
    const bool success = client.DownloadAudio(url, snapshot.job_id, snapshot.size, temp_path, downloaded, error,
                                              [&](size_t bytes, size_t total) {
                                                  if (total == 0) return;
                                                  if (!mid_download_logged && bytes > 0) {
                                                      LogDiagnosticMemory("mid_download");
                                                      mid_download_logged = true;
                                                  }
                                                  const int percent = static_cast<int>(std::min<size_t>(100, bytes * 100 / total));
                                                  const int64_t now_ms = esp_timer_get_time() / 1000;
                                                  if (percent == last_percent && now_ms - last_ui_ms < 200) return;
                                                  if (percent != last_percent || now_ms - last_ui_ms >= 200) {
                                                      last_percent = percent;
                                                      last_ui_ms = now_ms;
                                                      auto display = Board::GetInstance().GetDisplay();
                                                      if (display != nullptr) display->ShowGeneratedDownloadProgress("Скачивание песни", percent);
                                                      ESP_LOGI(kTag, "MUSIC_UI DOWNLOAD_PROGRESS percent=%d", percent);
                                                  }
                                              });
    LogMemory("after_download");
    LogDiagnosticMemory("after_download_complete");
    if (!success) {
        unlink(temp_path.c_str());
        ESP_LOGW(kTag, "MUSIC_DIAG DOWNLOAD_FAIL reason=%s job_id=%s", error.c_str(), snapshot.job_id.c_str());
        std::lock_guard<std::mutex> lock(mutex_);
        if (job_.active && job_.job_id == snapshot.job_id) {
            ++job_.download_attempts;
            const uint32_t delay = std::min<uint32_t>(kDownloadRetryMaxSec, kDownloadRetryBaseSec << std::min<uint32_t>(job_.download_attempts - 1, 4));
            job_.next_download_at = static_cast<int64_t>(time(nullptr)) + delay;
            job_.status = "ready";
            job_.download_status = "failed";
            job_.downloaded_size = downloaded;
            job_.error = Bounded(error, 240);
            PersistLocked();
            ESP_LOGW(kTag, "MUSIC_DOWNLOAD_FAIL id=%s reason=%s retry_sec=%u", snapshot.job_id.c_str(), error.c_str(), static_cast<unsigned>(delay));
        }
        release_gate();
        return false;
    }
    ShowDownloadOverlay("Скачивание песни", 100);
    ESP_LOGI(kTag, "MUSIC_UI DOWNLOAD_PROGRESS percent=100");
    ESP_LOGI(kTag, "MUSIC_DIAG PART_COMPLETE job_id=%s part_path=%s size=%u", snapshot.job_id.c_str(),
             temp_path.c_str(), static_cast<unsigned>(downloaded));
    if (rename(temp_path.c_str(), final_path.c_str()) != 0) {
        ESP_LOGW(kTag, "MUSIC_DIAG FINAL_RENAME_FAIL job_id=%s error=%s", snapshot.job_id.c_str(), strerror(errno));
        unlink(temp_path.c_str());
        error = "final_rename_failed";
        std::lock_guard<std::mutex> lock(mutex_);
        if (job_.active && job_.job_id == snapshot.job_id) {
            job_.status = "ready";
            job_.download_status = "failed";
            job_.error = error;
            job_.next_download_at = static_cast<int64_t>(time(nullptr)) + kDownloadRetryBaseSec;
            PersistLocked();
        }
        release_gate();
        return false;
    }
    struct stat final_stat = {};
    if (stat(final_path.c_str(), &final_stat) != 0 || final_stat.st_size <= 0) {
        ESP_LOGW(kTag, "MUSIC_DIAG LOCAL_CHECK_FAIL job_id=%s path=%s", snapshot.job_id.c_str(), final_path.c_str());
        ESP_LOGW(kTag, "MUSIC_DIAG DOWNLOAD_FAIL reason=final_file_check job_id=%s", snapshot.job_id.c_str());
        unlink(final_path.c_str());
        release_gate();
        return false;
    }
    ESP_LOGI(kTag, "MUSIC_DIAG FINAL_RENAME job_id=%s final_path=%s final_size=%u", snapshot.job_id.c_str(),
             final_path.c_str(), static_cast<unsigned>(final_stat.st_size));
    LogDiagnosticMemory("after_final_rename");

    std::string evicted_filename;
    bool already_indexed = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<GeneratedMusicRecord> records;
        std::string load_error;
        const bool valid = GeneratedMusicStorage::Load(kGeneratedMusicIndex, records, load_error);
        if (!valid && !load_error.empty()) {
            ESP_LOGW(kTag, "MUSIC_DIAG GMDB_FAIL reason=load error=%s", load_error.c_str());
            ESP_LOGW(kTag, "MUSIC_INDEX_CORRUPT format=GMDB error=%s reset_metadata=1", load_error.c_str());
            records.clear();
        }
        const size_t old_count = records.size();
        ESP_LOGI(kTag, "MUSIC_DIAG GMDB_BEFORE count=%u new_filename=%s", static_cast<unsigned>(old_count), filename.c_str());
        const auto duplicate = std::find_if(records.begin(), records.end(), [&](const auto& record) {
            return record.filename == filename;
        });
        if (duplicate != records.end()) {
            already_indexed = true;
        } else {
            if (records.size() >= GeneratedMusicStorage::kMaxRecords) {
                evicted_filename = records.front().filename;
                ESP_LOGI(kTag, "MUSIC_DIAG FIFO_EVICT_PLAN count=%u oldest_filename=%s",
                         static_cast<unsigned>(records.size()), evicted_filename.c_str());
                records.erase(records.begin());
            }
            records.push_back(GeneratedMusicRecord{filename});
            std::string save_error;
            if (!GeneratedMusicStorage::SaveAtomic(kGeneratedMusicIndex, kGeneratedMusicIndexTmp,
                                                   records, save_error)) {
                ESP_LOGW(kTag, "MUSIC_DIAG GMDB_FAIL reason=commit error=%s", save_error.c_str());
                ESP_LOGW(kTag, "MUSIC_INDEX_SAVE_FAILED error=%s orphan=%s", save_error.c_str(), final_path.c_str());
                if (job_.active && job_.job_id == snapshot.job_id) {
                    job_.status = "ready";
                    job_.download_status = "failed";
                    job_.error = "generated_index_commit_failed";
                    job_.next_download_at = static_cast<int64_t>(time(nullptr)) + kDownloadRetryBaseSec;
                    PersistLocked();
                }
                release_gate();
                return false;
            }
            ESP_LOGI(kTag, "MUSIC_DIAG GMDB_COMMIT old_count=%u new_count=%u appended=1 evicted=%s",
                     static_cast<unsigned>(old_count), static_cast<unsigned>(records.size()),
                     evicted_filename.empty() ? "none" : evicted_filename.c_str());
            LogDiagnosticMemory("after_gmdb_commit");
        }

        if (job_.active && job_.job_id == snapshot.job_id) {
            job_.status = "local";
            job_.download_status = "local";
            job_.local_filename = filename;
            job_.downloaded_size = static_cast<size_t>(final_stat.st_size);
            job_.download_attempts = 0;
            job_.next_download_at = 0;
            job_.error.clear();
            PersistLocked();
            ESP_LOGI(kTag, "MUSIC_DIAG JOBS_TERMINAL job_id=%s status=local", job_.job_id.c_str());
        }
    }
    if (already_indexed) {
        ESP_LOGI(kTag, "MUSIC_CACHE_DUPLICATE filename=%s", filename.c_str());
        ESP_LOGI(kTag, "MUSIC_DIAG READY_ALREADY_LOCAL job_id=%s filename=%s", snapshot.job_id.c_str(), filename.c_str());
        ESP_LOGI(kTag, "MUSIC_DIAG LOCAL_READY job_id=%s filename=%s file_exists=1 file_size=%u gmdb_contains=1",
                 snapshot.job_id.c_str(), filename.c_str(), static_cast<unsigned>(final_stat.st_size));
        ESP_LOGI(kTag, "MUSIC_DIAG STATUS_LOCAL job_id=%s status=%s download_status=local downloaded=%u local_filename=%s",
                 snapshot.job_id.c_str(), snapshot.status.c_str(), static_cast<unsigned>(final_stat.st_size), filename.c_str());
        LogDiagnosticMemory("after_download_return");
        release_gate();
        return true;
    }

    if (!evicted_filename.empty()) {
        const std::string victim_path = std::string(kGeneratedMusicDir) + "/" + evicted_filename;
        if (unlink(victim_path.c_str()) != 0 && errno != ENOENT)
        {
            ESP_LOGI(kTag, "MUSIC_DIAG FIFO_DELETE filename=%s result=FAIL", evicted_filename.c_str());
            ESP_LOGW(kTag, "MUSIC_CACHE_ORPHAN path=%s errno=%d", victim_path.c_str(), errno);
        } else {
            ESP_LOGI(kTag, "MUSIC_DIAG FIFO_DELETE filename=%s result=PASS", evicted_filename.c_str());
            ESP_LOGI(kTag, "MUSIC_CACHE_EVICT filename=%s", evicted_filename.c_str());
        }
    }
    ESP_LOGI(kTag, "MUSIC_DIAG LOCAL_READY job_id=%s filename=%s file_exists=1 file_size=%u gmdb_contains=1",
             snapshot.job_id.c_str(), filename.c_str(), static_cast<unsigned>(final_stat.st_size));
    ESP_LOGI(kTag, "MUSIC_DIAG STATUS_LOCAL job_id=%s status=%s download_status=local downloaded=%u local_filename=%s",
             snapshot.job_id.c_str(), snapshot.status.c_str(), static_cast<unsigned>(final_stat.st_size), filename.c_str());
    ESP_LOGI(kTag, "MUSIC_DOWNLOAD_DONE id=%s bytes=%u path=%s", snapshot.job_id.c_str(), static_cast<unsigned>(final_stat.st_size), final_path.c_str());
    LogDiagnosticMemory("after_download_return");
    release_gate();
    return true;
}

void MusicService::ProcessReadyDownloads() {
    if (!CanStartGeneratedDownload()) {
        const char* reason = DownloadBlockReason();
        std::lock_guard<std::mutex> lock(mutex_);
        if (reason != nullptr && last_deferred_reason_ != reason) {
            ESP_LOGI(kTag, "MUSIC_DOWNLOAD_DEFERRED reason=%s", reason);
            last_deferred_reason_ = reason;
        }
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        last_deferred_reason_.clear();
    }
    PendingJob candidate;
    const int64_t now = static_cast<int64_t>(time(nullptr));
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (job_.active && job_.status == "ready" && job_.download_status != "local" &&
            job_.download_status != "downloading" && job_.next_download_at <= now) candidate = job_;
    }
    if (candidate.active) {
        DownloadReadyJob(candidate);
        ESP_LOGI(kTag, "MUSIC_DOWNLOAD_STACK unused_bytes=%u",
                 static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr) * sizeof(StackType_t)));
    }
}

MusicService::PendingJob MusicService::FindJobLocked(const std::string& job_id) const {
    return job_.active && job_.job_id == job_id ? job_ : PendingJob{};
}

MusicService::PendingJob MusicService::FindLatestJobLocked() const {
    return job_.active ? job_ : PendingJob{};
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
    ESP_LOGI(kTag, "MUSIC_DIAG POLL_TASK_START reason=pending_work");
    for (;;) {
        PollPendingJobs();
        bool should_exit = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            should_exit = !HasPendingJobsLocked();
            if (should_exit) poll_task_ = nullptr;
        }
        if (should_exit) {
            ESP_LOGI(kTag, "MUSIC_DIAG POLL_TASK_STOP reason=no_pending_work");
            LogDiagnosticMemory("poll_task_exit");
            vTaskDelete(nullptr);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(kPollIntervalMs));
    }
}

void MusicService::PollPendingJobs() {
    std::string url;
    std::string job_id;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (job_.active && (job_.status == "queued" || job_.status == "submitting" || job_.status == "generating")) {
            job_id = job_.job_id;
        }
    }
    if (!job_id.empty()) url = ResolveBridgeUrl();
    if (!url.empty() && !job_id.empty()) {
        LogMemory("before_poll");
        MusicBridgeClient client;
        MusicJobStatus status;
        if (client.GetJob(url, job_id, status)) {
            ApplyStatus(status);
        } else {
            ESP_LOGW(kTag, "MUSIC_POLL_ERROR job_id=%s error=%s", job_id.c_str(), status.error.c_str());
            InvalidateRuntimeUrl(url);
        }
        LogMemory("after_poll");
    }
    ProcessReadyDownloads();
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
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "success", true);
    cJSON_AddBoolToObject(root, "valid", job.active);
    cJSON_AddBoolToObject(root, "exists", job.active);
    if (!job.active) {
        cJSON_AddStringToObject(root, "message", "no_current_generation");
        char* empty_rendered = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);
        if (!empty_rendered) return "{\"success\":true,\"valid\":false,\"exists\":false}";
        std::string empty_result(empty_rendered);
        cJSON_free(empty_rendered);
        return empty_result;
    }
    cJSON_AddStringToObject(root, "job_id", job.job_id.c_str());
    cJSON_AddStringToObject(root, "status", job.status.c_str());
    cJSON_AddStringToObject(root, "title", job.title.c_str());
    if (job.size > 0) cJSON_AddNumberToObject(root, "size", static_cast<double>(job.size));
    if (job.duration > 0) cJSON_AddNumberToObject(root, "duration", job.duration);
    if (!job.error.empty()) cJSON_AddStringToObject(root, "error", job.error.c_str());
    if (!job.download_status.empty() || job.status == "ready" || job.status == "local") {
        const std::string download_status = job.download_status.empty() ? "pending" : job.download_status;
        cJSON_AddStringToObject(root, "download_status", download_status.c_str());
        cJSON_AddBoolToObject(root, "downloaded", download_status == "local");
        if (!job.local_filename.empty()) cJSON_AddStringToObject(root, "local_filename", job.local_filename.c_str());
    }
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
        Property("duration_seconds", kPropertyTypeInteger, 320, 0, 600),
    });
    mcp.AddTool(
        "music.generate",
        "Start one asynchronous song generation. The device automatically discovers and checks the local music service; do not ask the user for network details. Write the title, concise style, and complete lyrics yourself, call this once, then tell the user that generation has started and they should wait. This completes the current request: after a successful call, do not call music.generation_status, music.play_generated, media tools, or any other music tool in the same turn, and do not start another generation. Wait for the user to ask later whether it is ready. duration_seconds defaults to 320 seconds when omitted; set it only when the user explicitly requests another duration. Generated music must stay in the local generated-music/Music Bridge flow and must never be routed to Home Assistant for playback or search.",
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
            if (IsGeneratedDownloadBusy()) {
                ESP_LOGI(kTag, "MUSIC_GENERATE_BLOCKED reason=generated_download_busy");
                return ErrorResult("generated_download_busy");
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (HasPendingJobsLocked()) {
                    ESP_LOGI(kTag, "MUSIC_DIAG GENERATE_BLOCKED reason=generation_in_progress job_id=%s status=%s",
                             job_.job_id.c_str(), job_.status.c_str());
                    cJSON* blocked = cJSON_CreateObject();
                    cJSON_AddBoolToObject(blocked, "ok", false);
                    cJSON_AddStringToObject(blocked, "reason", "generation_in_progress");
                    cJSON_AddStringToObject(blocked, "job_id", job_.job_id.c_str());
                    cJSON_AddStringToObject(blocked, "status", job_.status.c_str());
                    return blocked;
                }
            }
            if (!StorageManager::GetInstance().IsSdCardMounted()) return ErrorResult("sd_card_required_for_generation_state");
            const std::string url = ResolveBridgeUrl();
            if (url.empty()) return ErrorResult("generator_unavailable");

            LogMemory("before_generate");
            MusicGenerateResult generated;
            MusicBridgeClient client;
            if (!client.Submit(url, title, style, lyrics, provider, duration_seconds, generated)) {
                LogMemory("after_generate_error");
                return ErrorResult(generated.error.empty() ? "bridge_request_failed" : generated.error);
            }
            if (!AddPendingJob(generated, title)) return ErrorResult("generation_state_persist_failed");
            // Generation is asynchronous. End the current voice conversation
            // after the assistant's acknowledgement instead of reopening the
            // auto-stop listening turn and inviting an immediate status call.
            BlockGenerationFollowups();
            Application::GetInstance().EndConversationAfterSpeech();
            ESP_LOGI(kTag, "MUSIC_DIAG JOB_TRACK job_id=%s status=%s duration_seconds=%d",
                     generated.job_id.c_str(), generated.status.c_str(), duration_seconds);
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
        "Report the status of the single current generation stored on SD. Use the optional job_id from music.generate when the user asks whether it is ready; do not regenerate a song because it is still generating. Generated music must not be searched or played through Home Assistant.",
        status_properties,
        [this](const PropertyList& properties) -> ReturnValue {
            if (IsGenerationFollowupBlocked()) return ErrorResult("wait_for_next_user_request");
            const std::string requested = properties["job_id"].value<std::string>();
            std::lock_guard<std::mutex> lock(mutex_);
            const PendingJob job = requested.empty() ? FindLatestJobLocked() : FindJobLocked(requested);
            return StatusResultJson(job);
        });

    PropertyList play_generated_properties({Property("query", kPropertyTypeString)});
    mcp.AddTool(
        "music.play_generated",
        "Play one final generated MP3 from the local /sdcard/generated_music directory. Search is local and deterministic: an exact filename match wins; otherwise a unique case-insensitive contains match is used. If zero or multiple files match, return an error. Never use media.play_sd or Home Assistant for generated music.",
        play_generated_properties,
        [this](const PropertyList& properties) -> ReturnValue {
            if (IsGenerationFollowupBlocked()) return ErrorResult("wait_for_next_user_request");
            const std::string query = properties["query"].value<std::string>();
            std::string error;
            if (!MediaPlayer::GetInstance().PlayGenerated(query, error))
                return ErrorResult(error.empty() ? "generated_playback_failed" : error);
            cJSON* result = cJSON_CreateObject();
            cJSON_AddBoolToObject(result, "success", true);
            cJSON_AddStringToObject(result, "source", "generated");
            cJSON_AddStringToObject(result, "query", query.c_str());
            cJSON_AddStringToObject(result, "title", MediaPlayer::GetInstance().GetTitle().c_str());
            return result;
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
