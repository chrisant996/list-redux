// Copyright (c) 2024 by Christopher Antos
// License: http://opensource.org/licenses/MIT

// vim: set et ts=4 sw=4 cino={0s:

#pragma once

#include <windows.h>

class Error;
class FileInfo;

int HasBackgroundColor(const WCHAR* p);
void ReportColorlessError(Error& e);

void InitColors();
const WCHAR* ApplyGradient(const WCHAR* color, ULONGLONG value, ULONGLONG min, ULONGLONG max);
const WCHAR* StripLineStyles(const WCHAR* color);

enum class ColorElement
{
    File,
    Selected,
    Tagged,
    SelectedTagged,
    Command,
    Divider,
    Content,
    CtrlCode,
    EndOfFileLine,
    MarkedLine,
    SearchFound,
    DebugRow,
    MAX
};

const WCHAR* GetBackColorParams(ColorElement element);
const WCHAR* GetTextColorParams(ColorElement element);
const WCHAR* GetColor(ColorElement element);

extern const WCHAR c_norm[];
extern const WCHAR c_error[];

