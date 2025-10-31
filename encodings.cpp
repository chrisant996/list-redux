// Copyright (c) 2025 by Christopher Antos
// License: http://opensource.org/licenses/MIT

// vim: set et ts=4 sw=4 cino={0s:

#include "pch.h"
#include "encodings.h"

#include <unordered_set>
#include <MLang.h>

static bool s_multibyte_enabled = true;
static HRESULT s_hr_coinit = E_UNEXPECTED;
static IMultiLanguage* s_mlang1 = nullptr;
static IMultiLanguage2* s_mlang = nullptr;

static const BYTE c_tag_Intel[] = { 0xff, 0xfe };       // Little endian.
static const BYTE c_tag_Motorola[] = { 0xfe, 0xff };    // Big endian.
static const BYTE c_tag_UTF8[] = { 0xef, 0xbb, 0xbf };
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

#pragma region // Utf8Accumulator

class Utf8Accumulator
{
public:
                    Utf8Accumulator() { Reset(); }
    void            Reset() { ZeroMemory(this, sizeof(*this)); }
    int32           Build(char c);      // 0=building, 1=completed, -1=invalid.
    bool            Ready() const { return m_length == m_expected; }
    void            ClearInvalid();
    uint32          Codepoint() const { assert(Ready()); return m_ax; }
    const char*     Bytes() const { assert(Ready()); return m_buffer; }
    uint32          Length() const { assert(Ready()); return m_length; }
private:
    uint32          m_ax;
    uint8           m_expected;         // Number of bytes expected.
    uint8           m_length;           // Number of bytes accumulated.
    char            m_buffer[4 + 1];    // Bytes accumulated.
    int8            m_invalid;          // An invalid data state occurred.
};

int32 Utf8Accumulator::Build(const char _c)
{
    // Returns:
    //  1   = A UTF8 codepoint has been completed; use Codepoint() and etc to
    //        get information about it.
    //  0   = A UTF8 codepoint is in progress but is not completed.
    //  -1  = Invalid UTF8 data has been detected in preceding data.  Use
    //        Length() to find out how many bytes were involved in the invalid
    //        data, use ClearInvalid() to clear the error state, and then call
    //        Build() again with the same byte to continue.
    //
    // Sometimes the current byte may be detected as invalid, but in that case
    // 0 is returned and the next call to Build() will return -1.  This is to
    // simplify the usage contract.

    // https://en.wikipedia.org/wiki/UTF-8
    //
    //  - Bytes that never appear in UTF-8: 0xC0, 0xC1, 0xF5–0xFF,
    //  - A "continuation byte" (0x80–0xBF) at the start of a character,
    //  - A non-continuation byte (or the string ending) before the end of a
    //    character.
    //  - An overlong encoding (0xE0 followed by less than 0xA0, or 0xF0
    //    followed by less than 0x90).
    //  - A 4-byte sequence that decodes to a value greater than U+10FFFF
    //    (0xF4 followed by 0x90 or greater).
    //
    // HOWEVER, overlong 0xC0 0x80 should be allowed for U+0000.

    if (m_invalid)
    {
        // -1 means preceding data was invalid.
        // 1 means deferred reporting; convert it into -1 as the data has now
        // become preceding data.
        if (m_invalid == 1)
            m_invalid = -1;
        // Keep reporting the error state until ClearInvalid() is called.
        return -1;
    }

    const uint8 c = uint8(_c);
    if (c <= 0x7F)
    {
        // A non-continuation byte (or the string ending) cannot appear before
        // the end of a character.
        if (!Ready())
        {
InvalidPrecedingData:
            m_invalid = -1;
            m_ax = 0xFFFD;
            return -1;
        }

        // An ASCII byte.
        m_expected = 1;
        m_length = 1;
        m_buffer[0] = c;
        m_ax = c;
        return 1;
    }
    else if (c >= 0xF5 || c == 0xC1)
    {
        // Bytes that never appear in UTF-8: 0xC1, 0xF5–0xFF.
        if (!Ready())
            goto InvalidPrecedingData;
InvalidCurrentData:
        m_expected = 1;
        m_length = 1;
        m_buffer[0] = c;
        m_ax = 0xFFFD;
        m_invalid = 1;
        return 0;
    }
    else if (c >= 0b11110000)
    {
        // A non-continuation byte (or the string ending) cannot appear before
        // the end of a character.
        if (!Ready())
            goto InvalidPrecedingData;

        // Start a four byte sequence.
        m_expected = 4;
        m_length = 1;
        m_buffer[0] = c;
        m_ax = c & 0b00000111;
        return 0;
    }
    else if (c >= 0b11100000)
    {
        // A non-continuation byte (or the string ending) cannot appear before
        // the end of a character.
        if (!Ready())
            goto InvalidPrecedingData;

        // Start a three byte sequence.
        m_expected = 3;
        m_length = 1;
        m_buffer[0] = c;
        m_ax = c & 0b00001111;
        return 0;
    }
    else if (c >= 0b11000000)
    {
        // A non-continuation byte (or the string ending) cannot appear before
        // the end of a character.
        if (!Ready())
            goto InvalidPrecedingData;

        // Start a two byte sequence.
        m_expected = 2;
        m_length = 1;
        m_buffer[0] = c;
        m_ax = c & 0b00011111;
        return 0;
    }
    else
    {
        // Continuation byte.
        assert(c >= 0b10000000);

        // A "continuation byte" (0x80–0xBF) cannot appear at the start of a
        // character.
        if (Ready())
            goto InvalidCurrentData;

        // Detect a 4-byte sequence that decodes to a value greater than
        // U+10FFFF (0xF4 followed by 0x90 or greater).
        if (m_ax == 4 && c >= 0x90 && m_expected == 4 && m_length == 1)
            goto InvalidPrecedingData;

        // Detect overlong encodings.
        if (m_ax == 0)
        {
            switch (m_expected)
            {
            case 3:
                // 0xE0 followed by less than 0xA0.
                if (c < 0xA0 && m_length == 1)
                    goto InvalidPrecedingData;
                break;
            case 4:
                // 0xF0 followed by less than 0x90.
                if (c < 0x90 && m_length == 1)
                    goto InvalidPrecedingData;
                break;
            case 2:
                // 0xC0 followed by 0x80 is an overlong encoding for U+0000,
                // which is accepted so that U+0000 can be encoded without
                // using any NUL bytes.  But no other use of 0xC0 is allowed.
                if (m_length == 1 && c != 0x80)
                    goto InvalidPrecedingData;
                break;
            }
        }

        m_buffer[m_length++] = c;
        m_ax = (m_ax << 6) | (c & 0b01111111);
        return Ready();
    }
}

