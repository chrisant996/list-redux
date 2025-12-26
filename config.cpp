// Copyright (c) 2025 by Christopher Antos
// License: http://opensource.org/licenses/MIT

// vim: set et ts=4 sw=4 cino={0s:

#include "pch.h"
#include "config.h"
#include "colors.h"
#include "input.h"
#include "output.h"
#include "viewer.h"
#include "vieweroptions.h"
#include "os.h"

#include <strsafe.h>

#ifdef USE_REGISTRY_FOR_CONFIG
// TODO:  ...
#else
bool ParseBoolean(const WCHAR* value, bool target)
{
    if (target)
        return (!wcsicmp(value, L"true") || !wcsicmp(value, L"1") || !wcsicmp(value, L"on") || !wcsicmp(value, L"yes"));
    else
        return (!wcsicmp(value, L"false") || !wcsicmp(value, L"0") || !wcsicmp(value, L"off") || !wcsicmp(value, L"no"));
}

const WCHAR* BooleanValue(bool value, BooleanStyle style)
{
    switch (style)
    {
    default:
    case BooleanStyle::TrueFalse:   return value ? L"True" : L"False";
    case BooleanStyle::Digit:       return value ? L"1" : L"0";
    case BooleanStyle::OnOff:       return value ? L"On" : L"Off";
    case BooleanStyle::YesNo:       return value ? L"Yes" : L"No";
    }
}

static void GetDetails(StrW& out)
{
    out.Append('1' + g_options.details);
}
static void SetDetails(const WCHAR* value)
{
    if (wcsicmp(value, L"1") ||
        wcsicmp(value, L"2") ||
        wcsicmp(value, L"3") ||
        wcsicmp(value, L"4"))
    {
        g_options.details = value[0] - '1';
    }
}

static void GetMaxLineLength(StrW& out)
{
    out.Printf(L"%u", g_options.max_line_length);
}

static void GetWrapping(StrW& out)
{
    out = BooleanValue(g_options.wrapping);
}
void SetWrapping(const WCHAR* wrapping)
{
    g_options.wrapping = wrapping;
}

#ifdef INCLUDE_MENU_ROW
static void GetMenuRow(StrW& out)
{
    out = BooleanValue(g_options.show_menu);
}
static void SetMenuRow(const WCHAR* value)
{
    g_options.show_menu = ParseBoolean(value);
}
#endif

static void GetScrollbar(StrW& out)
{
    out = BooleanValue(g_options.show_scrollbar);
}
static void SetScrollbar(const WCHAR* value)
{
    g_options.show_scrollbar = ParseBoolean(value);
}

static void GetAsciiFilter(StrW& out)
{
    out = BooleanValue(g_options.ascii_filter);
}
static void SetAsciiFilter(const WCHAR* value)
{
    g_options.ascii_filter = ParseBoolean(value);
}

static void GetShowLineEndings(StrW& out)
{
    out = BooleanValue(g_options.show_line_endings);
}
static void SetShowLineEndings(const WCHAR* value)
{
    g_options.show_line_endings = ParseBoolean(value);
}

static void GetShowLineNumbers(StrW& out)
{
    out = BooleanValue(g_options.show_line_numbers);
}
static void SetShowLineNumbers(const WCHAR* value)
{
    g_options.show_line_numbers = ParseBoolean(value);
}

static void GetShowFileOffsets(StrW& out)
{
    out = BooleanValue(g_options.show_file_offsets);
}
static void SetShowFileOffsets(const WCHAR* value)
{
    g_options.show_file_offsets = ParseBoolean(value);
}

static void GetHexGrouping(StrW& out)
{
    out.Printf(L"%u", g_options.hex_grouping);
}
static void SetHexGrouping(const WCHAR* value)
{
    ULONGLONG n;
    if (ParseULongLong(value, n) && n < 4)
        g_options.hex_grouping = uint8(n);
}

static void GetShowEndOfFileLine(StrW& out)
{
    out = BooleanValue(g_options.show_endoffile_line);
}
static void SetShowEndOfFileLine(const WCHAR* value)
{
    g_options.show_endoffile_line = ParseBoolean(value);
}

static void GetTabWidth(StrW& out)
{
    out.Printf(L"%u", g_options.tab_width);
}
static void SetTabWidth(const WCHAR* value)
{
    ULONGLONG n;
    if (ParseULongLong(value, n) && n >= 2 && n <= 8)
        g_options.tab_width = uint16(n);
}

typedef void (*get_func_t)(StrW&);
typedef void (*set_func_t)(const WCHAR*);

struct OptionDefinition
{
    const WCHAR*    name;
    get_func_t      get_fn;
    set_func_t      set_fn;
};

