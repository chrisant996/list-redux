// Copyright (c) 2025 by Christopher Antos
// License: http://opensource.org/licenses/MIT

// vim: set et ts=4 sw=4 cino={0s:

#include "pch.h"
#include "filetype.h"

#include <MLang.h>

static HRESULT s_hr_coinit = E_UNEXPECTED;
static IMultiLanguage2* s_mlang = nullptr;

static const BYTE c_tag_Intel[] = { 0xff, 0xfe };		// Little endian.
static const BYTE c_tag_Motorola[] = { 0xfe, 0xff };	// Big endian.
static const BYTE c_tag_PDF[] = { '%', 'P', 'D', 'F', '-' };

static_assert(sizeof(c_tag_Intel) == 2);
static_assert(sizeof(c_tag_Motorola) == 2);
static_assert(sizeof(c_tag_PDF) == 5);

// Look up Ctrl code by bit to find whether it means file is binary.
//                                   33222222222211111111110000000000
//                                   10987654321098765432109876543210
static const DWORD c_ctrl_binary = 0b00000011111111111100000101111111;
// Bit 0 is ambiguous; it could be a UTF16 file.
// BEL/TAB/LF/VT/FF/CR/EOF ctrl codes are textual.

inline bool IsBinary(BYTE c)
{
    return (c <= 26 && (c_ctrl_binary & (1 << c)));
}

bool TryCoInitialize()
{
    static bool s_inited = false;
    if (!s_inited)
        s_hr_coinit = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    return SUCCEEDED(s_hr_coinit);
}
bool IsCoInitialized()
{
    return SUCCEEDED(s_hr_coinit);
}

static bool DetectCodePage(const BYTE* bytes, int32 length, UINT* codepage, StrW* encoding_name)
{
    if (!IsCoInitialized())
        return false;

    // Shrink the length until the last character does not have the high bit
    // set.  This is meant to avoid ending on a severed multi-byte character,
    // which could skew the encoding detection.
    while (length > 0 && (bytes[length-1] & 0x80))
        --length;
    if (length <= 0)
        return false;

    static HRESULT s_hr_cocreate;
    if (!s_mlang)
    {
        // Cache an instance and reuse it.
        s_hr_cocreate = CoCreateInstance(CLSID_CMultiLanguage, NULL, CLSCTX_INPROC_SERVER, IID_IMultiLanguage2, (void**)&s_mlang);
    }

    HRESULT hr = s_hr_cocreate;
    if (SUCCEEDED(hr))
    {
        DetectEncodingInfo info[1] = {};
        INT scores = _countof(info);
        hr = s_mlang->DetectInputCodepage(0, 0, reinterpret_cast<CHAR*>(const_cast<BYTE*>(bytes)), &length, info, &scores);
        if (SUCCEEDED(hr))
        {
            if (codepage)
                *codepage = info[0].nCodePage;
            if (encoding_name)
            {
                MIMECPINFO codepageinfo;
                if (SUCCEEDED(s_mlang->GetCodePageInfo(info[0].nCodePage, info[0].nLangID, &codepageinfo)))
                    encoding_name->Set(codepageinfo.wszDescription);
            }
        }
    }

    return SUCCEEDED(hr);
}

FileDataType AnalyzeFileType(const BYTE* const bytes, const size_t count, UINT* codepage, StrW* encoding_name)
{
    if (!count)
        return FileDataType::Binary;

    // Special case certain file type tags for binary files that could
    // otherwise appear as text.
    if (count >= sizeof(c_tag_PDF) && !memcmp(bytes, c_tag_PDF, sizeof(c_tag_PDF)))
        return FileDataType::Binary;

    // Check for Unicode files.
    if (count >= sizeof(c_tag_Intel))
    {
        if (!memcmp(bytes, c_tag_Intel, sizeof(c_tag_Intel)))
            return FileDataType::Binary;
        if (!memcmp(bytes, c_tag_Motorola, sizeof(c_tag_Motorola)))
            return FileDataType::Binary;
    }

    // Check for binary files by scanning the first 4096 bytes for control
    // characters other than BEL, TAB, CR, LF, VT, FF, or ^Z.
    const BYTE* p = bytes;
    for (size_t ii = count; ii--; ++p)
    {
        if (IsBinary(*p))
            return FileDataType::Binary;
    }

    if (codepage)
    {
        *codepage = 0;
        encoding_name->Clear();
        assert(count < 1024 * 1024);
        if (!DetectCodePage(bytes, uint32(count), codepage, encoding_name) ||
            !(*codepage == 20127 || *codepage == 437 /*|| *codepage == CP_UTF8*/))
        {
            *codepage = 437;
            encoding_name->Set(L"OEM-US");
        }
    }

    return FileDataType::Text;
}