void Utf8Accumulator::ClearInvalid()
{
    assert(m_invalid);
    m_expected = 0;
    m_length = 0;
    m_ax = 0;
    m_invalid = 0;
}

#pragma endregion // Utf8Accumulator
#pragma region // MLang

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

static HRESULT EnsureMLang()
{
    static bool s_inited = false;
    static HRESULT s_hr_ensure = E_UNEXPECTED;
    if (!s_inited)
    {
        static HRESULT s_hr_cocreate1 = CoCreateInstance(CLSID_CMultiLanguage, NULL, CLSCTX_INPROC_SERVER, IID_IMultiLanguage, (void**)&s_mlang1);
        static HRESULT s_hr_cocreate2 = CoCreateInstance(CLSID_CMultiLanguage, NULL, CLSCTX_INPROC_SERVER, IID_IMultiLanguage2, (void**)&s_mlang);
        if (FAILED(s_hr_cocreate1) || FAILED(s_hr_cocreate2))
        {
            if (s_mlang1)
            {
                s_mlang1->Release();
                s_mlang1 = nullptr;
            }
            if (s_mlang)
            {
                s_mlang->Release();
                s_mlang = nullptr;
            }
        }
        s_hr_ensure = FAILED(s_hr_cocreate2) ? s_hr_cocreate2 : s_hr_cocreate1;
        s_inited = true;
    }
    return s_hr_ensure;
}

