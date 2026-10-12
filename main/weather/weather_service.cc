#include "weather_service.h"

#include "board.h"
#include "mcp_server.h"
#include "media/internet_radio_player.h"
#include "settings.h"

#include <cJSON.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <utility>

#include <sys/stat.h>
#include <unistd.h>

namespace {
constexpr char kTag[] = "Weather";
constexpr size_t kMaxResponseBytes = 64 * 1024;
constexpr time_t kMinimumValidTime = 1704067200;
constexpr char kCachePath[] = "/sdcard/weather_cache.dat";
constexpr char kCacheTmpPath[] = "/sdcard/weather_cache.tmp";
constexpr uint32_t kCacheMagic = 0x31544857;  // WTH1
constexpr uint16_t kCacheVersion = 2;
constexpr char kSettingsNamespace[] = "weather";
constexpr size_t kMaxSearchResults = 5;
constexpr uint32_t kCandidateGenerationLimit = 1000000;
constexpr time_t kMcpCacheTtlSeconds = 5 * 60;
constexpr uint32_t kWeatherTaskStackBytes = 10240;
#if CONFIG_BOARD_TYPE_FREENOVE_FNK0104S
constexpr UBaseType_t kWeatherTaskPriority = tskIDLE_PRIORITY + 1;
constexpr bool kRequiresReadyGate = true;
#else
constexpr UBaseType_t kWeatherTaskPriority = 2;
constexpr bool kRequiresReadyGate = false;
#endif
#if CONFIG_SPIRAM
constexpr UBaseType_t kWeatherTaskMemoryCaps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
constexpr char kWeatherTaskStackLocation[] = "PSRAM";
#else
constexpr UBaseType_t kWeatherTaskMemoryCaps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
constexpr char kWeatherTaskStackLocation[] = "internal RAM";
#endif

struct __attribute__((packed)) WeatherCacheDay {
    float temp_min;
    float temp_max;
    uint16_t weather_code;
    uint8_t precipitation_probability;
    uint8_t has_precipitation;
    float precipitation_mm;
};

struct __attribute__((packed)) WeatherCacheRecord {
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    int64_t updated_at;
    float temperature;
    float apparent_temperature;
    float wind_speed;
    uint16_t wind_direction;
    uint16_t weather_code;
    uint8_t humidity;
    uint8_t is_day;
    uint8_t has_apparent_temperature;
    uint8_t has_humidity;
    uint8_t has_precipitation;
    uint8_t reserved[3];
    float precipitation_mm;
    WeatherCacheDay days[3];
    uint32_t checksum;
};

uint32_t CacheChecksum(const void* data, size_t size) {
    const auto bytes = static_cast<const uint8_t*>(data);
    uint32_t hash = 2166136261U;
    for (size_t i = 0; i < size; ++i) hash = (hash ^ bytes[i]) * 16777619U;
    return hash;
}

bool IsTimeValid() { return time(nullptr) >= kMinimumValidTime; }

void LogMemory(const char* phase) {
    ESP_LOGI(kTag,
             "WEATHER_MEM phase=%s internal_free=%u internal_largest=%u internal_min=%u "
             "dma_free=%u dma_largest=%u psram_free=%u psram_largest=%u",
             phase,
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
             static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_DMA)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_DMA)),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)));
}

bool JsonNumber(cJSON* parent, const char* key, double& value) {
    auto item = cJSON_GetObjectItemCaseSensitive(parent, key);
    if (!cJSON_IsNumber(item) || !std::isfinite(item->valuedouble)) return false;
    value = item->valuedouble;
    return true;
}

bool JsonOptionalNumber(cJSON* parent, const char* key, double& value) {
    auto item = cJSON_GetObjectItemCaseSensitive(parent, key);
    if (item == nullptr) return false;
    return JsonNumber(parent, key, value);
}

bool JsonArrayNumber(cJSON* parent, const char* key, size_t index, double& value) {
    auto array = cJSON_GetObjectItemCaseSensitive(parent, key);
    auto item = cJSON_IsArray(array) ? cJSON_GetArrayItem(array, static_cast<int>(index)) : nullptr;
    if (!cJSON_IsNumber(item) || !std::isfinite(item->valuedouble)) return false;
    value = item->valuedouble;
    return true;
}

uint16_t MapOpenMeteoCode(uint16_t code) {
    switch (code) {
        case 0: return 1000;
        case 1:
        case 2: return 1003;
        case 3: return 1009;
        case 45:
        case 48: return 1030;
        case 51:
        case 53:
        case 55:
        case 56:
        case 57: return 1150;
        case 61:
        case 63:
        case 80:
        case 81: return 1183;
        case 65:
        case 82: return 1195;
        case 71:
        case 73:
        case 75:
        case 77:
        case 85:
        case 86: return 1066;
        case 95:
        case 96:
        case 99: return 1087;
        default: return 1009;
    }
}

