#include "weather_service.h"

#include "board.h"
#include "mcp_server.h"
#include "media/internet_radio_player.h"
#include <cJSON.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstdio>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <memory>
#include <string>
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
constexpr uint32_t kWeatherTaskStackBytes = 8192;
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

void LogMemory(const char* phase) {
    ESP_LOGI(kTag,
             "WEATHER_MEM phase=%s internal_free=%u internal_min=%u largest_block=%u psram_free=%u",
             phase,
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
             static_cast<unsigned>(
                 heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
             static_cast<unsigned>(
                 heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)));
}

bool JsonNumber(cJSON* parent, const char* key, double& value) {
    auto item = cJSON_GetObjectItemCaseSensitive(parent, key);
    if (!cJSON_IsNumber(item)) return false;
    value = item->valuedouble;
    return true;
}

bool JsonArrayNumber(cJSON* parent, const char* key, size_t index, double& value) {
    auto array = cJSON_GetObjectItemCaseSensitive(parent, key);
    auto item = cJSON_IsArray(array) ? cJSON_GetArrayItem(array, static_cast<int>(index)) : nullptr;
    if (!cJSON_IsNumber(item)) return false;
    value = item->valuedouble;
    return true;
}

uint16_t MapOpenMeteoCode(uint16_t code) {
    switch (code) {
        case 0: return 1000;                         // clear
        case 1:
        case 2: return 1003;                         // partly cloudy
        case 3: return 1009;                         // overcast
        case 45:
        case 48: return 1030;                        // fog
        case 51:
        case 53:
        case 55:
        case 56:
        case 57: return 1150;                        // drizzle
        case 61:
        case 63:
        case 80:
        case 81: return 1183;                        // rain/showers
        case 65:
        case 82: return 1195;                        // heavy rain/showers
        case 71:
        case 73:
        case 75:
        case 77:
        case 85:
        case 86: return 1066;                        // snow
        case 95:
        case 96:
        case 99: return 1087;                        // thunderstorm
        default: return 1009;
    }
}

bool ParseOpenMeteoResponse(cJSON* root, WeatherData& parsed) {
    auto current = cJSON_GetObjectItemCaseSensitive(root, "current");
    auto daily = cJSON_GetObjectItemCaseSensitive(root, "daily");
    if (!cJSON_IsObject(current) || !cJSON_IsObject(daily)) return false;

    double temp = 0, code = 0, wind = 0, direction = 0, is_day = 0;
    if (!JsonNumber(current, "temperature_2m", temp) ||
        !JsonNumber(current, "weather_code", code) ||
        !JsonNumber(current, "wind_speed_10m", wind) ||
        !JsonNumber(current, "wind_direction_10m", direction) ||
        !JsonNumber(current, "is_day", is_day)) {
        return false;
    }
    if (code < 0 || code > UINT16_MAX || direction < 0 || direction > UINT16_MAX) return false;

    parsed.current = {static_cast<float>(temp), static_cast<float>(wind),
                      static_cast<uint16_t>(direction),
                      MapOpenMeteoCode(static_cast<uint16_t>(code)), is_day != 0};
    for (size_t i = 0; i < 3; ++i) {
        double min = 0, max = 0, daily_code = 0, rain = 0;
        if (!JsonArrayNumber(daily, "temperature_2m_min", i, min) ||
            !JsonArrayNumber(daily, "temperature_2m_max", i, max) ||
            !JsonArrayNumber(daily, "weather_code", i, daily_code) ||
            !JsonArrayNumber(daily, "precipitation_probability_max", i, rain) ||
            daily_code < 0 || daily_code > UINT16_MAX || rain < 0 || rain > 100) {
            return false;
        }
        parsed.days[i] = {static_cast<float>(min), static_cast<float>(max),
                          MapOpenMeteoCode(static_cast<uint16_t>(daily_code)),
                          static_cast<uint8_t>(rain)};
    }
    return true;
}
}  // namespace

const WeatherLocation kFnkWeatherLocation = {54.3520f, 18.6466f, "Гданьск"};

WeatherService& WeatherService::GetInstance() {
    static WeatherService instance;
    return instance;
}

