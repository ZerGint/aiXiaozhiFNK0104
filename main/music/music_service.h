#pragma once

#include <cstdint>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <mutex>
#include <atomic>
#include <string>

#include "music_bridge_client.h"
#include "mcp_server.h"

class MusicService {
public:
    static constexpr size_t kMaxTitleLength = 96;
    static constexpr size_t kMaxStyleLength = 256;
    static constexpr size_t kMaxLyricsLength = 2048;
    static constexpr size_t kMaxProviderLength = 32;
    static constexpr uint32_t kPollIntervalMs = 20000;
    static constexpr uint32_t kDiscoveryRetryIntervalMs = 30000;

    static MusicService& GetInstance();

    void Initialize();
    void RegisterMcpTools();
    bool SetBridgeUrl(const std::string& url, std::string& error);
    std::string GetBridgeUrl() const;
    bool CanStartGeneratedDownload() const;
    bool TryBeginGeneratedDownload();
    void EndGeneratedDownload();
    bool IsGeneratedDownloadBusy() const { return generated_download_busy_.load(); }
    void ConfirmGeneratedDownload(const std::string& job_id);
    void DeclineGeneratedDownload(const std::string& job_id);
    bool RequestGeneratedDownload(const std::string& job_id, std::string& error);
    void BeginVoiceConversation();
    bool IsGenerationFollowupBlocked() const;
    void BlockGenerationFollowups();

private:
    struct PendingJob {
        bool active = false;
        std::string job_id;
        std::string title;
        std::string status;
        size_t size = 0;
        double duration = 0;
        std::string filename;
        std::string download_status;
        std::string local_filename;
        size_t downloaded_size = 0;
        uint32_t download_attempts = 0;
        int64_t next_download_at = 0;
        std::string error;
        bool library_source = false;
        bool user_library_source = false;
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
    bool PersistLocked();
    bool AddPendingJob(const MusicGenerateResult& generated, const std::string& title);
    void ApplyStatus(const MusicJobStatus& status);
    void LoadLocalIndex();
    void ProcessReadyDownloads();
    bool DownloadReadyJob(const PendingJob& job);
    void ShowReadyPrompt(const PendingJob& job);
    std::string SearchBridgeLibrary(const std::string& query, std::string& error);
    std::string SearchUserBridgeLibrary(const std::string& query, std::string& error);
    bool RequestLibraryDownload(const std::string& track_id, bool user_library, std::string& error);
    void RecordDownloadFailure(const std::string& job_id, const std::string& reason);
    bool NetworkSafeForDownload() const;
    const char* DownloadBlockReason(bool include_gate = true) const;
    void ShowDownloadOverlay(const char* title, int percent);
    void HideDownloadOverlay();
    static bool SafeFilename(const std::string& remote, const std::string& job_id, std::string& output);
    PendingJob FindJobLocked(const std::string& job_id) const;
    PendingJob FindLatestJobLocked() const;
    static bool IsTerminal(const std::string& status);
    static bool ValidateText(const std::string& value, size_t max_length);
    static bool NormalizeUrl(const std::string& input, std::string& output);
    static std::string ErrorResult(const std::string& error);
    static std::string GenerateResultJson(const MusicGenerateResult& result);
    static std::string StatusResultJson(const PendingJob& job);

    mutable std::mutex mutex_;
    std::string bridge_url_;
    std::string runtime_bridge_url_;
    int64_t last_discovery_attempt_ms_ = 0;
    bool mdns_initialized_ = false;
    PendingJob job_{};
    TaskHandle_t poll_task_ = nullptr;
    bool initialized_ = false;
    std::atomic<bool> generated_download_busy_{false};
    bool generation_followup_blocked_ = false;
    bool download_requested_ = false;
    std::string prompt_shown_job_id_;
    std::string last_deferred_reason_;
};
