#include "time_offset.h"

#include <esp_log.h>

#include <algorithm>
#include <atomic>

#include "settings.h"

namespace {

constexpr char kTag[] = "TimeOffset";
constexpr char kNamespace[] = "display";
constexpr char kKey[] = "time_offset_h";
constexpr int kNotLoaded = 1000;

std::atomic<int> g_hours{kNotLoaded};

}  // namespace

namespace TimeOffset {

int GetHours() {
    int hours = g_hours.load(std::memory_order_acquire);
    if (hours == kNotLoaded) {
        Settings settings(kNamespace, false);
        hours = std::clamp(static_cast<int>(settings.GetInt(kKey, 0)), kMinHours, kMaxHours);
        g_hours.store(hours, std::memory_order_release);
    }
    return hours;
}

int SetHours(int hours) {
    hours = std::clamp(hours, kMinHours, kMaxHours);
    {
        Settings settings(kNamespace, true);
        settings.SetInt(kKey, hours);
    }
    g_hours.store(hours, std::memory_order_release);
    ESP_LOGI(kTag, "TIME_OFFSET_SET hours=%d", hours);
    return hours;
}

void ToLocalTime(time_t value, struct tm* out) {
    const time_t shifted = value + static_cast<time_t>(GetHours()) * 3600;
    localtime_r(&shifted, out);
}

}  // namespace TimeOffset
