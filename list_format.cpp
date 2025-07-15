// Copyright (c) 2024 by Christopher Antos
// License: http://opensource.org/licenses/MIT

// vim: set et ts=4 sw=4 cino={0s:

#include "pch.h"
#include "list_format.h"
#include "sorting.h"
#include "filesys.h"
#include "colors.h"
#include "output.h"
#include "ecma48.h"
#include "wcwidth.h"
#include "wcwidth_iter.h"
#include "columns.h"

#include <algorithm>
#include <memory>

enum class Justification { None, FAT, NONFAT };

static WCHAR s_chTruncated = L'\x2026'; // Horizontal Ellipsis character.
static const WCHAR c_dir_up[] = L"\x2191";
static const WCHAR c_dir_down[] = L"\x2193";
static const WCHAR c_tag_char[] = L"\x25c0";
static const WCHAR c_div_char[] = L"\x2595"; //L"\x2592"; //L"\x2502"; //L"\x250a";
static ColorScaleFields s_scale_fields = SCALE_NONE;
static bool s_gradient = true;
static bool s_mini_decimal = true;
static bool s_no_dir_tag = false;
static WCHAR s_size_style = 'm';
static WCHAR s_time_style = 0;

/*
 * Configuration functions.
 */

bool SetColorScale(const WCHAR* s)
{
    if (!s)
        return false;
    if (!_wcsicmp(s, L"") || !_wcsicmp(s, L"all"))
        s_scale_fields = ~SCALE_NONE;
    else if (!_wcsicmp(s, L"none"))
        s_scale_fields = SCALE_NONE;
    else if (!_wcsicmp(s, L"size"))
        s_scale_fields = SCALE_SIZE;
    else if (!_wcsicmp(s, L"time") || !_wcsicmp(s, L"date") || !_wcsicmp(s, L"age"))
        s_scale_fields = SCALE_TIME;
    else
        return false;
    return true;
}

ColorScaleFields GetColorScaleFields()
{
    return s_scale_fields;
}

bool SetColorScaleMode(const WCHAR* s)
{
    if (!s)
        return false;
    else if (!_wcsicmp(s, L"fixed"))
        s_gradient = false;
    else if (!_wcsicmp(s, L"gradient"))
        s_gradient = true;
    else
        return false;
    return true;
}

bool IsGradientColorScaleMode()
{
    return s_gradient;
}

/*
 * Formatter functions.
 */

static LCID s_lcid = 0;
static unsigned s_locale_date_time_len = 0;
static WCHAR s_locale_date[80];
static WCHAR s_locale_time[80];
static WCHAR s_locale_monthname[12][10];
static unsigned s_locale_monthname_len[12];
static unsigned s_locale_monthname_longest_len = 1;
static WCHAR s_decimal[2];
static WCHAR s_thousand[2];

