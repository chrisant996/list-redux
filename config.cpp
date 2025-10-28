// Copyright (c) 2025 by Christopher Antos
// License: http://opensource.org/licenses/MIT

// vim: set et ts=4 sw=4 cino={0s:

#include "pch.h"
#include "config.h"
#include "colors.h"
#include "viewer.h"
#include "os.h"

#include <strsafe.h>

#ifdef USE_REGISTRY_FOR_CONFIG
// TODO:  ...
#else
static void ReadOptions(const WCHAR* ini_filename)
{
    WCHAR value[256];

    ReadConfigString(ini_filename, L"Options", L"Scrollbar", value, _countof(value), L"1");
    SetViewerScrollbar(!wcsicmp(value, L"true") || !wcsicmp(value, L"1") || !wcsicmp(value, L"on") || !wcsicmp(value, L"yes"));
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
void ReadConfigString(HKEY hkeyApp, const WCHAR* name, WCHAR* out, uint32 max_len, const WCHAR* default_value)
{
    DWORD type;
    DWORD len = max_len;
    if (RegGetValueW(hkeyApp, nullptr, name, RRF_RT_REG_SZ, &type, out, &len) != ERROR_SUCCESS ||
        type != REG_SZ || !len || len >= max_len)
    {
        StringCchCopy(out, max_len, default_value);
    }
}
#else
void ReadConfigString(const WCHAR* ini_filename, const WCHAR* section, const WCHAR* name, WCHAR* out, uint32 max_len, const WCHAR* default_value)
{
    if (ini_filename && *ini_filename)
        GetPrivateProfileStringW(section, name, default_value, out, max_len, ini_filename);
    else
        StringCchCopy(out, max_len, default_value);
}
#endif