bool ParseOpenMeteoResponse(cJSON* root, uint8_t forecast_days, WeatherData& parsed) {
    auto current = cJSON_GetObjectItemCaseSensitive(root, "current");
    auto daily = cJSON_GetObjectItemCaseSensitive(root, "daily");
    if (!cJSON_IsObject(current) || !cJSON_IsObject(daily)) return false;

    double temp = 0, code = 0, wind = 0, direction = 0, is_day = 0;
    if (!JsonNumber(current, "temperature_2m", temp) ||
        !JsonNumber(current, "weather_code", code) ||
        !JsonNumber(current, "wind_speed_10m", wind) ||
        !JsonNumber(current, "wind_direction_10m", direction) ||
        !JsonNumber(current, "is_day", is_day) || code < 0 || code > UINT16_MAX ||
        direction < 0 || direction > UINT16_MAX) {
        return false;
    }

    parsed.current.temperature = static_cast<float>(temp);
    parsed.current.wind_speed = static_cast<float>(wind);
    parsed.current.wind_direction = static_cast<uint16_t>(direction);
    parsed.current.weather_code = MapOpenMeteoCode(static_cast<uint16_t>(code));
    parsed.current.is_day = is_day != 0;
    double value = 0;
    if (JsonOptionalNumber(current, "apparent_temperature", value)) {
        parsed.current.apparent_temperature = static_cast<float>(value);
        parsed.current.has_apparent_temperature = true;
    }
    if (JsonOptionalNumber(current, "relative_humidity_2m", value) && value >= 0 && value <= 100) {
        parsed.current.humidity = static_cast<uint8_t>(value);
        parsed.current.has_humidity = true;
    }
    if (JsonOptionalNumber(current, "precipitation", value) && value >= 0) {
        parsed.current.precipitation_mm = static_cast<float>(value);
        parsed.current.has_precipitation = true;
    }

    auto min_array = cJSON_GetObjectItemCaseSensitive(daily, "temperature_2m_min");
    auto max_array = cJSON_GetObjectItemCaseSensitive(daily, "temperature_2m_max");
    auto code_array = cJSON_GetObjectItemCaseSensitive(daily, "weather_code");
    auto rain_array = cJSON_GetObjectItemCaseSensitive(daily, "precipitation_probability_max");
    if (!cJSON_IsArray(min_array) || !cJSON_IsArray(max_array) || !cJSON_IsArray(code_array) ||
        !cJSON_IsArray(rain_array)) {
        return false;
    }

    const uint8_t days = std::min<uint8_t>(forecast_days, 3);
    for (size_t i = 0; i < days; ++i) {
        double min = 0, max = 0, daily_code = 0, rain = 0;
        if (!JsonArrayNumber(daily, "temperature_2m_min", i, min) ||
            !JsonArrayNumber(daily, "temperature_2m_max", i, max) ||
            !JsonArrayNumber(daily, "weather_code", i, daily_code) ||
            !JsonArrayNumber(daily, "precipitation_probability_max", i, rain) ||
            daily_code < 0 || daily_code > UINT16_MAX || rain < 0 || rain > 100) {
            return false;
        }
        parsed.days[i].temp_min = static_cast<float>(min);
        parsed.days[i].temp_max = static_cast<float>(max);
        parsed.days[i].weather_code = MapOpenMeteoCode(static_cast<uint16_t>(daily_code));
        parsed.days[i].precipitation_probability = static_cast<uint8_t>(rain);
        if (JsonArrayNumber(daily, "precipitation_sum", i, value) && value >= 0) {
            parsed.days[i].precipitation_mm = static_cast<float>(value);
            parsed.days[i].has_precipitation = true;
        }
    }
    return true;
}

std::string JsonError(const char* reason, const char* detail = nullptr) {
    cJSON* result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "ok", false);
    cJSON_AddStringToObject(result, "error", reason ? reason : "weather_error");
    if (detail && *detail) cJSON_AddStringToObject(result, "detail", detail);
    char* text = cJSON_PrintUnformatted(result);
    std::string output = text ? text : "{\"ok\":false,\"error\":\"weather_error\"}";
    if (text) cJSON_free(text);
    cJSON_Delete(result);
    return output;
}

std::string JsonText(cJSON* root) {
    char* text = cJSON_PrintUnformatted(root);
    std::string output = text ? text : "{}";
    if (text) cJSON_free(text);
    cJSON_Delete(root);
    return output;
}

std::string UrlEncode(const std::string& input) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string output;
    output.reserve(input.size() + 16);
    for (unsigned char ch : input) {
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.' || ch == '~') {
            output.push_back(static_cast<char>(ch));
        } else {
            output.push_back('%');
            output.push_back(kHex[ch >> 4]);
            output.push_back(kHex[ch & 0x0F]);
        }
    }
    return output;
}

std::string LanguageForQuery(const std::string& query) {
    if (query.find("а") != std::string::npos || query.find("я") != std::string::npos ||
        query.find("Р") != std::string::npos) {
        return "ru";
    }
    if (query.find("ą") != std::string::npos || query.find("ć") != std::string::npos ||
        query.find("ę") != std::string::npos || query.find("ł") != std::string::npos ||
        query.find("ń") != std::string::npos || query.find("ó") != std::string::npos ||
        query.find("ś") != std::string::npos || query.find("ź") != std::string::npos ||
        query.find("ż") != std::string::npos) {
        return "pl";
    }
    return "en";
}

bool ReadHttpJson(const char* operation, const std::string& url, std::string& body,
                  std::string& error) {
    const int64_t start_us = esp_timer_get_time();
    ESP_LOGI(kTag, "WEATHER_HTTP_START op=%s", operation);
    LogMemory("http_before_open");
    auto network = Board::GetInstance().GetNetwork();
    auto http = network ? network->CreateHttp(0) : nullptr;
    if (!http) {
        error = "network_unavailable";
        ESP_LOGW(kTag, "WEATHER_HTTP_END op=%s ok=0 error=%s elapsed_ms=%u", operation,
                 error.c_str(), static_cast<unsigned>((esp_timer_get_time() - start_us) / 1000));
        return false;
    }
    http->SetTimeout(10000);
    http->SetHeader("Accept", "application/json");
    if (!http->Open("GET", url)) {
        error = "http_open_failed";
        http.reset();
        ESP_LOGW(kTag, "WEATHER_HTTP_RELEASE op=%s opened=0 elapsed_ms=%u", operation,
                 static_cast<unsigned>((esp_timer_get_time() - start_us) / 1000));
        return false;
    }
    const int64_t opened_us = esp_timer_get_time();
    const int status = http->GetStatusCode();
    const size_t content_length = http->GetBodyLength();
    ESP_LOGI(kTag, "WEATHER_HTTP_HEADERS op=%s status=%d content_length=%u elapsed_ms=%u", operation,
             status, static_cast<unsigned>(content_length),
             static_cast<unsigned>((opened_us - start_us) / 1000));
    if (status != 200) {
        error = "http_status";
        http->Close();
        http.reset();
        ESP_LOGI(kTag, "WEATHER_HTTP_RELEASE op=%s elapsed_ms=%u", operation,
                 static_cast<unsigned>((esp_timer_get_time() - start_us) / 1000));
        return false;
    }
    if (content_length > kMaxResponseBytes) {
        error = "response_too_large";
        http->Close();
        http.reset();
        ESP_LOGI(kTag, "WEATHER_HTTP_RELEASE op=%s elapsed_ms=%u", operation,
                 static_cast<unsigned>((esp_timer_get_time() - start_us) / 1000));
        return false;
    }
    body.clear();
    if (content_length > 0) body.reserve(content_length);
    char chunk[2048];
    int bytes_read = 0;
    while ((bytes_read = http->Read(chunk, sizeof(chunk))) > 0) {
        if (body.size() + static_cast<size_t>(bytes_read) > kMaxResponseBytes) {
            error = "response_too_large";
            break;
        }
        body.append(chunk, static_cast<size_t>(bytes_read));
    }
    http->Close();
    const int64_t close_us = esp_timer_get_time();
    http.reset();
    LogMemory("http_after_release");
    ESP_LOGI(kTag, "WEATHER_HTTP_RELEASE op=%s bytes=%u elapsed_ms=%u", operation,
             static_cast<unsigned>(body.size()),
             static_cast<unsigned>((close_us - start_us) / 1000));
    if (!error.empty()) {
        ESP_LOGW(kTag, "WEATHER_HTTP_END op=%s ok=0 error=%s elapsed_ms=%u", operation,
                 error.c_str(), static_cast<unsigned>((close_us - start_us) / 1000));
        return false;
    }
    if (bytes_read < 0) {
        error = "http_read_failed";
        ESP_LOGW(kTag, "WEATHER_HTTP_END op=%s ok=0 error=%s elapsed_ms=%u", operation,
                 error.c_str(), static_cast<unsigned>((close_us - start_us) / 1000));
        return false;
    }
    ESP_LOGI(kTag, "WEATHER_HTTP_END op=%s ok=1 bytes=%u elapsed_ms=%u", operation,
             static_cast<unsigned>(body.size()),
             static_cast<unsigned>((close_us - start_us) / 1000));
    return true;
}

