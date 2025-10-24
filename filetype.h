// Copyright (c) 2025 by Christopher Antos
// License: http://opensource.org/licenses/MIT

// vim: set et ts=4 sw=4 cino={0s:

#pragma once

#include <memory>

class StrW;

enum class FileDataType { Binary, Text };
FileDataType AnalyzeFileType(const BYTE* bytes, size_t count, UINT* codepage=nullptr, StrW* encoding_name=nullptr);

// Decodes input into UTF32 codepoints.
struct IDecoder
{
    // Returns true if the decoder is valid (was initialized successfully).
    virtual bool Valid() const = 0;
    // Returns a UTF32 codepoint -- *NOT* a UTF16 codepoint!
    virtual uint32 Decode(const BYTE* p, uint32 available, uint32& num_bytes) = 0;
    // Returns the native character size (1 for char, 2 for WCHAR).
    virtual uint32 CharSize() const { return 1; }
    // Returns the next native character from the input pointer (caller is
    // responsible for preventing overread by not calling).
    virtual uint32 NextChar(const BYTE* p) const { return *p; }
};
std::unique_ptr<IDecoder> CreateDecoder(UINT codepage);

bool TryCoInitialize();
bool IsCoInitialized();
bool GetCodePageName(UINT cp, StrW& encoding_name);
UINT GetSingleByteOEMCP(StrW* encoding_name=nullptr);
UINT EnsureSingleByteCP(UINT cp);

void SetMultiByteEnabled(bool enabled);

