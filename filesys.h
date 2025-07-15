// Copyright (c) 2024 by Christopher Antos
// License: http://opensource.org/licenses/MIT

// vim: set et ts=4 sw=4 cino={0s:

#pragma once

#include "str.h"

bool IsPseudoDirectory(const WCHAR* dir);
unsigned IsExtendedPath(const WCHAR* p);
void GetCwd(StrW& dir, WCHAR chDrive='\0');
bool GetDrive(const WCHAR* pattern, StrW& drive, Error& e);
bool IsFATDrive(const WCHAR* path, Error& e);
bool IsHidden(const WIN32_FIND_DATA& fd);

enum class FileType { Invalid, Device, Dir, File };
FileType GetFileType(const WCHAR* p);