bool GetCodePageName(UINT cp, StrW& encoding_name)
{
// TODO:  Use predefined custom names, e.g. MIME names?  Take codepages and
// names from the ICU support in Windows?  But list-redux can't directly use
// the ICU support because it's only available in certain builds of Windows 10
// and newer.

    // First try MLang.
    MIMECPINFO codepageinfo;
    EnsureMLang();
    if (s_mlang1 && SUCCEEDED(s_mlang1->GetCodePageInfo(cp, &codepageinfo)))
    {
        encoding_name.Set(codepageinfo.wszDescription);
        return true;
    }

    // Then try the system.
    CPINFOEXW info;
    if (GetCPInfoExW(cp, 0/*reserved*/, &info))
    {
        const WCHAR* p = StrChr(info.CodePageName, '(');
        const WCHAR* p2 = p ? StrChr(p, ')') : nullptr;
        if (p && p2)
            encoding_name.Set(p + 1, p2 - (p + 1));
        else
            encoding_name.Set(info.CodePageName);
        return true;
    }

    // Special case for 437 if neither MLang nor the system could identify it.
    if (cp == 437)
    {
        encoding_name.Set(L"OEM-US");
        return true;
    }

    // Synthesize a name.
    encoding_name.Clear();
    encoding_name.Printf(L"CP %u", cp);
    return true;
}

UINT GetSingleByteOEMCP(StrW* encoding_name)
{
    UINT cp = GetOEMCP();
    switch (cp)
    {
    case 932:
    case 936:
    case 949:
    case 950:
        // These are multibyte OEM codepages.  Fall back to a single-byte
        // codepage, i.e. 437 which is the US OEM codepage.
        cp = 437;
        break;
    }

    if (encoding_name)
        GetCodePageName(cp, *encoding_name);

    return cp;
}

UINT EnsureSingleByteCP(UINT cp)
{
    switch (cp)
    {
    case 437:   // OEM - United States                  or OEM-US
    case 708:   // Arabic (ASMO 708)
    case 720:   // Arabic (DOS)
    case 737:   // OEM - Greek 437G                     or MS-DOS Greek
    case 775:   // OEM - Baltic                         or MS-DOS Baltic Rim
    case 850:   // OEM - Multilingual Latin I           or MS-DOS Latin 1
    case 852:   // Central European (DOS)               or MS-DOS Latin 2
    case 855:   // OEM - Cyrillic                       or MS-DOS Cyrillic
    case 857:   // OEM - Turkish                        or MS-DOS Turkish
    case 858:   // OEM - Multilingual Latin I + Euro    or Western European with Euro sign
    case 860:   // OEM - Portuguese                     or MS-DOS Portuguese
    case 861:   // OEM - Icelandic                      or MS-DOS Icelandic
    case 862:   // Hebrew (DOS)
    case 863:   // OEM - Canadian French                or MS-DOS French Canada
    case 864:   // OEM - Arabic
    case 865:   // OEM - Nordic                         or MS-DOS Nordic
    case 866:   // Cyrillic (DOS)                       or MS-DOS Cyrillic Russian
    case 869:   // OEM - Modern Greek                   or MS-DOS Greek 2
    case 874:   // Thai (Windows)
        return cp;
    default:    // Fall back to OEM-US.
        return 437;
    }
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

    UINT cp = 0;
    HRESULT hr = EnsureMLang();
    if (SUCCEEDED(hr))
    {
        DetectEncodingInfo info[1] = {};
        INT scores = _countof(info);
        hr = s_mlang->DetectInputCodepage(0, 0, reinterpret_cast<CHAR*>(const_cast<BYTE*>(bytes)), &length, info, &scores);
        if (SUCCEEDED(hr))
        {
            cp = info[0].nCodePage;
            if (encoding_name)
            {
                MIMECPINFO codepageinfo;
                if (SUCCEEDED(s_mlang->GetCodePageInfo(cp, info[0].nLangID, &codepageinfo)))
                    encoding_name->Set(codepageinfo.wszDescription);
            }
        }
    }

    if (!s_multibyte_enabled && cp != 20127 && cp != 437)
    {
        if (encoding_name)
            encoding_name->Clear();
        return false;
    }

    if (codepage)
        *codepage = cp;

    return SUCCEEDED(hr);
}

