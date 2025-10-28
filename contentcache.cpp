// Copyright (c) 2025 by Christopher Antos
// License: http://opensource.org/licenses/MIT

// vim: set et ts=4 sw=4 cino={0s:

#include "pch.h"
#include "colors.h"
#include "contentcache.h"
#include "vieweroptions.h"
#include "wcwidth.h"
#include "wcwidth_iter.h"
#include "filetype.h"
#include "signaled.h"

#include <algorithm>

#ifdef DEBUG
#define DEBUG_LINE_PARSING
#endif

#ifdef DEBUG_LINE_PARSING
#include "output.h" // for dbgprintf()
#endif

// static const WCHAR c_eol_marker[] = L"\x1b[0;33;48;2;80;0;80m\u22a6\x1b[m";
// static const WCHAR c_eol_marker[] = L"\x1b[36m\u22a6\x1b[m";
static const WCHAR c_eol_marker[] = L"\x1b[36m\u2022\x1b[m";

constexpr unsigned c_tab_width = 8;

static HANDLE s_piped_stdin = 0;
void SetPipedInput()
{
    s_piped_stdin = GetStdHandle(STD_INPUT_HANDLE);
    if (s_piped_stdin == INVALID_HANDLE_VALUE)
        s_piped_stdin = 0;

    SetStdHandle(STD_INPUT_HANDLE, CreateFile(L"CONIN$", GENERIC_READ|GENERIC_WRITE, FILE_SHARE_READ|FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, 0));
}

static const WCHAR* const c_oem437[] =
{
    L" ",       // NUL
    L"\u263a",  // ☺
    L"\u263b",  // ☻
    L"\u2666",  // ♥
    L"\u2665",  // ♦
    L"\u2664",  // ♣
    L"\u2663",  // ♠
    L"\u2022",  // •
    L"\u25db",  // ◘
    L"\u25cb",  // ○
    L"\u25d9",  // ◙
    L"\u2642",  // ♂
    L"\u2640",  // ♀
    L"\u266a",  // ♪
    L"\u266b",  // ♫
    L"\u263c",  // ☼
    L"\u25ba",  // ►
    L"\u25c4",  // ◄
    L"\u2195",  // ↕
    L"\u203c",  // ‼
    L"\u00b6",  // ¶
    L"\u00a7",  // §
    L"\u25ac",  // ▬
    L"\u21a8",  // ↨
    L"\u2191",  // ↑
    L"\u2193",  // ↓
    L"\u2192",  // →
    L"\u2190",  // ←
    L"\u221f",  // ∟
    L"\u2194",  // ↔
    L"\u25b2",  // ▲
    L"\u25bc",  // ▼
};
static_assert(_countof(c_oem437) == 32);

inline bool IsWhiteSpace(uint32 c)
{
    return (c == ' ' || c == '\t' || c == '\r' || c == '\n');
}

static DWORD GetSystemPageSize()
{
    SYSTEM_INFO sysinfo;
    GetSystemInfo(&sysinfo);
    return std::max<DWORD>(64*1024, std::max<DWORD>(sysinfo.dwPageSize, sysinfo.dwAllocationGranularity));
}

static const DWORD s_page_size = GetSystemPageSize();

#pragma region // FoundLine

void FoundLine::Clear()
{
    is_valid = false;
    is_line = true;
    line = 0;
    offset = 0;
    len = 0;
}

void FoundLine::MarkLine(size_t found_line)
{
    is_valid = true;
    is_line = true;
    line = found_line;
    offset = 0;
    len = 0;
}

void FoundLine::MarkOffset(FileOffset found_offset)
{
    is_valid = true;
    is_line = false;
    line = 0;
    offset = found_offset;
    len = 0;
}

void FoundLine::Found(size_t found_line, unsigned found_offset, unsigned found_len)
{
    is_valid = true;
    is_line = true;
    line = found_line;
    offset = found_offset;
    len = found_len;
}

void FoundLine::Found(FileOffset found_offset, unsigned found_len)
{
    is_valid = true;
    is_line = false;
    line = 0;
    offset = found_offset;
    len = found_len;
}

#pragma endregion // FoundLine
#pragma region // PipeChunk

PipeChunk::PipeChunk()
{
    m_bytes = static_cast<BYTE*>(VirtualAlloc(nullptr, s_page_size, MEM_COMMIT|MEM_RESERVE, PAGE_READWRITE));
}

PipeChunk::PipeChunk(PipeChunk&& other)
{
    Move(std::move(other));
}

PipeChunk::~PipeChunk()
{
    if (m_bytes)
        VirtualFree(m_bytes, 0, MEM_RELEASE);
}

PipeChunk& PipeChunk::operator=(PipeChunk&& other)
{
    Move(std::move(other));
    return *this;
}

DWORD PipeChunk::Capacity() const
{
    return s_page_size;
}

void PipeChunk::Wrote(DWORD wrote)
{
    assert(wrote <= Available());
    m_used += wrote;
    assert(Used() <= Capacity());
}

void PipeChunk::Move(PipeChunk&& other)
{
    m_bytes = other.m_bytes;
    other.m_bytes = nullptr;
    m_used = other.m_used;
    other.m_used = 0;
}

#pragma endregion // PipeChunk
#pragma region // FileLineIter

FileLineIter::FileLineIter(const ViewerOptions& options)
: m_options(options)
{
}

FileLineIter::~FileLineIter()
{
}

FileLineIter& FileLineIter::operator=(FileLineIter&& other)
{
    // m_options can't be updated, and it doesn't need to be.
    m_wrap = other.m_wrap;
    m_codepage = other.m_codepage;
    m_binary_file = other.m_binary_file;
    m_offset = other.m_offset;
    m_bytes = other.m_bytes;
    m_count = other.m_count;
    m_available = other.m_available;
    m_decoder = std::move(other.m_decoder);
    m_width_state.reset(); // Does not carry over.
    m_pending_length = other.m_pending_length;
    m_pending_width = other.m_pending_width;
    m_pending_wrap_length = other.m_pending_wrap_length;
    m_pending_wrap_width = other.m_pending_wrap_width;

    other.Reset();

    return *this;
}

void FileLineIter::Reset()
{
    // m_wrap carries over.
    // m_codepage carries over.
    m_binary_file = true;
    m_offset = 0;
    m_bytes = nullptr;
    m_count = 0;
    m_available = 0;
    // m_decoder carries over.
    m_width_state.reset();
    m_pending_length = 0;
    m_pending_width = 0;
    m_pending_wrap_length = 0;
    m_pending_wrap_width = 0;
}

void FileLineIter::SetEncoding(FileDataType type, UINT codepage)
{
    m_binary_file = (type == FileDataType::Binary);
    m_codepage = codepage;
    m_decoder = CreateDecoder(m_codepage);
}

void FileLineIter::SetWrapWidth(uint32 wrap)
{
    m_wrap = wrap ? wrap : m_options.max_line_length;
}

void FileLineIter::SetBytes(FileOffset offset, const BYTE* bytes, const size_t available)
{
    assert(!m_count);
    m_offset = offset;
    m_bytes = bytes;
    m_count = min<size_t>(available, c_data_buffer_main);
    m_available = available;
// REVIEW:  Does the width state need to span across adjacent buffers?
    m_width_state.reset();
}

