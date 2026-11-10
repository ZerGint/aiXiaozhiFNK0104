#ifndef TIME_OFFSET_H
#define TIME_OFFSET_H

#include <ctime>

// User correction for the clock shown on the device, in whole hours. The
// system time keeps the value received from the server; only times presented
// to the user (clock, weather date) are shifted. Persisted in NVS namespace
// "display", key "time_offset_h".
namespace TimeOffset {

constexpr int kMinHours = -2;
constexpr int kMaxHours = 2;

int GetHours();
// Clamps to [kMinHours, kMaxHours], persists and returns the stored value.
int SetHours(int hours);
// Converts a timestamp to broken-down local time with the user correction.
void ToLocalTime(time_t value, struct tm* out);

}  // namespace TimeOffset

#endif  // TIME_OFFSET_H
