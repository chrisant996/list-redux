// Copyright (c) 2025 by Christopher Antos
// License: http://opensource.org/licenses/MIT

// vim: set et ts=4 sw=4 cino={0s:

#include "pch.h"
#include "filetypeconfig.h"
#include "fileinfo.h"

#if 0
enum ConfigType
{
    General,
    SourceFiles,
    MAX,
};
#endif

struct ConfigDetails
{
#if 0
    ConfigType      type;
#endif
    const char*     ext_list;
    BYTE            hanging_extra;
};

static const ConfigDetails c_config_map[] =
{
    { ".c.h.cpp.hpp.cxx.hxx.cc.",           8 },
    { ".cs.",                               8 },
    { ".cmd.bat.btm.",                      8 },
    { ".pl.pm.",                            8 },
    { ".ps1.psm1.",                         8 },
    { ".ts.tsx.js.jsx.",                    8 },
    { ".lua.",                              8 },
    { ".rs.",                               8 },
    { ".xml.htm.html.shtm.shtml.xaml.",     8 },
    { ".rc.",                               8 },
    { ".idl.odl.",                          8 },
    { ".asm.inc.",                          8 },
    { ".i.pp.",                             8 },
};
#if 0
static_assert(_countof(c_config_map) == size_t(ConfigType::MAX));
#endif

void ApplyFileTypeConfig(const WCHAR* p, ViewerOptions& options)
{
    const WCHAR* ext = FindExtension(p);
    if (!ext)
        return;

    StrA needle;
    needle.SetW(ext);
    needle.Append('.');
    for (const auto& c : c_config_map)
    {
        if (strstr(c.ext_list, needle.Text()))
        {
            options.hanging_extra = c.hanging_extra;
            return;
        }
    }

    options.hanging_extra = 0;
}

#if 0
#pragma region Compile Time Validation
// Compile-time trampoline to check sorted order (requires C++17).
template <size_t N>
constexpr bool is_sorted_trampoline()
{
    for (size_t i = 1; i < N; ++i)
    {
        if (unsigned(c_config_map[i].type) < unsigned(c_config_map[i - 1].type))
            return false;   // Found out-of-order element.
    }
    return true;
}
static_assert(is_sorted_trampoline<_countof(c_config_map)>(), "c_config_map is not in enum sequence order.");
#pragma endregion Compile Time Validation
#endif
