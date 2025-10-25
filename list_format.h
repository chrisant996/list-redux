// Copyright (c) 2025 by Christopher Antos
// License: http://opensource.org/licenses/MIT

// vim: set et ts=4 sw=4 cino={0s:

#pragma once

#include "fileinfo.h"
#include "str.h"

#include <vector>

unsigned WidthForFileInfo(const FileInfo* pfi, int details, int size_width=0);
unsigned WidthForFileInfoSize(const FileInfo* pfi, int details, int size_width=0);
unsigned WidthForDirectorySize(int details);
unsigned FormatFileInfo(StrW& s, const FileInfo* pfi, unsigned max_width, int details, bool selected, bool tagged, int size_width=0);
#if 0
void FormatFileInfoForPopupList(StrW& s, const FileInfo* pfi, unsigned max_width, int size_width=0);
#endif
unsigned FormatFileData(StrW& s, const WIN32_FIND_DATAW& fd);

void InitLocale();

enum ColorScaleFields { SCALE_NONE = 0, SCALE_TIME = 1<<0, SCALE_SIZE = 1<<1, };
DEFINE_ENUM_FLAG_OPERATORS(ColorScaleFields);

bool SetColorScale(const WCHAR* s);
ColorScaleFields GetColorScaleFields();
bool SetColorScaleMode(const WCHAR* s);
bool IsGradientColorScaleMode();

void FormatSize(StrW& s, unsigned __int64 cbSize, unsigned max_width, WCHAR chStyle=0, const WCHAR* color=nullptr, const WCHAR* fallback_color=nullptr);
void FormatFilename(StrW& s, const FileInfo* pfi, unsigned max_width, const WCHAR* color=nullptr);

