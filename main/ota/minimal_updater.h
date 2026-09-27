#pragma once

#include <cstdint>

#include "custom_ota_policy.h"

namespace MinimalUpdater {

enum class State : uint8_t {
    IDLE = 0,
    UPDATE_REQUESTED = 1,
    STAGING = 2,
    STAGED = 3,
    FAILED = 4,
    INSTALL_REQUESTED = 5,
    INSTALLING = 6,
    PENDING_VERIFY = 7,
    VALIDATED = 8,
};

bool ReadState(State* state);
bool WriteUpdateRequest(const CustomOtaPolicy::StableUpdateMetadata& metadata);
bool ReadDevRequestConsumed(bool* consumed);
bool MarkDevRequestConsumed();
bool MarkState(State state, const char* reason, const char* version);

// Reports and validates a newly installed image during the first normal boot.
// The validation is local and must complete before cloud-dependent work.
void ReportPendingVerification();
bool ValidatePendingUpdate();

// Used by the legacy OTA mark-valid path to avoid bypassing post-update
// validation while a custom update is still pending or has failed validation.
bool IsValidationPendingOrFailed();

// Called immediately after NVS initialization and before Board/Application.
// Returns true only when the boot must remain in minimal updater mode.
bool RunIfRequested();

}  // namespace MinimalUpdater
