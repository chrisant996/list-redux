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

static void ReadOptions(const WCHAR* ini_filename)
{
    WCHAR value[256];

    if (ReadConfigString(ini_filename, L"Options", L"Details", value, _countof(value)))
    {
        if (wcsicmp(value, L"1") ||
            wcsicmp(value, L"2") ||
            wcsicmp(value, L"3") ||
            wcsicmp(value, L"4"))
        {
            g_options.details = value[0] - '1';
        }
    }

#ifndef DEBUG
    // MaxLineLength is overridden in DEBUG builds, so avoid writing out the
    // value when running a DEBUG build.
    if (ReadConfigString(ini_filename, L"Options", L"MaxLineLength", value, _countof(value)))
        SetMaxLineLength(value);
#endif

    if (ReadConfigString(ini_filename, L"Options", L"Wrap", value, _countof(value)))
        SetWrapping(ParseBoolean(value));

#ifdef INCLUDE_MENU_ROW
    if (ReadConfigString(ini_filename, L"Options", L"MenuRow", value, _countof(value)))
        g_options.show_menu = ParseBoolean(value);
#endif

    if (ReadConfigString(ini_filename, L"Options", L"Scrollbar", value, _countof(value)))
        SetViewerScrollbar(ParseBoolean(value));

    if (ReadConfigString(ini_filename, L"Options", L"Emulate", value, _countof(value)))
        SetEmulation(value);
}

static bool WriteOptions(const WCHAR* ini_filename)
{
    bool ok = true;
    WCHAR sz[128];
    StrW value;

    sz[0] = '1' + g_options.details;
    sz[1] = 0;
    ok &= WriteConfigString(ini_filename, L"Options", L"Details", sz);

    value.Clear();
    value.Printf(L"%u", g_options.max_line_length);
    ok &= WriteConfigString(ini_filename, L"Options", L"MaxLineLength", value.Text());
    ok &= WriteConfigString(ini_filename, L"Options", L"Wrap", BooleanValue(g_options.wrapping, BooleanStyle::YesNo));
#ifdef INCLUDE_MENU_ROW
    ok &= WriteConfigString(ini_filename, L"Options", L"MenuRow", BooleanValue(g_options.show_menu, BooleanStyle::YesNo));
#endif
    ok &= WriteConfigString(ini_filename, L"Options", L"Scrollbar", BooleanValue(g_options.show_scrollbar, BooleanStyle::YesNo));
    ok &= WriteConfigString(ini_filename, L"Options", L"Emulate", GetEmulationConfigValue());

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
