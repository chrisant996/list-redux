// Copyright (c) 2024 by Christopher Antos
// License: http://opensource.org/licenses/MIT

// vim: set et ts=4 sw=4 cino={0s:

#pragma once

#include <windows.h>
#include "scroll_car.h"

class Error;
class FileInfo;

int HasBackgroundColor(const WCHAR* p);
void ReportColorlessError(Error& e);

void InitColors();
const WCHAR* ApplyGradient(const WCHAR* color, ULONGLONG value, ULONGLONG min, ULONGLONG max);
const WCHAR* StripLineStyles(const WCHAR* color);

enum class ColorElement
{
    Error,
    File,
    Selected,
    Tagged,
    SelectedTagged,
    Header,
    Command,
    Divider,
    LineNumber,
    Content,
    CtrlCode,
    EndOfFileLine,
    MarkedLine,
    SearchFound,
    DebugRow,
    SweepDivider,
    SweepFile,
    FloatingScrollBar,
    PopupBorder,
#ifndef USE_HALF_CHARS
    PopupScrollCar,
#endif
    PopupHeader,
    PopupFooter,
    PopupContent,
    PopupContentDim,
    PopupSelect,
    MAX
};

const WCHAR* GetTextColorParams(ColorElement element);
const WCHAR* GetColor(ColorElement element);
StrW MakeColor(ColorElement element);

extern const WCHAR c_norm[];

