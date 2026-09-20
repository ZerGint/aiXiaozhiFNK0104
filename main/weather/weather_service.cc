#include "weather_service.h"

#include "weather_secrets.h"

#include "board.h"
#include "media/internet_radio_player.h"
#include "media/radio_json_framer.h"

#include <cJSON.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstdio>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>

namespace {
constexpr char kTag[] = "Weather";
constexpr size_t kMaxResponseBytes = 64 * 1024;
constexpr time_t kMinimumValidTime = 1704067200;  // 2024-01-01 UTC
constexpr char kCachePath[] = "/sdcard/weather_cache.dat";
constexpr char kCacheTmpPath[] = "/sdcard/weather_cache.tmp";
constexpr uint32_t kCacheMagic = 0x31544857;  // WTH1
constexpr uint16_t kCacheVersion = 1;
#if CONFIG_BOARD_TYPE_FREENOVE_FNK0104S
constexpr UBaseType_t kWeatherTaskPriority = tskIDLE_PRIORITY + 1;
constexpr bool kRequiresReadyGate = true;
#else
constexpr UBaseType_t kWeatherTaskPriority = 2;
constexpr bool kRequiresReadyGate = false;
#endif

struct __attribute__((packed)) WeatherCacheDay {
    float temp_min;
    float temp_max;
    uint16_t weather_code;
    uint8_t precipitation_probability;
    uint8_t reserved;
};

struct __attribute__((packed)) WeatherCacheRecord {
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    int64_t updated_at;
    float temperature;
    float wind_speed;
    uint16_t wind_direction;
    uint16_t weather_code;
    uint8_t is_day;
    uint8_t reserved[3];
    WeatherCacheDay days[3];
    uint32_t checksum;
};

uint32_t CacheChecksum(const void* data, size_t size) {
    const auto bytes = static_cast<const uint8_t*>(data);
    uint32_t hash = 2166136261U;
    for (size_t i = 0; i < size; ++i) {
        hash = (hash ^ bytes[i]) * 16777619U;
    }
    return hash;
}

bool IsTimeValid() {
    return time(nullptr) >= kMinimumValidTime;
}

void LogMemory() {
    ESP_LOGI(kTag, "WEATHER_MEM internal_free=%u largest_block=%u psram_free=%u",
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)));
}

bool JsonNumber(cJSON* parent, const char* key, double& value) {
    auto item = cJSON_GetObjectItemCaseSensitive(parent, key);
    if (!cJSON_IsNumber(item)) return false;
    value = item->valuedouble;
    return true;
}

bool ConditionCode(cJSON* parent, double& value) {
    auto condition = cJSON_GetObjectItemCaseSensitive(parent, "condition");
    return cJSON_IsObject(condition) && JsonNumber(condition, "code", value);
}

std::string_view JsonMember(std::string_view input, std::string_view key, char opener,
                            char closer) {
    bool in_string = false;
    bool escaped = false;
    size_t string_begin = 0;
    for (size_t pos = 0; pos < input.size(); ++pos) {
        const char c = input[pos];
        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                in_string = false;
                if (input.substr(string_begin, pos - string_begin) != key) continue;
                size_t value = pos + 1;
                while (value < input.size() &&
                       std::isspace(static_cast<unsigned char>(input[value]))) {
                    ++value;
                }
                if (value == input.size() || input[value++] != ':') continue;
                while (value < input.size() &&
                       std::isspace(static_cast<unsigned char>(input[value]))) {
                    ++value;
                }
                if (value == input.size() || input[value] != opener) continue;

                int depth = 1;
                bool value_string = false;
                bool value_escaped = false;
                for (size_t end = value + 1; end < input.size(); ++end) {
                    const char value_char = input[end];
                    if (value_string) {
                        if (value_escaped) value_escaped = false;
                        else if (value_char == '\\') value_escaped = true;
                        else if (value_char == '"') value_string = false;
                    } else if (value_char == '"') {
                        value_string = true;
                    } else if (value_char == opener) {
                        ++depth;
                    } else if (value_char == closer && --depth == 0) {
                        return input.substr(value, end - value + 1);
                    }
                }
                return {};
            }
            continue;
        }
        if (c == '"') {
            in_string = true;
            string_begin = pos + 1;
        }
    }
    return {};
}

