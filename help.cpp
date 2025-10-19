// Copyright (c) 2025 by Christopher Antos
// License: http://opensource.org/licenses/MIT

// vim: set et ts=4 sw=4 cino={0s:

#include "pch.h"
#include "help.h"
#include "version.h"
#include "res.h"

struct HelpText
{
    UINT            idr;
    const WCHAR*    title;
};

static const HelpText c_help_text[] =
{
    { IDR_HELP_CHOOSER,     L"Help for File Chooser" },
    { IDR_HELP_VIEWER,      L"Help for File Viewer" },
};

ViewerOutcome ViewHelp(Help help, Error& e)
{
    const auto& help_text = c_help_text[size_t(help)];

    const HINSTANCE hinst = GetModuleHandle(nullptr);
    const HRSRC hInfo = FindResource(hinst, MAKEINTRESOURCE(help_text.idr), L"HELPTEXT");
    if (!hInfo)
    {
LSysError:
        e.Sys();
        return ViewerOutcome::CONTINUE;
    }

    const DWORD dwSize = SizeofResource(hinst, hInfo);
    const HGLOBAL hData = LoadResource(hinst, hInfo);
    if (!hData)
        goto LSysError;

    const void* const pv = LockResource(hData);
    if (!pv)
        goto LSysError;

    StrA text;
    text.Printf("\n\t\t\t==== LIST REDUX v%s ====\n\n\t%s\n\n", VERSION_STR, STR_COPYRIGHTASCII);
    text.Append("\t\tIn memory of Vernon D. Buerg, 1948-2009,\n\t\t  author of the original LIST for DOS.");
    text.Append("\n\n");
    text.Append(reinterpret_cast<const char*>(pv), dwSize);
    return ViewText(text.Text(), e, help_text.title);
}