FileDataType AnalyzeFileType(const BYTE* const bytes, const size_t count, UINT* codepage, StrW* encoding_name)
{
    if (!count)
    {
        if (encoding_name)
            encoding_name->Set(L"Empty File");
binary_encoding:
        if (codepage)
            *codepage = GetSingleByteOEMCP();
        return FileDataType::Binary;
    }

    // Special case certain file type tags for binary files that could
    // otherwise appear as text.
    if (count >= sizeof(c_tag_PDF) && !memcmp(bytes, c_tag_PDF, sizeof(c_tag_PDF)))
    {
        if (encoding_name)
            encoding_name->Set(L"PDF File");
        goto binary_encoding;
    }

    // Check for Unicode files.
    if (count >= sizeof(c_tag_Intel))
    {
        if (!memcmp(bytes, c_tag_Intel, sizeof(c_tag_Intel)))
        {
            if (codepage)
                *codepage = CP_WINUNICODE;
            if (encoding_name)
                GetCodePageName(CP_WINUNICODE, *encoding_name);
            return FileDataType::Text;
        }
        if (!memcmp(bytes, c_tag_Motorola, sizeof(c_tag_Motorola)))
        {
            if (codepage)
                *codepage = 1201;
            if (encoding_name)
                GetCodePageName(1201, *encoding_name);
            return FileDataType::Text;
        }
    }

    // Check for UTF8 files.
    if (s_multibyte_enabled && count >= sizeof(c_tag_UTF8))
    {
        if (!memcmp(bytes, c_tag_UTF8, sizeof(c_tag_UTF8)))
        {
            if (codepage)
                *codepage = CP_UTF8;
            if (encoding_name)
                encoding_name->Set(L"Unicode (UTF-8)");
            return FileDataType::Text;
        }
    }

    // Check for binary files by scanning the first 4096 bytes for control
    // characters other than BEL, TAB, CR, LF, VT, FF, or ^Z.
    const BYTE* p = bytes;
    for (size_t ii = count; ii--; ++p)
    {
        if (IsBinary(*p))
        {
            if (encoding_name)
                encoding_name->Clear();
            StrW tmp;
            if (codepage)
            {
                *codepage = GetSingleByteOEMCP(&tmp);
                if (encoding_name->Empty())
                {
                    encoding_name->Set(L"Binary File");
                    if (!tmp.Empty())
                        encoding_name->Printf(L" (%s)", tmp.Text());
                }
            }
            return FileDataType::Binary;
        }
    }

    if (codepage)
    {
        *codepage = 0;
        encoding_name->Clear();
        assert(count < 1024 * 1024);
        if (!DetectCodePage(bytes, uint32(count), codepage, encoding_name))
        {
            *codepage = 437;
            encoding_name->Set(L"OEM-US");
        }
    }

    return FileDataType::Text;
}

#pragma endregion // MLang
#pragma region // Decoders

class SingleByteDecoder : public IDecoder
{
public:
                    SingleByteDecoder() = default;
                    ~SingleByteDecoder() = default;
    bool            Valid() const override;
    uint32          Decode(const BYTE* p, uint32 available, uint32& num_bytes) override;
};

bool SingleByteDecoder::Valid() const
{
    return true;
}

uint32 SingleByteDecoder::Decode(const BYTE* p, uint32 available, uint32& num_bytes)
{
    assert(available > 0);
    num_bytes = 1;
    return *p;
}

class Utf8Decoder : public IDecoder
{
public:
                    Utf8Decoder() = default;
                    ~Utf8Decoder() = default;
    bool            Valid() const override;
    uint32          Decode(const BYTE* p, uint32 available, uint32& num_bytes) override;
};

bool Utf8Decoder::Valid() const
{
    return true;
}

uint32 Utf8Decoder::Decode(const BYTE* p, uint32 available, uint32& num_bytes)
{
#ifdef DEBUG
    const BYTE* orig = p;
#endif
    Utf8Accumulator acc;
    while (available)
    {
        const int32 b = acc.Build(*p);
        if (b < 0)
            break;
        ++p;
        --available;
        if (b > 0)
            break;
    }
    num_bytes = acc.Length();
#ifdef DEBUG
    assert(size_t(num_bytes) == p - orig);
#endif
    return acc.Codepoint();
}

class MultiByteDecoder : public IDecoder
{
public:
                    MultiByteDecoder(UINT codepage);
                    ~MultiByteDecoder();
    bool            Valid() const override;
    uint32          Decode(const BYTE* p, uint32 available, uint32& num_bytes) override;
private:
    uint32          DecodeOneCodepoint(const CHAR* src, UINT src_size, WCHAR* dst, UINT dst_size);
private:
    const UINT      m_codepage;
    CPINFOEXW       m_info;
    IMLangConvertCharset* m_converter = nullptr;
};