bool ParseCurrent(std::string_view object, WeatherCurrent& current) {
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> json(
        cJSON_ParseWithLength(object.data(), object.size()), &cJSON_Delete);
    double temp = 0, code = 0, wind = 0, direction = 0, is_day = 0;
    if (!cJSON_IsObject(json.get()) || !JsonNumber(json.get(), "temp_c", temp) ||
        !ConditionCode(json.get(), code) || !JsonNumber(json.get(), "wind_kph", wind) ||
        !JsonNumber(json.get(), "wind_degree", direction) ||
        !JsonNumber(json.get(), "is_day", is_day)) {
        return false;
    }
    current = {static_cast<float>(temp), static_cast<float>(wind / 3.6),
               static_cast<uint16_t>(direction), static_cast<uint16_t>(code), is_day != 0};
    return true;
}

bool ParseDay(std::string_view object, WeatherDay& day) {
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> json(
        cJSON_ParseWithLength(object.data(), object.size()), &cJSON_Delete);
    double min = 0, max = 0, code = 0, rain = 0;
    if (!cJSON_IsObject(json.get()) || !JsonNumber(json.get(), "mintemp_c", min) ||
        !JsonNumber(json.get(), "maxtemp_c", max) || !ConditionCode(json.get(), code) ||
        !JsonNumber(json.get(), "daily_chance_of_rain", rain)) {
        return false;
    }
    day = {static_cast<float>(min), static_cast<float>(max), static_cast<uint16_t>(code),
           static_cast<uint8_t>(rain)};
    return true;
}
}  // namespace

const WeatherLocation kFnkWeatherLocation = {54.3520f, 18.6466f, "Гданьск"};

WeatherService& WeatherService::GetInstance() {
    static WeatherService instance;
    return instance;
}

void WeatherService::Initialize() {
    portENTER_CRITICAL(&lock_);
    if (initialized_) {
        portEXIT_CRITICAL(&lock_);
        return;
    }
    initialized_ = true;
    portEXIT_CRITICAL(&lock_);
    LoadCache();
    ESP_LOGI(kTag, "WEATHER_INIT lat=%.4f lon=%.4f city=%s", kFnkWeatherLocation.latitude,
             kFnkWeatherLocation.longitude, kFnkWeatherLocation.name);
}

void WeatherService::SetNetworkConnected(bool connected) {
    portENTER_CRITICAL(&lock_);
    network_connected_ = connected;
    if (connected) pending_refresh_ = true;
    portEXIT_CRITICAL(&lock_);
}

void WeatherService::SetApplicationIdle(bool idle) {
    portENTER_CRITICAL(&lock_);
    application_idle_ = idle;
    portEXIT_CRITICAL(&lock_);
}

void WeatherService::SetAudioReady(bool ready) {
    portENTER_CRITICAL(&lock_);
    audio_ready_ = ready;
    portEXIT_CRITICAL(&lock_);
}

void WeatherService::Tick() {
    bool due = false;
    bool pending = false;
    bool has_data = false;
    bool ready = false;
    bool log_wait = false;
    bool log_synced = false;
    const bool time_valid = IsTimeValid();
    const bool radio_active = InternetRadioPlayer::GetInstance().IsActive();
    const uint32_t now = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    portENTER_CRITICAL(&lock_);
    pending = pending_refresh_;
    has_data = data_.valid;
    ready = !kRequiresReadyGate || (application_idle_ && audio_ready_);
    due = network_connected_ && time_valid && !data_.request_active &&
          ready && !radio_active &&
          (pending || !data_.valid || now - last_attempt_ms_ >= kUpdateIntervalMs);
    if (network_connected_ && !time_valid && !time_wait_logged_) {
        time_wait_logged_ = true;
        log_wait = true;
    } else if (time_valid && time_wait_logged_) {
        time_wait_logged_ = false;
        log_synced = true;
    }
    portEXIT_CRITICAL(&lock_);
    if (log_wait) ESP_LOGI(kTag, "WEATHER_WAIT_TIME_SYNC");
    if (log_synced) {
        ESP_LOGI(kTag, "WEATHER_TIME_SYNCED timestamp=%lld", static_cast<long long>(time(nullptr)));
    }
    if (due) Refresh(pending ? "deferred" : (has_data ? "interval" : "startup"));
}

