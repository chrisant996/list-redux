// Copyright (c) 2025 by Christopher Antos
// License: http://opensource.org/licenses/MIT

// vim: set et ts=4 sw=4 cino={0s:

#pragma once

#include <windows.h>
#include "str.h"
#include "error.h"

namespace OS {

bool IsPseudoDirectory(const WCHAR* dir);
unsigned IsExtendedPath(const WCHAR* p);

bool GetEnv(const WCHAR* name, StrW& value);
void GetCwd(StrW& dir, WCHAR chDrive='\0');
bool GetDrive(const WCHAR* pattern, StrW& drive, Error& e);
bool GetFullPathName(const WCHAR* name, StrW& full, Error& e);
bool IsFATDrive(const WCHAR* path, Error& e);
bool IsHidden(const WIN32_FIND_DATA& fd);

}