void AddLocationJson(cJSON* result, const WeatherLocation& location) {
    cJSON_AddStringToObject(result, "name", location.name.c_str());
    cJSON_AddStringToObject(result, "country", location.country.c_str());
    cJSON_AddStringToObject(result, "country_code", location.country_code.c_str());
    cJSON_AddStringToObject(result, "region", location.region.c_str());
    cJSON_AddStringToObject(result, "timezone", location.timezone.c_str());
    cJSON_AddStringToObject(result, "location_id", location.provider_id.c_str());
    cJSON_AddNumberToObject(result, "latitude", location.latitude);
    cJSON_AddNumberToObject(result, "longitude", location.longitude);
}

void AddWeatherJson(cJSON* result, const WeatherLocation& location, const WeatherData& data,
                    uint8_t forecast_days) {
    AddLocationJson(result, location);
    cJSON_AddNumberToObject(result, "updated_at", static_cast<double>(data.last_successful_update));
    cJSON_AddNumberToObject(result, "temperature_c", data.current.temperature);
    cJSON_AddNumberToObject(result, "weather_code", data.current.weather_code);
    cJSON_AddStringToObject(result, "condition", WeatherService::DescriptionForCode(data.current.weather_code));
    cJSON_AddNumberToObject(result, "wind_speed_mps", data.current.wind_speed);
    cJSON_AddNumberToObject(result, "wind_direction_deg", data.current.wind_direction);
    cJSON_AddBoolToObject(result, "is_day", data.current.is_day);
    if (data.current.has_apparent_temperature)
        cJSON_AddNumberToObject(result, "feels_like_c", data.current.apparent_temperature);
    if (data.current.has_humidity)
        cJSON_AddNumberToObject(result, "humidity_percent", data.current.humidity);
    if (data.current.has_precipitation)
        cJSON_AddNumberToObject(result, "precipitation_mm", data.current.precipitation_mm);
    cJSON* forecast = cJSON_AddArrayToObject(result, "forecast");
    for (uint8_t i = 0; i < std::min<uint8_t>(forecast_days, 3); ++i) {
        const auto& day = data.days[i];
        cJSON* item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "day_offset", i);
        cJSON_AddNumberToObject(item, "temperature_min_c", day.temp_min);
        cJSON_AddNumberToObject(item, "temperature_max_c", day.temp_max);
        cJSON_AddNumberToObject(item, "weather_code", day.weather_code);
        cJSON_AddStringToObject(item, "condition", WeatherService::DescriptionForCode(day.weather_code));
        cJSON_AddNumberToObject(item, "precipitation_probability_percent", day.precipitation_probability);
        if (day.has_precipitation) cJSON_AddNumberToObject(item, "precipitation_mm", day.precipitation_mm);
        cJSON_AddItemToArray(forecast, item);
    }
}
}  // namespace

struct WeatherService::Request {
    enum class Kind { Refresh, Search, OneOffWeather } kind = Kind::Refresh;
    uint32_t id = 0;
    std::string query;
    std::string country;
    std::string region;
    std::string candidate_id;
    uint8_t forecast_days = 3;
    std::function<void(const std::string&)> callback;
};

const WeatherLocation kFnkWeatherLocation = {54.3520, 18.6466, "Гданьск", "Poland", "PL",
                                              "Pomeranian Voivodeship", "Europe/Warsaw", "default-gdansk"};

WeatherService& WeatherService::GetInstance() {
    static WeatherService instance;
    return instance;
}