void InitLocale()
{
    WCHAR tmp[80];

    // NOTE: Set a breakpoint on GetLocaleInfo in CMD.  Observe in the
    // assembly code that before it calls GetLocaleInfo, it calls
    // GetUserDefaultLCID and then tests for certain languages and ... uses
    // English instead.  I don't understand why it's doing that, but since I'm
    // trying to behave similarly I guess I'll do the same?
    s_lcid = GetUserDefaultLCID();
    if ((PRIMARYLANGID(s_lcid) == LANG_ARABIC) ||
        (PRIMARYLANGID(s_lcid) == LANG_FARSI) ||
        (PRIMARYLANGID(s_lcid) == LANG_HEBREW) ||
        (PRIMARYLANGID(s_lcid) == LANG_HINDI) ||
        (PRIMARYLANGID(s_lcid) == LANG_TAMIL) ||
        (PRIMARYLANGID(s_lcid) == LANG_THAI))
        s_lcid = MAKELCID(MAKELANGID(LANG_ENGLISH, SUBLANG_DEFAULT), SORT_DEFAULT); // 0x409

    if (!GetLocaleInfo(s_lcid, LOCALE_SDECIMAL, s_decimal, _countof(s_decimal)))
        wcscpy_s(s_decimal, L".");
    if (!GetLocaleInfo(s_lcid, LOCALE_STHOUSAND, s_thousand, _countof(s_thousand)))
        wcscpy_s(s_thousand, L",");

    static const struct { LCTYPE lst; const WCHAR* dflt; } c_monthname_lookup[] =
    {
        { LOCALE_SABBREVMONTHNAME1,  L"Jan" },
        { LOCALE_SABBREVMONTHNAME2,  L"Feb" },
        { LOCALE_SABBREVMONTHNAME3,  L"Mar" },
        { LOCALE_SABBREVMONTHNAME4,  L"Apr" },
        { LOCALE_SABBREVMONTHNAME5,  L"May" },
        { LOCALE_SABBREVMONTHNAME6,  L"Jun" },
        { LOCALE_SABBREVMONTHNAME7,  L"Jul" },
        { LOCALE_SABBREVMONTHNAME8,  L"Aug" },
        { LOCALE_SABBREVMONTHNAME9,  L"Sep" },
        { LOCALE_SABBREVMONTHNAME10, L"Oct" },
        { LOCALE_SABBREVMONTHNAME11, L"Nov" },
        { LOCALE_SABBREVMONTHNAME12, L"Dec" },
    };
    static_assert(_countof(c_monthname_lookup) == 12, "wrong number of month strings");
    static_assert(_countof(c_monthname_lookup) == _countof(s_locale_monthname), "wrong number of month strings");
    for (unsigned i = _countof(c_monthname_lookup); i--;)
    {
        if (!GetLocaleInfo(s_lcid, c_monthname_lookup[i].lst, s_locale_monthname[i], _countof(s_locale_monthname[i])))
            wcscpy_s(s_locale_monthname[i], c_monthname_lookup[i].dflt);
        s_locale_monthname_len[i] = __wcswidth(s_locale_monthname[i]);
        s_locale_monthname_longest_len = clamp<unsigned>(s_locale_monthname_len[i], s_locale_monthname_longest_len, 9);
    }

    // Get the locale-dependent short date and time formats.
    // (It looks like a loop, but it's just so 'break' can short circuit out.)
    while (true)
    {
        // First the date format...
        if (!GetLocaleInfo(s_lcid, LOCALE_SSHORTDATE, tmp, _countof(tmp)))
        {
            if (!GetLocaleInfo(s_lcid, LOCALE_IDATE, tmp, _countof(tmp)))
                break;

            if (tmp[0] == '0')
                wcscpy_s(tmp, L"MM/dd/yy");
            else if (tmp[0] == '1')
                wcscpy_s(tmp, L"dd/MM/yy");
            else if (tmp[0] == '2')
                wcscpy_s(tmp, L"yy/MM/dd");
            else
                break;
        }

        // Massage the locale date format so it's fixed width.
        StrW s;
        bool quoted = false;
        for (const WCHAR* pch = tmp; *pch; pch++)
        {
            if (*pch == '\'')
            {
                quoted = !quoted;
                s.Append(*pch);
            }
            else if (quoted)
            {
                s.Append(*pch);
            }
            else
            {
                unsigned c = 0;
                const WCHAR* pchCount = pch;
                while (*pchCount && *pchCount == *pch)
                {
                    s.Append(*pch);
                    pchCount++;
                    c++;
                }
                if (*pch == 'd' || *pch == 'M')
                {
                    if (c == 1)
                        s.Append(*pch);
                    else if (c == 4)
                        s.SetLength(s.Length() - 1);
                }
                pch = pchCount - 1;
            }
        }

        if (s.Length() >= _countof(s_locale_date))
            break;
        wcscpy_s(s_locale_date, s.Text());

        // Next the time format...
        if (!GetLocaleInfo(s_lcid, LOCALE_SSHORTTIME, tmp, _countof(tmp)))
            wcscpy_s(tmp, L"hh:mm tt");

        // Massage the locale time format so it's fixed width.
        s.Clear();
        quoted = false;
        for (const WCHAR* pch = tmp; *pch; pch++)
        {
            if (*pch == '\'')
            {
                quoted = !quoted;
                s.Append(*pch);
            }
            else if (quoted)
            {
                s.Append(*pch);
            }
            else if (*pch == 'h' || *pch == 'H' || *pch == 'm')
            {
                unsigned c = 0;
                const WCHAR* pchCount = pch;
                while (*pchCount && *pchCount == *pch)
                {
                    s.Append(*pch);
                    pchCount++;
                    c++;
                }
                if (c == 1)
                    s.Append(*pch);
                pch = pchCount - 1;
            }
            else
            {
                s.Append(*pch);
            }
        }

        if (s.Length() >= _countof(s_locale_time))
            break;
        wcscpy_s(s_locale_time, s.Text());

        s_locale_date_time_len = unsigned(wcslen(s_locale_date) + 2 + wcslen(s_locale_time));
        break;
    }
}

