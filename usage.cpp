// Copyright (c) 2025 by Christopher Antos
// License: http://opensource.org/licenses/MIT

// vim: set et ts=4 sw=4 cino={0s:

#include "pch.h"
#include "usage.h"

#include <vector>
#include <map>
#include <algorithm>

const char c_usage[] = "%s -? for help.";

enum FlagSection
{
    USAGE,
    TBD,
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
    "[arg]",
    "Description of arg.\n"
};

static const FlagUsageInfo c_usage_info[] =
{
    // USAGE -----------------------------------------------------------------
    { USAGE,    "-?, --help",               "Display this help text.\n" },
    { USAGE,    "-V, --version",            "Display version information.\n" },

    // TBD -------------------------------------------------------------------
#if 0
    { TBD,      "-X",                       "Something.\n" },
#endif
};

static const char c_usage_prolog[] =
"TBD\n"
"\n"
"%s [options] [filespec [filespec ...]]\n"
"\n"
;

static const char c_usage_epilog[] =
"\n"
"TBD.\n"
"\n"
"Environment variables:\n"
"\n"
"TBD.\n"
;

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

    const char* p = info.desc;
    while (*p)
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
            case TBD:   u.Append("\nTBD:\n"); break;
            }
        }

        AppendFlagUsage(u, info);
    }

    u.Append(c_usage_epilog);
    return u.Text();
}