static const OptionDefinition c_option_defs[] =
{
    { L"Details",           GetDetails, SetDetails },
#ifndef DEBUG
    // MaxLineLength is overridden in DEBUG builds, so avoid writing out the
    // value when running a DEBUG build.
    { L"MaxLineLength",     GetMaxLineLength, SetMaxLineLength },
#endif
    { L"Wrap",              GetWrapping, SetWrapping },
    { L"AsciiFilter",       GetAsciiFilter, SetAsciiFilter },
    { L"ShowLineEndings",   GetShowLineEndings, SetShowLineEndings },
    { L"ShowLineNumbers",   GetShowLineNumbers, SetShowLineNumbers },
    { L"ShowFileOffsets",   GetShowFileOffsets, SetShowFileOffsets },
    { L"HexGrouping",       GetHexGrouping, SetHexGrouping },
    { L"ShowEndOfFileLine", GetShowEndOfFileLine, SetShowEndOfFileLine },
    { L"TabWidth",          GetTabWidth, SetTabWidth },
#ifdef INCLUDE_MENU_ROW
    { L"MenuRow",           GetMenuRow, SetMenuRow },
#endif
    { L"Scrollbar",         GetScrollbar, SetScrollbar },
    { L"Emulate",           GetEmulation, SetEmulation },
};

static void ReadOptions(const WCHAR* ini_filename)
{
    WCHAR value[256];
    for (const auto& opt : c_option_defs)
    {
        if (ReadConfigString(ini_filename, L"Options", opt.name, value, _countof(value)))
            opt.set_fn(value);
    }
}

static bool WriteOptions(const WCHAR* ini_filename)
{
    bool ok = true;
    StrW value;
    for (const auto& opt : c_option_defs)
    {
        value.Clear();
        opt.get_fn(value);
        ok &= WriteConfigString(ini_filename, L"Options", opt.name, value.Text());
    }
    return ok;
}
#endif

bool LoadConfig()
{
#ifdef USE_REGISTRY_FOR_CONFIG
    HKEY hkeyUser = 0;
    HKEY hkeyApp = 0;
    if (RegOpenCurrentUser(KEY_READ, &hkeyUser))
        RegOpenKeyA(hkeyUser, "Software\\ListRedux", &hkeyApp);

    ReadColors(hkeyApp);
    ReadOptions(hkeyApp);

    if (hkeyApp)
        RegCloseKey(hkeyApp);
    if (hkeyUser)
        RegCloseKey(hkeyUser);
#else
    PathW ini_filename;
    StrW userprofile;
    if (!OS::GetEnv(L"USERPROFILE", userprofile))
        return false;

    ini_filename.SetMaybeRooted(userprofile.Text(), L".listredux");

    ReadColors(ini_filename.Text());
    ReadOptions(ini_filename.Text());
    return true;
#endif
}

bool SaveConfig(Error& e)
{
#ifdef USE_REGISTRY_FOR_CONFIG
#error NYI
#else
    PathW ini_filename;
    StrW userprofile;
    if (!OS::GetEnv(L"USERPROFILE", userprofile))
    {
        e.Set(L"Unable to save configuration; USERPROFILE environment variable is not set.");
        return false;
    }

    ini_filename.SetMaybeRooted(userprofile.Text(), L".listredux");

    bool ok = true;
    ok &= WriteColors(ini_filename.Text());
    ok &= WriteOptions(ini_filename.Text());

    if (!ok)
        e.Set(L"Unable to save one or more configuration settings.");

    return ok;
#endif
}

#ifdef USE_REGISTRY_FOR_CONFIG
bool ReadConfigString(HKEY hkeyApp, const WCHAR* name, WCHAR* out, uint32 max_len, const WCHAR* default_value)
{
    DWORD type;
    DWORD len = max_len;
    if (RegGetValueW(hkeyApp, nullptr, name, RRF_RT_REG_SZ, &type, out, &len) != ERROR_SUCCESS ||
        type != REG_SZ || !len || len >= max_len)
    {
        StringCchCopy(out, max_len, default_value ? default_value : L"");
        return false;
    }
    return true;
}
#else
bool ReadConfigString(const WCHAR* ini_filename, const WCHAR* section, const WCHAR* name, WCHAR* out, uint32 max_len, const WCHAR* default_value)
{
    if (!max_len)
        return false;

    out[0] = 0;

    if (ini_filename && *ini_filename)
    {
        GetPrivateProfileStringW(section, name, L"", out, max_len, ini_filename);
        if (out[0])
            return true;
    }

    StringCchCopy(out, max_len, default_value ? default_value : L"");
    return false;
}

bool WriteConfigString(const WCHAR* ini_filename, const WCHAR* section, const WCHAR* name, const WCHAR* value)
{
    if (ini_filename && *ini_filename)
        return !!WritePrivateProfileStringW(section, name, value, ini_filename);

    return false;
}
#endif