void WeatherService::RegisterMcpTools() {
    auto& mcp = McpServer::GetInstance();
    mcp.AddAsyncTool(
        "self.weather",
        "Weather operations. Set action to one of: search_city, get_city, set_city, get_weather. "
        "For search_city, query is required and the result contains verified candidate_id values; "
        "never invent coordinates or IDs. For set_city, use a recent candidate_id returned by "
        "search_city; it is persisted in NVS and becomes the Weather screen location. For get_city, "
        "return the persistent city. For get_weather, omit candidate_id for the persistent city or "
        "provide a verified candidate_id for a one-off forecast; forecast_days is 1..3. "
        "The tool preserves ambiguous-city clarification and the existing Open-Meteo forecast flow.",
        PropertyList({Property("action", kPropertyTypeString),
                      Property("query", kPropertyTypeString, ""),
                      Property("country", kPropertyTypeString, ""),
                      Property("region", kPropertyTypeString, ""),
                      Property("candidate_id", kPropertyTypeString, ""),
                      Property("forecast_days", kPropertyTypeInteger, 3, 1, 3)}),
        [this](const PropertyList& properties, AsyncMcpCompletion completion) mutable {
            const std::string action = properties["action"].value<std::string>();
            static std::atomic<uint32_t> next_call_id{0};
            const uint32_t call_id = ++next_call_id;
            ESP_LOGI(kTag, "WEATHER_MCP_START call=%u action=%s", static_cast<unsigned>(call_id),
                     action.c_str());
            if (action == "search_city") {
                auto complete_async = [call_id, action, completion = std::move(completion)](
                                           const std::string& result) mutable {
                    ESP_LOGI(kTag, "WEATHER_MCP_RESULT_READY call=%u action=%s bytes=%u",
                             static_cast<unsigned>(call_id), action.c_str(),
                             static_cast<unsigned>(result.size()));
                    completion(result);
                    ESP_LOGI(kTag, "WEATHER_MCP_RESULT_RETURNED call=%u action=%s",
                             static_cast<unsigned>(call_id), action.c_str());
                };
                SearchCitiesAsync(
                    properties["query"].value<std::string>(),
                    properties["country"].value<std::string>(),
                    properties["region"].value<std::string>(),
                    std::move(complete_async));
                return;
            }
            if (action == "get_weather") {
                auto complete_async = [call_id, action, completion = std::move(completion)](
                                           const std::string& result) mutable {
                    ESP_LOGI(kTag, "WEATHER_MCP_RESULT_READY call=%u action=%s bytes=%u",
                             static_cast<unsigned>(call_id), action.c_str(),
                             static_cast<unsigned>(result.size()));
                    completion(result);
                    ESP_LOGI(kTag, "WEATHER_MCP_RESULT_RETURNED call=%u action=%s",
                             static_cast<unsigned>(call_id), action.c_str());
                };
                GetWeatherAsync(
                    properties["candidate_id"].value<std::string>(),
                    static_cast<uint8_t>(properties["forecast_days"].value<int>()),
                    std::move(complete_async));
                return;
            }
            if (action == "get_city") {
                cJSON* result = cJSON_CreateObject();
                cJSON_AddBoolToObject(result, "ok", true);
                AddLocationJson(result, GetLocation());
                completion(result);
                ESP_LOGI(kTag, "WEATHER_MCP_RESULT_RETURNED call=%u action=%s",
                         static_cast<unsigned>(call_id), action.c_str());
                return;
            }
            if (action == "set_city") {
                std::string error;
                const bool ok = SetLocationByCandidate(properties["candidate_id"].value<std::string>(), error);
                cJSON* result = cJSON_CreateObject();
                cJSON_AddBoolToObject(result, "ok", ok);
                if (ok) {
                    cJSON_AddStringToObject(result, "message", "weather_city_changed");
                    AddLocationJson(result, GetLocation());
                } else {
                    cJSON_AddStringToObject(result, "error", error.c_str());
                }
                completion(result);
                ESP_LOGI(kTag, "WEATHER_MCP_RESULT_RETURNED call=%u action=%s",
                         static_cast<unsigned>(call_id), action.c_str());
                return;
            }
            ESP_LOGW(kTag, "WEATHER_MCP_RESULT_READY call=%u action=%s error=invalid_action",
                     static_cast<unsigned>(call_id), action.c_str());
            completion(JsonError("invalid_action"));
            ESP_LOGI(kTag, "WEATHER_MCP_RESULT_RETURNED call=%u action=%s",
                     static_cast<unsigned>(call_id), action.c_str());
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
    LoadLocation();
    LoadCache();
    if (xTaskCreateWithCaps(WeatherWorkerTask, "weather_worker", kWeatherTaskStackBytes, this,
                            kWeatherTaskPriority, &worker_task_,
                            kWeatherTaskMemoryCaps) != pdPASS) {
        worker_start_failed_ = true;
        ESP_LOGE(kTag, "WEATHER_WORKER_CREATE_FAILED");
    } else {
        ESP_LOGI(kTag, "WEATHER_WORKER_READY stack_caps=%s stack_bytes=%u",
                 kWeatherTaskStackLocation, static_cast<unsigned>(kWeatherTaskStackBytes));
    }
    const auto location = GetLocation();
    ESP_LOGI(kTag, "WEATHER_INIT lat=%.4f lon=%.4f city=%s country=%s", location.latitude,
             location.longitude, location.name.c_str(), location.country.c_str());
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
    due = network_connected_ && time_valid && !data_.request_active && ready && !radio_active &&
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
    if (log_synced) ESP_LOGI(kTag, "WEATHER_TIME_SYNCED timestamp=%lld", static_cast<long long>(time(nullptr)));
    if (due) Refresh(pending ? "deferred" : (has_data ? "interval" : "startup"));
}

bool WeatherService::Enqueue(std::shared_ptr<Request> request) {
    if (!request) return false;
    if (worker_task_ == nullptr) {
        if (worker_start_failed_) return false;
        Initialize();
    }
    if (worker_task_ == nullptr) return false;
    {
        std::lock_guard<std::mutex> lock(request_mutex_);
        if (requests_.size() >= 4) return false;
        request->id = ++next_request_id_;
        requests_.push_back(std::move(request));
    }
    xTaskNotifyGive(worker_task_);
    return true;
}

bool WeatherService::PopRequest(std::shared_ptr<Request>& request) {
    std::lock_guard<std::mutex> lock(request_mutex_);
    if (requests_.empty()) return false;
    request = std::move(requests_.front());
    requests_.erase(requests_.begin());
    return true;
}

void WeatherService::WeatherWorkerTask(void* arg) {
    static_cast<WeatherService*>(arg)->WorkerLoop();
    vTaskDeleteWithCaps(nullptr);
}

void WeatherService::WorkerLoop() {
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        std::shared_ptr<Request> request;
        while (PopRequest(request)) {
            const char* kind = request->kind == Request::Kind::Refresh
                                   ? "refresh"
                                   : (request->kind == Request::Kind::Search ? "search" : "one_off");
            const int64_t start_us = esp_timer_get_time();
            ESP_LOGI(kTag, "WEATHER_WORKER_START id=%u kind=%s", static_cast<unsigned>(request->id),
                     kind);
            if (request->kind == Request::Kind::Refresh) RunRefresh();
            else if (request->kind == Request::Kind::Search) RunSearch(request);
            else RunOneOffWeather(request);
            ESP_LOGI(kTag, "WEATHER_WORKER_END id=%u kind=%s elapsed_ms=%u",
                     static_cast<unsigned>(request->id), kind,
                     static_cast<unsigned>((esp_timer_get_time() - start_us) / 1000));
            request.reset();
        }
    }
}

bool WeatherService::Refresh(const char* reason) {
    const bool radio_active = InternetRadioPlayer::GetInstance().IsActive();
    const bool time_valid = IsTimeValid();
    bool request_already_active = false;
    portENTER_CRITICAL(&lock_);
    if (!network_connected_) {
        portEXIT_CRITICAL(&lock_);
        return false;
    }
    const bool safe = time_valid && (!kRequiresReadyGate || (application_idle_ && audio_ready_)) && !radio_active;
    if (data_.request_active || !safe) {
        if (!pending_refresh_) {
            pending_refresh_ = true;
            ++data_.generation;
        }
        data_.refresh_deferred = true;
        data_.last_request_failed = false;
        request_already_active = data_.request_active;
        portEXIT_CRITICAL(&lock_);
        ESP_LOGI(kTag, "WEATHER_REFRESH_DEFERRED reason=%s active=%d", reason,
                 request_already_active ? 1 : 0);
        return true;
    }
    pending_refresh_ = false;
    data_.request_active = true;
    data_.refresh_deferred = false;
    last_attempt_ms_ = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    ++data_.generation;
    portEXIT_CRITICAL(&lock_);

    auto request = std::make_shared<Request>();
    request->kind = Request::Kind::Refresh;
    if (!Enqueue(request)) {
        portENTER_CRITICAL(&lock_);
        data_.request_active = false;
        pending_refresh_ = true;
        data_.refresh_deferred = true;
        data_.last_request_failed = true;
        portEXIT_CRITICAL(&lock_);
        ESP_LOGE(kTag, "WEATHER_UPDATE_FAILED reason=worker_unavailable keeping_cached_data=%d", data_.valid);
        return false;
    }
    ESP_LOGI(kTag, "WEATHER_REFRESH_REQUEST reason=%s", reason);
    return true;
}

void WeatherService::RunRefresh() {
    const bool radio_active = InternetRadioPlayer::GetInstance().IsActive();
    const bool time_valid = IsTimeValid();
    bool allowed = false;
    portENTER_CRITICAL(&lock_);
    allowed = network_connected_ && (!kRequiresReadyGate || (application_idle_ && audio_ready_)) &&
              time_valid && !radio_active;
    if (!allowed) {
        data_.request_active = false;
        pending_refresh_ = true;
        data_.refresh_deferred = true;
    }
    portEXIT_CRITICAL(&lock_);
    if (!allowed) {
        ESP_LOGI(kTag, "WEATHER_REFRESH_DEFERRED reason=admission_changed");
        return;
    }
    WeatherData parsed{};
    std::string error;
    const auto location = GetLocation();
    LogMemory("worker_refresh_start");
    const bool ok = FetchWeather(location, 3, parsed, error);
    const bool radio_active_after_http = InternetRadioPlayer::GetInstance().IsActive();
    bool still_allowed = false;
    portENTER_CRITICAL(&lock_);
    still_allowed = network_connected_ &&
                    (!kRequiresReadyGate || (application_idle_ && audio_ready_)) &&
                    !radio_active_after_http;
    if (!still_allowed) {
        data_.request_active = false;
        pending_refresh_ = true;
        data_.refresh_deferred = true;
        data_.last_request_failed = false;
    }
    portEXIT_CRITICAL(&lock_);
    if (!still_allowed) {
        ESP_LOGI(kTag, "WEATHER_REFRESH_CANCELLED reason=admission_changed_after_http");
        return;
    }
    if (ok) {
        parsed.last_successful_update = time(nullptr);
        parsed.valid = true;
    }
    CompletePersistentRefresh(parsed, ok ? nullptr : error.c_str(), location.provider_id);
}

void WeatherService::CompletePersistentRefresh(const WeatherData& parsed, const char* failure,
                                               const std::string& source_location_id) {
    {
        std::lock_guard<std::mutex> lock(location_mutex_);
        if (location_.provider_id != source_location_id) {
            portENTER_CRITICAL(&lock_);
            data_.request_active = false;
            pending_refresh_ = true;
            data_.refresh_deferred = true;
            data_.last_request_failed = false;
            ++data_.generation;
            portEXIT_CRITICAL(&lock_);
            ESP_LOGI(kTag, "WEATHER_REFRESH_DISCARDED reason=location_changed");
            return;
        }
    }
    bool had_cache = false;
    bool rerun_pending = false;
    portENTER_CRITICAL(&lock_);
    had_cache = data_.valid;
    rerun_pending = pending_refresh_;
    if (parsed.valid) {
        WeatherData result = parsed;
        result.generation = data_.generation + 1;
        result.refresh_deferred = rerun_pending;
        data_ = result;
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
        ESP_LOGI(kTag, "WEATHER_UPDATE_OK timestamp=%lld", static_cast<long long>(parsed.last_successful_update));
    } else {
        ESP_LOGE(kTag, "WEATHER_UPDATE_FAILED reason=%s keeping_cached_data=%d",
                 failure ? failure : "unknown", had_cache ? 1 : 0);
    }
    LogMemory("after_update");
}

bool WeatherService::FetchWeather(const WeatherLocation& location, uint8_t forecast_days,
                                  WeatherData& parsed, std::string& error) {
    forecast_days = std::max<uint8_t>(1, std::min<uint8_t>(forecast_days, 3));
    char url[768];
    snprintf(url, sizeof(url),
             "https://api.open-meteo.com/v1/forecast?latitude=%.6f&longitude=%.6f&"
             "current=temperature_2m,apparent_temperature,relative_humidity_2m,precipitation,weather_code,"
             "wind_speed_10m,wind_direction_10m,is_day&daily=temperature_2m_min,temperature_2m_max,weather_code,"
             "precipitation_probability_max,precipitation_sum&forecast_days=%u&timezone=auto&wind_speed_unit=ms",
             location.latitude, location.longitude, static_cast<unsigned>(forecast_days));
    ESP_LOGI(kTag, "WEATHER_HTTP_START city=%s lat=%.5f lon=%.5f days=%u", location.name.c_str(),
             location.latitude, location.longitude, static_cast<unsigned>(forecast_days));
    std::string body;
    if (!ReadHttpJson("forecast", url, body, error)) return false;
    const int64_t parse_start_us = esp_timer_get_time();
    ESP_LOGI(kTag, "WEATHER_JSON_PARSE_START op=forecast bytes=%u",
             static_cast<unsigned>(body.size()));
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> json(
        cJSON_ParseWithLength(body.data(), body.size()), &cJSON_Delete);
    const bool parse_ok = json && ParseOpenMeteoResponse(json.get(), forecast_days, parsed);
    ESP_LOGI(kTag, "WEATHER_JSON_PARSE_DONE op=forecast ok=%d elapsed_ms=%u", parse_ok ? 1 : 0,
             static_cast<unsigned>((esp_timer_get_time() - parse_start_us) / 1000));
    if (!parse_ok) {
        error = "invalid_weather_response";
        return false;
    }
    ESP_LOGI(kTag, "WEATHER_PARSE_OK provider=open_meteo city=%s temp=%.1f code=%u",
             location.name.c_str(), parsed.current.temperature, parsed.current.weather_code);
    return true;
}

bool WeatherService::FetchGeocoding(const std::string& query, const std::string& country,
                                    const std::string& region,
                                    std::vector<WeatherCityCandidate>& results, std::string& error) {
    if (query.size() < 2 || query.size() > 80) {
        error = "invalid_query";
        return false;
    }
    std::string name = query;
    if (!region.empty()) name += ", " + region;
    else if (!country.empty() && country.size() != 2) name += ", " + country;
    std::string url = "https://geocoding-api.open-meteo.com/v1/search?name=" + UrlEncode(name) +
                      "&count=5&language=" + UrlEncode(LanguageForQuery(query)) + "&format=json";
    if (country.size() == 2) url += "&countryCode=" + UrlEncode(country);
    std::string body;
    if (!ReadHttpJson("geocoding", url, body, error)) return false;
    const int64_t parse_start_us = esp_timer_get_time();
    ESP_LOGI(kTag, "WEATHER_JSON_PARSE_START op=geocoding bytes=%u",
             static_cast<unsigned>(body.size()));
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> json(
        cJSON_ParseWithLength(body.data(), body.size()), &cJSON_Delete);
    auto array = json ? cJSON_GetObjectItemCaseSensitive(json.get(), "results") : nullptr;
    if (!json || (array != nullptr && !cJSON_IsArray(array))) {
        error = "invalid_geocoding_response";
        ESP_LOGI(kTag, "WEATHER_JSON_PARSE_DONE op=geocoding ok=0 elapsed_ms=%u",
                 static_cast<unsigned>((esp_timer_get_time() - parse_start_us) / 1000));
        return false;
    }
    ESP_LOGI(kTag, "WEATHER_JSON_PARSE_DONE op=geocoding ok=1 elapsed_ms=%u",
             static_cast<unsigned>((esp_timer_get_time() - parse_start_us) / 1000));
    if (array == nullptr) return true;
    const int count = std::min(cJSON_GetArraySize(array), static_cast<int>(kMaxSearchResults));
    for (int i = 0; i < count; ++i) {
        auto item = cJSON_GetArrayItem(array, i);
        double id = 0, latitude = 0, longitude = 0;
        auto name_item = cJSON_GetObjectItemCaseSensitive(item, "name");
        if (!cJSON_IsString(name_item) || !JsonNumber(item, "id", id) ||
            !JsonNumber(item, "latitude", latitude) || !JsonNumber(item, "longitude", longitude) ||
            latitude < -90 || latitude > 90 || longitude < -180 || longitude > 180) {
            continue;
        }
        WeatherCityCandidate candidate;
        char id_buffer[32];
        snprintf(id_buffer, sizeof(id_buffer), "om:%lld", static_cast<long long>(id));
        candidate.candidate_id = id_buffer;
        candidate.name = name_item->valuestring;
        auto copy_string = [&item](const char* key) {
            auto value = cJSON_GetObjectItemCaseSensitive(item, key);
            return cJSON_IsString(value) ? std::string(value->valuestring) : std::string();
        };
        candidate.country = copy_string("country");
        candidate.country_code = copy_string("country_code");
        candidate.region = copy_string("admin1");
        candidate.timezone = copy_string("timezone");
        candidate.latitude = latitude;
        candidate.longitude = longitude;
        results.push_back(std::move(candidate));
    }
    return true;
}

void WeatherService::SearchCitiesAsync(const std::string& query, const std::string& country,
                                       const std::string& region,
                                       std::function<void(const std::string&)> callback) {
    auto request = std::make_shared<Request>();
    request->kind = Request::Kind::Search;
    request->query = query;
    request->country = country;
    request->region = region;
    request->callback = std::move(callback);
    if (!Enqueue(request)) {
        if (request->callback) request->callback(JsonError("weather_worker_unavailable"));
    }
}

void WeatherService::RunSearch(const std::shared_ptr<Request>& request) {
    std::vector<WeatherCityCandidate> candidates;
    std::string error;
    bool ok = false;
    portENTER_CRITICAL(&lock_);
    const bool network_connected = network_connected_;
    portEXIT_CRITICAL(&lock_);
    if (network_connected && IsTimeValid()) ok = FetchGeocoding(request->query, request->country, request->region, candidates, error);
    else error = network_connected ? "time_not_synced" : "network_unavailable";

    if (ok) {
        std::lock_guard<std::mutex> lock(location_mutex_);
        recent_candidates_ = candidates;
        candidate_updated_ms_ = static_cast<uint32_t>(esp_timer_get_time() / 1000);
        candidate_generation_ = (candidate_generation_ + 1) % kCandidateGenerationLimit;
    } else {
        std::lock_guard<std::mutex> lock(location_mutex_);
        recent_candidates_.clear();
        candidate_updated_ms_ = static_cast<uint32_t>(esp_timer_get_time() / 1000);
        candidate_generation_ = (candidate_generation_ + 1) % kCandidateGenerationLimit;
    }
    cJSON* result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "ok", ok);
    cJSON_AddStringToObject(result, "query", request->query.c_str());
    cJSON_AddNumberToObject(result, "count", static_cast<double>(candidates.size()));
    {
        std::lock_guard<std::mutex> lock(location_mutex_);
        cJSON_AddNumberToObject(result, "search_generation", static_cast<double>(candidate_generation_));
    }
    if (!ok) cJSON_AddStringToObject(result, "error", error.c_str());
    cJSON* array = cJSON_AddArrayToObject(result, "candidates");
    for (const auto& candidate : candidates) {
        cJSON* item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "candidate_id", candidate.candidate_id.c_str());
        cJSON_AddStringToObject(item, "name", candidate.name.c_str());
        cJSON_AddStringToObject(item, "country", candidate.country.c_str());
        cJSON_AddStringToObject(item, "country_code", candidate.country_code.c_str());
        cJSON_AddStringToObject(item, "region", candidate.region.c_str());
        cJSON_AddStringToObject(item, "timezone", candidate.timezone.c_str());
        cJSON_AddNumberToObject(item, "latitude", candidate.latitude);
        cJSON_AddNumberToObject(item, "longitude", candidate.longitude);
        cJSON_AddItemToArray(array, item);
    }
    if (request->callback) request->callback(JsonText(result));
}