MultiByteDecoder::MultiByteDecoder(UINT codepage)
: m_codepage(codepage)
{
    assert(codepage != CP_UTF7); // UTF7 has special rules for resync after invalid input.
    assert(codepage != CP_UTF8); // UTF8 has special rules for resync after invalid input.
    EnsureMLang();
    if (!s_mlang ||
        FAILED(s_mlang->CreateConvertCharset(codepage, CP_WINUNICODE, 0/*MLCONVCHARF_NONE*/, &m_converter)) ||
        !m_converter ||
        !GetCPInfoExW(codepage, 0/*reserved*/, &m_info))
    {
        if (m_converter)
        {
            m_converter->Release();
            m_converter = nullptr;
        }
    }
}

MultiByteDecoder::~MultiByteDecoder()
{
    if (m_converter)
    {
        m_converter->Release();
        m_converter = nullptr;
    }
}

bool MultiByteDecoder::Valid() const
{
    return !!m_converter;
}

uint32 MultiByteDecoder::DecodeOneCodepoint(const CHAR* src, UINT src_size, WCHAR* dst, UINT dst_size)
{
#ifdef USE_MLANG_FOR_DECODE
    switch (m_converter->DoConversionToUnicode(src, &src_size, dst, &dst_size))
    {
    case S_OK:
        return dst_size;
    case S_FALSE:
        assert(false);
        // Is S_FALSE reserved for certain cases that can maybe use special
        // handling logic?
        __fallthrough;
    default:
        return 0;
    }
#else
    const int num = MultiByteToWideChar(m_codepage, MB_ERR_INVALID_CHARS, src, src_size, dst, dst_size);
    return num > 0 ? num : 0;
#endif
}

uint32 MultiByteDecoder::Decode(const BYTE* p, uint32 available, uint32& num_bytes)
{
    assert(available > 0);
    assert(Valid());

    // If the input is a lead byte, then decode the input.
    for (const BYTE* range = m_info.LeadByte; range[0] || range[1]; range += 2)
    {
        if (range[0] <= *p && *p <= range[1])
        {
            CHAR* src = const_cast<CHAR*>(reinterpret_cast<const CHAR*>(p));
            WCHAR dst[8];

            if (available > m_info.MaxCharSize)
                available = m_info.MaxCharSize;

            for (uint32 num = 1; num < available; ++num)
            {
                const uint32 dst_size = DecodeOneCodepoint(src, num, dst, _countof(dst));
                if (dst_size)
                {
                    assert(dst_size == 1 || dst_size == 2);
                    uint32 c = dst[0];
                    if (dst_size == 2)
                    {
                        c <<= 10;
                        c += dst[1];
                        c -= 0x35fdc00;
                    }
                    num_bytes = num;
                    return c;
                }
            }

            break;
        }
    }

    // Upon failure or if the input wasn't a lead byte, then return the
    // literal input byte.
    num_bytes = 1;
    return *p;
}

class Utf16Decoder : public IDecoder
{
public:
                    Utf16Decoder(UINT codepage);
                    ~Utf16Decoder() = default;
    bool            Valid() const override;
    uint32          Decode(const BYTE* p, uint32 available, uint32& num_bytes) override;
    uint32          CharSize() const override;
    uint32          NextChar(const BYTE* p) const override;
private:
    WCHAR           Next(const BYTE* p) const;
private:
    const bool      m_byte_swap;
};

Utf16Decoder::Utf16Decoder(UINT codepage)
: m_byte_swap(codepage == 1201)
{
    assert(codepage == 1200 || codepage == 1201);
}

bool Utf16Decoder::Valid() const
{
    return true;
}

uint32 Utf16Decoder::Decode(const BYTE* p, uint32 available, uint32& num_bytes)
{
    assert(available > 0);
    assert(Valid());

    if (available < 2)
    {
invalid_truncated:
        num_bytes = available;
        return 0xFFFD;
    }

    const WCHAR wch = Next(p);
    if (wch < 0xD800 || wch > 0xDFFF)
    {
        num_bytes = 2;
        return wch;
    }
    if (wch >= 0xDC00 && wch <= 0xDFFF)
    {
invalid_one_wchar:
        num_bytes = 2;
        return 0xFFFD;
    }

    assert(wch >= 0xD800 && wch <= 0xDBFF);

    if (available < 4)
        goto invalid_truncated;

    p += 2;
    const WCHAR wch2 = Next(p);
    if (wch < 0xDC00 || wch > 0xDFFF)
        goto invalid_one_wchar;

    uint32 c = wch;
    c <<= 10;
    c += wch2;
    c -= 0x35fdc00;
    num_bytes = 4;
    return c;
}