struct AttrChar
{
    WCHAR ch;
    DWORD dwAttr;
};

static const AttrChar c_attr_chars[] =
{
    { 'a', FILE_ATTRIBUTE_ARCHIVE },
    { 's', FILE_ATTRIBUTE_SYSTEM },
    { 'h', FILE_ATTRIBUTE_HIDDEN },
    { 'r', FILE_ATTRIBUTE_READONLY },
};
static const WCHAR c_attr_mask[] = L"ashr";

static void FormatAttributes(StrW& s, const DWORD dwAttr)
{
    WCHAR chNotSet = '-';

    const WCHAR* prev_color = nullptr;

    for (const auto ac : c_attr_chars)
    {
        const DWORD bit = (dwAttr & ac.dwAttr);
#if 0
        if (use_color)
        {
            const WCHAR* color = GetAttrLetterColor(bit);
            if (color != prev_color)
            {
                s.AppendColorElseNormal(color);
                prev_color = color;
            }
        }
#endif
        s.Append(bit ? ac.ch : chNotSet);
    }

    s.AppendNormalIf(prev_color);
}

static void JustifyFilename(StrW& s, const StrW& name, unsigned max_name_width, unsigned max_ext_width)
{
    assert(*name.Text() != '.');
    assert(max_name_width);
    assert(max_ext_width);

    const unsigned orig_len = s.Length();

    unsigned name_len = name.Length();
    unsigned name_width = __wcswidth(name.Text());
    unsigned ext_width = 0;
    const WCHAR* ext = FindExtension(name.Text());

    if (ext)
    {
        ext_width = __wcswidth(ext);
        name_width -= ext_width;
        name_len = unsigned(ext - name.Text());
        assert(*ext == '.');
        ext++;
        ext_width--;
    }

    if (!ext_width)
    {
        const unsigned combined_width = max_name_width + 1 + max_ext_width;
        if (name_width <= combined_width)
        {
            s.Append(name);
        }
        else
        {
            StrW tmp;
            tmp.Set(name);
            TruncateWcwidth(tmp, combined_width, s_chTruncated);
            s.Append(tmp);
        }
    }
    else
    {
        StrW tmp;
        tmp.Set(name.Text(), name_len);
        TruncateWcwidth(tmp, max_name_width, 0);
        tmp.AppendSpaces(max_name_width - name_width);
        tmp.Append(name_width > max_name_width ? '.' : ' ');
        s.Append(tmp);
        if (ext_width > max_ext_width)
        {
            tmp.Clear();
            tmp.Set(ext);
            TruncateWcwidth(tmp, max_ext_width, s_chTruncated);
            s.Append(tmp);
        }
        else
        {
            s.Append(ext);
        }
    }

    assert(max_name_width + 1 + max_ext_width >= __wcswidth(s.Text() + orig_len));
    s.AppendSpaces(max_name_width + 1 + max_ext_width - __wcswidth(s.Text() + orig_len));
}

void FormatFilename(StrW& s, const FileInfo* pfi, unsigned max_width, const WCHAR* color)
{
    const StrW& name = pfi->GetName();

    s.AppendColor(color);

    StrW tmp;
    const WCHAR* p = name.Text();
    unsigned name_width = 0;

    if (max_width)
    {
        const unsigned truncate_width = max_width - !!pfi->IsDirectory();
        name_width = __wcswidth(name.Text());
        if (name_width > truncate_width)
        {
            if (truncate_width)
            {
                if (!tmp.Length())
                    tmp.Set(p);
                name_width = TruncateWcwidth(tmp, truncate_width, s_chTruncated);
                p = tmp.Text();
            }
        }
    }

    if (max_width && p != tmp.Text())
        name_width = __wcswidth(p);

    if (pfi->IsDirectory())
    {
        if (pfi->IsPseudoDirectory())
            s.Append(c_dir_up);
        else
            s.Append(c_dir_down);
        if (max_width)
            ++name_width;
    }

    s.Append(p);

    if (max_width)
        s.AppendSpaces(max_width - name_width);

        const WCHAR* nolines = StripLineStyles(color);
    if (nolines != color)
    {
        unsigned spaces = 0;
        unsigned len = s.Length();
        while (len && s.Text()[len - 1] == ' ')
        {
            --len;
            ++spaces;
        }
        s.SetLength(len);
        if (nolines != color)
            s.AppendColor(nolines);
        s.AppendSpaces(spaces);
    }

    s.AppendNormalIf(color);
}