bool WeatherService::Refresh(const char* reason) {
    const bool radio_active = InternetRadioPlayer::GetInstance().IsActive();
    bool request_already_active = false;
    const bool time_valid = IsTimeValid();

    portENTER_CRITICAL(&lock_);
    if (!network_connected_) {
        portEXIT_CRITICAL(&lock_);
        return false;
    }

    const bool safe = time_valid &&
                      (!kRequiresReadyGate || (application_idle_ && audio_ready_)) &&
                      !radio_active;
    if (data_.request_active || !safe) {
        if (!pending_refresh_) {
            pending_refresh_ = true;
            ++data_.generation;
        }
        data_.refresh_deferred = true;
        data_.last_request_failed = false;
        request_already_active = data_.request_active;
        portEXIT_CRITICAL(&lock_);
        if (request_already_active) {
            ESP_LOGI(kTag, "WEATHER_REFRESH_DEFERRED reason=%s request_active=1", reason);
        } else if (radio_active) {
            ESP_LOGI(kTag, "WEATHER_REFRESH_DEFERRED reason=%s waiting_for=radio_idle", reason);
        } else if (!time_valid) {
            ESP_LOGI(kTag, "WEATHER_REFRESH_DEFERRED reason=%s waiting_for=time_sync", reason);
        } else {
            ESP_LOGI(kTag, "WEATHER_REFRESH_DEFERRED reason=%s waiting_for=ready_state", reason);
        }
        return true;
    }
    pending_refresh_ = false;
    data_.request_active = true;
    data_.refresh_deferred = false;
    last_attempt_ms_ = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    ++data_.generation;
    portEXIT_CRITICAL(&lock_);

    ESP_LOGI(kTag, "WEATHER_REFRESH_REQUEST reason=%s", reason);
    if (xTaskCreate(RefreshTask, "weather_http", 8192, this, kWeatherTaskPriority, nullptr) != pdPASS) {
        portENTER_CRITICAL(&lock_);
        data_.request_active = false;
        pending_refresh_ = true;
        data_.refresh_deferred = true;
        data_.last_request_failed = true;
        ++data_.generation;
        portEXIT_CRITICAL(&lock_);
        ESP_LOGE(kTag, "WEATHER_UPDATE_FAILED reason=task_create keeping_cached_data=%d",
                 data_.valid);
        return false;
    }
    return true;
}

void WeatherService::RefreshTask(void* arg) {
    static_cast<WeatherService*>(arg)->RunRefresh();
    vTaskDelete(nullptr);
}

