// Copyright (c) 2025 by Christopher Antos
// License: http://opensource.org/licenses/MIT

// vim: set et ts=4 sw=4 cino={0s:

#include "pch.h"
#include "filetype.h"

#include <MLang.h>

#define USE_CUSTOM_UTF8_DECODER

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
        if (encoding_name)
            encoding_name->Set(L"OEM-US");
        return 437;
    }

    if (encoding_name)
    {
        EnsureMLang();
        encoding_name->Clear();
        if (s_mlang1)
        {
            MIMECPINFO codepageinfo;
            if (SUCCEEDED(s_mlang1->GetCodePageInfo(cp, &codepageinfo)))
                encoding_name->Set(codepageinfo.wszDescription);
        }
    }
    return cp;
}

UINT EnsureSingleByteCP(UINT cp)
{
    switch (cp)
    {
    case 437:                   // OEM-US
    case 708:                   // Arabic (ASMO 708)
    case 720:                   // Arabic (box drawing characters in "usual locations")
    case 737:                   // MS-DOS Greek
    case 775:                   // MS-DOS Baltic Rim
    case 850:                   // MS-DOS Latin 1
    case 852:                   // MS-DOS Latin 2
    case 855:                   // MS-DOS Cyrillic
    case 857:                   // MS-DOS Turkish
    case 858:                   // Western European with Euro sign
    case 860:                   // MS-DOS Portuguese
    case 861:                   // MS-DOS Icelandic
    case 862:                   // MS-DOS Hebrew
    case 863:                   // MS-DOS French Canada
    case 864:                   // Arabic
    case 865:                   // MS-DOS Nordic
    case 866:                   // MS-DOS Cyrillic Russian
    case 869:                   // MS-DOS Greek 2
    case 874:                   // Thai
        return cp;
    default:
        return 437;             // Fall back to OEM-US.
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
            // FUTURE:  Handle UTF16-LE.  Also maybe try to get localized
            // codepage name for 1200?
            if (encoding_name)
                encoding_name->Set(L"Binary File (UTF-16)");
            goto binary_file;
        }
        if (!memcmp(bytes, c_tag_Motorola, sizeof(c_tag_Motorola)))
        {
            // FUTURE:  Handle UTF16-BE.  Also maybe try to get localized
            // codepage name for 1201?
            if (encoding_name)
                encoding_name->Set(L"Binary File (UTF-16 Big Endian)");
            goto binary_file;
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
binary_file:
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
    UINT            m_codepage = 0;
    uint32          m_max_decode_bytes = 0;
    IMLangConvertCharset* m_converter = nullptr;
};

MultiByteDecoder::MultiByteDecoder(UINT codepage)
: m_codepage(codepage)
{
#ifdef USE_CUSTOM_UTF8_DECODER
    assert(codepage != CP_UTF7); // UTF7 has special rules for resync after invalid input.
    assert(codepage != CP_UTF8); // UTF8 has special rules for resync after invalid input.
#endif
    EnsureMLang();
    if (s_mlang && SUCCEEDED(s_mlang->CreateConvertCharset(codepage, CP_WINUNICODE, 0/*MLCONVCHARF_NONE*/, &m_converter)))
    {
        switch (m_codepage)
        {
        case CP_WINUNICODE:
            m_max_decode_bytes = 4;
            break;
        case CP_UTF7:
        case CP_UTF8:
            m_max_decode_bytes = 8;
            break;
        case 1252:
            m_max_decode_bytes = 2;
            break;
        // PERF:  Other known maximums for codepages...
        default:
            m_max_decode_bytes = 16;
            break;
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

uint32 MultiByteDecoder::Decode(const BYTE* p, uint32 available, uint32& num_bytes)
{
    assert(available > 0);
    assert(Valid());

// PERF:  For common codepages, optimize decoding by looking up whether a byte
// is a lead byte, and avoid using interactive mlang calls when a byte is
// known to not be part of a multibyte encoding?

// TODO:  Be careful not to sever any UTF8 byte sequence...

    CHAR* src = const_cast<CHAR*>(reinterpret_cast<const CHAR*>(p));
    UINT src_size;
    WCHAR dst[8];
    UINT dst_size;

    if (available > m_max_decode_bytes)
        available = m_max_decode_bytes;

    for (uint32 num = 1; num < available; ++num)
    {
        src_size = num;
        dst_size = _countof(dst);
        switch (m_converter->DoConversionToUnicode(src, &src_size, dst, &dst_size))
        {
        case S_OK:
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
            break;
        case S_FALSE:
            assert(false);
            // Is S_FALSE reserved for certain cases that can maybe use
            // special handling logic?
            __fallthrough;
        default:
            break;
        }
    }

    num_bytes = 1;
    return *p;
}

std::unique_ptr<IDecoder> CreateDecoder(UINT codepage)
{
    if (s_multibyte_enabled)
    {
        switch (codepage)
        {
        case CP_WINUNICODE:
            // FUTURE:  UTF16 requires special handling; for now treat it like
            // binary data.
            return std::make_unique<SingleByteDecoder>();
        case CP_UTF7:
        case CP_UTF8:
#ifdef USE_CUSTOM_UTF8_DECODER
            return std::make_unique<Utf8Decoder>();
#else
            return std::make_unique<MultiByteDecoder>(codepage);
#endif
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
