// Copyright (c) 2024 by Christopher Antos
// License: http://opensource.org/licenses/MIT

// vim: set et ts=4 sw=4 cino={0s:

#pragma once

#include <vector>

enum class FileType { Invalid, Device, Dir, File };
FileType GetFileType(const WCHAR* p);

int Recycle(const std::vector<StrW>& names, Error& e);