FileLineIter::Outcome FileLineIter::Next(const BYTE*& out_bytes, uint32& out_length, uint32& out_width)
{
    out_bytes = m_bytes;

    if (!m_bytes)
    {
        out_length = m_pending_length;
        out_width = m_pending_width;
        m_pending_length = 0;
        m_pending_width = 0;
        return (out_length > 0) ? BreakNewline : Exhausted;
    }

    // Find end of line.
    bool newline = false;
    uint32 can_consume = 0;
    // PERF:  This can end up revisiting the same bytes multiple times if a
    // line wraps before the newline, up to max_line_length.  Could it be
    // worth caching and reusing the can_consume length?
    assert(m_options.max_line_length > m_pending_length);
    const size_t max_consume = min<size_t>(m_count, m_options.max_line_length - m_pending_length);
    if (m_decoder->CharSize() == 1)
    {
        for (const BYTE* walk = m_bytes; can_consume < max_consume; ++walk)
        {
            const BYTE c = walk[0];
            if (c == '\r' && can_consume + 1 <= m_available && walk[1] == '\n')
            {
                can_consume += 2;
                newline = true;
                break;
            }
            else if (c == '\n')
            {
                ++can_consume;
                newline = true;
                break;
            }
            else
            {
                ++can_consume;
            }
        }
    }
    else
    {
        assert(m_decoder->CharSize() == 2);
        for (const BYTE* walk = m_bytes; can_consume < max_consume;)
        {
            if (can_consume + 2 > m_available)
                break;
            const WCHAR c = m_decoder->NextChar(walk);
            if (c == '\r' && can_consume + 4 <= m_available && m_decoder->NextChar(walk + 2) == '\n')
            {
                can_consume += 4;
                newline = true;
                break;
            }
            else if (c == '\n')
            {
                can_consume += 2;
                newline = true;
                break;
            }
            else
            {
                can_consume += 2;
                walk += 2;
            }
        }
    }

#ifdef DEBUG
    const size_t cr_extended = (can_consume > m_count) ? can_consume - m_count : 0;
#endif

    // Get cells until it's time to wrap.
    Outcome outcome = Exhausted;
    const uint32 orig_pending_length = m_pending_length;
    {
        uint32 index = 0;
        uint32 pending_wrap_length = m_pending_wrap_length;
        uint32 pending_wrap_width = m_pending_wrap_width;
        const BYTE* walk = m_bytes;
        while (true)
        {
            assert(index <= m_count + !!newline);
            assert(index <= m_available);

            if (index >= can_consume)
            {
                // Reached end of consumable range.
                if (newline)
                    outcome = BreakNewline;
                break;
            }

            if (m_pending_length + 1 >= m_options.max_line_length)
            {
                // Line exceeds max bytes.
                outcome = BreakMax;
                break;
            }

            uint32 c;
            uint32 clen;
            uint32 blen;
            if (m_binary_file)
            {
                // Multibyte encodings are not supported in binary file mode.
                // Raw bytes are processed using a single-byte OEM codepage.
                // AnalyzeFileType() simplifies the algorithm here by ensuring
                // any multibyte OEM codepages fall back to 437 instead.
                c = walk[0];
                blen = 1;

                // Calc width of codepoint.
                if (c == '\t' && m_options.ctrl_mode != CtrlMode::EXPAND && m_options.tab_mode != TabMode::RAW)
                    clen = c_tab_width - (m_pending_width % c_tab_width);
                else if (c > 0 && c < ' ')
                    clen = (m_options.ctrl_mode == CtrlMode::EXPAND) ? 2 : 1;
                else
                    clen = 1;
            }
            else
            {
                // FUTURE:  In modern encodings (OEM and ANSI encodings) both
                // 0x0D and 0x0A are never be part of a multibyte sequence.
                // But EBCDIC is different:  control characters can be part of
                // multibyte sequences in EBCDIC.

                // Let the decoder read past can_consume.
                c = m_decoder->Decode(walk, uint32(m_available - index), blen);
                if (can_consume < index + blen && m_available > can_consume)
                    break; // Not enough data; resync and continue.

                // Calc width of codepoint.
                if (c == '\t' && m_options.ctrl_mode != CtrlMode::EXPAND && m_options.tab_mode != TabMode::RAW)
                    clen = c_tab_width - (m_pending_width % c_tab_width);
                else if (c == '\n')
                    clen = 0;
                else if (c == '\r' && index + 1 < can_consume && walk[1] == '\n')
                    clen = 0;
                else if (c > 0 && c < ' ')
                    clen = (m_options.ctrl_mode == CtrlMode::EXPAND) ? 2 : 1;
                else if (c == 0xfeff)
                {
                    if (m_offset == 0)
                    {
                        clen = 0;
                    }
                    else
                    {
                        c = 0xfffd;
                        goto calc_width;
                    }
                }
                else
                {
calc_width:
                    if (m_width_state.next(c))
                    {
                        clen = m_width_state.width();
                    }
                    else
                    {
// BUGBUG:  The width can increase later in a sequence, but the wrapping logic
// here quits as soon as the width exceeds the wrapping limit, and that can
// potentially sever a Unicode character sequence.
                        clen = m_width_state.width_delta();
                    }
                }
            }

            if (m_wrap > clen && m_pending_width + clen > m_wrap)
            {
                // Wrapping width reached.
                if (m_pending_wrap_length)
                {
                    m_pending_length = m_pending_wrap_length;
                    m_pending_width = m_pending_wrap_width;
                    outcome = BreakWrapSkip;
                }
                else
                {
                    outcome = BreakWrap;
                }
                break;
            }

            // Smart wrapping at whitespace in text files.
            if (!m_binary_file)
            {
                if (!IsWhiteSpace(c))
                {
                    pending_wrap_length = m_pending_length + blen;
                    pending_wrap_width = m_pending_width + clen;
                }
                else
                {
                    m_pending_wrap_length = pending_wrap_length;
                    m_pending_wrap_width = pending_wrap_width;
                }
            }

            m_pending_length += blen;
            m_pending_width += clen;

            index += blen;
            walk += blen;
        }
    }

    const uint32 length = m_pending_length - orig_pending_length;
    const bool resync = (m_pending_length < orig_pending_length);
#ifdef DEBUG
    if (m_pending_length - orig_pending_length < m_pending_length)
    {
        // A pending wrap can effectively cause the line to be shortened to a
        // length that falls within a previous data buffer!
        assert(m_count >= (m_pending_length - orig_pending_length - cr_extended));
        assert(m_available >= (m_pending_length - orig_pending_length));
    }
#endif

    out_length = m_pending_length;
    out_width = m_pending_width;
    if (outcome != Exhausted)
    {
        m_pending_length = 0;
        m_pending_width = 0;
        m_pending_wrap_length = 0;
        m_pending_wrap_width = 0;
    }

    if (resync)
    {
        assert(outcome == BreakWrapSkip);
        m_count = 0;
        m_available = 0;
        return BreakWrapResync;
    }

    m_bytes += length;
    if (m_count >= length && outcome != Exhausted)
        m_count -= length;
    else
        m_count = 0;
    m_available -= length;

    return outcome;
}