void WeatherService::RunRefresh() {
    const bool radio_active = InternetRadioPlayer::GetInstance().IsActive();
    const bool time_valid = IsTimeValid();
    bool allowed = false;
    portENTER_CRITICAL(&lock_);
    allowed = network_connected_ &&
              (!kRequiresReadyGate || (application_idle_ && audio_ready_)) && time_valid &&
              !radio_active;
    if (!allowed) {
        data_.request_active = false;
        if (!pending_refresh_) {
            pending_refresh_ = true;
            ++data_.generation;
        }
        data_.refresh_deferred = true;
    }
    portEXIT_CRITICAL(&lock_);
    if (!allowed) {
        ESP_LOGI(kTag, "WEATHER_REFRESH_DEFERRED reason=admission_changed");
        return;
    }

    const int64_t started = esp_timer_get_time();
    LogMemory();
    char url[512];
    snprintf(url, sizeof(url),
             "https://api.weatherapi.com/v1/forecast.json?key=%s&q=%.4f,%.4f&days=3&"
             "aqi=no&alerts=no&lang=ru",
             WEATHER_API_KEY, kFnkWeatherLocation.latitude, kFnkWeatherLocation.longitude);
    if (WEATHER_API_KEY[0] == '\0') {
        ESP_LOGE(kTag, "WEATHER_UPDATE_FAILED reason=missing_api_key keeping_cached_data=%d",
                 data_.valid);
        portENTER_CRITICAL(&lock_);
        data_.request_active = false;
        data_.refresh_deferred = pending_refresh_;
        data_.last_request_failed = true;
        ++data_.generation;
        portEXIT_CRITICAL(&lock_);
        return;
    }
    ESP_LOGI(kTag, "WEATHER_HTTP_START");
    auto network = Board::GetInstance().GetNetwork();
    auto http = network ? network->CreateHttp(0) : nullptr;
    std::string body;
    int status = 0;
    const char* failure = "network";
    if (http) {
        http->SetTimeout(10000);
        http->SetHeader("Accept", "application/json");
        if (http->Open("GET", url)) {
            status = http->GetStatusCode();
            const size_t content_length = http->GetBodyLength();
            if (status == 200 && content_length <= kMaxResponseBytes) {
                failure = nullptr;
                if (content_length > 0) body.reserve(content_length);
                char chunk[2048];
                int bytes_read = 0;
                while ((bytes_read = http->Read(chunk, sizeof(chunk))) > 0) {
                    if (body.size() + static_cast<size_t>(bytes_read) > kMaxResponseBytes) {
                        failure = "response_too_large";
                        break;
                    }
                    body.append(chunk, static_cast<size_t>(bytes_read));
                }
                if (!failure) failure = bytes_read < 0 ? "read_error" : nullptr;
            } else {
                failure = status == 200 ? "response_too_large" : "http_status";
            }
            http->Close();
        }
    }
    ESP_LOGI(kTag, "WEATHER_HTTP_RESULT status=%d bytes=%u elapsed_ms=%lld", status,
             static_cast<unsigned>(body.size()), (esp_timer_get_time() - started) / 1000);

    WeatherData parsed{};
    if (!failure) {
        const std::string_view response(body);
        const auto current = JsonMember(response, "current", '{', '}');
        const auto forecast_days = JsonMember(response, "forecastday", '[', ']');
        int day_index = 0;
        bool ok = !current.empty() && !forecast_days.empty() &&
                  ParseCurrent(current, parsed.current) &&
                  ForEachJsonObject(forecast_days, [&](std::string_view forecast_day) {
                      if (day_index >= 3) return true;
                      const auto day = JsonMember(forecast_day, "day", '{', '}');
                      if (day.empty() || !ParseDay(day, parsed.days[day_index])) return false;
                      ++day_index;
                      return true;
                  }) &&
                  day_index == 3;
        if (ok) {
            parsed.valid = true;
            parsed.last_successful_update = time(nullptr);
            ESP_LOGI(kTag,
                     "WEATHER_PARSE_OK temp=%.1f code=%u wind=%.1f tomorrow=%.1f..%.1f "
                     "code=%u rain=%u day2=%.1f..%.1f code=%u rain=%u",
                     parsed.current.temperature, parsed.current.weather_code,
                     parsed.current.wind_speed, parsed.days[1].temp_min, parsed.days[1].temp_max,
                     parsed.days[1].weather_code, parsed.days[1].precipitation_probability,
                     parsed.days[2].temp_min, parsed.days[2].temp_max,
                     parsed.days[2].weather_code, parsed.days[2].precipitation_probability);
        } else {
            failure = "invalid_json";
            ESP_LOGE(kTag, "WEATHER_PARSE_ERROR reason=%s", failure);
        }
    }

    portENTER_CRITICAL(&lock_);
    const bool had_cache = data_.valid;
    const bool rerun_pending = pending_refresh_;
    if (parsed.valid) {
        parsed.generation = data_.generation + 1;
        parsed.refresh_deferred = rerun_pending;
        data_ = parsed;
    } else {
        data_.request_active = false;
        data_.refresh_deferred = rerun_pending;
        data_.last_request_failed = true;
        data_.stale = data_.valid;
        ++data_.generation;
    }
    portEXIT_CRITICAL(&lock_);
    if (parsed.valid) {
        SaveCache(parsed);
        ESP_LOGI(kTag, "WEATHER_UPDATE_OK timestamp=%lld",
                 static_cast<long long>(parsed.last_successful_update));
    } else {
        ESP_LOGE(kTag, "WEATHER_UPDATE_FAILED reason=%s keeping_cached_data=%d",
                 failure ? failure : "unknown", had_cache);
    }
    LogMemory();
}

bool WeatherService::LoadCache() {
    std::unique_ptr<FILE, decltype(&fclose)> file(fopen(kCachePath, "rb"), &fclose);
    if (!file) return false;

    WeatherCacheRecord record{};
    const bool complete = fread(&record, 1, sizeof(record), file.get()) == sizeof(record) &&
                          fgetc(file.get()) == EOF;
    const uint32_t expected_checksum = CacheChecksum(&record, offsetof(WeatherCacheRecord, checksum));
    if (!complete || record.magic != kCacheMagic || record.version != kCacheVersion ||
        record.size != sizeof(record) || record.checksum != expected_checksum ||
        record.updated_at < kMinimumValidTime || !std::isfinite(record.temperature) ||
        !std::isfinite(record.wind_speed)) {
        ESP_LOGW(kTag, "WEATHER_CACHE_INVALID");
        return false;
    }

    WeatherData cached{};
    cached.current = {record.temperature, record.wind_speed, record.wind_direction,
                      record.weather_code, record.is_day != 0};
    for (size_t i = 0; i < 3; ++i) {
        if (!std::isfinite(record.days[i].temp_min) ||
            !std::isfinite(record.days[i].temp_max) ||
            record.days[i].precipitation_probability > 100) {
            ESP_LOGW(kTag, "WEATHER_CACHE_INVALID");
            return false;
        }
        cached.days[i] = {record.days[i].temp_min, record.days[i].temp_max,
                          record.days[i].weather_code,
                          record.days[i].precipitation_probability};
    }
    cached.last_successful_update = static_cast<time_t>(record.updated_at);
    cached.generation = 1;
    cached.valid = true;
    cached.stale = true;

    portENTER_CRITICAL(&lock_);
    data_ = cached;
    portEXIT_CRITICAL(&lock_);
    ESP_LOGI(kTag, "WEATHER_CACHE_LOADED timestamp=%lld bytes=%u",
             static_cast<long long>(cached.last_successful_update),
             static_cast<unsigned>(sizeof(record)));
    return true;
}

