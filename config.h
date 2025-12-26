// Copyright (c) 2025 by Christopher Antos
// License: http://opensource.org/licenses/MIT

// vim: set et ts=4 sw=4 cino={0s:

#pragma once

#include <windows.h>

bool LoadConfig();
bool SaveConfig(Error& e);

enum class BooleanStyle { TrueFalse, Digit, OnOff, YesNo };
bool ParseBoolean(const WCHAR* value, bool target=true);
const WCHAR* BooleanValue(bool value, BooleanStyle style);

#ifdef USE_REGISTRY_FOR_CONFIG
bool ReadConfigString(HKEY hkeyApp, const WCHAR* name, WCHAR* out, uint32 max_len, const WCHAR* default_value);
#else
bool ReadConfigString(const WCHAR* ini_filename, const WCHAR* section, const WCHAR* name, WCHAR* out, uint32 max_len, const WCHAR* default_value=nullptr);
bool WriteConfigString(const WCHAR* ini_filename, const WCHAR* section, const WCHAR* name, const WCHAR* value);
#endif
