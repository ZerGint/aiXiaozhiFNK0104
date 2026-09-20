#pragma once

#include <cstdint>
#include <ctime>
#include <freertos/FreeRTOS.h>

enum class WeatherIcon : uint8_t { Clear, PartlyCloudy, Cloudy, Fog, Rain, Storm, Snow };

struct WeatherLocation {
    float latitude;
    float longitude;
    const char* name;
};

struct WeatherCurrent {
    float temperature;
    float wind_speed;
    uint16_t wind_direction;
    uint16_t weather_code;
    bool is_day;
};

struct WeatherDay {
    float temp_min;
    float temp_max;
    uint16_t weather_code;
    uint8_t precipitation_probability;
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

    static WeatherService& GetInstance();
    void Initialize();
    void SetNetworkConnected(bool connected);
    void SetApplicationIdle(bool idle);
    void SetAudioReady(bool ready);
    void Tick();
    bool Refresh(const char* reason);
    WeatherData GetSnapshot() const;

    static WeatherIcon IconForCode(int code);
    static const char* DescriptionForCode(int code);
    static void FormatWindDirection(uint16_t degrees, char* output, size_t output_size);
    static void FormatDate(time_t value, char* output, size_t output_size);

private:
    WeatherService() = default;
    static void RefreshTask(void* arg);
    void RunRefresh();
    bool LoadCache();
    void SaveCache(const WeatherData& data) const;

    mutable portMUX_TYPE lock_ = portMUX_INITIALIZER_UNLOCKED;
    WeatherData data_{};
    bool initialized_ = false;
    bool network_connected_ = false;
    bool application_idle_ = false;
    bool audio_ready_ = false;
    bool pending_refresh_ = false;
    bool time_wait_logged_ = false;
    uint32_t last_attempt_ms_ = 0;
};

extern const WeatherLocation kFnkWeatherLocation;