void WeatherService::RegisterMcpTool() {
    McpServer::GetInstance().AddTool(
        "weather.get_current",
        "Returns the current weather data already cached by this device and shown in its weather UI. "
        "Use it when the user asks about the current local weather. This tool does not perform a new "
        "internet request or refresh the weather data; use the returned timestamp and age to describe "
        "how current the result is.",
        PropertyList(),
        [this](const PropertyList&) -> ReturnValue {
            const WeatherData data = GetSnapshot();
            if (!data.valid) {
                ESP_LOGI(kTag, "WEATHER_MCP_CALL valid=0");
                cJSON* result = cJSON_CreateObject();
                cJSON_AddBoolToObject(result, "valid", false);
                cJSON_AddStringToObject(result, "reason", "weather_not_available");
                return result;
            }

            const time_t now = time(nullptr);
            const int64_t age_seconds =
                now >= data.last_successful_update ? now - data.last_successful_update : 0;
            const bool stale = data.stale || data.last_request_failed;
            ESP_LOGI(kTag, "WEATHER_MCP_CALL valid=1 stale=%d age_seconds=%lld",
                     stale ? 1 : 0, static_cast<long long>(age_seconds));

            cJSON* result = cJSON_CreateObject();
            cJSON_AddBoolToObject(result, "valid", true);
            cJSON_AddStringToObject(result, "location", kFnkWeatherLocation.name);
            cJSON_AddNumberToObject(result, "temperature_c", data.current.temperature);
            cJSON_AddNumberToObject(result, "wind_speed_mps", data.current.wind_speed);
            cJSON_AddNumberToObject(result, "wind_direction_deg", data.current.wind_direction);
            cJSON_AddNumberToObject(result, "weather_code", data.current.weather_code);
            cJSON_AddStringToObject(result, "condition",
                                    DescriptionForCode(data.current.weather_code));
            cJSON_AddBoolToObject(result, "is_day", data.current.is_day);
            cJSON_AddNumberToObject(result, "updated_at",
                                    static_cast<double>(data.last_successful_update));
            cJSON_AddNumberToObject(result, "age_seconds", static_cast<double>(age_seconds));
            cJSON_AddBoolToObject(result, "stale", stale);
            return result;
        });
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
    LogMemory("before_task_create");
    if (xTaskCreate(RefreshTask, "weather_http", kWeatherTaskStackBytes, this,
                    kWeatherTaskPriority, nullptr) != pdPASS) {
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
    LogMemory("after_task_create");
    auto* service = static_cast<WeatherService*>(arg);
    service->RunRefresh();
    // ESP-IDF 6.1 on ESP32-S3 reports uxTaskGetStackHighWaterMark() in bytes.
    const uint32_t unused_bytes = static_cast<uint32_t>(uxTaskGetStackHighWaterMark(nullptr));
    const uint32_t max_used_bytes = unused_bytes < kWeatherTaskStackBytes
                                        ? kWeatherTaskStackBytes - unused_bytes
                                        : 0;
    ESP_LOGI(kTag,
             "WEATHER_STACK configured_bytes=%u unused_bytes=%u max_used_bytes=%u",
             static_cast<unsigned>(kWeatherTaskStackBytes), static_cast<unsigned>(unused_bytes),
             static_cast<unsigned>(max_used_bytes));
    LogMemory("after_run_before_delete");
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
    LogMemory("run_start");
    char url[512];
    snprintf(url, sizeof(url),
             "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f&"
             "current=temperature_2m,weather_code,wind_speed_10m,wind_direction_10m,is_day&"
             "daily=temperature_2m_min,temperature_2m_max,weather_code,"
             "precipitation_probability_max&forecast_days=3&timezone=auto&wind_speed_unit=ms",
             kFnkWeatherLocation.latitude, kFnkWeatherLocation.longitude);
    LogMemory("before_http");
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
                LogMemory("after_http_body");
                if (!failure) failure = bytes_read < 0 ? "read_error" : nullptr;
            } else {
                failure = status == 200 ? "response_too_large" : "http_status";
            }
            http->Close();
        }
    }
    ESP_LOGI(kTag, "WEATHER_HTTP_RESULT status=%d bytes=%u elapsed_ms=%lld", status,
             static_cast<unsigned>(body.size()), (esp_timer_get_time() - started) / 1000);
    ESP_LOGI(kTag, "OPEN_METEO_BODY_BYTES=%u", static_cast<unsigned>(body.size()));

    WeatherData parsed{};
    if (!failure) {
        LogMemory("before_json_parse");
        std::unique_ptr<cJSON, decltype(&cJSON_Delete)> json(
            cJSON_ParseWithLength(body.data(), body.size()), &cJSON_Delete);
        LogMemory("after_json_parse");
        const bool ok = json && ParseOpenMeteoResponse(json.get(), parsed);
        LogMemory("after_extract");
        if (ok) {
            parsed.valid = true;
            parsed.last_successful_update = time(nullptr);
            ESP_LOGI(kTag,
                     "WEATHER_PARSE_OK provider=open_meteo temp=%.1f code=%u wind=%.1f tomorrow=%.1f..%.1f "
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
        json.reset();
        std::string().swap(body);
        LogMemory("after_body_release");
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
    LogMemory("after_update");
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
