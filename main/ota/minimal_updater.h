#pragma once

#include <cstdint>

#include "custom_ota_policy.h"

namespace MinimalUpdater {

// Persistent OTA lifecycle. Heavy work runs only in UPDATE mode after an
// explicit user request and reboot.
enum class State : uint8_t {
    STABLE = 0,
    UPDATE = 1,
    INSTALLED = 2,
};

bool ReadState(State* state);
bool WriteUpdateRequest(const CustomOtaPolicy::StableUpdateMetadata& metadata);
bool MarkState(State state, const char* reason, const char* version);

void ReportPendingVerification();
bool ValidatePendingUpdate();
bool IsValidationPendingOrFailed();

// Called before Board/Application creation. Returns true only when the boot
// must remain in minimal updater mode.
bool RunIfRequested();

}  // namespace MinimalUpdater
