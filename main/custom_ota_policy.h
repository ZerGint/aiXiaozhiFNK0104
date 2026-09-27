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

bool StageStableUpdate();
bool StageStableUpdateOnNetwork(NetworkInterface* network);
bool CheckForStableUpdate();
bool IsUpdateAvailable();
const StableUpdateMetadata& GetStableUpdateMetadata();
bool RequestFirmwareInstall();
bool TriggerDevUpdateRequestOnce();
void ReportStagedUpdate();

}  // namespace CustomOtaPolicy