void WeatherService::SaveCache(const WeatherData& data) const {
    WeatherCacheRecord record{};
    record.magic = kCacheMagic;
    record.version = kCacheVersion;
    record.size = sizeof(record);
    record.updated_at = static_cast<int64_t>(data.last_successful_update);
    record.temperature = data.current.temperature;
    record.wind_speed = data.current.wind_speed;
    record.wind_direction = data.current.wind_direction;
    record.weather_code = data.current.weather_code;
    record.is_day = data.current.is_day ? 1 : 0;
    for (size_t i = 0; i < 3; ++i) {
        record.days[i] = {data.days[i].temp_min, data.days[i].temp_max,
                          data.days[i].weather_code, data.days[i].precipitation_probability, 0};
    }
    record.checksum = CacheChecksum(&record, offsetof(WeatherCacheRecord, checksum));

    unlink(kCacheTmpPath);
    std::unique_ptr<FILE, decltype(&fclose)> file(fopen(kCacheTmpPath, "wb"), &fclose);
    if (!file || fwrite(&record, 1, sizeof(record), file.get()) != sizeof(record) ||
        fflush(file.get()) != 0) {
        ESP_LOGW(kTag, "WEATHER_CACHE_SAVE_FAILED");
        unlink(kCacheTmpPath);
        return;
    }
    file.reset();
    if ((unlink(kCachePath) != 0 && errno != ENOENT) ||
        rename(kCacheTmpPath, kCachePath) != 0) {
        ESP_LOGW(kTag, "WEATHER_CACHE_SAVE_FAILED");
        unlink(kCacheTmpPath);
        return;
    }
    ESP_LOGI(kTag, "WEATHER_CACHE_SAVED bytes=%u", static_cast<unsigned>(sizeof(record)));
}

WeatherData WeatherService::GetSnapshot() const {
    portENTER_CRITICAL(&lock_);
    WeatherData copy = data_;
    if (copy.valid && static_cast<uint32_t>(esp_timer_get_time() / 1000) - last_attempt_ms_ >=
                          kUpdateIntervalMs) {
        copy.stale = true;
    }
    portEXIT_CRITICAL(&lock_);
    return copy;
}

WeatherIcon WeatherService::IconForCode(int code) {
    if (code == 1000) return WeatherIcon::Clear;
    if (code == 1003) return WeatherIcon::PartlyCloudy;
    if (code == 1006 || code == 1009) return WeatherIcon::Cloudy;
    if (code == 1030 || code == 1135 || code == 1147) return WeatherIcon::Fog;
    if (code == 1087 || code >= 1273) return WeatherIcon::Storm;
    if (code == 1066 || code == 1069 || code == 1072 || code == 1114 || code == 1117 ||
        (code >= 1204 && code <= 1237) || (code >= 1255 && code <= 1264)) {
        return WeatherIcon::Snow;
    }
    return WeatherIcon::Rain;
}

const char* WeatherService::DescriptionForCode(int code) {
    switch (IconForCode(code)) {
        case WeatherIcon::Clear: return "Солнечно";
        case WeatherIcon::PartlyCloudy: return "Малооблачно";
        case WeatherIcon::Cloudy: return "Облачно";
        case WeatherIcon::Fog: return "Туман";
        case WeatherIcon::Rain: return "Дождь";
        case WeatherIcon::Storm: return "Гроза";
        case WeatherIcon::Snow: return "Снег";
    }
    return "Погода";
}

void WeatherService::FormatWindDirection(uint16_t degrees, char* output, size_t output_size) {
    static constexpr const char* kDirections[] = {"С", "СВ", "В", "ЮВ",
                                                  "Ю", "ЮЗ", "З", "СЗ"};
    snprintf(output, output_size, "%s", kDirections[((degrees + 22) / 45) % 8]);
}

void WeatherService::FormatDate(time_t value, char* output, size_t output_size) {
    struct tm local{};
    localtime_r(&value, &local);
    strftime(output, output_size, "%d.%m.%Y", &local);
}
