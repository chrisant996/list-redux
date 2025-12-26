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
static bool ParseBoolean(const WCHAR* value, bool target=true)
{
    if (target)
        return (!wcsicmp(value, L"true") || !wcsicmp(value, L"1") || !wcsicmp(value, L"on") || !wcsicmp(value, L"yes"));
    else
        return (!wcsicmp(value, L"false") || !wcsicmp(value, L"0") || !wcsicmp(value, L"off") || !wcsicmp(value, L"no"));
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

    if (ReadConfigString(ini_filename, L"Options", L"Scrollbar", value, _countof(value)))
        SetViewerScrollbar(ParseBoolean(value));

    if (ReadConfigString(ini_filename, L"Options", L"Wrap", value, _countof(value)))
        SetWrapping(ParseBoolean(value));

#ifdef INCLUDE_MENU_ROW
    if (ReadConfigString(ini_filename, L"Options", L"MenuRow", value, _countof(value)))
        g_options.show_menu = ParseBoolean(value);
#endif

    if (ReadConfigString(ini_filename, L"Options", L"MaxLineLength", value, _countof(value)))
        SetMaxLineLength(value);

    if (ReadConfigString(ini_filename, L"Options", L"Emulate", value, _countof(value)))
    {
        if (ParseBoolean(value, true))
            SetEmulation(true);
        else if (ParseBoolean(value, false))
            SetEmulation(false);
        else
            SetEmulation(-1);
    }
}
#endif

void LoadConfig()
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
    if (OS::GetEnv(L"USERPROFILE", userprofile))
        ini_filename.SetMaybeRooted(userprofile.Text(), L".listredux");

    ReadColors(ini_filename.Text());
    ReadOptions(ini_filename.Text());
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
#endif
