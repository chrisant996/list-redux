// Copyright (c) 2025 by Christopher Antos
// License: http://opensource.org/licenses/MIT

// LIST-Redux is a modern terminal-based file list program in the spirit of
// the famous LIST Enhanced by Vernon D. Buerg.
//
// The original site was Buerg Software:
// http://www.buerg.com/list.htm
//
// That site no longer exists, as Mr. Buerg passed away in 2009.
// The Wayback Machine has a copy of the site here:
// https://web.archive.org/web/20080704121832/http://www.buerg.com/list.htm

// vim: set et ts=4 sw=4 cino={0s:

#include "pch.h"

#include <typeinfo>
#include <shellapi.h>

#include "version.h"
#include "argv.h"
#include "options.h"
#include "chooser.h"
#include "viewer.h"
#include "list_format.h"
#include "output.h"
#include "filesys.h"
#include "sorting.h"
#include "scan.h"
#include "colors.h"
#include "usage.h"
#include "wcwidth.h"
#include "filetype.h"
#include "os.h"

#include <memory>
#include <algorithm>

static const WCHAR c_opts[] = L"/:+?@:V";

static const WCHAR* get_env_prio(const WCHAR* a, const WCHAR* b=nullptr, const WCHAR* c=nullptr, const WCHAR** which=nullptr)
{
    const WCHAR* env = nullptr;
    if (a)
    {
        env = _wgetenv(a);
        if (env && which)
            *which = a;
    }
    if (!env && b)
    {
        env = _wgetenv(b);
        if (env && which)
            *which = b;
    }
    if (!env && c)
    {
        env = _wgetenv(c);
        if (env && which)
            *which = c;
    }
    return env;
}