bool FileLineIter::SkipWhitespace(uint32 curr_len, uint32& skipped)
{
    uint32 skip = 0;
    assert(m_options.max_line_length >= curr_len);

    size_t max_skip = min<size_t>(m_options.max_line_length - curr_len, m_count);
    if (max_skip > m_count)
        max_skip = m_count;

    bool done = false;
    for (const BYTE* walk = m_bytes; true; ++skip, ++walk)
    {
        if (skip >= max_skip)
        {
            done = true;
            break;
        }

        const BYTE c = *walk;
        if (!IsWhiteSpace(c))
        {
            done = true;
            break;
        }
    }

    m_bytes += skip;
    m_count -= skip;
    m_available -= skip;

    skipped = skip;
    return done;
}

#pragma endregion // FileLineIter
#pragma region // FileLineMap

FileLineMap::FileLineMap(const ViewerOptions& options)
: m_line_iter(options)
{
}

FileLineMap& FileLineMap::operator=(FileLineMap&& other)
{
    m_wrap = other.m_wrap;
    m_lines = std::move(other.m_lines);
    m_line_numbers = std::move(other.m_line_numbers);
    m_codepage = other.m_codepage;
    m_encoding_name = std::move(other.m_encoding_name);
    m_current_line_number = other.m_current_line_number;
    m_processed = other.m_processed;
    m_pending_begin = other.m_pending_begin;
    m_line_iter = std::move(other.m_line_iter);
    m_skip_whitespace = other.m_skip_whitespace;
    m_wrapped_current_line = other.m_wrapped_current_line;
    m_is_unicode_encoding = other.m_is_unicode_encoding;
#ifdef USE_SMALL_DATA_BUFFER
    m_need_type = other.m_need_type;
#endif

    other.Clear();

    return *this;
}

bool FileLineMap::SetWrapWidth(unsigned wrap)
{
    if (m_wrap != wrap)
    {
        m_wrap = wrap;
        Clear();
        return true;
    }
    return false;
}

void FileLineMap::Clear()
{
    // m_wrap carries over
    m_lines.clear();
    m_line_numbers.clear();
    m_codepage = 0;
    m_encoding_name.Clear();
    m_current_line_number = 1;
    m_processed = 0;
    m_pending_begin = 0;
    m_line_iter.Reset();
    m_skip_whitespace = 0;
    m_wrapped_current_line = false;
    m_is_unicode_encoding = false;
#ifdef USE_SMALL_DATA_BUFFER
    m_need_type = true;
#endif
}

#ifdef USE_SMALL_DATA_BUFFER
void FileLineMap::SetFileType(FileDataType type, UINT codepage, const WCHAR* encoding_name)
{
    m_codepage = codepage;
    m_encoding_name = encoding_name;
    m_line_iter.SetEncoding(type, m_codepage);
    m_need_type = false;
}
#endif

void FileLineMap::Next(const BYTE* bytes, size_t available)
{
    if (!m_processed)
    {
#ifdef USE_SMALL_DATA_BUFFER
        if (m_need_type)
#endif
        {
            const FileDataType type = AnalyzeFileType(bytes, available, &m_codepage, &m_encoding_name);
            m_line_iter.SetEncoding(type, m_codepage);
        }

        switch (m_codepage)
        {
        case CP_UTF7:
        case CP_UTF8:
        case CP_WINUNICODE:     // UTF16-LE
        case 1201:              // UTF16-BE
            m_is_unicode_encoding = true;
            break;
        default:
            m_is_unicode_encoding = false;
            break;
        }
    }

    m_line_iter.SetBytes(m_processed, bytes, available);
    m_line_iter.SetWrapWidth(m_wrap);

do_skip_whitespace:
    if (m_skip_whitespace)
    {
        uint32 skipped;
        assert(m_processed >= m_pending_begin);
        assert(!((m_processed - m_pending_begin) & ~0xffff));
        const bool done = m_line_iter.SkipWhitespace(uint32(m_processed - m_pending_begin), skipped);
        m_pending_begin += skipped;
        m_processed += skipped;
        if (done)
            m_skip_whitespace = 0;
        else if (!m_line_iter.More())
        {
            if (m_skip_whitespace < 2)
            {
                // This case is meant to handle when the last line in a file wraps
                // and is followed by whitespace.  Once it reaches the end of the
                // file, More() will always return false, so this needs a way to
                // stop skipping whitespace even when More() returns false.
                ++m_skip_whitespace;
                return;
            }
            m_skip_whitespace = 0;
        }
    }

    const BYTE* line_ptr;
    uint32 line_length;
    uint32 line_width;
#ifdef DEBUG
    uint32 consumed = 0;
#endif
    while (true)
    {
        const FileLineIter::Outcome outcome = m_line_iter.Next(line_ptr, line_length, line_width);
#ifdef DEBUG_LINE_PARSING
        if (outcome != FileLineIter::Exhausted && outcome != FileLineIter::BreakNewline)
            dbgprintf(L"\u2193 outcome %s",
                      ((outcome == FileLineIter::BreakMax) ? L"BreakMax" :
                       (outcome == FileLineIter::BreakWrap) ? L"BreakWrap" :
                       (outcome == FileLineIter::BreakWrapSkip) ? L"BreakWrapSkip" :
                       (outcome == FileLineIter::BreakWrapResync) ? L"BreakWrapResync" : L"??"));
#endif
        if (outcome == FileLineIter::Exhausted)
            break;

        assert(line_length);
        m_lines.emplace_back(m_pending_begin);
        if (m_wrap && !IsBinaryFile())
            m_line_numbers.emplace_back(m_current_line_number);
#ifdef DEBUG_LINE_PARSING
        dbgprintf(L"finished line %lu; offset %lu (%lx), length %lu, width %lu", m_lines.size(), m_pending_begin, m_pending_begin, line_length, line_width);
#endif
#ifdef DEBUG
        consumed += line_length;
#endif
        m_processed = m_pending_begin + line_length;
        m_pending_begin = m_processed;
        switch (outcome)
        {
        case FileLineIter::BreakMax:
        case FileLineIter::BreakWrap:
            if (!IsBinaryFile())
                break;
            __fallthrough;
        case FileLineIter::BreakNewline:
            ++m_current_line_number;
            m_wrapped_current_line = false;
            break;
        case FileLineIter::BreakWrapSkip:
            m_skip_whitespace = 1;
            goto do_skip_whitespace;
        case FileLineIter::BreakWrapResync:
            return;
        }
    }

    m_processed = m_pending_begin + line_length;
}

size_t FileLineMap::CountFriendlyLines() const
{
    if (m_line_numbers.size())
        return m_line_numbers.back();
    return m_lines.size();
}

FileOffset FileLineMap::GetOffset(size_t index) const
{
    assert(!index || index < m_lines.size());
    return index ? m_lines[index] : 0;  // Uses 0 when m_lines is empty.
}

