// Copyright (c) 2024 by Christopher Antos
// License: http://opensource.org/licenses/MIT

// vim: set et ts=4 sw=4 cino={0s:

#pragma once

#include <windows.h>
#include "str.h"

#include <memory>
#include <vector>

class FileInfo
{
public:
                        FileInfo() {}
                        ~FileInfo() {}

                        // For std::stable_sort.
                        FileInfo(FileInfo&& other) = default;
    FileInfo&           operator=(FileInfo&& other) = default;

    void                Init(const WIN32_FIND_DATA* pfd, const WCHAR* dir=nullptr);

    DWORD               GetAttributes() const { return m_dwAttr; }
    const FILETIME&     GetModifiedTime() const { return m_ftModified; }
    const unsigned __int64& GetSize() const { return *reinterpret_cast<const unsigned __int64*>(&m_ulSize); }
    const StrW&         GetName() const { return m_name; }
    const WCHAR*        GetDirectory() const;
    void                GetPathName(StrW& s) const;

    bool                IsPseudoDirectory() const;
    bool                IsDirectory() const;

    void                UpdateAttributes(DWORD attr) { m_dwAttr = attr; }

private:
    StrW                m_name;
    ULARGE_INTEGER      m_ulSize = {};
    FILETIME            m_ftModified = {};
    DWORD               m_dwAttr = INVALID_FILE_ATTRIBUTES;
    int                 m_dir = -1;

    static std::vector<StrW> s_dirs;
};

bool IsPseudoDirectory(const WCHAR* dir);
const WCHAR* FindExtension(const WCHAR* file);
const WCHAR* FindName(const WCHAR* file);

