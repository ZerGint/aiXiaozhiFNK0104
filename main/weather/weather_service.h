#pragma once

#include <atomic>
#include <cstdint>
#include <ctime>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

enum class WeatherIcon : uint8_t { Clear, PartlyCloudy, Cloudy, Fog, Rain, Storm, Snow };

struct WeatherLocation {
    double latitude = 0.0;
    double longitude = 0.0;
    std::string name;
    std::string country;
    std::string country_code;
    std::string region;
    std::string timezone;
    std::string provider_id;
};

struct WeatherCityCandidate {
    std::string candidate_id;
    std::string name;
    std::string country;
    std::string country_code;
    std::string region;
    std::string timezone;
    double latitude = 0.0;
    double longitude = 0.0;
};

struct WeatherCurrent {
    float temperature = 0.0f;
    float apparent_temperature = 0.0f;
    float wind_speed = 0.0f;
    uint16_t wind_direction = 0;
    uint16_t weather_code = 0;
    uint8_t humidity = 0;
    float precipitation_mm = 0.0f;
    bool has_apparent_temperature = false;
    bool has_humidity = false;
    bool has_precipitation = false;
    bool is_day = false;
};

struct WeatherDay {
    float temp_min = 0.0f;
    float temp_max = 0.0f;
    uint16_t weather_code = 0;
    uint8_t precipitation_probability = 0;
    float precipitation_mm = 0.0f;
    bool has_precipitation = false;
};

struct WeatherData {
    WeatherCurrent current{};
    WeatherDay days[3]{};
    time_t last_successful_update = 0;
    uint32_t generation = 0;
    bool valid = false;
    bool stale = false;
    bool request_active = false;
    bool refresh_deferred = false;
    bool last_request_failed = false;
};

class WeatherService {
public:
    static constexpr uint32_t kUpdateIntervalMs = 30U * 60U * 1000U;
    static constexpr uint32_t kCandidateLifetimeMs = 10U * 60U * 1000U;

    static WeatherService& GetInstance();
    void Initialize();
    void SetNetworkConnected(bool connected);
    void SetApplicationIdle(bool idle);
    void SetAudioReady(bool ready);
    void Tick();
    bool Refresh(const char* reason);
    WeatherData GetSnapshot() const;
    WeatherLocation GetLocation() const;
    void SetLocationChangedCallback(std::function<void()> callback);

    // These methods enqueue work on the single long-lived weather worker.
    void SearchCitiesAsync(const std::string& query, const std::string& country,
                           const std::string& region,
                           std::function<void(const std::string&)> callback);
    void GetWeatherAsync(const std::string& candidate_id, uint8_t forecast_days,
                         std::function<void(const std::string&)> callback);
    bool SetLocationByCandidate(const std::string& candidate_id, std::string& error);

    void RegisterMcpTools();

    static WeatherIcon IconForCode(int code);
    static const char* DescriptionForCode(int code);
    static void FormatWindDirection(uint16_t degrees, char* output, size_t output_size);
    static void FormatDate(time_t value, char* output, size_t output_size);

private:
    WeatherService() = default;
    struct Request;

    static void WeatherWorkerTask(void* arg);
    void WorkerLoop();
    bool Enqueue(std::shared_ptr<Request> request);
    bool PopRequest(std::shared_ptr<Request>& request);
    void RunRefresh();
    void RunSearch(const std::shared_ptr<Request>& request);
    void RunOneOffWeather(const std::shared_ptr<Request>& request);
    // Tries the last working provider first, then the other one.
    bool FetchOpenMeteo(const WeatherLocation& location, uint8_t forecast_days,
                        WeatherData& parsed, std::string& error);
    bool FetchMetNo(const WeatherLocation& location, WeatherData& parsed, std::string& error);
    bool FetchWeather(const WeatherLocation& location, uint8_t forecast_days,
                      WeatherData& parsed, std::string& error);
    bool FetchGeocoding(const std::string& query, const std::string& country,
                        const std::string& region, std::vector<WeatherCityCandidate>& results,
                        std::string& error);
    bool LoadCache();
    bool SaveCache(const WeatherData& data) const;
    bool LoadLocation();
    bool SaveLocation(const WeatherLocation& location, std::string& error);
    bool ResolveCandidate(const std::string& candidate_id, WeatherLocation& location,
                          std::string& error) const;
    void CompletePersistentRefresh(const WeatherData& parsed, const char* failure,
                                   const std::string& source_location_id);

    mutable portMUX_TYPE lock_ = portMUX_INITIALIZER_UNLOCKED;
    WeatherData data_{};
    bool initialized_ = false;
    bool network_connected_ = false;
    bool application_idle_ = false;
    bool audio_ready_ = false;
    bool pending_refresh_ = false;
    bool time_wait_logged_ = false;
    uint32_t last_attempt_ms_ = 0;

    mutable std::mutex request_mutex_;
    std::vector<std::shared_ptr<Request>> requests_;
    std::atomic<uint32_t> next_request_id_{0};
    TaskHandle_t worker_task_ = nullptr;
    bool worker_start_failed_ = false;

    mutable std::mutex location_mutex_;
    WeatherLocation location_;
    std::vector<WeatherCityCandidate> recent_candidates_;
    uint32_t candidate_generation_ = 0;
    uint32_t candidate_updated_ms_ = 0;
    uint32_t location_revision_ = 0;
    std::function<void()> location_changed_callback_;
};

extern const WeatherLocation kFnkWeatherLocation;
