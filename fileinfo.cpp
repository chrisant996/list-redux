// Copyright (c) 2024 by Christopher Antos
// License: http://opensource.org/licenses/MIT

// vim: set et ts=4 sw=4 cino={0s:

#include "pch.h"
#include "fileinfo.h"
#include "os.h"

std::vector<StrW> FileInfo::s_dirs;

void FileInfo::Init(const WIN32_FIND_DATA* pfd, const WCHAR* dir)
{
    m_name.Set(pfd->cFileName);

    m_dwAttr = pfd->dwFileAttributes;
    m_ftModified = pfd->ftLastWriteTime;

    m_ulSize.LowPart = pfd->nFileSizeLow;
    m_ulSize.HighPart = pfd->nFileSizeHigh;

    if (dir)
    {
        for (size_t ii = 0; ii < s_dirs.size(); ++ii)
        {
            if (wcscmp(dir, s_dirs[ii].Text()) == 0)
            {
                m_dir = int(ii);
                break;
            }
        }

        if (m_dir < 0)
        {
            m_dir = int(s_dirs.size());
            s_dirs.emplace_back(dir);
        }
    }
}

const WCHAR* FileInfo::GetDirectory() const
{
    if (size_t(m_dir) < s_dirs.size())
        return s_dirs[m_dir].Text();
    return nullptr;
}

void FileInfo::GetPathName(StrW& s) const
{
    s.Clear();
    const WCHAR* dir = GetDirectory();
    if (dir)
    {
        s.Append(dir);
        EnsureTrailingSlash(s);
    }
    s.Append(m_name);
}

bool FileInfo::IsPseudoDirectory() const
{
    if (GetAttributes() & FILE_ATTRIBUTE_DIRECTORY)
        return OS::IsPseudoDirectory(m_name.Text());
    return false;
}

bool FileInfo::IsDirectory() const
{
    return !!(GetAttributes() & FILE_ATTRIBUTE_DIRECTORY);
}

const WCHAR* FindExtension(const WCHAR* file)
{
    const WCHAR* ext = 0;

    for (const WCHAR* p = file; *p; p++)
    {
        if (*p == ' ' || *p == '\t' || *p == '\\' || *p == '/')
            ext = 0;
        else if (*p == '.')
            ext = p;
    }

    return ext;
}

const WCHAR* FindName(const WCHAR* file)
{
    const WCHAR* name = file;

    for (const WCHAR* p = file; *p; p++)
    {
        if (*p == '\\' || *p == '/')
            name = p + 1;
    }

    return name;
}

