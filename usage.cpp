// Copyright (c) 2025 by Christopher Antos
// License: http://opensource.org/licenses/MIT

// vim: set et ts=4 sw=4 cino={0s:

#include "pch.h"
#include "usage.h"
#include "viewer.h"

#include <vector>
#include <map>
#include <algorithm>

const char c_usage[] = "%s -? for help.";

enum FlagSection
{
    USAGE,
    FLAGS,
    MAX
};

struct FlagUsageInfo
{
    FlagSection         section;
    const char*         flag;
    const char*         desc;
};

static const FlagUsageInfo c_usage_args =
{
    USAGE,
    "[filespec [filespec ...]]",
    "Filespecs can be directories, file patterns, or file names.  If one or "
    "more directories are provided, a file chooser is shown for the first "
    "directory.  Otherwise, files matching file patterns or names are loaded "
    "into a file viewer.\n"
};

static const FlagUsageInfo c_usage_info[] =
{
    // USAGE -----------------------------------------------------------------
    { USAGE,    "-?, --help",               "Display this help text.\n" },
    { USAGE,    "-V, --version",            "Display version information.\n" },

    // FLAGS -----------------------------------------------------------------
    { FLAGS,    "-@ file",                  "Load files named in 'file' into a file viewer.\n" },
    { FLAGS,    "--emulate",                "Use built-in terminal emulator.\n" },
    { FLAGS,    "--emulate=mode",           "Override using terminal emulator.  'mode' can be 'off', 'on', or 'auto' (the default).\n" },
    { FLAGS,    "--no-emulate",             "Use native terminal (no emulation).\n" },
    { FLAGS,    "--input-file file",        "Load files named in 'file' into a file viewer.\n" },
    { FLAGS,    "--line num",               "Go to line 'num' in file viewer (base 10 by default).\n" },
    { FLAGS,    "--max-line-length num",    "Override the maximum line length (between 16 and $(MAXMAXLINELEN)).\n" },
    { FLAGS,    "--multibyte",              "Auto-detecting multibyte encodings.\n" },
    { FLAGS,    "--no-multibyte",           "Do not auto-detect multibyte encodings.\n" },
    { FLAGS,    "--offset num",             "Go to offset 'num' in file viewer (base 16 by default).\n" },
    { FLAGS,    "--wrapping",               "Wrap lines wider than the terminal.\n" },
    { FLAGS,    "--no-wrapping",            "Only wrap lines at maximum line length ($(MAXMAXLINELEN)).\n" },
};

static const char c_usage_prolog[] =
"List Redux - A File Viewing and Browsing Utility\n"
"\n"
"  \032This tool is a throwback to the famous LIST.COM for DOS, which was "
    "written by Vernon D. Buerg (1948-2009).  List Redux lets you browse "
    "files or view files, with various options.\n"
"\n"
"%s [options] [filespec [filespec ...]]\n"
"\n"
;

static const char c_usage_epilog[] =
#if 0
"\n"
"TBD.\n"
"\n"
"Environment variables:\n"
"\n"
"TBD.\n"
#else
""
#endif
;

static void DoReplacements(const char* in, StrA& out)
{
    out.Clear();
    while (*in)
    {
        const char* repl = strstr(in, "$(");
        if (repl)
        {
            out.Append(in, repl - in);
            if (strnicmp(repl, "$(MAXMAXLINELEN)", 16) == 0)
            {
                out.Printf("%u", GetMaxMaxLineLength());
                in = repl + 16;
            }
            else
            {
                out.Append(repl, 2);
                in = repl + 2;
            }
        }
        else
        {
            out.Append(in);
            in += strlen(in);
        }
    }
}

static unsigned s_flag_col_width = 24;
static void AppendFlagUsage(StrA& u, const FlagUsageInfo& info, bool skip_leading_spaces=false)
{
    const char* flag = info.flag;
    if (skip_leading_spaces)
    {
        while (*flag == ' ')
            ++flag;
    }

    unsigned flag_len = 2 + unsigned(strlen(flag));
    u.Append("  ");
    u.Append(flag);
    if (flag_len + 2 > s_flag_col_width)
    {
        u.Append("\n");
        flag_len = 0;
    }
    u.AppendSpaces(s_flag_col_width - flag_len);
    u.Append("\032");

    StrA desc;
    DoReplacements(info.desc, desc);
    for (const char* p = desc.Text(); *p;)
    {
        const char* const end = strchr(p, '\n');
        size_t len = end ? (end - p) : strlen(p);
        u.Append(p, len);
        u.Append("\n");
        len += !!end;
        p += len;
        if (*p)
            u.AppendSpaces(s_flag_col_width);
    }
}

static int CmpFlagChar(char a, char b)
{
    static bool s_init = true;
    static int s_order[256];
    if (s_init)
    {
        static char c_ordered[] =
        "\x00\x01\x02\x03\x04\x05\x06\x07\x08\x09\x0a\x0b\x0c\x0d\x0e\x0f"
        "\x10\x11\x12\x13\x14\x15\x16\x17\x18\x19\x1a\x1b\x1c\x1d\x1e\x1f"
        ", "    // So "-?," sorts before "-? foo".
        "!\"#$%&'()*+./"
        "0123456789"
        ":;<=>?@"
        "aAbBcCdDeEfFgGhHiIjJkKlLmMnNoOpPqQrRsStTuUvVwWxXyYzZ"
        "[\\]^_`"
        "{|}~"
        "-"
        "\x7f"
        ;
        static_assert(sizeof(c_ordered) == _countof(s_order) / 2 + 1, "wrong number of characters in c_ordered");
        for (int i = 0; i < sizeof(c_ordered) - 1; ++i)
            s_order[c_ordered[i]] = i;
        for (int i = 128; i < 256; ++i)
            s_order[i] = i;
        s_init = false;
    }
    return (s_order[a] - s_order[b]);
}

static bool CmpFlagName(size_t a, size_t b)
{
    const auto& a_info = c_usage_info[a];
    const auto& b_info = c_usage_info[b];
    const char* a_str = a_info.flag;
    const char* b_str = b_info.flag;

#ifndef KEEP_ASSOCIATED_TOGETHER
    while (*a_str == ' ') ++a_str;
    while (*b_str == ' ') ++b_str;
#endif

    const int a_long = (a_str[0] && a_str[1] == '-');
    const int b_long = (b_str[0] && b_str[1] == '-');

    int n = a_long - b_long;
    if (n)
        return (n < 0);

    while (true)
    {
        const unsigned a_c = *a_str;
        const unsigned b_c = *b_str;
        n = CmpFlagChar(a_c, b_c);
        if (n)
            return (n < 0);
        if (!a_c || !b_c)
        {
            assert(!a_c && !b_c);
            return false;
        }
        ++a_str;
        ++b_str;
    }
}

StrA MakeUsageString(unsigned flag_col_width)
{
    s_flag_col_width = flag_col_width;

    StrA u;
    u.Append(c_usage_prolog);
    AppendFlagUsage(u, c_usage_args);
    u.Append("\n");

    FlagSection section = USAGE;
    for (const auto& info : c_usage_info)
    {
        if (section != info.section)
        {
            section = info.section;
            switch (section)
            {
            case FLAGS: u.Append("\nFLAGS:\n\n"); break;
            }
        }

        AppendFlagUsage(u, info);
    }

    u.Append(c_usage_epilog);
    return u.Text();
}

