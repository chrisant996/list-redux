// Copyright (c) 2025 by Christopher Antos
// License: http://opensource.org/licenses/MIT

// vim: set et ts=4 sw=4 cino={0s:

#pragma once

#include <windows.h>
#include "str.h"
#include "fileinfo.h"

#include <vector>

enum class ViewerOutcome { CONTINUE, RETURN, EXITAPP };

ViewerOutcome ViewFiles(const std::vector<StrW>& files, StrW& dir, Error& e);
ViewerOutcome ViewText(const char* text, Error& e, const WCHAR* title=nullptr, bool help=false);

void SetMaxLineLength(const WCHAR* arg);
void SetPipedInput();
void SetViewerScrollbar(bool scrollbar);
void SetViewerGotoLine(size_t line);
void SetViewerGotoOffset(uint64 offset);

