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

void ReadColors(const WCHAR* ini_filename);

const WCHAR* ApplyGradient(const WCHAR* color, ULONGLONG value, ULONGLONG min, ULONGLONG max);
const WCHAR* StripLineStyles(const WCHAR* color);
const WCHAR* BlendColors(const WCHAR* a, const WCHAR* b, BYTE alpha, bool back=false, bool opposite_a=false, bool opposite_b=false);

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
    Whitespace,
    CtrlCode,
    FilteredByte,
    EndOfFileLine,
    MarkedLine,
    SearchFound,
    DebugRow,
    SweepDivider,
    SweepFile,
    FloatingScrollBar,
    PopupBorder,
    PopupScrollCar,
    PopupHeader,
    PopupFooter,
    PopupContent,
    PopupContentDim,
    PopupSelect,
    EditedByte,
    SavedByte,
    KeyName,
    MAX
};

enum class ColorConversion { TextOnly, TextAsBack, BackAsText, SwapTextAndBack, StylesOnly };

const WCHAR* ConvertColorParams(ColorElement element, ColorConversion convert=ColorConversion::TextOnly);
const WCHAR* GetColor(ColorElement element);
StrW MakeColor(ColorElement element);

extern const WCHAR c_norm[];