size_t FileLineMap::GetLineNumber(size_t index) const
{
    assert(!index || index < m_lines.size());
    if (m_line_numbers.size())
    {
        assert(m_line_numbers.size() == m_lines.size());
        return m_line_numbers[index];
    }
    return index + 1;
}

void FileLineMap::GetLineText(const BYTE* p, size_t num_bytes, StrW& out, bool hex_mode) const
{
    const UINT cp = GetCodePage(hex_mode);
    switch (cp)
    {
    case CP_WINUNICODE:
    case 1201:
        {
            const size_t num_chars = (num_bytes + 1) / 2;
            BYTE* o = reinterpret_cast<BYTE*>(out.Reserve(num_chars + 1));
            memcpy(o, p, num_bytes);
            if (num_bytes & 1)
            {
                o[num_bytes - 1] = 0xFD;
                o[num_bytes] = 0xFF;
            }
            if (cp == 1201)
            {
                for (size_t i = num_bytes & ~1; i;)
                {
                    i -= 2;
                    const BYTE t = o[i];
                    o[i] = o[i+1];
                    o[i+1] = t;
                }
            }
            out.OverrideLength(num_chars);
        }
        break;
    default:
        out.SetFromCodepage(cp, reinterpret_cast<const char*>(p), num_bytes);
        break;
    }
}

size_t FileLineMap::FriendlyLineNumberToIndex(size_t line) const
{
    if (m_line_numbers.size())
    {
        const auto iter = std::lower_bound(m_line_numbers.begin(), m_line_numbers.end(), line);
        if (iter == m_line_numbers.end())
            line = m_lines.size();
        else
            line = iter - m_line_numbers.begin();
    }
    else
    {
        if (line)
            --line;
    }
    return line;
}

size_t FileLineMap::OffsetToIndex(FileOffset offset) const
{
    auto iter = std::upper_bound(m_lines.begin(), m_lines.end(), offset);
    size_t index = (iter == m_lines.end()) ? m_lines.size() : (iter - m_lines.begin());
    if (index)
        --index;
    return index;
}

bool FileLineMap::IsUTF8Compatible() const
{
    switch (GetCodePage())
    {
    case 20127:         // 7-bit US-ASCII (Windows)
    case CP_UTF8:       // UTF-8
        return true;
    }
    return false;
}

UINT FileLineMap::GetCodePage(bool hex_mode) const
{
    if (hex_mode)
        return EnsureSingleByteCP(m_codepage);
    return m_codepage;
}

const WCHAR* FileLineMap::GetEncodingName(bool hex_mode) const
{
    if (m_codepage)
    {
        if (hex_mode)
        {
            static StrW s_hexmode_encoding_name;
            StrW tmp;
            if (GetCodePageName(GetCodePage(hex_mode), tmp))
            {
                s_hexmode_encoding_name.Clear();
                s_hexmode_encoding_name.Printf(L"Hex Mode (%s)", tmp.Text());
                return s_hexmode_encoding_name.Text();
            }
        }
        else if (!m_encoding_name.Empty())
        {
            return m_encoding_name.Text();
        }
    }
    return IsBinaryFile() ? L"Binary" : L"Text";
}

#pragma endregion // FileLineMap
#pragma region // ContentCache

ContentCache::ContentCache(const ViewerOptions& options)
: m_options(options)
, m_map(options)
{
}

ContentCache& ContentCache::operator=(ContentCache&& other)
{
    // m_options can't be updated, and it doesn't need to be.
    m_file = other.m_file;
    m_size = other.m_size;
    m_redirected = other.m_redirected;
    m_chunks = std::move(other.m_chunks);
    m_text = other.m_text;
    m_map = std::move(other.m_map);
    m_completed = other.m_completed;
    m_eof = other.m_eof;
    m_data = other.m_data;
    m_data_offset = other.m_data_offset;
    m_data_length = other.m_data_length;
    m_data_slop = other.m_data_slop;

    other.m_file = INVALID_HANDLE_VALUE;
    other.m_data = nullptr;
    other.Close();

    return *this;
}

bool ContentCache::EnsureDataBuffer(Error& e)
{
    if (!m_data)
    {
        m_data = static_cast<BYTE*>(malloc(c_data_buffer_slop + c_data_buffer_main + c_data_buffer_slop));
        if (!m_data)
        {
            e.Sys(ERROR_NOT_ENOUGH_MEMORY);
            return false;
        }
    }
    return true;
}

bool ContentCache::HasContent() const
{
    return (IsOpen() || IsPipe() || m_text);
}

bool ContentCache::SetTextContent(const char* text, Error& e)
{
    Close();

    if (!EnsureDataBuffer(e))
        return false;

    m_text = text;
    m_size = strlen(text);
    m_eof = true;
    return true;
}

bool ContentCache::Open(const WCHAR* name, Error& e)
{
    Close();

    if (!EnsureDataBuffer(e))
        return false;

    m_redirected = (!wcscmp(name, L"<stdin>") && s_piped_stdin);

    if (!m_redirected)
    {
        m_file = CreateFileW(name, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, 0);
        if (m_file == INVALID_HANDLE_VALUE)
        {
            e.Sys();
            return false;
        }

        LARGE_INTEGER liSize;
        if (GetFileSizeEx(m_file, &liSize))
            m_size = liSize.QuadPart;

#ifdef USE_SMALL_DATA_BUFFER
        // Debug builds use a very small read chunk size, which greatly
        // degrades the accuracy of file type and encoding detection.
        // Compensate by pre-reading and analyzing a big chunk.
        const DWORD to_read = 4096 * 24;
        DWORD bytes_read;
        BYTE* buffer = new BYTE[to_read];
        if (buffer)
        {
            if (ReadFile(m_file, buffer, to_read, &bytes_read, nullptr))
            {
                UINT codepage;
                StrW encoding_name;
                FileDataType type = AnalyzeFileType(buffer, bytes_read, &codepage, &encoding_name);
                m_map.SetFileType(type, codepage, encoding_name.Text());
            }
            delete [] buffer;
        }
#endif

        return true;
    }
    else
    {
        const HANDLE hin = s_piped_stdin;
        s_piped_stdin = 0;
        if (!hin || hin == INVALID_HANDLE_VALUE)
        {
            e.Sys(ERROR_NO_DATA);
            return false;
        }
        while (true)
        {
            if (m_chunks.empty() || !m_chunks.back().Available())
                m_chunks.emplace_back();

            PipeChunk& chunk = m_chunks.back();
            const DWORD to_read = chunk.Available();
            DWORD bytes_read;
            if (!ReadFile(hin, chunk.WritePtr(), to_read, &bytes_read, nullptr))
            {
                const DWORD err = GetLastError();
                if (err && err != ERROR_HANDLE_EOF && err != ERROR_BROKEN_PIPE)
                    e.Sys(err);
                m_eof = true;
                return !e.Test();
            }
            chunk.Wrote(bytes_read);
            m_size += bytes_read;
        }
    }
}

void ContentCache::Close()
{
    if (m_file != INVALID_HANDLE_VALUE)
    {
        CloseHandle(m_file);
        m_file = INVALID_HANDLE_VALUE;
    }

    m_size = 0;
    m_chunks.swap(PipeChunks {});
    m_text = nullptr;
    m_redirected = false;
    m_eof = false;

    Reset();

    m_data_offset = 0;
    m_data_length = 0;
    m_data_slop = 0;
}

