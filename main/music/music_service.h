#pragma once

#include <array>
#include <cstdint>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <mutex>
#include <string>

#include "music_bridge_client.h"
#include "mcp_server.h"

class MusicService {
public:
    static constexpr size_t kMaxTrackedJobs = 4;
    static constexpr size_t kMaxTitleLength = 96;
    static constexpr size_t kMaxStyleLength = 256;
    static constexpr size_t kMaxLyricsLength = 2048;
    static constexpr size_t kMaxProviderLength = 16;
    static constexpr uint32_t kPollIntervalMs = 20000;
    static constexpr uint32_t kDiscoveryRetryIntervalMs = 30000;

    static MusicService& GetInstance();

    void Initialize();
    void RegisterMcpTools();
    bool SetBridgeUrl(const std::string& url, std::string& error);
    std::string GetBridgeUrl() const;

private:
    struct PendingJob {
        bool active = false;
        std::string job_id;
        std::string title;
        std::string status;
        std::string progress;
        std::string audio_url;
        std::string format;
        size_t size = 0;
        std::string error;
        int64_t updated_at = 0;
    };

    MusicService() = default;
    static void PollTaskEntry(void* arg);
    void PollTask();
    void EnsurePollTask();
    void PollPendingJobs();
    std::string ResolveBridgeUrl();
    bool EnsureMdns();
    void InvalidateRuntimeUrl(const std::string& url);
    bool HasPendingJobsLocked() const;
    void LoadState();
    void PersistLocked();
    bool AddPendingJob(const MusicGenerateResult& generated, const std::string& title);
    void ApplyStatus(const MusicJobStatus& status);
    PendingJob FindJobLocked(const std::string& job_id) const;
    PendingJob FindLatestJobLocked() const;
    static bool IsTerminal(const std::string& status);
    static bool ValidateText(const std::string& value, size_t max_length);
    static bool NormalizeUrl(const std::string& input, std::string& output);
    static cJSON* JobToJson(const PendingJob& job);
    static std::string ErrorResult(const std::string& error);
    static std::string GenerateResultJson(const MusicGenerateResult& result);
    static std::string StatusResultJson(const PendingJob& job);

    mutable std::mutex mutex_;
    std::string bridge_url_;
    std::string runtime_bridge_url_;
    int64_t last_discovery_attempt_ms_ = 0;
    bool mdns_initialized_ = false;
    std::array<PendingJob, kMaxTrackedJobs> jobs_{};
    TaskHandle_t poll_task_ = nullptr;
    bool initialized_ = false;
};
