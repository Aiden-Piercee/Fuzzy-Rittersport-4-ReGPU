#pragma once
// The version this build reports. The launcher compares it with the latest GitHub release.
// Bump all four together; this file is the only place the number lives. fm4.rc (the exe's version resource) and
// the launcher's csproj (its assembly version) both read it.
#define FM2_VERSION_MAJOR 0
#define FM2_VERSION_MINOR 6
#define FM2_VERSION_PATCH 0
#define FM2_VERSION "0.6.0"
