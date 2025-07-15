// Copyright (c) 2025 by Christopher Antos
// License: http://opensource.org/licenses/MIT

// vim: set et ts=4 sw=4 cino={0s:

#pragma once

#include <windows.h>
#include "fileinfo.h"

#include <memory>

class Error;

bool ScanFiles(int argc, const WCHAR** argv, std::vector<FileInfo>& files, StrW& dir, Error& e);
bool ScanPattern(const WCHAR* pattern, std::vector<FileInfo>& files, Error& e);