static unsigned GetSizeFieldWidthByStyle(WCHAR chStyle)
{
    switch (chStyle)
    {
    case 'm':
        return 4 + (s_mini_decimal ? 2 : 0);
    case 's':
        return 9;
    default:
        return 16;
    }
}

void FormatSize(StrW& s, unsigned __int64 cbSize, unsigned max_width, WCHAR chStyle, const WCHAR* color, const WCHAR* fallback_color)
{
    // FUTURE: CMD shows size for FILE_ATTRIBUTE_OFFLINE files in parentheses
    // to indicate it could take a while to retrieve them.

    if (!CanUseEscapeCodes())
        color = nullptr;
    else
    {
#if 0
        if (!color)
            color = GetSizeColor(cbSize);
#endif
        if (!color)
            color = fallback_color;
#if 0
        if (s_gradient && (s_scale_fields & SCALE_SIZE))
        {
            const WCHAR* gradient = ApplyGradient(color ? color : L"", cbSize, settings.m_min_size[*which], settings.m_max_size[*which]);
            if (gradient)
                color = gradient;
        }
#endif
    }

    const WCHAR* unit_color = nullptr;
    s.AppendColorNoLineStyles(color);

    switch (chStyle)
    {
    case 'm':
        {
            const unsigned iLoFrac = 2;
            const unsigned iHiFrac = 2;
            static const WCHAR c_size_chars[] = { 'K', 'K', 'M', 'G', 'T' };
            // The possible units must match the possible color scales.
            static_assert(_countof(c_size_chars) == 5, "size mismatch");

#if 0
            unit_color = (!nocolor && !(s_gradient && (s_scale_fields & SCALE_SIZE)) && which) ? GetSizeUnitColor(cbSize) : nullptr;
#endif

            double dSize = double(cbSize);
            unsigned iChSize = 0;

            while (dSize > 999 && iChSize + 1 < _countof(c_size_chars))
            {
                dSize /= 1024;
                iChSize++;
            }

            const unsigned mini_width = max_width ? max_width : 4;
            const bool abbrev = (s_mini_decimal || (iChSize >= iLoFrac && iChSize <= iHiFrac && dSize + 0.05 < 10.0));

            if (abbrev)
            {
                if (!iChSize)
                {
                    // Special case:  show 1..999 bytes as "1K", 0 bytes as "0K".
                    if (cbSize)
                    {
                        dSize /= 1024;
                        iChSize++;
                    }
                    dSize += 0.05;
                    if (dSize < 0.1 && cbSize)
                        cbSize = 1;
                    else
                        cbSize = static_cast<unsigned __int64>(dSize * 10);
                }
                else
                {
                    dSize += 0.05;
                    cbSize = static_cast<unsigned __int64>(dSize * 10);
                }
                assert(implies(max_width, max_width > 3));
                assert(mini_width > 3);
                s.Printf(L"%*I64u.%I64u", mini_width - 3, cbSize / 10, cbSize % 10);
            }
            else
            {
                dSize += 0.5;
                cbSize = static_cast<unsigned __int64>(dSize);
                if (!iChSize)
                {
#if 0
                    if (s_mini_bytes && cbSize <= 999)
                    {
                        s.Printf(L"%*I64u", mini_width, cbSize);
                        // s.Printf(L"%3I64u%c", cbSize, c_size_chars[iChSize]);
                        // s.Printf(L"%I64u.%I64u%c", cbSize / 100, (cbSize / 10) % 10, c_size_chars[iChSize + 1]);
                        break;
                    }
#endif

                    // Special case:  show 1..999 bytes as "1K", 0 bytes as "0K".
                    if (cbSize)
                    {
                        cbSize = 1;
                        iChSize++;
                    }
                }
                assert(implies(max_width, max_width > 1));
                assert(mini_width > 1);
                s.Printf(L"%*I64u", mini_width - 1, cbSize);
            }

            s.AppendColor(unit_color);
            s.Append(&c_size_chars[iChSize], 1);
        }
        break;

    case 's':
        {
            // If size fits in 8 digits, report it as is.

            if (cbSize < 100000000)
            {
                assert(implies(max_width, max_width > 1));
                s.Printf(L"%*I64u ", max_width ? max_width - 1 : 8, cbSize);
                break;
            }

            // Otherwise try to show fractional Megabytes or Terabytes.

            WCHAR chSize = 'M';
            double dSize = double(cbSize) / (1024 * 1024);

            if (dSize + 0.05 >= 1000000)
            {
                chSize = 'T';
                dSize /= 1024 * 1024;
            }

            dSize += 0.05;
            cbSize = static_cast<unsigned __int64>(dSize * 10);

            assert(implies(max_width, max_width > 3));
            s.Printf(L"%*I64u.%1I64u%c", max_width ? max_width - 3 : 6, cbSize / 10, cbSize % 10, chSize);
        }
        break;

    default:
        s.Printf(L"%*I64u", max_width ? max_width : 16, cbSize);
        break;
    }

    s.AppendNormalIf(color || unit_color);
}

