// Copyright (c) 2025 by Christopher Antos
// License: http://opensource.org/licenses/MIT

// vim: set et ts=4 sw=4 cino={0s:

#include "pch.h"
#include "os.h"

namespace OS {

bool IsPseudoDirectory(const WCHAR* dir)
{
    if (dir[0] != '.')
        return false;
    if (!dir[1])
        return true;
    if (dir[1] != '.')
        return false;
    if (!dir[2])
        return true;
    return false;
}

unsigned IsExtendedPath(const WCHAR* p)
{
    if (p[0] == '\\' &&
        p[1] == '\\' &&
        p[2] == '?' &&
        p[3] == '\\')
        return 4;
    return 0;
}

bool GetEnv(const WCHAR* name, StrW& value)
{
    const DWORD needed = GetEnvironmentVariableW(name, nullptr, 0);
    if (!needed)
    {
failed:
        value.Clear();
        return false;
    }

    WCHAR* data = value.Reserve(needed);
    if (!data)
        goto failed;

    const DWORD used = GetEnvironmentVariableW(name, data, value.Capacity());
    if (!used || used >= value.Capacity())
        goto failed;

    return true;
}

void GetCwd(StrW& dir, WCHAR chDrive)
{
    dir.Clear();

    // If no drive specified, get the current working directory.
    if (!chDrive)
    {
        StrW tmp;
        tmp.ReserveMaxPath();
        if (GetCurrentDirectory(tmp.Capacity(), tmp.Reserve()))
            dir.Set(tmp);
        return;
    }

    // Get the specified drive's cwd from the environment table.
    WCHAR name[4] = L"=C:";
    name[1] = WCHAR(towupper(chDrive));

    StrW value;
    value.ReserveMaxPath();
    if (GetEnvironmentVariable(name, value.Reserve(), value.Capacity()) && !value.Empty())
    {
        dir.Set(value);
        return;
    }

    // Otherwise assume root.
    dir.Printf(L"%c:\\", _totupper(chDrive));
    return;
}

bool GetDrive(const WCHAR* pattern, StrW& drive, Error& e)
{
    drive.Clear();

    if (!pattern || !*pattern)
        return false;

    bool unc = false;
    StrW extended;

    // Advance past \\?\ or \\?\UNC\.
    unsigned extended_len = IsExtendedPath(pattern);
    if (extended_len)
    {
        extended.Set(pattern, extended_len);
        pattern += extended_len;
        if (wcsnicmp(pattern, L"UNC\\", 4) == 0)
        {
            unc = true;
            extended.Append(pattern, 4);
            pattern += 4;
        }

        if (!*pattern)
            return false;
    }

    // For UNC paths, return the \\server\share as the drive.
    if (unc || (pattern[0] == '\\' &&
                pattern[1] == '\\'))
    {
        // Find end of \\server part.
        const WCHAR* tmp = wcschr(pattern + (unc ? 0 : 2), '\\');
        if (!tmp)
            return false;

        // Find end of \\server\share part.
        tmp = wcschr(tmp + 1, '\\');
        const size_t len = tmp ? tmp - pattern : wcslen(pattern);
        if (len > MaxPath())
        {
            e.Sys(ERROR_FILENAME_EXCED_RANGE);
            return false;
        }

        extended.Append(pattern, len);
        drive = std::move(extended);
        return true;
    }

    // Use drive letter from pattern, if present.
    if (pattern[0] &&
        pattern[1] == ':')
    {
        drive.Set(pattern, 2);
        drive.ToUpper();
        return true;
    }

    // Otherwise use drive letter from cwd.
    GetCwd(drive);
    if (drive.Length())
    {
        drive.SetLength(1);
        drive.Append(':');
    }
    return true;
}

bool GetFullPathName(const WCHAR* name, StrW& full, Error& e)
{
    full.Clear();
    full.ReserveMaxPath();

    LPWSTR file_part;
    const DWORD len = ::GetFullPathName(name, full.Capacity(), full.Reserve(), &file_part);
    if (!len)
    {
        e.Sys();
        full.Clear();
        return false;
    }
    else if (len >= full.Capacity())
    {
        e.Sys(ERROR_FILENAME_EXCED_RANGE);
        full.Clear();
        return false;
    }

    return true;
}

bool IsFATDrive(const WCHAR* path, Error& e)
{
    StrW drive;
    DWORD cbComponentMax;
    WCHAR name[MAX_PATH + 1];

    if (!GetDrive(path, drive, e))
        return false;

    EnsureTrailingSlash(drive);

    if (!GetVolumeInformation(drive.Text(), 0, 0, 0, &cbComponentMax, 0, name, _countof(name)))
    {
        // Ignore ERROR_DIR_NOT_ROOT; consider SUBST drives as not FAT.
        const DWORD dwErr = GetLastError();
        if (dwErr != ERROR_DIR_NOT_ROOT)
            e.Sys(dwErr);
        return false;
    }

    return (!wcsicmp(name, L"FAT") && cbComponentMax == 12); // 12 == 8.3
}

bool IsHidden(const WIN32_FIND_DATA& fd)
{
    return (fd.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN);
}

} // namespace OS
