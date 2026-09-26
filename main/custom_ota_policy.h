#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace CustomOtaPolicy {

bool IsStrictStableVersion(const char* version);
bool ShouldCheckForUpdates(const char* current_version);
bool IsValidRemoteStableVersion(const char* version);
int CompareStableVersions(const char* left, const char* right);

bool StageStableUpdate();
void ReportStagedUpdate();

}  // namespace CustomOtaPolicy