int __cdecl _tmain(int argc, const WCHAR** argv)
{
    Error e;
    StrW s;

    const HANDLE hout = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!IsConsole(hout))
    {
        e.Set(L"error: stdout is redirected.");
        e.Report();
        return 1;
    }

    initialize_wcwidth();

    // Remember the app name, and generate the short usage text.

    StrW fmt;
    StrW app;
    StrW usage;
    app.Set(argc ? FindName(argv[0]) : L"LIST");
    const WCHAR* const ext = FindExtension(app.Text());
    if (ext)
        app.SetEnd(ext);
    app.ToLower();
    fmt.SetA(c_usage);
    usage.Printf(fmt.Text(), app.Text());

    // Skip past app name so we can parse command line options.

    if (argc)
    {
        argc--;
        argv++;
    }

    enum
    {
        LOI_UNIQUE_IDS              = 0x7FFF,
        LOI_MAX_LINE_LENGTH,
        LOI_MULTIBYTE,
        LOI_NO_MULTIBYTE,
    };

    static LongOption<WCHAR> long_opts[] =
    {
        { L"help",                  nullptr,            '?' },
        { L"version",               nullptr,            'V' },
        { L"max-line-length",       nullptr,            LOI_MAX_LINE_LENGTH, LOHA_REQUIRED },
        { L"multibyte",             nullptr,            LOI_MULTIBYTE },
        { L"no-multibyte",          nullptr,            LOI_NO_MULTIBYTE },
        { nullptr }
    };

    Options opts(99);

    WCHAR ch;
    const WCHAR* opt_value;

    // Then parse options from the command line.

    if (!opts.Parse(argc, argv, c_opts, usage.Text(), OPT_ANY|OPT_ANYWHERE|OPT_LONGABBR, long_opts))
    {
        fputws(opts.ErrorString(), stderr);
        SetGracefulExit();
        return 1;
    }

    // Full usage text.

    if (opts['?'])
    {
        const unsigned width = 80;
        s.Clear();
        app.ToUpper();
        fmt.SetA(MakeUsageString((width >= 88) ? 32 : 24));
        s.Printf(fmt.Text(), app.Text());
        ExpandTabs(s.Text(), s);
        WrapText(s.Text(), s, width);
        OutputConsole(hout, s.Text());
        SetGracefulExit();
        return 0;
    }

    // Version information.

    if (opts['V'])
    {
        app.ToUpper();
        s.Clear();
        s.Printf(L"List Redux %hs, built %hs\nhttps://github.com/chrisant996/list-redux\n", VERSION_STR, __DATE__);
        OutputConsole(hout, s.Text());
        SetGracefulExit();
        return 0;
    }

    // Interpret the options.

    InitLocale();

    const LongOption<WCHAR>* long_opt;
    std::vector<StrW> files;

    for (unsigned ii = 0; opts.GetValue(ii, ch, opt_value, &long_opt); ii++)
    {
        switch (ch)
        {
        case '@':
            {
                StrA line;
                StrW name;
                line.ReserveMaxPath();
                bool first = true;
                bool utf8 = false;
                FILE* f = _wfopen(opt_value, L"r");
                if (f)
                {
                    while (!feof(f))
                    {
                        line.Clear();
                        fgets(line.Reserve(), line.Capacity(), f);
                        line.ResyncLength();
                        line.TrimRight();

                        const char* p = line.Text();
                        size_t len = line.Length();
                        if (first)
                        {
                            utf8 = (p[0] == 0xef && p[1] == 0xbb && p[2] == 0xbf);
                            len -= (utf8 ? 3 : 0);
                            first = false;
                        }

                        if (len)
                        {
                            Error e2;
                            name.SetFromCodepage(utf8 ? CP_UTF8 : CP_ACP, p, len);
                            if (OS::GetFullPathName(name.Text(), s, e2))
                                files.emplace_back(std::move(s));
                        }
                    }
                    fclose(f);
                }
            }
            break;

        case 'X':
            // TODO:  Etc.
            break;

        default:
            if (!long_opt)
                continue; // Other flags are handled separately further below.
            switch (long_opt->value)
            {
            case LOI_MAX_LINE_LENGTH:
                SetMaxLineLength(opt_value);
                break;
            case LOI_MULTIBYTE:
            case LOI_NO_MULTIBYTE:
                SetMultiByteEnabled(long_opt->value == LOI_MULTIBYTE);
                break;
            }
            break;
        }
    }

    InitColors();
    TryCoInitialize();

    StrW dir;
    std::vector<FileInfo> fileinfos;
    bool navigate = false;
    bool done = false;

    const bool piped = !IsConsole(GetStdHandle(STD_INPUT_HANDLE));
    if (piped)
    {
        done = true;
        files.insert(files.begin(), L"<stdin>");
        SetPipedInput();
    }
    else
    {
        navigate = (argc || files.empty()) && !ScanFiles(argc, argv, fileinfos, dir, e, true/*cmdline*/);
        if (e.Test())
            return e.Report();

        std::stable_sort(fileinfos.begin(), fileinfos.end(), CmpFileInfo);

        if (!navigate || !files.empty())
        {
            for (const auto& info : fileinfos)
            {
                if (!info.IsDirectory())
                {
                    info.GetPathName(s);
                    files.emplace_back(std::move(s));
                }
            }
            fileinfos.clear();
            navigate = files.empty();
        }
    }

    Interactive interactive;
    Chooser chooser(&interactive);

    if (piped)
    {
        ViewFiles(files, s, e);
    }
    else if (navigate)
    {
        chooser.Navigate(dir.Text(), std::move(fileinfos));
        assert(fileinfos.empty());
    }

    while (!done && !e.Test())
    {
        if (files.size())
        {
            switch (ViewFiles(files, s, e))
            {
            case ViewerOutcome::RETURN:
                if (navigate)
                    break;
                __fallthrough;
            case ViewerOutcome::EXITAPP:
                done = true;
                break;
            }

            files.clear();
        }
        else
        {
            switch (chooser.Go(e))
            {
            case ChooserOutcome::VIEWONE:
                files.clear();
                s = chooser.GetSelectedFile();
                if (s.Length())
                    files.emplace_back(std::move(s));
                break;
            case ChooserOutcome::VIEWTAGGED:
                files = chooser.GetTaggedFiles();
                break;
            case ChooserOutcome::EXITAPP:
                done = true;
                break;
            }
        }
    }

    if (e.Test())
    {
        interactive.End();
        return e.Report();
    }

    SetGracefulExit();
    return 0;
}

