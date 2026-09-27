#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace CustomOtaPolicy {

struct StableUpdateMetadata {
    bool update_available = false;
    char version[32] = {};
    uint32_t size = 0;
    char sha256[65] = {};
    char url[256] = {};
};

bool IsStrictStableVersion(const char* version);
bool ShouldCheckForUpdates(const char* current_version);
bool IsValidRemoteStableVersion(const char* version);
int CompareStableVersions(const char* left, const char* right);

bool StageStableUpdate();
bool CheckForStableUpdate();
bool IsUpdateAvailable();
const StableUpdateMetadata& GetStableUpdateMetadata();
void ReportStagedUpdate();

}  // namespace CustomOtaPolicy