static const WCHAR* GetDirectorySizeTag(WCHAR chStyle)
{
    static const WCHAR* const c_rgszSizeTags[] =
    {
        L"  <DIR>",         L"  <DIR>",     L" <D>",
    };
    static_assert(_countof(c_rgszSizeTags) == 1 * 3, "size mismatch");

    unsigned iWidth;
    switch (chStyle)
    {
    case 'm':
        iWidth = 2;
        break;
    case 's':
        iWidth = 1;
        break;
    default:
        iWidth = 0;
        break;
    }
    return c_rgszSizeTags[0 * 3 + iWidth];
}

static void FormatFileSize(StrW& s, const FileInfo* pfi, WCHAR chStyle=0, const WCHAR* fallback_color=nullptr, unsigned size_width=0)
{
    const WCHAR* const tag = pfi->IsDirectory() ? GetDirectorySizeTag(chStyle) : nullptr;

#ifdef DEBUG
    const unsigned orig_len = s.Length();
#endif

    const unsigned max_width = size_width ? size_width : GetSizeFieldWidthByStyle(chStyle);

    if (tag)
    {
        const bool can_use_color = CanUseEscapeCodes();
        if (s_no_dir_tag ||
            (can_use_color && (s_scale_fields & SCALE_SIZE)))
        {
            const unsigned trailing = (chStyle == 's');
            s.AppendSpaces(max_width - 1 - trailing);
#if 0
            const WCHAR* color = can_use_color ? GetColorByKey(L"xx") : nullptr;
            s.AppendColor(color);
#endif
            s.Append(L"-");
#if 0
            s.AppendNormalIf(color);
#endif
            s.AppendSpaces(trailing);
        }
        else
        {
            if (!can_use_color)
                fallback_color = nullptr;
            s.AppendColorNoLineStyles(fallback_color);
            if (s_mini_decimal)
            {
                // Right align.
                s.AppendSpaces(max_width - unsigned(wcslen(tag)));
                s.Append(tag);
            }
            else
            {
                // Left align.
                s.Append(tag);
                s.AppendSpaces(max_width - unsigned(wcslen(tag)));
            }
            s.AppendNormalIf(fallback_color);
        }
    }
    else
    {
        FormatSize(s, pfi->GetSize(), max_width, chStyle, nullptr, fallback_color);
    }
}

static void FormatLocaleDateTime(StrW& s, const SYSTEMTIME* psystime)
{
    WCHAR tmp[128];

    if (GetDateFormat(s_lcid, 0, psystime, s_locale_date, tmp, _countof(tmp)))
        s.Append(tmp);
    s.Append(L"  ");
    if (GetTimeFormat(s_lcid, 0, psystime, s_locale_time, tmp, _countof(tmp)))
        s.Append(tmp);
}

static unsigned GetTimeFieldWidthByStyle(WCHAR chStyle)
{
    switch (chStyle)
    {
    case 'l':           assert(s_locale_date_time_len); return s_locale_date_time_len;
    case 'p':           return 12;      // "DD Mmm  YYYY"  or  "DD Mmm HH:mm"
    case 'o':           return 16;      // "YYYY-MM-DD HH:mm"
    case 's':           return 14;      // "MM/DD/YY HH:mm"
    case 'm':           return 11;      // "MM/DD HH:mm"  or  "MM/DD  YYYY"
    default:            return 16;      // "MM/DD/YYYY HH:mm"
    }
}

static const SYSTEMTIME& NowAsLocalSystemTime()
{
    static const SYSTEMTIME now = [](){
        SYSTEMTIME systime;
        GetLocalTime(&systime);
        return systime;
    }();
    return now;
}

