#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

class NetworkInterface;

namespace CustomOtaPolicy {

struct StableUpdateMetadata {
    bool update_available = false;
    char version[32] = {};
    uint32_t size = 0;
    char sha256[65] = {};
    char url[256] = {};
    char board[32] = {};
    char chip[16] = {};
};

bool IsStrictStableVersion(const char* version);
bool ShouldCheckForUpdates(const char* current_version);
bool IsValidRemoteStableVersion(const char* version);
int CompareStableVersions(const char* left, const char* right);

bool StageStableUpdateOnNetwork(NetworkInterface* network);
bool ClearUpdateDirectory();
bool CheckForStableUpdate();
bool IsUpdateAvailable();
const StableUpdateMetadata& GetStableUpdateMetadata();
bool RequestFirmwareInstall();
void ReportStagedUpdate();
// Removes staged firmware and metadata only after the new app has been
// validated and marked boot-valid.
bool CleanupStagedFiles();
// Installs the already verified SD staged image into the ESP-IDF selected
// inactive OTA partition.  This function never changes the boot partition
// until all staged-image checks and esp_ota_end() have succeeded.
bool InstallStagedUpdate();

}  // namespace CustomOtaPolicy
