// Copyright (c) 2025 by Christopher Antos
// License: http://opensource.org/licenses/MIT

// vim: set et ts=4 sw=4 cino={0s:

#include "pch.h"
#include "scan.h"
#include "filesys.h"
#include "output.h"
#include "os.h"

static void AdjustSlashes(StrW& s)
{
    for (const WCHAR* walk = s.Text(); *walk; walk++)
    {
        if (*walk == '/')
            s.SetAt(walk, '\\');
    }
}

static bool IsDirPattern(const StrW& s)
{
    if (s.Length() && s.Text()[s.Length() - 1] == '\\')
        return true;
    const DWORD dwAttr = GetFileAttributes(s.Text());
    return (dwAttr != 0xffffffff && (dwAttr & FILE_ATTRIBUTE_DIRECTORY));
}

inline bool IsDriveOnly(const WCHAR* p)
{
    p += OS::IsExtendedPath(p);

    // If it does not have any path character, is 2 chars long and has a
    // ':' it must be a drive.
    return (p[0] &&
            p[0] != '\\' &&
            p[0] != '/' &&
            p[1] == ':' &&
            !p[2]);
}

inline void AddStar(StrW& s)
{
    EnsureTrailingSlash(s);
// REVIEW:  If the file system is FAT, is it necessary to append "*.*" instead of just "*"?
    s.Append(L"*");
}

bool ParsePatterns(int argc, const WCHAR** argv, std::vector<StrW>& patterns, Error& e)
{
    StrW tmp;

    // Collect patterns from argv.

    patterns.clear();
    while (argc)
    {
        const WCHAR* p = argv[0];

        if (wcschr(p, '"'))
        {
            // Strip any quotes.
            tmp.Clear();
            for (const WCHAR* walk = p; *walk; ++walk)
            {
                if (*walk != '"')
                    tmp.Append(walk, 1);
            }
            patterns.emplace_back(std::move(tmp));
        }
        else
        {
            patterns.emplace_back(p);
        }

        argc--;
        argv++;
    }

    // If no patterns present, add the current working directory.

    if (!patterns.size())
    {
        OS::GetCwd(tmp);
        patterns.emplace_back(std::move(tmp));
    }

    // Analyze the patterns to determine whether to list files in a directory
    // or open files in the viewer.

    bool open_files = false;
    for (auto& pattern : patterns)
    {
        if (!pattern.Length())
            OS::GetCwd(pattern);

        AdjustSlashes(pattern);

        if (IsDirPattern(pattern))
        {
            // If the pattern is a directory, use list mode and append * as
            // the wildcard.  This means if any directory is specified, then
            // all other patterns are ignored and list mode is used for only
            // the first directory pattern specified.
            StrW pat = std::move(pattern);
            if (IsDriveOnly(pat.Text()))
                OS::GetCwd(pat, *pat.Text());
            AddStar(pat);
            patterns.clear();
            patterns.emplace_back(std::move(pat));
            return false;
        }
        else
        {
            // Any non-directory implies opening files into the viewer.
            open_files = true;
        }
    }

    return open_files;
}

static void StripFilePart(StrW& s)
{
    const WCHAR* name = FindName(s.Text());
    if (name && name > s.Text())
        s.SetEnd(name);
    else
        s.Set(L".\\");
}

bool ScanPattern(const WCHAR* pattern, std::vector<FileInfo>& files, Error& e)
{
    WIN32_FIND_DATA fd;
    SHFind shFind = FindFirstFileW(pattern, &fd);

    if (shFind.Empty())
    {
        const DWORD dwErr = GetLastError();
        if (dwErr == ERROR_FILE_NOT_FOUND)
        {
        }
        else
        {
            e.Sys(dwErr);
            return false;
        }
    }
    else
    {
        StrW dir;

        {
            dir.Set(pattern);
            StripFilePart(dir);

            WCHAR* file_part;
            StrW full;
            full.ReserveMaxPath();
            const DWORD len = GetFullPathName(dir.Text(), full.Capacity(), full.Reserve(), &file_part);
            if (!len)
            {
                e.Sys();
                return false;
            }
            else if (len >= full.Capacity())
            {
                e.Sys(ERROR_FILENAME_EXCED_RANGE);
                return false;
            }

            dir = std::move(full);
        }

        do
        {
            if (fd.cFileName[0] != '.' || fd.cFileName[1])
            {
                FileInfo info;
                info.Init(&fd, dir.Text());
                files.emplace_back(std::move(info));
            }
        }
        while (FindNextFile(shFind, &fd));

        const DWORD dwErr = GetLastError();
        if (dwErr && dwErr != ERROR_NO_MORE_FILES)
        {
            e.Sys(dwErr);
            return false;
        }
    }

    return true;
}

static bool ScanPatterns(const std::vector<StrW>& patterns, std::vector<FileInfo>& files, Error& e)
{
    files.clear();

    StrW s;
    for (const auto& pat : patterns)
    {
        if (!ScanPattern(pat.Text(), files, e))
            return false;
    }

    return true;
}

bool ScanFiles(int argc, const WCHAR** argv, std::vector<FileInfo>& files, StrW& dir, Error& e)
{
    files.clear();

    std::vector<StrW> patterns;
    bool open_files = ParsePatterns(argc, argv, patterns, e);

    if (!e.Test())
    {
        if (ScanPatterns(patterns, files, e) && !files.size())
        {
            // No matching files; take only the directory portion from the
            // first pattern.
            StrW tmp;
            tmp.Set(patterns[0]);
            const WCHAR* strip = FindName(tmp.Text());
            if (strip)
                tmp.SetEnd(strip);
            else
                OS::GetCwd(tmp);
            AddStar(tmp);
            patterns.clear();
            patterns.emplace_back(std::move(tmp));

            // Try again.
            assert(patterns.size() == 1);
            if (!ScanPatterns(patterns, files, e) || !files.size())
            {
                // One last try, using the current working directory.
                OS::GetCwd(patterns[0]);
                AddStar(patterns[0]);
                ScanPatterns(patterns, files, e);
            }

            // Fall back to showing a file list.
            open_files = false;
        }
    }

    if (!open_files && patterns.size())
        dir.Set(patterns[0]);

    return open_files;
}

