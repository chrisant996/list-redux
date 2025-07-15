// Copyright (c) 2025 by Christopher Antos
// License: http://opensource.org/licenses/MIT

// vim: set et ts=4 sw=4 cino={0s:

#pragma once

class StrW;

enum class FileDataType { Binary, Text };
FileDataType AnalyzeFileType(const BYTE* bytes, size_t count, UINT* codepage=nullptr, StrW* encoding_name=nullptr);

bool TryCoInitialize();
bool IsCoInitialized();