bool WeatherService::ResolveCandidate(const std::string& candidate_id, WeatherLocation& location,
                                      std::string& error) const {
    if (candidate_id.empty()) {
        location = GetLocation();
        return true;
    }
    const uint32_t now = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    std::lock_guard<std::mutex> lock(location_mutex_);
    if (now - candidate_updated_ms_ > kCandidateLifetimeMs) {
        error = "candidate_expired";
        return false;
    }
    for (const auto& candidate : recent_candidates_) {
        if (candidate.candidate_id != candidate_id) continue;
        location.latitude = candidate.latitude;
        location.longitude = candidate.longitude;
        location.name = candidate.name;
        location.country = candidate.country;
        location.country_code = candidate.country_code;
        location.region = candidate.region;
        location.timezone = candidate.timezone;
        location.provider_id = candidate.candidate_id;
        return true;
    }
    error = "candidate_not_found";
    return false;
}

void WeatherService::GetWeatherAsync(const std::string& candidate_id, uint8_t forecast_days,
                                     std::function<void(const std::string&)> callback) {
    auto request = std::make_shared<Request>();
    request->kind = Request::Kind::OneOffWeather;
    request->candidate_id = candidate_id;
    request->forecast_days = std::max<uint8_t>(1, std::min<uint8_t>(forecast_days, 3));
    request->callback = std::move(callback);
    if (!Enqueue(request)) {
        if (request->callback) request->callback(JsonError("weather_worker_unavailable"));
    }
}