uint32 Utf16Decoder::CharSize() const
{
    return 2;
}

uint32 Utf16Decoder::NextChar(const BYTE* p) const
{
    return Next(p);
}

WCHAR Utf16Decoder::Next(const BYTE* p) const
{
    if (m_byte_swap)
        return WCHAR(p[1]) | (WCHAR(p[0]) << 8);
    else
        return WCHAR(p[0]) | (WCHAR(p[1]) << 8);
}

std::unique_ptr<IDecoder> CreateDecoder(UINT codepage)
{
    if (s_multibyte_enabled)
    {
        switch (codepage)
        {
        case CP_WINUNICODE:     // Unicode (UTF-16 Little Endian)
        case 1201:              // Unicode (UTF-16 Big Endian)
            return std::make_unique<Utf16Decoder>(codepage);
        case CP_UTF7:
        case CP_UTF8:
            return std::make_unique<Utf8Decoder>();
        }

        const UINT sbcp = EnsureSingleByteCP(codepage);
        if (sbcp == codepage)
            return std::make_unique<SingleByteDecoder>();
    }

    std::unique_ptr<IDecoder> decoder;
    if (s_multibyte_enabled && SUCCEEDED(EnsureMLang()))
        decoder = std::make_unique<MultiByteDecoder>(codepage);
    if (!decoder || !decoder->Valid())
        decoder = std::make_unique<SingleByteDecoder>();
    return decoder;
}

void SetMultiByteEnabled(bool enabled)
{
    s_multibyte_enabled = enabled;
}

#pragma endregion // Decoders
#pragma region // Available Encodings

static std::unordered_set<UINT>* s_enum_codepages = nullptr;

static BOOL CALLBACK CodePageEnumProcW(LPWSTR lpCodePageString)
{
    const UINT codepage = _wtoi(lpCodePageString);
    s_enum_codepages->emplace(codepage);
    return true;
}

bool IsCodePageAllowed(UINT cp)
{
    // TODO:  Change this to an inclusion list of supported codepages (e.g.
    // the MIME codepages from ICU?).
    if (cp == CP_UTF7)
    {
        // Disallowed because it's obsolete, it was never officially supported
        // by the Unicode Consortium, it has security issues, and it has
        // complexity issues because of its dependence on Base64.
        return false;
    }
    return true;
}

std::vector<EncodingDefinition> GetAvailableEncodings()
{
    std::unordered_set<UINT> installed_codepages;
    std::unordered_set<UINT> codepages;
    std::vector<EncodingDefinition> encodings;

    // These codepages are always installed.
    installed_codepages.emplace(CP_UTF8);
    installed_codepages.emplace(CP_WINUNICODE);
    installed_codepages.emplace(1201);
    // BUGBUG:  What if codepage 437 (our fallback) isn't installed?

    // First get installed codepages, to be able to filter MLang's codepages.
    assert(!s_enum_codepages);
    s_enum_codepages = &installed_codepages;
    EnumSystemCodePagesW(CodePageEnumProcW, CP_INSTALLED);
    s_enum_codepages = nullptr;

    // Get the intersection of installed codepages and codepages from MLang.
    EnsureMLang();
    if (s_mlang)
    {
        IEnumCodePage* pecp = nullptr;
        if (SUCCEEDED(s_mlang1->EnumCodePages(MIMECONTF_VALID, &pecp)) && pecp)
        {
            StrA s;
            MIMECPINFO rg[8];
            ULONG fetched = 0;
            while (pecp->Next(_countof(rg), rg, &fetched) == S_OK)
            {
                for (ULONG i = 0; i < fetched; ++i)
                {
                    const UINT cp = rg[i].uiCodePage;
                    if (IsCodePageAllowed(cp) &&
                        installed_codepages.find(cp) != installed_codepages.end() &&
                        codepages.find(cp) == codepages.end())
                    {
                        EncodingDefinition encoding;
                        encoding.codepage = cp;
                        encoding.encoding_name = rg[i].wszDescription;
                        codepages.emplace(cp);
                        encodings.emplace_back(std::move(encoding));
                    }
                }
            }
            pecp->Release();
            pecp = nullptr;
        }
    }

    return encodings;
}

#pragma endregion // Available Encodings
