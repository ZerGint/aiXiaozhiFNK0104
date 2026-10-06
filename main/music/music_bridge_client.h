#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <functional>
#include <vector>

struct MusicGenerateResult {
    bool success = false;
    std::string job_id;
    std::string status;
    std::string error;
};

struct MusicJobStatus {
    bool success = false;
    std::string job_id;
    std::string status;
    std::string title;
    std::string progress;
    std::string audio_url;
    std::string format;
    size_t size = 0;
    double duration = 0;
    std::string filename;
    std::string error;
};

struct MusicLibraryTrack {
    std::string id;
    std::string title;
    std::string filename;
    std::string provider;
    std::string created_at;
    size_t size = 0;
    double duration = 0;
};

class MusicBridgeClient {
public:
    static constexpr size_t kMaxUrlLength = 256;
    static constexpr size_t kMaxJobIdLength = 64;
    static constexpr uint32_t kRequestTimeoutMs = 8000;

    bool Submit(const std::string& base_url,
                const std::string& title,
                const std::string& style,
                const std::string& lyrics,
                const std::string& provider,
                int duration_seconds,
                MusicGenerateResult& result) const;
    bool GetJob(const std::string& base_url,
                const std::string& job_id,
                MusicJobStatus& result) const;
    bool ListLibrary(const std::string& base_url,
                     std::vector<MusicLibraryTrack>& result,
                     std::string& error) const;
    bool GetLibraryTrack(const std::string& base_url,
                         const std::string& track_id,
                         MusicLibraryTrack& result,
                         std::string& error) const;
    bool DownloadAudio(const std::string& base_url,
                       const std::string& job_id,
                       size_t expected_size,
                       const std::string& output_path,
                       size_t& downloaded_size,
                       std::string& error,
                       const std::function<void(size_t, size_t)>& progress = {}) const;
    bool DownloadLibraryAudio(const std::string& base_url,
                              const std::string& track_id,
                              size_t expected_size,
                              const std::string& output_path,
                              size_t& downloaded_size,
                              std::string& error,
                              const std::function<void(size_t, size_t)>& progress = {}) const;
    bool Health(const std::string& base_url, std::string& error) const;

private:
    bool Request(const std::string& url,
                 int method,
                 const std::string& post_data,
                 int& status_code,
                 std::string& response_body,
                 std::string& error) const;
    bool DownloadAudioEndpoint(const std::string& url,
                               const std::string& id,
                               size_t expected_size,
                               const std::string& output_path,
                               size_t& downloaded_size,
                               std::string& error,
                               const std::function<void(size_t, size_t)>& progress) const;
};
