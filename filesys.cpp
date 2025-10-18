// Copyright (c) 2024 by Christopher Antos
// License: http://opensource.org/licenses/MIT

// vim: set et ts=4 sw=4 cino={0s:

#include "pch.h"
#include "filesys.h"

FileType GetFileType(const WCHAR* p)
{
    WIN32_FIND_DATA fd;
    SHFind h = FindFirstFile(p, &fd);
    if (h.Empty())
        return FileType::Invalid;
    if (fd.dwFileAttributes == DWORD(-1))
        return FileType::Invalid;
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DEVICE)
        return FileType::Device;
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        return FileType::Dir;
    return FileType::File;
}

