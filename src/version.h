// Single source of the version number, included by both the resource script and
// the C++ code.
//
// The project follows semantic versioning, so the version people see is
// MAJOR.MINOR.PATCH and the release tag is exactly "v" plus that string. The
// Windows VERSIONINFO resource is the one place that needs a fourth field, so
// the comma form keeps it and nothing else does.
#pragma once

#define ZDISPLAY_VERSION_MAJOR 1
#define ZDISPLAY_VERSION_MINOR 1
#define ZDISPLAY_VERSION_PATCH 0

#define ZDISPLAY_VERSION_COMMA 1,1,0,0   ///< VERSIONINFO requires four fields
#define ZDISPLAY_VERSION_STR   "1.1.0"
#define ZDISPLAY_VERSION_WSTR  L"1.1.0"
