// Copyright (c) 2025 by Christopher Antos
// License: http://opensource.org/licenses/MIT

// vim: set et ts=4 sw=4 cino={0s:

#pragma once

#include <windows.h>

void LoadConfig();

#ifdef USE_REGISTRY_FOR_CONFIG
void ReadConfigString(HKEY hkeyApp, const WCHAR* name, WCHAR* out, uint32 max_len, const WCHAR* default_value);
#else
void ReadConfigString(const WCHAR* ini_filename, const WCHAR* section, const WCHAR* name, WCHAR* out, uint32 max_len, const WCHAR* default_value);
#endif
