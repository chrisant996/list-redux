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

static bool ScanPattern(const WCHAR* pattern, std::vector<FileInfo>& files, Error& e, const bool include_files, const bool include_dirs)
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

            StrW full;
            full.ReserveMaxPath();
            if (!OS::GetFullPathName(dir.Text(), full, e))
                return false;

            dir = std::move(full);
        }

        do
        {
            const bool is_dir = !!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
            if (is_dir && !include_dirs)
                continue;
            if (!is_dir && !include_files)
                continue;

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

    PathW s;
    for (const auto& pat : patterns)
    {
        const WCHAR* name = FindName(pat.Text());
        const bool pure_star = (name[0] == '*' && !name[1]);
        if (!ScanPattern(pat.Text(), files, e, true/*include_files*/, pure_star/*include_dirs*/))
            return false;
        if (!pure_star)
        {
            s.Set(pat);
            s.EnsureTrailingSlash();
            s.ToParent();
            s.JoinComponent(L"*");
            if (!ScanPattern(s.Text(), files, e, false/*include_files*/, true/*include_dirs*/))
                return false;
        }
    }

    return true;
}

bool ScanFiles(int argc, const WCHAR** argv, std::vector<FileInfo>& files, StrW& dir, Error& e, bool cmdline)
{
    files.clear();

    std::vector<StrW> patterns;
    bool open_files = ParsePatterns(argc, argv, patterns, e);

    if (!e.Test())
    {
        const bool pure_star = (patterns.size() == 1 && patterns[0].Length() == 1 && patterns[0].Text()[0] == '*');
        if (ScanPatterns(patterns, files, e) && cmdline && !pure_star)
        {
            size_t num_files = 0;
            for (const auto& file : files)
            {
                if (!file.IsDirectory())
                    ++num_files;
            }
            if (!num_files)
            {
                PathW pat;

                // No matching files; take only the directory portion from the
                // first pattern.
                {
                    StrW tmp;
                    tmp.Set(patterns[0]);
                    tmp.SetEnd(FindName(tmp.Text()));
                    if (!tmp.Empty())
                    {
                        OS::GetFullPathName(tmp.Text(), pat, e);
                        e.Clear();
                    }
                }

                if (pat.Empty())
                    OS::GetCwd(pat);
                pat.JoinComponent(L"*");

                patterns.clear();
                patterns.emplace_back(pat);

                // Try again.
                assert(patterns.size() == 1);
                if (!ScanPatterns(patterns, files, e) || !files.size())
                {
                    // One last try, using the current working directory.
                    OS::GetCwd(pat);
                    pat.JoinComponent(L"*");
                    patterns[0] = pat.Text();
                    ScanPatterns(patterns, files, e);
                }

                // Fall back to showing a file list.
                open_files = false;
            }
        }
    }

    if (patterns.size())
        dir.Set(patterns[0]);
    else
        dir.Clear();

    return open_files;
}

