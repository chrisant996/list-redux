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
bool WriteColors(const WCHAR* ini_filename);

const WCHAR* ApplyGradient(const WCHAR* color, ULONGLONG value, ULONGLONG min, ULONGLONG max);
const WCHAR* StripLineStyles(const WCHAR* color);
const WCHAR* BlendColors(const WCHAR* a, const WCHAR* b, BYTE alpha, bool back=false, bool opposite_a=false, bool opposite_b=false);

enum class ColorElement
{
    // General
    Error,
    Header,
    Footer,
#ifdef INCLUDE_MENU_ROW
    MenuRow,
#endif
    KeyName,

    // Scrollbar
    FloatingScrollBar,
    ScrollBar,
    ScrollBarCar,

    // Input
    Input,
    InputSelection,
    InputHorizScroll,

    // Popup
    PopupBorder,
    PopupScrollCar,
    PopupHeader,
    PopupFooter,
    PopupContent,
    PopupContentDim,
    PopupSelect,

    // Miscellaneous
    SweepDivider,
    SweepFile,
    DebugRow,

    // Chooser
    File,
    Selected,
    Tagged,
    SelectedTagged,
    Divider,

    // Viewer
    LineNumber,
    Content,
    Whitespace,
    CtrlCode,
    FilteredByte,
    EndOfFileLine,
    MarkedLine,
    BookmarkedLine,
    SearchFound,
    EditedByte,
    SavedByte,

    MAX
};

enum class ColorConversion { TextOnly, TextAsBack, BackAsText, SwapTextAndBack, StylesOnly };

const WCHAR* ConvertColorParams(ColorElement element, ColorConversion convert=ColorConversion::TextOnly);
const WCHAR* GetColor(ColorElement element);
StrW MakeColor(ColorElement element);

extern const WCHAR c_norm[];
extern const WCHAR c_clreol[];