void ContentCache::Reset()
{
    m_map.Clear();
    m_completed = false;
}

void ContentCache::SetWrapWidth(unsigned wrap)
{
    if (m_map.SetWrapWidth(wrap))
    {
        assert(!m_map.Count());
        m_completed = false;
    }
}

unsigned ContentCache::FormatLineData(const size_t line, unsigned left_offset, StrW& s, const unsigned max_width, Error& e, const WCHAR* const color, const FoundLine* const found_line)
{
    if (!EnsureFileData(line, e))
        return 0;
    if (line >= m_map.Count())
        return 0;

    assert(!found_line || (!found_line->Empty() && found_line->is_line));
    const FileOffset offset = GetOffset(line);

    assert(offset >= m_data_offset);
    const BYTE* ptr = m_data + (offset - m_data_offset);
    const unsigned len = GetLength(line);
    assert(ptr + len <= m_data + m_data_length);

    StrW tmp;
    m_map.GetLineText(ptr, len, tmp);

    unsigned visible_len = 0;
    unsigned total_cells = 0;
    int32 truncate_cells = -1;
    int32 truncate_len = -1;

    bool need_found_highlight = false;
    bool highlighting_found_text = false;

    auto append_text = [&](const WCHAR* text, unsigned text_len, unsigned cells=1)
    {
        if (found_line && line == found_line->line && found_line->len && text == tmp.Text() + found_line->offset)
            need_found_highlight = true;
        else if (need_found_highlight && text >= tmp.Text() + found_line->offset + found_line->len)
            need_found_highlight = false;

        if (visible_len >= left_offset)
        {
            if (left_offset)
            {
                left_offset = 0;
                visible_len = 0;
            }
            if (need_found_highlight)
            {
                s.AppendColor(GetColor(ColorElement::SearchFound));
                need_found_highlight = false;
                highlighting_found_text = true;
            }
            else if (highlighting_found_text && text >= tmp.Text() + found_line->offset + found_line->len)
            {
                s.Append(c_norm);
                if (color)
                    s.AppendColor(color);
                highlighting_found_text = false;
            }
            s.Append(text, text_len);
        }
        visible_len += cells;
        total_cells += cells;
    };

    const WCHAR* const end = tmp.Text() + tmp.Length();
    const WCHAR* walk = tmp.Text();
    const WCHAR* const maybe_bom = (offset == 0 && !m_map.IsBinaryFile() && m_map.IsUnicodeEncoding()) ? walk : nullptr;
    while (walk < end)
    {
        if (!*walk)
        {
            if (!left_offset && visible_len >= max_width)
                goto LOut;
            append_text(L" ", 1);
            ++walk;
        }
        else
        {
            wcwidth_iter inner_iter(walk);
            while (const int32 c = inner_iter.next())
            {
                if (!left_offset && visible_len >= max_width)
                    goto LOut;

                if (c == '\r' && !m_map.IsBinaryFile() && inner_iter.more() && *inner_iter.get_pointer() == '\n')
                {
                    // Omit trailing \r\n at end of line in a text file.
                }
                else if (c == '\n' && !m_map.IsBinaryFile() && !inner_iter.more())
                {
                    // Omit trailing \n at end of line in a text file.
                }
                else if (c == '\t' && m_options.ctrl_mode != CtrlMode::EXPAND && m_options.tab_mode != TabMode::RAW)
                {
                    unsigned spaces = c_tab_width - (total_cells % c_tab_width);
                    const bool something_fits = (visible_len + spaces > left_offset);
                    const bool apply_color = (m_options.tab_mode == TabMode::HIGHLIGHT && something_fits && !color);
                    if (apply_color)
                        s.AppendColor(GetColor(ColorElement::CtrlCode));
                    while (spaces--)
                    {
                        if (m_options.tab_mode == TabMode::HIGHLIGHT)
                            append_text(spaces ? L"-" : L">", 1);
                        else
                            append_text(L" ", 1);
                        if (!left_offset && visible_len >= max_width)
                            break;
                    }
                    s.AppendNormalIf(apply_color);
                }
                else if (c >= 0 && c < ' ')
                {
                    if (m_options.ctrl_mode == CtrlMode::EXPAND)
                    {
                        const bool something_fits = (visible_len + 2 > left_offset);
                        const bool apply_color = (something_fits && !color);
                        if (apply_color)
                            s.AppendColor(GetColor(ColorElement::CtrlCode));
                        append_text(L"^", 1);
                        if (left_offset || visible_len < max_width)
                        {
                            WCHAR text[2] = { WCHAR('@' + c) };
                            append_text(text, 1);
                        }
                        s.AppendNormalIf(apply_color);
                    }
#ifdef INCLUDE_CTRLMODE_PERIOD
                    else if (m_options.ctrl_mode == CtrlMode::PERIOD)
                    {
                        assert(left_offset || visible_len < max_width);
                        const bool apply_color = (!left_offset && !color);
                        if (apply_color)
                            s.AppendColor(GetColor(ColorElement::CtrlCode));
                        append_text(L".", 1);
                        s.AppendNormalIf(apply_color);
                    }
#endif
#ifdef INCLUDE_CTRLMODE_SPACE
                    else if (m_options.ctrl_mode == CtrlMode::SPACE)
                    {
                        assert(left_offset || visible_len < max_width);
                        append_text(L" ", 1);
                    }
#endif
                    else
                    {
                        assert(m_options.ctrl_mode == CtrlMode::OEM437);
                        assert(left_offset || visible_len < max_width);
                        append_text(c_oem437[c], 1);
                    }
                }
                else if (maybe_bom == walk && c == 0xfeff)
                {
                    // Omit byte order mark at beginning of file.
                }
                else
                {
                    const int32 clen = inner_iter.character_wcwidth_signed();
                    if (clen < 0)
                    {
                        assert(left_offset || visible_len < max_width);
                        const bool something_fits = (visible_len >= left_offset);
                        if (something_fits)
                        {
                            if (!color)
                                s.AppendColor(GetColor(ColorElement::CtrlCode));
                            append_text(L"?", 1);
                            if (!color)
                                s.Append(c_norm);
                        }
                        else
                        {
                            ++visible_len;
                        }
                    }
                    else
                    {
// TODO:  Figure out what should happen for this assert now...
//                        assert(clen == 1);
                        if (!left_offset)
                        {
                            if (truncate_cells < 0 && visible_len + clen > max_width)
                            {
                                assert(visible_len <= max_width);
                                truncate_cells = visible_len;
                                truncate_len = s.Length();
                            }
                            if (visible_len + clen > max_width)
                            {
                                s.SetLength(truncate_len);
                                visible_len = truncate_cells;
                                assert(visible_len <= max_width);
                                return visible_len;
                            }
                        }
                        append_text(inner_iter.character_pointer(), inner_iter.character_length(), clen);
                    }
                }
            }

            assert(walk + wcslen(walk) == inner_iter.get_pointer());
            walk = inner_iter.get_pointer();
        }
    }

LOut:
    if (m_options.show_debug_info && visible_len < max_width)
    {
        append_text(c_eol_marker, -1);
        if (color)
            s.AppendColor(color);
    }
    else if (highlighting_found_text)
    {
        s.Append(c_norm);
        if (color)
            s.AppendColor(color);
    }
    if (left_offset)
        visible_len = 0;
#ifdef DEBUG
    assert(visible_len <= max_width);
    if (!left_offset && visible_len >= max_width)
        assert(visible_len == max_width || m_map.GetWrapWidth());
#endif
    return visible_len;
}