static const SYSTEMTIME& NowAsSystemTime()
{
    static const SYSTEMTIME now = [](){
        SYSTEMTIME systime;
        GetSystemTime(&systime);
        return systime;
    }();
    return now;
}

static const FILETIME& NowAsFileTime()
{
    static const FILETIME now = [](){
        FILETIME ft;
        SystemTimeToFileTime(&NowAsSystemTime(), &ft);
        return ft;
    }();
    return now;
}

static void FormatTime(StrW& s, const FileInfo* pfi, WCHAR chStyle, const WCHAR* fallback_color=nullptr)
{
    SYSTEMTIME systime;

    {
        FILETIME ft;
        FileTimeToLocalFileTime(&pfi->GetModifiedTime(), &ft);
        FileTimeToSystemTime(&ft, &systime);
    }

#ifdef DEBUG
    const unsigned orig_len = s.Length();
#endif

    const WCHAR* color = nullptr;

    {
#if 0
        color = GetColorByKey(L"da");
#endif
        if (!color)
            color = fallback_color;
#if 0
        if (s_gradient && (s_scale_fields & SCALE_TIME))
        {
            const WCHAR* gradient = ApplyGradient(color ? color : L"", FileTimeToULONGLONG(pfi->GetFileTime(which)), settings.m_min_time[which], settings.m_max_time[which]);
            if (gradient)
                color = gradient;
        }
#endif
        s.AppendColorNoLineStyles(color);
    }

    switch (chStyle)
    {
    case 'l':
        // Locale format.
        FormatLocaleDateTime(s, &systime);
        break;

    case 'p':
        // Compact format, 12 characters (depending on width of longest
        // abbreviated month name).
        {
            const SYSTEMTIME& systimeNow = NowAsLocalSystemTime();
            const unsigned iMonth = clamp<WORD>(systime.wMonth, 1, 12) - 1;
            const unsigned iMonthFile = unsigned(systime.wYear) * 12 + iMonth;
            const unsigned iMonthNow = unsigned(systimeNow.wYear) * 12 + systimeNow.wMonth - 1;
            const bool fShowYear = (iMonthFile > iMonthNow || iMonthFile + 6 < iMonthNow);
            s.Printf(s_locale_monthname[iMonth]);
            s.AppendSpaces(s_locale_monthname_longest_len - s_locale_monthname_len[iMonth]);
            s.Printf(L" %2u", systime.wDay);
            if (fShowYear)
                s.Printf(L"  %04u", systime.wYear);
            else
                s.Printf(L" %02u:%02u", systime.wHour, systime.wMinute);
        }
        break;

    case 'o':
        // long-iso format, 16 characters.
        s.Printf(L"%04u-%02u-%02u %2u:%02u",
                 systime.wYear,
                 systime.wMonth,
                 systime.wDay,
                 systime.wHour,
                 systime.wMinute);
        break;

    case 's':
        // 14 characters.
        s.Printf(L"%2u/%02u/%02u %2u:%02u",
                 systime.wMonth,
                 systime.wDay,
                 systime.wYear % 100,
                 systime.wHour,
                 systime.wMinute);
        break;

    case 'm':
        // 11 characters.
        {
            const SYSTEMTIME& systimeNow = NowAsLocalSystemTime();
            const unsigned iMonthFile = unsigned(systime.wYear) * 12 + systime.wMonth - 1;
            const unsigned iMonthNow = unsigned(systimeNow.wYear) * 12 + systimeNow.wMonth - 1;
            const bool fShowYear = (iMonthFile > iMonthNow || iMonthFile + 6 < iMonthNow);
            if (fShowYear)
            {
                //s.Printf(L"%2u/%02u/%04u ",
                s.Printf(L"%2u/%02u  %04u",
                //s.Printf(L"%2u/%02u--%04u",
                //s.Printf(L"%2u/%02u %04u*",
                         systime.wMonth,
                         systime.wDay,
                         systime.wYear);
            }
            else
            {
                s.Printf(L"%2u/%02u %02u:%02u",
                         systime.wMonth,
                         systime.wDay,
                         systime.wHour,
                         systime.wMinute);
            }
        }
        break;

    case 'n':
    default:
        // 16 characters.
        s.Printf(L"%2u/%02u/%04u %2u:%02u",
                 systime.wMonth,
                 systime.wDay,
                 systime.wYear,
                 systime.wHour,
                 systime.wMinute);
        break;
    }

    s.AppendNormalIf(color);
}