void WeatherService::RunOneOffWeather(const std::shared_ptr<Request>& request) {
    WeatherLocation location;
    std::string error;
    if (!ResolveCandidate(request->candidate_id, location, error)) {
        if (request->callback) request->callback(JsonError(error.c_str()));
        return;
    }
    const WeatherData cached = GetSnapshot();
    const auto persistent_location = GetLocation();
    const time_t now = time(nullptr);
    const bool cache_location_matches =
        std::abs(location.latitude - persistent_location.latitude) < 0.000001 &&
        std::abs(location.longitude - persistent_location.longitude) < 0.000001;
    if (request->candidate_id.empty() && cached.valid && !cached.stale &&
        cached.last_successful_update > 0 && now >= cached.last_successful_update &&
        now - cached.last_successful_update <= kMcpCacheTtlSeconds && cache_location_matches) {
        cJSON* result = cJSON_CreateObject();
        cJSON_AddBoolToObject(result, "ok", true);
        cJSON_AddBoolToObject(result, "stale", false);
        AddWeatherJson(result, location, cached, request->forecast_days);
        ESP_LOGI(kTag, "WEATHER_MCP_CACHE_HIT lat=%.5f lon=%.5f days=%u age_sec=%u",
                 location.latitude, location.longitude,
                 static_cast<unsigned>(request->forecast_days),
                 static_cast<unsigned>(now - cached.last_successful_update));
        if (request->callback) request->callback(JsonText(result));
        return;
    }
    portENTER_CRITICAL(&lock_);
    const bool network_connected = network_connected_;
    portEXIT_CRITICAL(&lock_);
    if (!network_connected) {
        if (request->callback) request->callback(JsonError("network_unavailable"));
        return;
    }
    if (!IsTimeValid()) {
        if (request->callback) request->callback(JsonError("time_not_synced"));
        return;
    }
    WeatherData data{};
    if (!FetchWeather(location, request->forecast_days, data, error)) {
        if (request->callback) request->callback(JsonError(error.c_str()));
        return;
    }
    data.valid = true;
    data.last_successful_update = time(nullptr);
    cJSON* result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "ok", true);
    cJSON_AddBoolToObject(result, "stale", false);
    AddWeatherJson(result, location, data, request->forecast_days);
    if (request->callback) request->callback(JsonText(result));
}