bool ContentCache::FormatHexData(FileOffset offset, unsigned row, unsigned hex_bytes, StrW& s, Error& e, const FoundLine* found_line)
{
    offset += row * hex_bytes;

    if (!EnsureHexData(offset, hex_bytes, e))
        return false;

    assert(offset < GetFileSize());
    assert(offset >= m_data_offset);
    const BYTE* ptr = m_data + (offset - m_data_offset);
    unsigned len = unsigned(std::min<FileOffset>(hex_bytes, GetFileSize() - offset));
    if (offset + len > m_data_offset + m_data_length)
    {
        len = unsigned(m_data_offset + m_data_length - offset);
        assert(len < 32); // ContentCache doesn't know m_hex_width, so use 32.
    }
    assert(ptr + len <= m_data + m_data_length);

    StrW tmp;
    m_map.GetLineText(ptr, len, tmp, true/*hex_mode*/);
    assert(tmp.Length() == len);
    if (tmp.Length() != len)
    {
        tmp.Clear();
        for (unsigned ii = 0; ii < len; ++ii)
        {
            if (ptr[ii] < 0x7f)
                tmp.Append(ptr[ii]);
            else
                // TODO:  Maybe apply color?
                tmp.Append(L".", 1);
        }
    }

    const WCHAR* marked_color = nullptr;
    bool highlighting_found_text = false;
    assert(!found_line || !found_line->Empty());
    if (found_line && offset <= found_line->offset && found_line->offset < offset + hex_bytes)
        marked_color = GetColor(ColorElement::MarkedLine);

    // Format the offset.
    if (offset % 0x400 == 0)
        s.AppendColor(L"1");
    s.Printf(L"%08.8x", offset);
    if (offset % 0x400 == 0)
        s.Append(c_norm);
    s.Append(L"  ", 2);

    // Format the hex bytes.
    if (marked_color)
        s.AppendColor(marked_color);
    for (unsigned ii = 0; ii < hex_bytes; ++ii)
    {
        if (highlighting_found_text && offset + ii == found_line->offset + found_line->len)
        {
            highlighting_found_text = false;
            s.Append(c_norm), s.AppendColor(marked_color);
        }
        if (ii)
        {
            if (ii % (1 << m_options.hex_grouping) == 0)
                s.Append(L"  ", ((ii % 8) == 0) ? 2 : 1);
        }
        if (marked_color && found_line->len && offset + ii == found_line->offset)
        {
            highlighting_found_text = true;
            s.AppendColor(GetColor(ColorElement::SearchFound));
        }
        if (ii < len)
        {
            const bool hilite_newline = (!highlighting_found_text && ptr[ii] == '\n' && !marked_color);
            if (hilite_newline)
                s.AppendColor(GetColor(ColorElement::CtrlCode));
            s.Printf(L"%02X", ptr[ii]);
            s.AppendNormalIf(hilite_newline);
        }
        else
        {
            s.Append(L"  ", 2);
        }
    }
    if (marked_color)
        s.Append(c_norm);

    // Format the text characters.
    s.Printf(L"  ", 2);
    s.AppendColor(GetColor(ColorElement::Divider));
    // s.Append(L"\u2502", 1);
    s.Append(L"*", 1);
    if (marked_color)
        s.AppendColor(marked_color);
    else
        s.Append(c_norm);
    highlighting_found_text = false;
    for (unsigned ii = 0; ii < len; ++ii)
    {
        const BYTE c = ptr[ii];
        if (marked_color)
        {
            if (found_line->len && offset + ii == found_line->offset)
            {
                highlighting_found_text = true;
                s.AppendColor(GetColor(ColorElement::SearchFound));
            }
            else if (highlighting_found_text && offset + ii == found_line->offset + found_line->len)
            {
                highlighting_found_text = false;
                s.Append(c_norm), s.AppendColor(marked_color);
            }
        }
        if (c > 0 && c < ' ')
        {
            const bool hilite_newline = (!highlighting_found_text && c == '\n' && !marked_color);
            if (hilite_newline)
                s.AppendColor(GetColor(ColorElement::CtrlCode));
            s.Append(c_oem437[c], 1);
            s.AppendNormalIf(hilite_newline);
        }
        else if (!c || wcwidth(tmp.Text()[ii]) != 1)
        {
            if (!marked_color)
                s.AppendColor(GetColor(ColorElement::Divider));
            s.Append(L".", 1);
            s.AppendNormalIf(!marked_color);
        }
        else
        {
            s.Append(tmp.Text() + ii, 1);
        }
    }
    if (marked_color)
        s.Append(c_norm);
    s.AppendColor(GetColor(ColorElement::Divider));
    // s.Append(L"\u2502", 1);
    s.Append(L"*", 1);
    s.Append(c_norm);

    return true;
}

bool ContentCache::ProcessThrough(size_t line, Error& e, bool cancelable)
{
    assert(!e.Test());

    bool ret = true;
    if (HasContent())
    {
        while (line >= m_map.Count() && !m_completed)
        {
            const FileOffset offset = m_map.Processed();
            if (!LoadData(offset, m_data_slop, e))
            {
                m_completed = true;
                return false;
            }

            const size_t to_process = m_data_offset + m_data_length - offset;
            if (!to_process)
            {
                ret = false;
                break;
            }

            const BYTE* data = m_data + (offset - m_data_offset);
            m_map.Next(data, to_process);

            if (m_size < m_map.Processed())
                m_size = m_map.Processed();

            if (cancelable && IsSignaled())
            {
                e.Set(E_ABORT);
                return false;
            }
        }

        if (m_map.Processed() >= m_size)
        {
            m_map.Next(nullptr, 0);
            m_completed = true;
        }
    }
    else
    {
        m_completed = true;
    }

    return ret;
}

bool ContentCache::ProcessToEnd(Error& e, bool cancelable)
{
    assert(!e.Test());
    if (!m_completed)
    {
        ProcessThrough(size_t(-1), e, cancelable);
        if (e.Code() == ERROR_HANDLE_EOF)
            e.Clear();
    }
    return !e.Test();
}

FileOffset ContentCache::GetMaxHexOffset(unsigned hex_width) const
{
    const FileOffset partial = (GetFileSize() % hex_width);
    return GetFileSize() + (partial ? hex_width - partial : 0);
}

unsigned ContentCache::GetLength(size_t line) const
{
    assert(line < Count());
    if (line < Count())
    {
        const FileOffset offset = GetOffset(line);
        const FileOffset next = (line + 1 < Count()) ? GetOffset(line + 1) : m_map.Processed();
        assert(next - offset <= 1024);
        return unsigned(next - offset);
    }
    return 0;
}