/*
 * Public functions.
 */

static unsigned WidthForFileInfoDetails(const FileInfo* pfi, int details, unsigned size_width)
{
    unsigned width = 0;

    if (details)
    {
        // Time.
        if (details >= 2)
        {
            ++width;                    // Space.
            width += GetTimeFieldWidthByStyle(s_time_style);
        }

        // Size.
        if (details >= 1)
        {
            ++width;                    // Space.
            width += WidthForFileInfoSize(pfi, details, size_width);
        }

        // Attributes.
        if (details >= 3)
        {
            ++width;                    // Space.
            width += 4;
        }
    }

    ++width;                            // Divider or tag indicator.

    return width;
}

inline WCHAR SizeStyleForDetails(int details)
{
    return (details >= 3) ? 0 : s_size_style;
}

unsigned WidthForFileInfoSize(const FileInfo* pfi, int details, int size_width)
{
    unsigned width = 0;

    if (details >= 1)
    {
        if (!size_width)
        {
            size_width = GetSizeFieldWidthByStyle(SizeStyleForDetails(details));
        }
        else if (size_width < 0)
        {
            if (pfi->IsDirectory())
            {
                // The width for directories is constant for a size_style, so
                // the caller is responsible for calculating it.
                size_width = 0;
            }
            else
            {
                size_width = 0;
                auto size = pfi->GetSize();
                do
                {
                    ++size_width;
                    size /= 10;
                }
                while (size);
            }
        }
        width += size_width;
    }

    return width;
}

unsigned WidthForDirectorySize(int details)
{
    const WCHAR* const tag = GetDirectorySizeTag(SizeStyleForDetails(details));
    assert(tag);
    return unsigned(wcslen(tag));
}

unsigned WidthForFileInfo(const FileInfo* pfi, int details, int size_width)
{
    assert(pfi);

    unsigned width = 0;

    if (pfi)
    {
        width += !!pfi->IsDirectory();  // Up or down arrow for directory.
        width += __wcswidth(pfi->GetName().Text());
    }

    width += WidthForFileInfoDetails(pfi, details, size_width);

    return width;
}

unsigned FormatFileInfo(StrW& s, const FileInfo* pfi, unsigned max_width, int details, bool selected, bool tagged, int size_width)
{
    const WCHAR* color = nullptr;
    if (CanUseEscapeCodes())
    {
        color = GetColor(selected ?
                         (tagged ? ColorElement::SelectedTagged : ColorElement::Selected) :
                         (tagged ? ColorElement::Tagged : ColorElement::File));
    }

    const unsigned orig_len = s.Length();

    s.AppendColor(color);

    const unsigned details_width = WidthForFileInfoDetails(pfi, details, size_width);
    const unsigned filename_width = (max_width > details_width ? max_width - details_width : 0);
    assert(filename_width > 0);
    FormatFilename(s, pfi, filename_width);
    assert(filename_width == cell_count(s.Text() + orig_len));

    if (details)
    {
        if (details >= 2)
        {
            s.AppendSpaces(1);
            FormatTime(s, pfi, s_time_style);
        }
        if (details >= 1)
        {
            s.AppendSpaces(1);
            FormatFileSize(s, pfi, SizeStyleForDetails(details), nullptr, size_width);
        }
        if (details >= 3)
        {
            s.AppendSpaces(1);
            FormatAttributes(s, pfi->GetAttributes());
        }
    }

    const WCHAR* div_color = nullptr;
    if (tagged)
        s.Append(c_tag_char);
    else
    {
        if (CanUseEscapeCodes())
        {
            div_color = GetTextColorParams(ColorElement::Divider);
            s.AppendColorOverlay(nullptr, div_color);
        }
        s.Append(c_div_char);
    }

    s.AppendNormalIf(color || div_color);

    assert(filename_width + details_width == cell_count(s.Text() + orig_len));

    return cell_count(s.Text() + orig_len);
}

unsigned FormatFileData(StrW& s, const WIN32_FIND_DATAW& fd)
{
    const unsigned orig_len = s.Length();

    FileInfo info;
    info.Init(&fd);

    FormatTime(s, &info, s_time_style);
    // s.AppendSpaces(2);
    // FormatFileSize(s, &info, 's');

    return cell_count(s.Text() + orig_len);
}