WeatherLocation WeatherService::GetLocation() const {
    std::lock_guard<std::mutex> lock(location_mutex_);
    return location_.name.empty() ? kFnkWeatherLocation : location_;
}

void WeatherService::SetLocationChangedCallback(std::function<void()> callback) {
    std::lock_guard<std::mutex> lock(location_mutex_);
    location_changed_callback_ = std::move(callback);
}

bool WeatherService::SetLocationByCandidate(const std::string& candidate_id, std::string& error) {
    WeatherLocation location;
    if (!ResolveCandidate(candidate_id, location, error)) return false;
    std::function<void()> callback;
    {
        std::lock_guard<std::mutex> lock(location_mutex_);
        const bool same = location_.provider_id == location.provider_id &&
                          std::abs(location_.latitude - location.latitude) < 0.000001 &&
                          std::abs(location_.longitude - location.longitude) < 0.000001;
        if (same) return true;
    }
    if (!SaveLocation(location, error)) return false;
    {
        std::lock_guard<std::mutex> lock(location_mutex_);
        location_ = location;
        ++location_revision_;
        callback = location_changed_callback_;
    }
    portENTER_CRITICAL(&lock_);
    pending_refresh_ = true;
    data_.valid = false;
    data_.stale = false;
    data_.last_request_failed = false;
    ++data_.generation;
    portEXIT_CRITICAL(&lock_);
    ESP_LOGI(kTag, "WEATHER_LOCATION_CHANGED city=%s country=%s lat=%.5f lon=%.5f",
             location.name.c_str(), location.country.c_str(), location.latitude, location.longitude);
    if (callback) callback();
    return true;
}

bool WeatherService::LoadLocation() {
    WeatherLocation loaded = kFnkWeatherLocation;
    Settings settings(kSettingsNamespace);
    if (settings.IsOpen()) {
        const std::string latitude = settings.GetString("latitude", "");
        const std::string longitude = settings.GetString("longitude", "");
        char* end = nullptr;
        const double parsed_lat = latitude.empty() ? loaded.latitude : strtod(latitude.c_str(), &end);
        const bool lat_ok = !latitude.empty() && end != latitude.c_str() && *end == '\0' && parsed_lat >= -90 && parsed_lat <= 90;
        end = nullptr;
        const double parsed_lon = longitude.empty() ? loaded.longitude : strtod(longitude.c_str(), &end);
        const bool lon_ok = !longitude.empty() && end != longitude.c_str() && *end == '\0' && parsed_lon >= -180 && parsed_lon <= 180;
        if (lat_ok && lon_ok) {
            loaded.latitude = parsed_lat;
            loaded.longitude = parsed_lon;
            loaded.name = settings.GetString("name", loaded.name);
            loaded.country = settings.GetString("country", loaded.country);
            loaded.country_code = settings.GetString("country_code", loaded.country_code);
            loaded.region = settings.GetString("region", loaded.region);
            loaded.timezone = settings.GetString("timezone", loaded.timezone);
            loaded.provider_id = settings.GetString("provider_id", loaded.provider_id);
        } else if (!latitude.empty() || !longitude.empty()) {
            ESP_LOGW(kTag, "WEATHER_LOCATION_INVALID_NVS using_default=1");
        }
    }
    std::lock_guard<std::mutex> lock(location_mutex_);
    location_ = std::move(loaded);
    return true;
}