bool ContentCache::Find(bool next, const WCHAR* needle, FoundLine& found_line, bool caseless, Error& e)
{
    StrW tmp;
    const unsigned needle_len = unsigned(wcslen(needle));
    assert(needle_len);

    if (!found_line.is_line || found_line.Empty())
    {
        if (!next)
        {
            ProcessToEnd(e, true/*cancelable*/);
            if (e.Test())
                return false;
        }

        if (found_line.Empty())
        {
            if (next)
                found_line.Found(0, 0, 0);
            else
                found_line.Found(Count(), 0, 0);
        }
        else
        {
            assert(!found_line.is_line);
            const size_t index = m_map.OffsetToIndex(found_line.offset);
            const unsigned offset = unsigned(found_line.offset - m_map.GetOffset(index));
            found_line.Found(index, offset, 0);
        }
    }

    assert(!found_line.Empty() && found_line.is_line);
    size_t index = found_line.line;
    while (true)
    {
        if (IsSignaled())
        {
            found_line.Found(index, 0, 0);
            e.Set(E_ABORT);
            return false;
        }

        if (next)
        {
            if (index + 1 >= Count())
            {
                ProcessThrough(index + 1, e, true/*cancelable*/);
                if (e.Test())
                {
                    if (e.Code() == E_ABORT)
                        found_line.Found(index, 0, 0);
                    return false;
                }
                if (index + 1 >= Count())
                    return false;
            }
            ++index;
        }
        else
        {
            // Going in reverse doesn't need to use ProcessThrough().
            if (!index || index > Count())
                return false;
            --index;
        }

        if (!EnsureFileData(index, e))
            return false;

        const FileOffset offset = GetOffset(index);
        assert(offset >= m_data_offset);
        const BYTE* const ptr = m_data + (offset - m_data_offset);
        unsigned len = GetLength(index);
        assert(len);
        assert(ptr + len <= m_data + m_data_length);

        // IMPORTANT:  This is how Find() handles searching across forced line
        // breaks -- it relies on the data buffer always having at least
        // c_data_buffer_slop bytes more than the current line (except at the
        // end of the file), and on max_needle being less than or equal to
        // c_data_buffer_slop.
        if (len && ptr[len - 1] != '\n')
        {
            // Extend len by needle_len - 1, not to extend the end of the
            // buffer, and also not to extend past a newline.  This only needs
            // to include enough extra to handle when the needle is split
            // across a max line length break; anything past that will be
            // caught when searching the next line.
            for (unsigned extend = needle_len - 1; extend-- && ptr + len < m_data + m_data_length && ptr[len - 1] != '\n';)
                ++len;
        }

        StrW tmp;
        m_map.GetLineText(ptr, len, tmp);

        // FUTURE:  Optional regex search?
        // PERF:  Boyer-Moore search?
        const WCHAR* const end = tmp.Text() + tmp.Length() - (needle_len - 1);
        for (const WCHAR* p = tmp.Text(); p < end; ++p)
        {
            const int n = caseless ? _wcsnicmp(p, needle, needle_len) : wcsncmp(p, needle, needle_len);
            if (n == 0)
            {
                found_line.Found(index, unsigned(p - tmp.Text()), needle_len);
                return true;
            }
        }
    }
}

bool ContentCache::Find(bool next, const WCHAR* needle, unsigned hex_width, FoundLine& found_line, bool caseless, Error& e)
{
    StrW tmp;
    const unsigned needle_len = unsigned(wcslen(needle));
    assert(needle_len);

    if (found_line.Empty())
    {
        if (next)
            found_line.Found(FileOffset(-1), 0);
        else
            found_line.Found(GetFileSize(), 0);
    }
    else if (found_line.is_line)
    {
        size_t index = m_map.FriendlyLineNumberToIndex(found_line.line + 1);
        found_line.Found(m_map.GetOffset(index) + found_line.offset, 0);
    }

    assert(!found_line.Empty() && !found_line.is_line);
    FileOffset offset = found_line.offset;
    while (true)
    {
        if (IsSignaled())
        {
            found_line.Found(offset, 0);
            e.Set(E_ABORT);
            return false;
        }

        if (next)
        {
            if (offset == FileOffset(-1))
                offset = 0;
            else if (offset + hex_width >= GetFileSize())
                return false;
            else
                offset = (offset & ~FileOffset(hex_width - 1)) + hex_width;
        }
        else
        {
            if (offset >= GetFileSize())
                offset = ((GetFileSize() + (hex_width - 1)) & ~FileOffset(hex_width - 1)) - hex_width;
            else if (!(offset & ~FileOffset(hex_width - 1)))
                return false;
            else
                offset = (offset & ~FileOffset(hex_width - 1)) - hex_width;
        }

        if (!EnsureHexData(offset, hex_width, e))
            return false;

        assert(offset >= m_data_offset);
        const BYTE* const ptr = m_data + (offset - m_data_offset);
        unsigned len = hex_width;
        assert(len);
        if (offset + len > m_data_offset + m_data_length)
        {
            len = unsigned(m_data_offset + m_data_length - offset);
            assert(len < hex_width);
        }
        assert(ptr + len <= m_data + m_data_length);

        // IMPORTANT:  This is how Find() handles searching across forced line
        // breaks -- it relies on the data buffer always having at least
        // c_data_buffer_slop bytes more than the current line (except at the
        // end of the file), and on max_needle being less than or equal to
        // c_data_buffer_slop.
        {
            // Extend len by needle_len - 1, not to extend the end of the
            // buffer.  This only needs to include enough extra to handle when
            // the needle is split across a max line length break; anything
            // past that will be caught when searching the next line.
            for (unsigned extend = needle_len - 1; extend-- && ptr + len < m_data + m_data_length;)
                ++len;
        }

// TODO:  Encodings.  But what does that even mean for hex mode?  Really it should have a hex entry mode.
// TODO:  Non-convertible characters will make conversion go haywire.
        StrW tmp;
        m_map.GetLineText(ptr, len, tmp, true/*hex_mode*/);

// TODO:  Optional regex search.
// TODO:  Boyer-Moore search.
        const WCHAR* const end = tmp.Text() + tmp.Length() - (needle_len - 1);
        for (const WCHAR* p = tmp.Text(); p < end; ++p)
        {
            const int n = caseless ? _wcsnicmp(p, needle, needle_len) : wcsncmp(p, needle, needle_len);
            if (n == 0)
            {
                found_line.Found(offset + (p - tmp.Text()), needle_len);
                return true;
            }
        }
    }
}

#ifdef DEBUG
enum LoadType { LT_NONE, LT_HEADOPT, LT_TAILOPT, LT_ABSOLUTE, LT_REDIRECT, LT_TEXT };
LoadType g_last_load_type = LT_NONE;
#endif

bool ContentCache::EnsureFileData(size_t line, Error& e)
{
    assert(HasContent());

    if (line >= Count())
    {
        if (!m_completed)
        {
            if (!ProcessThrough(line, e))
                return false;
        }

        if (line >= Count())
        {
            e.Sys(ERROR_HANDLE_EOF);
            return false;
        }
    }

    const FileOffset offset = GetOffset(line);
    const unsigned length = GetLength(line);

    if (offset < m_data_offset || offset + length > m_data_offset + std::max<intptr_t>(0, intptr_t(m_data_length) - m_data_slop))
    {
        if (!LoadData(offset, m_data_slop, e))
            return false;
    }

#if 0
#ifdef DEBUG
    assert(offset >= m_data_offset);
    const BYTE* ptr = m_data + (offset - m_data_offset);
    if (!(ptr + length <= m_data + m_data_length))
        dbgprintf(L"g_last_load_type == %u", g_last_load_type);
    assert(ptr + length <= m_data + m_data_length);
#endif
#endif

    return true;
}

bool ContentCache::EnsureHexData(FileOffset offset, unsigned length, Error& e)
{
    assert(HasContent());

    if (offset >= GetFileSize())
    {
        e.Sys(ERROR_HANDLE_EOF);
        return false;
    }

    if (offset + length > GetFileSize())
        length = unsigned(GetFileSize() - offset);

    if (offset < m_data_offset || offset + length > m_data_offset + std::max<intptr_t>(0, intptr_t(m_data_length) - m_data_slop))
    {
        if (!LoadData(offset, m_data_slop, e))
            return false;
    }

#if 0
#ifdef DEBUG
    assert(offset >= m_data_offset);
    const BYTE* ptr = m_data + (offset - m_data_offset);
    if (!(ptr + length <= m_data + m_data_length))
        dbgprintf(L"g_last_load_type == %u", g_last_load_type);
    assert(ptr + length <= m_data + m_data_length);
#endif
#endif

    return true;
}

bool ContentCache::LoadData(const FileOffset offset, DWORD& end_slop, Error& e)
{
    assert(HasContent());

    const DWORD c_data_buffer_max = c_data_buffer_slop + c_data_buffer_main + c_data_buffer_slop;

    FileOffset begin = offset;
    FileOffset end = offset + c_data_buffer_main + c_data_buffer_slop;

    if (begin)
    {
        if (begin > c_data_buffer_slop)
            begin -= c_data_buffer_slop;
        else
            begin = 0;
    }
    if ((m_redirected || m_text) && end > m_size)
    {
        end = m_size;
    }

    DWORD kept_at_head = 0;
    DWORD kept_at_tail = 0;
    DWORD to_read = DWORD(end - begin);
    assert(begin <= end);

    if (m_text)
    {
        assert(m_eof);
        assert(end <= m_size);
        assert(begin <= end);
        m_data_offset = begin;
        m_data_length = to_read;
        m_data_slop = 0;
        memmove(m_data, m_text + begin, to_read);
#ifdef DEBUG
        g_last_load_type = LT_TEXT;
#endif
        return !!to_read;
    }

    if (m_redirected)
    {
        assert(m_eof);
        size_t index = begin / s_page_size;
        DWORD ofs = begin % s_page_size;
        assert(!kept_at_head);
        m_data_offset = begin;
        m_data_length = 0;
        while (to_read)
        {
            assert(index < m_chunks.size());
            auto& chunk = m_chunks[index];
            assert(chunk.Used() >= ofs);
            const DWORD len = std::min<DWORD>(to_read, chunk.Used() - ofs);
            if (!len)
                break;
            memmove(m_data + m_data_length, chunk.Bytes() + ofs, len);
            assert(to_read >= len);
            to_read -= len;
            m_data_length += len;
            ++index;
            ofs += len;
            ofs %= s_page_size;
        }
        if (begin + c_data_buffer_main < end)
            m_data_slop = DWORD(end - (begin + c_data_buffer_main));
        else
            m_data_slop = 0;
#ifdef DEBUG
        g_last_load_type = LT_REDIRECT;
#endif
        return true;
    }

#ifdef DEBUG
    g_last_load_type = LT_ABSOLUTE;
#endif

    if (begin < m_data_offset + m_data_length && end > m_data_offset)
    {
        // There is overlap with already-loaded data.  Try to reuse the
        // already-loaded data.
        if (begin >= m_data_offset && end >= m_data_offset + m_data_length)
        {
            // Shift part of the data to the beginning of the buffer and then
            // adjust kept_at_head and to_read to fill the rest of the
            // buffer with data from the disk.

            // Calc offset to what part of the data to keep.
            const size_t offset_to_begin_in_data = (begin - m_data_offset);
            assert(end >= begin + offset_to_begin_in_data);
            // Calc how much data to keep.
            const size_t keep_length = std::min<size_t>(m_data_length, end - begin) - offset_to_begin_in_data;
            assert(keep_length <= c_data_buffer_max);
            // Shift the data to keep.
            memmove(m_data, m_data + offset_to_begin_in_data, keep_length);
            // Adjust what to read from file to fill the rest of the buffer.
            assert(to_read >= keep_length);
            kept_at_head = DWORD(keep_length);
            to_read -= DWORD(keep_length);
            assert(int(to_read) >= 0);
            assert(keep_length + to_read == DWORD(end - begin));
#ifdef DEBUG
            g_last_load_type = LT_HEADOPT;
#endif
        }
        else if (begin < m_data_offset && end < m_data_offset + m_data_length)
        {
            // Shift part of the data to the end of the buffer and then adjust
            // kept_at_head and to_read to fill the rest of the buffer with
            // data from the disk.

// TODO:  Verify accuracy and correctness.
            // Calc how much data to keep.
            assert(end > m_data_offset);
            const size_t keep_length = (end - m_data_offset);
            // Calc offset to where kept data belongs.
            assert(m_data_offset > begin);
            const size_t offset_to_dest_for_data = (m_data_offset - begin);
            // Shift the data to keep.
            memmove(m_data + offset_to_dest_for_data, m_data, keep_length);
            // Adjust what to read from file to fill the rest of the buffer.
            assert(to_read >= keep_length);
            kept_at_tail = DWORD(keep_length);
            to_read -= DWORD(keep_length);
            assert(int(to_read) >= 0);
            assert(keep_length + to_read == DWORD(end - begin));
#ifdef DEBUG
            g_last_load_type = LT_TAILOPT;
#endif
        }
    }

    LARGE_INTEGER liMove;
    liMove.QuadPart = begin + kept_at_head;
    if (!SetFilePointerEx(m_file, liMove, nullptr, FILE_BEGIN))
    {
        e.Sys();
        m_eof = true;
        return false;
    }

    DWORD bytes_read = 0;
    assert(kept_at_head + to_read + kept_at_tail <= c_data_buffer_max);
    if (!ReadFile(m_file, m_data + kept_at_head, to_read, &bytes_read, nullptr))
    {
        const DWORD err = GetLastError();
        if (err && err != ERROR_HANDLE_EOF)
            e.Sys(err);
        m_eof = true;
        assert(!bytes_read);
        return false;
    }

    m_data_offset = begin;
    m_data_length = kept_at_head + bytes_read + kept_at_tail;
    if (begin + c_data_buffer_main < end)
        m_data_slop = DWORD(end - (begin + c_data_buffer_main));
    else
        m_data_slop = 0;
    if (bytes_read < to_read)
        m_eof = true;
    return true;
}

#pragma endregion // ContentCache