bool WeatherService::SaveLocation(const WeatherLocation& location, std::string& error) {
    char latitude[32], longitude[32];
    snprintf(latitude, sizeof(latitude), "%.8f", location.latitude);
    snprintf(longitude, sizeof(longitude), "%.8f", location.longitude);
    Settings settings(kSettingsNamespace, true);
    if (!settings.IsOpen()) {
        error = "nvs_open_failed";
        return false;
    }
    const bool written = settings.TrySetString("latitude", latitude) && settings.TrySetString("longitude", longitude) &&
                         settings.TrySetString("name", location.name) && settings.TrySetString("country", location.country) &&
                         settings.TrySetString("country_code", location.country_code) && settings.TrySetString("region", location.region) &&
                         settings.TrySetString("timezone", location.timezone) && settings.TrySetString("provider_id", location.provider_id);
    if (!written || !settings.Commit()) {
        settings.DiscardChanges();
        error = "nvs_save_failed";
        ESP_LOGE(kTag, "WEATHER_LOCATION_SAVE_FAILED reason=%s", error.c_str());
        return false;
    }
    return true;
}

bool WeatherService::LoadCache() {
    std::unique_ptr<FILE, decltype(&fclose)> file(fopen(kCachePath, "rb"), &fclose);
    if (!file) return false;
    WeatherCacheRecord record{};
    const bool complete = fread(&record, 1, sizeof(record), file.get()) == sizeof(record) && fgetc(file.get()) == EOF;
    const uint32_t expected_checksum = CacheChecksum(&record, offsetof(WeatherCacheRecord, checksum));
    if (!complete || record.magic != kCacheMagic || record.version != kCacheVersion || record.size != sizeof(record) ||
        record.checksum != expected_checksum || record.updated_at < kMinimumValidTime || !std::isfinite(record.temperature) ||
        !std::isfinite(record.wind_speed)) {
        ESP_LOGW(kTag, "WEATHER_CACHE_INVALID");
        return false;
    }
    WeatherData cached{};
    cached.current.temperature = record.temperature;
    cached.current.apparent_temperature = record.apparent_temperature;
    cached.current.wind_speed = record.wind_speed;
    cached.current.wind_direction = record.wind_direction;
    cached.current.weather_code = record.weather_code;
    cached.current.humidity = record.humidity;
    cached.current.is_day = record.is_day != 0;
    cached.current.has_apparent_temperature = record.has_apparent_temperature != 0;
    cached.current.has_humidity = record.has_humidity != 0;
    cached.current.has_precipitation = record.has_precipitation != 0;
    cached.current.precipitation_mm = record.precipitation_mm;
    for (size_t i = 0; i < 3; ++i) {
        cached.days[i].temp_min = record.days[i].temp_min;
        cached.days[i].temp_max = record.days[i].temp_max;
        cached.days[i].weather_code = record.days[i].weather_code;
        cached.days[i].precipitation_probability = record.days[i].precipitation_probability;
        cached.days[i].has_precipitation = record.days[i].has_precipitation != 0;
        cached.days[i].precipitation_mm = record.days[i].precipitation_mm;
    }
    cached.last_successful_update = static_cast<time_t>(record.updated_at);
    cached.generation = 1;
    cached.valid = true;
    cached.stale = true;
    portENTER_CRITICAL(&lock_);
    data_ = cached;
    portEXIT_CRITICAL(&lock_);
    ESP_LOGI(kTag, "WEATHER_CACHE_LOADED timestamp=%lld bytes=%u", static_cast<long long>(cached.last_successful_update),
             static_cast<unsigned>(sizeof(record)));
    return true;
}

bool WeatherService::SaveCache(const WeatherData& data) const {
    WeatherCacheRecord record{};
    record.magic = kCacheMagic;
    record.version = kCacheVersion;
    record.size = sizeof(record);
    record.updated_at = static_cast<int64_t>(data.last_successful_update);
    record.temperature = data.current.temperature;
    record.apparent_temperature = data.current.apparent_temperature;
    record.wind_speed = data.current.wind_speed;
    record.wind_direction = data.current.wind_direction;
    record.weather_code = data.current.weather_code;
    record.humidity = data.current.humidity;
    record.is_day = data.current.is_day ? 1 : 0;
    record.has_apparent_temperature = data.current.has_apparent_temperature ? 1 : 0;
    record.has_humidity = data.current.has_humidity ? 1 : 0;
    record.has_precipitation = data.current.has_precipitation ? 1 : 0;
    record.precipitation_mm = data.current.precipitation_mm;
    for (size_t i = 0; i < 3; ++i) {
        record.days[i] = {data.days[i].temp_min, data.days[i].temp_max, data.days[i].weather_code,
                          data.days[i].precipitation_probability,
                          static_cast<uint8_t>(data.days[i].has_precipitation ? 1 : 0),
                          data.days[i].precipitation_mm};
    }
    record.checksum = CacheChecksum(&record, offsetof(WeatherCacheRecord, checksum));
    unlink(kCacheTmpPath);
    std::unique_ptr<FILE, decltype(&fclose)> file(fopen(kCacheTmpPath, "wb"), &fclose);
    if (!file || fwrite(&record, 1, sizeof(record), file.get()) != sizeof(record) || fflush(file.get()) != 0) {
        ESP_LOGW(kTag, "WEATHER_CACHE_SAVE_FAILED");
        unlink(kCacheTmpPath);
        return false;
    }
    file.reset();
    if ((unlink(kCachePath) != 0 && errno != ENOENT) || rename(kCacheTmpPath, kCachePath) != 0) {
        ESP_LOGW(kTag, "WEATHER_CACHE_SAVE_FAILED");
        unlink(kCacheTmpPath);
        return false;
    }
    ESP_LOGI(kTag, "WEATHER_CACHE_SAVED bytes=%u", static_cast<unsigned>(sizeof(record)));
    return true;
}

WeatherData WeatherService::GetSnapshot() const {
    portENTER_CRITICAL(&lock_);
    WeatherData copy = data_;
    if (copy.valid && static_cast<uint32_t>(esp_timer_get_time() / 1000) - last_attempt_ms_ >= kUpdateIntervalMs) copy.stale = true;
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
        (code >= 1204 && code <= 1237) || (code >= 1255 && code <= 1264)) return WeatherIcon::Snow;
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
    static constexpr const char* kDirections[] = {"С", "СВ", "В", "ЮВ", "Ю", "ЮЗ", "З", "СЗ"};
    snprintf(output, output_size, "%s", kDirections[((degrees + 22) / 45) % 8]);
}

void WeatherService::FormatDate(time_t value, char* output, size_t output_size) {
    struct tm local{};
    localtime_r(&value, &local);
    strftime(output, output_size, "%d.%m.%Y", &local);
}
