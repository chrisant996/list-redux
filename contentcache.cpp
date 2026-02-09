// Copyright (c) 2025 by Christopher Antos
// License: http://opensource.org/licenses/MIT

// vim: set et ts=4 sw=4 cino={0s:

#include "pch.h"
#include "input.h"
#include "colors.h"
#include "contentcache.h"
#include "vieweroptions.h"
#include "wcwidth.h"
#include "wcwidth_iter.h"
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
static const WCHAR c_div_char[] = L":"; //L"\u2590"; //L"\u2595"; //L":";

#ifdef DEBUG
const unsigned c_min_num_width = 3;
#else
const unsigned c_min_num_width = 6;
#endif
const unsigned c_min_hexofs_width = 6;
const unsigned c_margin_padding = 2;

constexpr unsigned c_find_horiz_scroll_threshold = 10;

static HANDLE s_piped_stdin = 0;
void SetPipedInput()
{
    s_piped_stdin = GetStdHandle(STD_INPUT_HANDLE);
    if (s_piped_stdin == INVALID_HANDLE_VALUE)
        s_piped_stdin = 0;

    HANDLE hin = CreateFile(L"CONIN$", GENERIC_READ|GENERIC_WRITE, FILE_SHARE_READ|FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, 0);
    SetStdHandle(STD_INPUT_HANDLE, hin);
    AutoMouseConsoleMode::SetStdInputHandle(hin);
}

const WCHAR* MakeOverlayColor(const WCHAR* color, const WCHAR* overlay)
{
    static StrW s_tmp;
    s_tmp = color;
    if (overlay && *overlay)
    {
        if (!s_tmp.Empty())
            s_tmp.Append(';');
        s_tmp.Append(overlay);
    }
    return s_tmp.Text();
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

#pragma region // FoundOffset

void FoundOffset::Clear()
{
    is_valid = false;
    offset = 0;
    len = 0;
}

bool FoundOffset::Equals(const FoundOffset& other) const
{
    if (!is_valid != !other.is_valid)
        return false;
    if (!is_valid && !other.is_valid)
        return true;
    assert(is_valid && other.is_valid);
    if (offset == other.offset && len == other.len)
        return true;
    return false;
}

void FoundOffset::MarkOffset(FileOffset found_offset)
{
    is_valid = true;
    offset = found_offset;
    len = 0;
}

void FoundOffset::Found(FileOffset found_offset, unsigned found_len)
{
    is_valid = true;
    offset = found_offset;
    len = found_len;
}

#pragma endregion // FoundOffset
#pragma region // PatchBlock

PatchBlock::PatchBlock(FileOffset offset)
: m_offset(offset)
, m_mask(0)
{
    assert((offset % sizeof(m_bytes)) == 0);
    ZeroMemory(m_bytes, sizeof(m_bytes));
}

bool PatchBlock::IsSet(FileOffset offset) const
{
    assert(offset >= m_offset);
    assert(offset < m_offset + sizeof(m_bytes));
    const unsigned index = unsigned(offset - m_offset);
    const unsigned mask = 1 << index;
    return !!(m_mask & mask);
}

BYTE PatchBlock::GetByte(FileOffset offset) const
{
    assert(IsSet(offset));
    const unsigned index = unsigned(offset - m_offset);
    return m_bytes[index];
}

void PatchBlock::SetByte(FileOffset offset, BYTE value, const BYTE* original)
{
    assert(offset >= m_offset);
    assert(offset < m_offset + sizeof(m_bytes));
    assert(implies(original, !IsSet(offset)));
    const unsigned index = unsigned(offset - m_offset);
    m_bytes[index] = value;
    if (original)
        m_original[index] = *original;
    m_mask |= 1 << index;
}

void PatchBlock::RevertByte(FileOffset offset)
{
    assert(IsSet(offset));
    const unsigned index = unsigned(offset - m_offset);
    m_mask &= ~(1 << index);
}

void PatchBlock::MergeFrom(const PatchBlock& other)
{
    for (unsigned i = 0; i < c_size; ++i)
    {
        const unsigned mask = (1 << i);
        if (other.m_mask & mask)
        {
            if (!(m_mask & mask))
                m_original[i] = other.m_original[i];
            m_bytes[i] = other.m_bytes[i];
        }
    }

    m_mask |= other.m_mask;
}

bool PatchBlock::Save(HANDLE hfile, bool original, Error& e)
{
    FileOffset offset;
    BYTE bytes[c_size];
    unsigned len = 0;

    bool past_end = false;
    for (unsigned index = 0; !past_end; ++index)
    {
        past_end = (index >= c_size);
        if (!past_end && IsSet(m_offset + index))
        {
            if (!len)
                offset = m_offset + index;
            bytes[len++] = original ? m_original[index] : m_bytes[index];
        }
        else if (len)
        {
            DWORD num_io;
            LARGE_INTEGER liSeek;
            liSeek.QuadPart = offset;
            // IMPORTANT:  It's tempting to want to read the current values
            // first, to improve accuracy of UndoSave in case concurrent file
            // writes might be happening.  BUT IT'S A TRAP!  It risks reading
            // values that were already previously written, if a previous save
            // fails partway through and the user retries the save.
            if (!SetFilePointerEx(hfile, liSeek, nullptr, FILE_BEGIN) ||
                !WriteFile(hfile, bytes, len, &num_io, nullptr))
            {
                e.Sys();
                StrW msg;
                msg.Printf(L"Error writing %u byte(s) at offset %08.8lx.", len, offset);
                e.Set(msg.Text());
                return false;
            }
            len = 0;
        }
    }

    return true;
}

#pragma endregion // PatchBlock
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
    m_explicit_wrap = other.m_explicit_wrap;

    m_codepage = other.m_codepage;
    m_binary_file = other.m_binary_file;
    m_decoder = std::move(other.m_decoder);

    m_offset = other.m_offset;
    m_bytes = other.m_bytes;
    m_count = other.m_count;
    m_available = other.m_available;
    m_width_state.reset(); // Does not carry over.
    m_pending_length = other.m_pending_length;
    m_pending_width = other.m_pending_width;
    m_pending_wrap_length = other.m_pending_wrap_length;
    m_pending_wrap_width = other.m_pending_wrap_width;
    m_pending_wrap_indent = other.m_pending_wrap_indent;
    m_consecutive_spaces = other.m_consecutive_spaces;
    m_hanging_indent = other.m_hanging_indent;
    m_any_nonspace = other.m_any_nonspace;

#ifdef DEBUG
    m_line_index = other.m_line_index;
#endif

    other.Reset();
    return *this;
}

void FileLineIter::Reset()
{
    // m_wrap and m_explicit_wrap carry over.

    m_codepage = 0;
    m_binary_file = true;
    m_decoder = nullptr;

    ClearProcessed();
}

void FileLineIter::ClearProcessed()
{
    m_offset = 0;
    m_bytes = nullptr;
    m_count = 0;
    m_available = 0;
    m_width_state.reset();
    m_pending_length = 0;
    m_pending_width = 0;
    m_pending_wrap_length = 0;
    m_pending_wrap_width = 0;
    m_pending_wrap_indent = 0;
    m_consecutive_spaces = m_options.internal_help_mode ? 0 : -1;
    m_hanging_indent = 0;
    m_any_nonspace = false;

#ifdef DEBUG
    m_line_index = 0;
#endif
}

void FileLineIter::SetEncoding(FileDataType type, UINT codepage)
{
    m_binary_file = (type == FileDataType::Binary);
    m_codepage = codepage;
    m_decoder = CreateDecoder(m_codepage);
}

void FileLineIter::SetWrapWidth(uint32 wrap)
{
    m_explicit_wrap = !!wrap;
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
        m_pending_wrap_length = 0;
        m_pending_wrap_width = 0;
        m_pending_wrap_indent = 0;
        m_consecutive_spaces = m_options.internal_help_mode ? 0 : -1;
        m_hanging_indent = 0;
        m_any_nonspace = false;
        return (out_length > 0) ? BreakNewline : Exhausted;
    }

    // Find end of line.
    bool newline = false;
    uint32 can_consume = 0;
    // PERF:  This can end up revisiting the same bytes multiple times if a
    // line wraps before the newline, up to max_line_length.  Could it be
    // worth caching and reusing the can_consume length?
    const size_t remaining = (m_options.max_line_length >= m_pending_length) ? m_options.max_line_length - m_pending_length : 0;
    const size_t max_consume = min<size_t>(m_count, remaining);
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
            {
                can_consume = uint32(m_available);
                break;
            }
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

            if (m_pending_length >= m_options.max_line_length)
            {
                // Line reached max bytes.
                outcome = BreakMax;
                break;
            }

            if (index >= can_consume)
            {
                // Reached end of consumable range.
                if (newline)
                {
                    outcome = BreakNewline;
                    // Newline cancels hanging indent.
                    m_pending_wrap_indent = 0;
                    m_consecutive_spaces = m_options.internal_help_mode ? 0 : -1;
                }
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
                if (c == '\t' && m_options.expand_tabs)
                    clen = m_options.tab_width - (m_pending_width % m_options.tab_width);
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
                if (c == '\t' && m_options.expand_tabs)
                    clen = m_options.tab_width - (m_pending_width % m_options.tab_width);
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

            // Accumulate hanging indent.
            if (!m_any_nonspace)
            {
                switch (c)
                {
                case ' ':
                    ++m_pending_wrap_indent;
                    break;
                case '\t':
                    m_pending_wrap_indent += m_options.tab_width - (m_pending_wrap_indent % m_options.tab_width);
                    break;
                default:
                    m_any_nonspace = true;
                    // If text is not indented at all, then disable the help
                    // mode special hanging indent (which is intended for
                    // indenting descriptions of keys).
                    if (!m_pending_length)
                        m_consecutive_spaces = -1;
                    break;
                }
            }
            else if (m_consecutive_spaces >= 0)
            {
                if (m_pending_length > 24)
                    m_consecutive_spaces = -1;
                else if (c == ' ')
                    ++m_consecutive_spaces;
                else if (m_consecutive_spaces != 2)
                    m_consecutive_spaces = 0;
                else
                {
                    m_pending_wrap_indent = m_pending_length;
                    m_consecutive_spaces = -1;
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
        // Apply hanging indent to next line.
        m_hanging_indent = 0;
        if (m_pending_wrap_indent || outcome >= BreakWrap)
        {
            m_hanging_indent = m_pending_wrap_indent + m_options.hanging_extra;
            m_hanging_indent = min(m_hanging_indent, m_options.max_line_length / 2);
        }
        // Reset pending state.
        m_pending_length = 0;
        m_pending_width = m_pending_wrap_indent;
        m_pending_wrap_length = 0;
        m_pending_wrap_width = m_pending_wrap_indent;
        m_consecutive_spaces = m_options.internal_help_mode ? 0 : -1;
        m_any_nonspace = !!m_hanging_indent;
#ifdef DEBUG
        ++m_line_index;
#endif
    }

    if (resync)
    {
        assert(outcome == BreakWrapSkip);
        m_count = 0;
        m_available = 0;
        return BreakWrapResyncSkip;
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
    m_lines = std::move(other.m_lines);
    m_formatting = std::move(other.m_formatting);
    m_line_numbers = std::move(other.m_line_numbers);

    m_current_line_number = other.m_current_line_number;
    m_processed = other.m_processed;
    m_pending_begin = other.m_pending_begin;
    m_line_iter = std::move(other.m_line_iter);
    m_skip_whitespace = other.m_skip_whitespace;
    m_wrapped_current_line = other.m_wrapped_current_line;

    m_wrap = other.m_wrap;

    m_detected_type = other.m_detected_type;
    m_detected_codepage = other.m_detected_codepage;
    m_codepage = other.m_codepage;
    m_detected_encoding_name = std::move(other.m_detected_encoding_name);
    m_encoding_name = std::move(other.m_encoding_name);
    m_is_unicode_encoding = other.m_is_unicode_encoding;
    m_need_type = other.m_need_type;

    other.Reset();
    return *this;
}

bool FileLineMap::SetWrapWidth(unsigned wrap)
{
    if (m_wrap != wrap)
    {
        m_wrap = wrap;
        ClearProcessed();
        return true;
    }
    return false;
}

void FileLineMap::Reset()
{
    ClearProcessed();
    m_line_iter.Reset();

    m_wrap = 0;

    m_detected_type = FileDataType::Binary;
    m_detected_codepage = 0;
    m_codepage = 0;
    m_detected_encoding_name.Clear();
    m_encoding_name.Clear();
    m_is_unicode_encoding = false;
    m_need_type = true;
}

void FileLineMap::ClearProcessed()
{
    m_lines.clear();
    m_formatting.clear();
    m_line_numbers.clear();

    m_current_line_number = 1;
    m_processed = 0;
    m_pending_begin = 0;
    m_line_iter.ClearProcessed();
    m_skip_whitespace = 0;
    m_wrapped_current_line = false;
}

void FileLineMap::OverrideEncoding(UINT codepage)
{
    StrW tmp;
    const FileDataType type = codepage ? FileDataType::Text : FileDataType::Binary;
    if (!codepage)
        codepage = GetSingleByteOEMCP();
    if (IsCodePageAllowed(codepage) && GetCodePageName(codepage, tmp))
    {
        m_codepage = codepage;
        m_encoding_name.Set(tmp);
        m_line_iter.SetEncoding(type, codepage);
    }
}

void FileLineMap::SetFileType(FileDataType type, UINT codepage, const WCHAR* encoding_name)
{
    m_detected_type = type;
    m_detected_codepage = codepage;
    if (encoding_name)
        m_detected_encoding_name = encoding_name;
    else
        GetCodePageName(codepage, m_detected_encoding_name);

    m_codepage = IsCodePageAllowed(codepage) ? codepage : GetSingleByteOEMCP();
    if (encoding_name)
        m_encoding_name = encoding_name;
    else
        GetCodePageName(m_codepage, m_encoding_name);

    m_line_iter.SetEncoding(type, m_codepage);
    m_need_type = false;
}

void FileLineMap::Next(const BYTE* bytes, size_t available)
{
    if (!m_processed)
    {
        if (m_need_type)
        {
            UINT codepage;
            StrW encoding_name;
            const FileDataType type = AnalyzeFileType(bytes, available, &codepage, &encoding_name);
            SetFileType(type, codepage, encoding_name.Text());
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
        FormattingInfo fmt;
        fmt.m_leading_indent = m_line_iter.HangingIndent();

        const FileLineIter::Outcome outcome = m_line_iter.Next(line_ptr, line_length, line_width);
#ifdef DEBUG_LINE_PARSING
        if (outcome != FileLineIter::Exhausted && outcome != FileLineIter::BreakNewline)
            dbgprintf(L"\u2193 outcome %s",
                      ((outcome == FileLineIter::BreakMax) ? L"BreakMax" :
                       (outcome == FileLineIter::BreakWrap) ? L"BreakWrap" :
                       (outcome == FileLineIter::BreakWrapSkip) ? L"BreakWrapSkip" :
                       (outcome == FileLineIter::BreakWrapResyncSkip) ? L"BreakWrapResyncSkip" : L"??"));
#endif
        if (outcome == FileLineIter::Exhausted)
            break;

        assert(line_length);
        m_lines.emplace_back(m_pending_begin);
        if (m_wrap && !IsBinaryFile())
            m_line_numbers.emplace_back(m_current_line_number);
        if (m_wrap)
            m_formatting.emplace_back(std::move(fmt));
        assert(m_lines.size() == m_line_iter.GetProcessedLineCount() ||
               m_lines.size() == m_line_iter.GetProcessedLineCount() + 1);
#ifdef DEBUG_LINE_PARSING
        dbgprintf(L"finished line %lu; offset %lu (%lx), length %lu, width %lu, leading indent %u", m_lines.size(), m_pending_begin, m_pending_begin, line_length, line_width, fmt.m_leading_indent);
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
        case FileLineIter::BreakWrapResyncSkip:
            m_skip_whitespace = 1;
            if (outcome == FileLineIter::BreakWrapSkip)
            {
                // The code for skipping whitespace is optimized to be outside and
                // immediately preceding the loop, so must directly jump to it.
                goto do_skip_whitespace;
            }
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

FormattingInfo FileLineMap::GetFormattingInfo(size_t index) const
{
    if (m_wrap)
    {
        assert(!index || index < m_formatting.size());
        assert(m_formatting.size() == m_lines.size());
        if (index < m_formatting.size())
            return m_formatting[index];
    }
    return {};
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

size_t FileLineMap::FirstLineNumberInHexRow(FileOffset offset, unsigned hex_width) const
{
    const size_t begin_line = OffsetToIndex(offset) + 1;
    const size_t end_line = OffsetToIndex(offset + hex_width - 1) + 1;
    if (begin_line < end_line)
    {
        for (FileOffset o = offset + 1; o < offset + hex_width; ++o)
        {
            const size_t l = OffsetToIndex(o) + 1;
            if (begin_line < l)
                return l;
        }
    }
    return begin_line;
}

bool FileLineMap::IsUTF8Compatible() const
{
    switch (GetCodePage())
    {
    case CP_USASCII:    // 7-bit US-ASCII (Windows)
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
            if (GetCodePageName(GetCodePage(hex_mode), s_hexmode_encoding_name))
                return s_hexmode_encoding_name.Text();
        }
        if (!m_encoding_name.Empty())
            return m_encoding_name.Text();
    }
    return IsBinaryFile() ? L"Binary" : L"Text";
}

const WCHAR* FileLineMap::GetDetectedEncodingName() const
{
    if (m_detected_codepage && !m_detected_encoding_name.Empty())
        return m_detected_encoding_name.Text();
    return IsBinaryFile() ? L"Binary" : L"Text";
}

#pragma endregion // FileLineMap
#pragma region // ContentCache

ContentCache::ContentCache(const ViewerOptions& options)
: m_options(options)
, m_map(options)
{
    SetSize(0);
}

ContentCache& ContentCache::operator=(ContentCache&& other)
{
    // m_options can't be updated, and it doesn't need to be.
    m_name = std::move(other.m_name);
    m_file = std::move(other.m_file);
    m_size = other.m_size;
    m_hex_size_width = other.m_hex_size_width;
    m_file_size_width = other.m_file_size_width;
    m_line_count_width = other.m_line_count_width;
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

void ContentCache::SetSize(FileOffset size)
{
    m_size = size;
    m_hex_size_width = c_min_hexofs_width;
    m_file_size_width = c_min_num_width;
    m_line_count_width = 0;

    if (size)
    {
        StrW tmp;
        tmp.Printf(L"%lx", size);
        m_hex_size_width = max(m_hex_size_width, tmp.Length());
        m_file_size_width = max(m_file_size_width, tmp.Length());
    }
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
    SetSize(strlen(text));
    m_eof = true;
    return true;
}

void ContentCache::SetEncoding(UINT codepage)
{
    ClearProcessed();
    m_map.OverrideEncoding(codepage);
}

bool ContentCache::Open(const WCHAR* name, Error& e)
{
    Close();

    if (!EnsureDataBuffer(e))
        return false;

    m_redirected = (!wcscmp(name, L"<stdin>") && s_piped_stdin);

    if (!m_redirected)
    {
        // Open for write as well, in case the file is edited in hex mode.
        m_file = CreateFileW(name, GENERIC_READ, FILE_SHARE_READ|FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, 0);
        if (m_file == INVALID_HANDLE_VALUE)
        {
            e.Sys();
            return false;
        }
        m_name.Set(name);

        LARGE_INTEGER liSize;
        if (GetFileSizeEx(m_file, &liSize))
            SetSize(liSize.QuadPart);

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
        FileOffset size = 0;
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
                SetSize(size);
                return !e.Test();
            }
            chunk.Wrote(bytes_read);
            size += bytes_read;
        }
    }
}

void ContentCache::Close()
{
    m_name.Clear();
    m_file.Close();

    SetSize(0);
    m_chunks.swap(PipeChunks {});
    m_text = nullptr;
    m_redirected = false;
    m_eof = false;

    ClearProcessed();

    m_data_offset = 0;
    m_data_length = 0;
    m_data_slop = 0;
}

void ContentCache::ClearProcessed()
{
    m_map.ClearProcessed();
    m_completed = false;
    if (!m_text && !m_redirected)
        m_eof = false;
}

void ContentCache::SetWrapWidth(unsigned wrap)
{
    if (m_map.SetWrapWidth(wrap))
    {
        assert(!m_map.Count());
        m_completed = false;
    }
}

unsigned ContentCache::CalcMarginWidth(bool hex_mode)
{
    StrW s;
    unsigned margin = 0;

    enum WhichNumberType { None, LineNo, LineOffset, HexOffset };
    WhichNumberType order[3];
    unsigned count = 0;

    if (hex_mode)
        order[count++] = HexOffset;
    if (m_options.show_line_numbers)
        order[count++] = LineNo;
    if (!hex_mode && m_options.show_file_offsets)
        order[count++] = LineOffset;

    for (unsigned i = 0; i < count; ++i)
    {
        switch (order[i])
        {
        case HexOffset:
            margin += m_hex_size_width + c_margin_padding;
            break;
        case LineNo:
            if (!m_line_count_width)
            {
                s.Clear();
                s.Printf(L"%lu", CountFriendlyLines());
                m_line_count_width = max(c_min_num_width, s.Length());
            }
            margin += m_line_count_width + hex_mode/*c_div_char*/ + c_margin_padding;
            break;
        case LineOffset:
#ifdef DEBUG
            s.Clear();
            s.Printf(L"%lx", Processed());
            m_file_size_width = max(c_min_num_width, s.Length());
#endif
            margin += m_file_size_width + c_margin_padding;
            break;
        }
    }

    return margin;
}

unsigned ContentCache::FormatLineData(const size_t line, bool middle, unsigned left_offset, StrW& s, const unsigned max_width, Error& e, const WCHAR* const color, const FoundOffset* const found_line, unsigned max_len)
{
    if (!EnsureFileData(line, e))
        return 0;
    if (line >= m_map.Count())
        return 0;

    assert(!found_line || !found_line->Empty());
    const FileOffset offset = GetOffset(line);

    const WCHAR* const norm = GetColor(ColorElement::Content);

    // Margin (line number and offset).

    if (m_options.show_line_numbers || m_options.show_file_offsets)
    {
#ifdef DEBUG
        const unsigned begin_index = s.Length();
        const unsigned margin_width = CalcMarginWidth(false/*hex_mode*/);
#endif
        s.AppendColorOverlay(norm, GetColor(ColorElement::LineNumber));
        if (m_options.show_line_numbers)
        {
            const size_t prev_num = (line > 0) ? GetLineNunber(line - 1) : 0;
            const size_t num = GetLineNunber(line);
            if (num > prev_num)
                s.Printf(L"%*lu%s", m_line_count_width, num, c_div_char);
            else
                s.Printf(L"%*s%s", m_line_count_width, L"", c_div_char);
        }
        if (m_options.show_file_offsets)
        {
            if (m_options.show_line_numbers)
                s.Append(L" ");
            s.Printf(L"%0*lx%s", m_file_size_width, offset, c_div_char);
        }
        if (middle)
            s.Append(L">", 1);
        s.AppendColor(norm);
        if (!middle)
            s.Append(L" ", 1);
#ifdef DEBUG
        assert(cell_count(s.Text() + begin_index) == margin_width);
#endif
    }

    // Line content.

    assert(offset >= m_data_offset);
    const BYTE* ptr = m_data + (offset - m_data_offset);
    const unsigned len = min(max_len, GetLength(line));
    assert(ptr + len <= m_data + m_data_length);

    StrW tmp;
    m_map.GetLineText(ptr, len, tmp);

    unsigned visible_len = 0;
    unsigned total_cells = 0;
    int32 truncate_cells = -1;
    int32 truncate_len = -1;

    bool need_found_highlight = false;
    bool highlighting_found_text = false;

    s.AppendColor(color ? color : norm);

    const auto fmt = m_map.GetFormattingInfo(line);
    if (fmt.m_leading_indent)
    {
        visible_len = min<uint32>(left_offset, fmt.m_leading_indent);
        total_cells = visible_len;
        s.AppendSpaces(fmt.m_leading_indent - visible_len);
    }

    // PREF:  Optimize color runs.  "{color}a{norm}{color}b{norm}" can be more
    // efficiently expressed as "{color}ab{norm}".

    auto append_text = [&](const WCHAR* text, unsigned text_len, unsigned cells=1)
    {
        assert(implies(need_found_highlight && found_line, found_line->offset >= offset));
        if (found_line && offset <= found_line->offset && found_line->offset < offset + len && text == tmp.Text() + found_line->offset - offset)
            need_found_highlight = true;
        if (need_found_highlight && text >= tmp.Text() + found_line->offset + found_line->len - offset)
            need_found_highlight = false;

        if (visible_len >= left_offset)
        {
            if (left_offset)
            {
                left_offset = 0;
                visible_len = 0;
            }
            if (need_found_highlight && !highlighting_found_text)
            {
                s.AppendColor(GetColor(ColorElement::SearchFound));
                highlighting_found_text = true;
            }
            else if (highlighting_found_text && !need_found_highlight)
            {
                s.AppendColor(color ? color : norm);
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

                if (c == '\r' && !m_options.show_line_endings && !m_map.IsBinaryFile() && inner_iter.more() && *inner_iter.get_pointer() == '\n')
                {
                    // Omit trailing \r\n at end of line in a text file.
                }
                else if (c == '\n' && !m_options.show_line_endings && !m_map.IsBinaryFile() && !inner_iter.more())
                {
                    // Omit trailing \n at end of line in a text file.
                }
                else if (c == '\t' && m_options.expand_tabs)
                {
                    unsigned spaces = m_options.tab_width - (total_cells % m_options.tab_width);
                    const bool something_fits = (visible_len + spaces > left_offset);
                    const bool apply_color = (m_options.show_whitespace && something_fits && !color);
                    if (apply_color)
                        s.AppendColorOverlay(norm, GetColor(ColorElement::Whitespace));
                    while (spaces--)
                    {
                        if (m_options.show_whitespace)
                            append_text(spaces ? L"-" : L">", 1);
                        else
                            append_text(L" ", 1);
                        if (!left_offset && visible_len >= max_width)
                            break;
                    }
                    if (apply_color)
                        s.AppendColor(norm);
                }
                else if (c >= 0 && c < ' ')
                {
                    const bool whitespace = (c == '\t' || c == '\r' || c == '\n');
                    const WCHAR* ctrl_color = nullptr;
                    if (!color)
                    {
                        ColorElement celm = whitespace ? ColorElement::Whitespace : ColorElement::CtrlCode;
                        if (c == '\r')
                        {
                            if (inner_iter.next() != '\n')
                                celm = ColorElement::CtrlCode;
                            inner_iter.unnext();
                        }
                        ctrl_color = GetColor(celm);
                    }
                    if (m_options.ctrl_mode == CtrlMode::EXPAND)
                    {
                        const bool something_fits = (visible_len + 2 > left_offset);
                        const bool apply_color = (ctrl_color && something_fits);
                        if (apply_color)
                            s.AppendColorOverlay(norm, ctrl_color);
                        append_text(L"^", 1);
                        if (left_offset || visible_len < max_width)
                        {
                            WCHAR text[2] = { WCHAR('@' + c) };
                            append_text(text, 1);
                        }
                        if (apply_color)
                            s.AppendColor(norm);
                    }
#ifdef INCLUDE_CTRLMODE_PERIOD
                    else if (m_options.ctrl_mode == CtrlMode::PERIOD)
                    {
                        assert(left_offset || visible_len < max_width);
                        const bool apply_color = (ctrl_color && !left_offset);
                        if (apply_color)
                            s.AppendColor(ctrl_color);
                        append_text(m_options.filter_byte_char);
                        if (apply_color)
                            s.AppendColor(norm);
                    }
#endif
#ifdef INCLUDE_CTRLMODE_SPACE
                    else if (m_options.ctrl_mode == CtrlMode::SPACE && !whitespace)
                    {
                        assert(left_offset || visible_len < max_width);
                        append_text(L" ", 1);
                    }
#endif
                    else
                    {
                        assert(m_options.ctrl_mode == CtrlMode::OEM437);
                        assert(left_offset || visible_len < max_width);
                        const bool apply_color = (ctrl_color && whitespace);
                        if (apply_color)
                            s.AppendColorOverlay(norm, ctrl_color);
                        append_text(c_oem437[c], 1);
                        if (apply_color)
                            s.AppendColor(norm);
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
                                s.AppendColorOverlay(norm, GetColor(c == '\t' ? ColorElement::Whitespace : ColorElement::CtrlCode));
                            // FUTURE:  Maybe '^' for ctrl codes and '?' for
                            // the 0xfffd codepoint?
                            append_text(L"?", 1);
                            if (!color)
                                s.AppendColor(norm);
                        }
                        else
                        {
                            ++visible_len;
                        }
                    }
                    else
                    {
                        bool white = false;
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
                            if (c == ' ' && m_options.show_whitespace)
                            {
                                white = true;
                                if (!color)
                                    s.AppendColorOverlay(norm, GetColor(ColorElement::Whitespace));
                                append_text(L"\u00b7", 1, 1);            // ·
                            }
                        }
                        if (!white)
                            append_text(inner_iter.character_pointer(), inner_iter.character_length(), clen);
                        if (!color && white)
                            s.AppendColor(norm);
                    }
                }
            }

            walk = inner_iter.get_pointer();
        }
    }

LOut:
    if (m_options.show_debug_info && visible_len < max_width)
    {
        append_text(c_eol_marker, -1);
        s.AppendColor(color ? color : norm);
    }
    else if (highlighting_found_text)
    {
        s.AppendColor(color ? color : norm);
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

bool ContentCache::FormatHexData(FileOffset offset, bool middle, unsigned row, unsigned hex_bytes, StrW& s, Error& e, const WCHAR* marked_color, const FoundOffset* found_line)
{
    offset += row * hex_bytes;

    if (!EnsureHexData(offset, hex_bytes, e))
        return false;

    StrW _norm;
    StrW _hilite;
    _norm.Set(GetColor(ColorElement::Content));
    _hilite.Set(_norm);
    if (offset % 0x400 == 0)
    {
        if (!_hilite.Empty())
            _hilite.Append(L";");
        _hilite.Append(L"1");
    }
    const WCHAR* const norm = _norm.Text();
    const WCHAR* const hilite = _hilite.Text();

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
    StrW tmp2;
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
            {
                tmp.AppendColorOverlay(norm, GetColor(ColorElement::FilteredByte));
                tmp.Append(m_options.filter_byte_char);
            }
        }
    }

    bool highlighting_found_text = false;

#ifdef DEBUG
    const unsigned begin_index = s.Length();
#endif

    // Format the offset.
    s.AppendColor((offset % 0x400 == 0) ? hilite : norm);
    s.Printf(L"%0*.*x", m_hex_size_width, m_hex_size_width, offset);
    if (middle)
    {
        s.AppendColorOverlay(norm, GetColor(ColorElement::LineNumber));
        s.Append(L">", 1);
        s.AppendColor(norm);
        s.Append(L" ", 1);
    }
    else
    {
        if (offset % 0x400 == 0)
            s.AppendColor(norm);
        s.Append(L"  ", 2);
    }

    // Format line number.
    if (m_options.show_line_numbers)
    {
        const size_t prev_line = (offset < hex_bytes) ? 0 : m_map.FirstLineNumberInHexRow(offset - hex_bytes, hex_bytes);
        const size_t this_line = m_map.FirstLineNumberInHexRow(offset, hex_bytes);
        tmp2.Clear();
        if (prev_line < this_line)
            tmp2.Printf(L"%zu%s", this_line, c_div_char);
        s.AppendColorOverlay(norm, GetColor(ColorElement::LineNumber));
        s.Printf(L"%*s", m_line_count_width + 1, tmp2.Text());
        s.AppendColor(norm);
        s.Append(L"  ", 2);
    }

#ifdef DEBUG
    assert(cell_count(s.Text() + begin_index) == CalcMarginWidth(true/*hex_mode*/));
#endif

    // Format the hex bytes.
    if (marked_color)
        s.AppendColor(marked_color);
    for (unsigned ii = 0; ii < hex_bytes; ++ii)
    {
        if (highlighting_found_text && offset + ii == found_line->offset + found_line->len)
        {
            assert(marked_color);
            highlighting_found_text = false;
            s.AppendColor(marked_color);
        }
        if (ii)
        {
            if (ii % (1 << m_options.hex_grouping) == 0)
                s.Append(L"  ", ((ii % 8) == 0) ? 2 : 1);
        }
        if (marked_color && found_line && found_line->len && offset + ii == found_line->offset)
        {
            highlighting_found_text = true;
            s.AppendColor(GetColor(ColorElement::SearchFound));
        }
        if (ii < len)
        {
            BYTE value = ptr[ii];
            bool colored = false;
            ColorElement byte_color;
            if (IsByteDirty(offset + ii, value, byte_color))
            {
                colored = true;
                s.AppendColorOverlay(norm, GetColor(byte_color));
            }
            else
            {
                colored = (!highlighting_found_text && ptr[ii] == '\n' && !marked_color);
                if (colored)
                    s.AppendColorOverlay(norm, GetColor(ColorElement::CtrlCode));
            }
            s.Printf(L"%02X", value);
            if (colored)
            {
                if (highlighting_found_text)
                    s.AppendColor(GetColor(ColorElement::SearchFound));
                else if (marked_color)
                    s.AppendColor(marked_color);
                else
                    s.AppendColor(norm);
            }
        }
        else
        {
            s.Append(L"  ", 2);
        }
    }
    if (marked_color)
        s.AppendColor(norm);

    // Format the text characters.
    StrW old_color;
    s.Printf(L"  ", 2);
    s.AppendColorOverlay(norm, GetColor(ColorElement::Divider));
    // s.Append(L"\u2502", 1);
    s.Append(L"*", 1);
    old_color = (marked_color ? marked_color : norm);
    s.AppendColor(old_color.Text());
    highlighting_found_text = false;
    for (unsigned ii = 0; ii < len; ++ii)
    {
        BYTE c = ptr[ii];
        bool edited = false;
        ColorElement byte_color;
        const WCHAR* new_color = marked_color ? marked_color : norm;

        if (IsByteDirty(offset + ii, c, byte_color))
        {
            edited = true;
            new_color = MakeOverlayColor(norm, GetColor(byte_color));
            tmp2.SetFromCodepage(m_map.GetCodePage(true), reinterpret_cast<const char*>(&c), 1);
            tmp.SetAt(tmp.Text() + ii, *tmp2.Text());
        }
        else if (marked_color)
        {
            assert(implies(!found_line, !highlighting_found_text));
            if (found_line)
            {
                if (found_line->len && offset + ii == found_line->offset)
                    highlighting_found_text = true;
                else if (highlighting_found_text && offset + ii == found_line->offset + found_line->len)
                    highlighting_found_text = false;
                if (highlighting_found_text)
                    new_color = GetColor(ColorElement::SearchFound);
            }
        }

        if (c > 0 && c < ' ')
        {
            if (m_options.ascii_filter)
            {
                goto filter_byte;
            }
            else
            {
                if (!highlighting_found_text && c == '\n' && !edited && !marked_color)
                    new_color = MakeOverlayColor(norm, GetColor(ColorElement::CtrlCode));
                if (!old_color.Equal(new_color))
                    s.AppendColor(new_color);
                s.Append(c_oem437[c], 1);
            }
        }
        else if (!c || wcwidth(tmp.Text()[ii]) != 1 || (m_options.ascii_filter && c > 0x7f))
        {
filter_byte:
            if (!edited && !marked_color)
                new_color = MakeOverlayColor(norm, GetColor(ColorElement::FilteredByte));
            if (!old_color.Equal(new_color))
                s.AppendColor(new_color);
            s.Append(m_options.filter_byte_char);
        }
        else
        {
            if (!old_color.Equal(new_color))
                s.AppendColor(new_color);
            s.Append(tmp.Text() + ii, 1);
        }

        old_color = new_color;
    }
    s.AppendColorOverlay(norm, GetColor(ColorElement::Divider));
    // s.Append(L"\u2502", 1);
    s.Append(L"*", 1);
    s.AppendColor(norm);

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
            m_line_count_width = 0;

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
                SetSize(m_map.Processed());

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
#ifdef DEBUG
            size_t total_bytes = 0;
            for (size_t i = 0; i < m_map.Count(); ++i)
            {
                const size_t len = GetLength(i);
                total_bytes += len;
            }
            assert(m_map.Processed() == m_size);
            assert(total_bytes == m_size);
#endif
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
        // IMPORTANT:  Must have processed through the _next_ line as well to
        // get an accurate length!
        assert(m_map.Processed() == m_size || line + 1 < Count());
        const FileOffset offset = GetOffset(line);
        const FileOffset next = (line + 1 < Count()) ? GetOffset(line + 1) : m_map.Processed();
        assert(next - offset <= 1024);
        return unsigned(next - offset);
    }
    return 0;
}

bool ContentCache::Find(bool next, const std::shared_ptr<Searcher>& searcher, unsigned max_width, FoundOffset& found_line, unsigned& left_offset, Error& e, bool first)
{
    StrW tmp;
    const unsigned needle_delta = searcher->GetNeedleDelta();

    if (found_line.Empty())
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
                found_line.Found(0, 0);
            else
                found_line.Found(m_size, 0);
        }
        else
        {
            const size_t index = m_map.OffsetToIndex(found_line.offset);
            const unsigned offset = unsigned(found_line.offset - m_map.GetOffset(index));
            found_line.Found(offset, 0);
        }
    }

    assert(!found_line.Empty());
    size_t index = m_map.OffsetToIndex(found_line.offset);
    while (true)
    {
        if (IsSignaled())
        {
            found_line.Found(GetOffset(index), 0);
            e.Set(E_ABORT);
            return false;
        }

        if (next)
        {
            // IMPORTANT:  Must process through the _next_ line to get an
            // accurate length for the current line.
            const unsigned c_resolve_pending_wrap = 1;
            if (index + !first + c_resolve_pending_wrap >= Count())
            {
                ProcessThrough(index + !first + c_resolve_pending_wrap, e, true/*cancelable*/);
                if (e.Test())
                {
                    if (e.Code() == E_ABORT)
                    {
                        found_line.Found(GetOffset(index), 0);
                        left_offset = 0;
                    }
                    return false;
                }
            }
            if (!first)
            {
                if (index + 1 >= Count())
                    return false;
                ++index;
            }
        }
        else
        {
            // Going in reverse doesn't need to use ProcessThrough().
            if (!first)
            {
                if (!index || index > Count())
                    return false;
                --index;
            }
        }

        if (!EnsureFileData(index, e))
            return false;

        const FileOffset offset = GetOffset(index);
        assert(offset >= m_data_offset);
        const BYTE* const ptr = m_data + (offset - m_data_offset);
        const unsigned real_len = GetLength(index);
        unsigned len = real_len;
        assert(len);
        assert(ptr + len <= m_data + m_data_length);

        if (needle_delta)
        {
            // IMPORTANT:  This is how Find() handles searching across forced
            // line breaks -- it relies on the data buffer always having at
            // least c_data_buffer_slop bytes more than the current line
            // (except at the end of the file), and on max_needle being less
            // than or equal to c_data_buffer_slop.
            if (len && ptr[len - 1] != '\n')
            {
                // Extend len by needle_len - 1, not to extend the end of the
                // buffer, and also not to extend past a newline.  This only
                // needs to include enough extra to handle when the needle is
                // split across a max line length break; anything past that
                // will be caught when searching the next line.
                unsigned extend = needle_delta - 1;
                while (extend-- && ptr + len < m_data + m_data_length && ptr[len - 1] != '\n')
                    ++len;
            }
        }

        // FUTURE:  Optional regex search?
        // PERF:  Boyer-Moore search?
        if (searcher->Match(m_map, ptr, len, e))
        {
            if (e.Test())
                return false;

            // Check found offset.
            const unsigned index_in_line = searcher->GetMatchStart();
            const unsigned needle_len = searcher->GetMatchLength();
            if (index_in_line >= real_len)
                return false; // Was actually found on the _next_ line.
            found_line.Found(GetOffset(index) + index_in_line, needle_len);
            // Calculate horizontal scroll offset.
            tmp.Clear();
            FormatLineData(index, false, 0, tmp, -1, e, nullptr, nullptr, index_in_line);
            const unsigned prefix_cells = cell_count(tmp.Text());
            tmp.Clear();
            FormatLineData(index, false, 0, tmp, -1, e, nullptr, nullptr, index_in_line + needle_len);
            const unsigned prefixneedle_cells = cell_count(tmp.Text());
            const unsigned needle_cells = prefixneedle_cells - prefix_cells;
            if (prefix_cells + needle_cells + c_find_horiz_scroll_threshold <= max_width)
            {
                left_offset = 0;
            }
            else
            {
                // Center the found text horizontally.
                const int center_offset = (max_width - needle_cells) / 2;
                left_offset = int(prefix_cells) - center_offset;
                // Nudge the left offset so it doesn't scroll past the
                // wrap width or line length.
                tmp.Clear();
                const unsigned line_cells = FormatLineData(index, false, 0, tmp, -1, e);
                if (line_cells)
                {
                    if (m_map.GetWrapWidth())
                        left_offset = min<int>(left_offset, int(line_cells) - min(max_width, m_map.GetWrapWidth()));
                    else
                        left_offset = min<int>(left_offset, int(line_cells) + c_find_horiz_scroll_threshold - max_width);
                }
                left_offset = max<int>(0, left_offset);
            }
            return true;
        }

        first = false;
    }
}

bool ContentCache::Find(bool next, const std::shared_ptr<Searcher>& searcher, unsigned hex_width, FoundOffset& found_line, Error& e, bool first)
{
    StrW tmp;
    const unsigned needle_delta = searcher->GetNeedleDelta();

    if (found_line.Empty())
    {
        if (next)
            found_line.Found(FileOffset(-1), 0);
        else
            found_line.Found(GetFileSize(), 0);
    }

    assert(!found_line.Empty());
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

        if (needle_delta)
        {
            // IMPORTANT:  This is how Find() handles searching across forced
            // line breaks -- it relies on the data buffer always having at
            // least c_data_buffer_slop bytes more than the current line
            // (except at the end of the file), and on max_needle being less
            // than or equal to c_data_buffer_slop.
            {
                // Extend len by needle_len - 1, not to extend the end of the
                // buffer.  This only needs to include enough extra to handle
                // when the needle is split across a max line length break;
                // anything past that will be caught when searching the next
                // line.
                unsigned extend = needle_delta - 1;
                while (extend-- && ptr + len < m_data + m_data_length)
                    ++len;
            }
        }

// TODO:  Non-convertible characters will make conversion go haywire.
        if (searcher->Match(m_map, ptr, len, e))
        {
            if (e.Test())
                return false;

            found_line.Found(offset + searcher->GetMatchStart(), searcher->GetMatchLength());
            return true;
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

void ContentCache::SetByte(FileOffset offset, BYTE value, bool high_nybble)
{
    const FileOffset block_offset = offset & ~(PatchBlock::c_size - 1);
    const unsigned index = offset & (PatchBlock::c_size - 1);

    Error e;
    if (!EnsureHexData(block_offset, PatchBlock::c_size, e))
        return;

    auto f = m_patch_blocks.find(block_offset);
    if (f == m_patch_blocks.end())
    {
        m_patch_blocks.emplace(block_offset, block_offset);
        f = m_patch_blocks.find(block_offset);
    }

    value &= 0x0f;
    if (high_nybble)
        value <<= 4;

    BYTE b;
    ColorElement color;
    const bool dirty = IsByteDirty(offset, b, color);
    if (!dirty)
    {
        const BYTE* ptr = m_data + (block_offset - m_data_offset);
        b = ptr[index];
    }

    value |= b & (high_nybble ? 0x0f : 0xf0);

    f->second.SetByte(offset, value, dirty ? nullptr : &b);
}

bool ContentCache::RevertByte(FileOffset offset)
{
    const FileOffset block_offset = offset & ~(PatchBlock::c_size - 1);
    auto f = m_patch_blocks.find(block_offset);
    if (f == m_patch_blocks.end())
        return false;

    if (!f->second.IsSet(offset))
        return false;

    f->second.RevertByte(offset);
    if (!f->second.IsDirty())
        m_patch_blocks.erase(block_offset);
    return true;
}

bool ContentCache::SaveBytes(Error& e)
{
    assert(IsOpen());
    assert(!IsPipe());
    if (!IsOpen() || IsPipe() || !IsDirty())
        return false;

    SHFile h = CreateFileW(m_name.Text(), GENERIC_READ|GENERIC_WRITE, FILE_SHARE_READ|FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, 0);
    if (h == INVALID_HANDLE_VALUE)
    {
        e.Sys();
        e.Set(L"Unable to open file for writing.");
        return false;
    }

    bool ok = true;
    for (auto& p : m_patch_blocks)
    {
        if (!p.second.Save(h, false/*original*/, e))
        {
            ok = false;
            break;
        }

        auto saved = m_patch_blocks_saved.find(p.first);
        if (saved != m_patch_blocks_saved.end())
            saved->second.MergeFrom(p.second);
        else
            m_patch_blocks_saved.emplace(p.first, p.second);
    }

    if (ok)
    {
        DiscardBytes();
        ClearProcessed();  // Make sure to reread the file.
    }

    return ok;
}

void ContentCache::UndoSave(Error& e)
{
    assert(!IsDirty());

    if (!IsOpen() || IsDirty() || m_patch_blocks_saved.empty())
        return;

    SHFile h = CreateFileW(m_name.Text(), GENERIC_READ|GENERIC_WRITE, FILE_SHARE_READ|FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, 0);
    if (h == INVALID_HANDLE_VALUE)
    {
        e.Sys();
        e.Set(L"Unable to open file for writing.");
        return;
    }

    for (auto& p : m_patch_blocks_saved)
    {
        if (!p.second.Save(h, true/*original*/, e))
            return;
    }

    m_patch_blocks_saved.clear();
    ClearProcessed();  // Make sure to reread the file.
}

bool ContentCache::IsByteDirty(FileOffset offset, BYTE& value, ColorElement& color) const
{
    const FileOffset block_offset = offset & ~(PatchBlock::c_size - 1);
    auto edited = m_patch_blocks.find(block_offset);
    if (edited != m_patch_blocks.end() && edited->second.IsSet(offset))
    {
        value = edited->second.GetByte(offset);
        color = ColorElement::EditedByte;
        return true;
    }
    auto saved = m_patch_blocks_saved.find(block_offset);
    if (saved != m_patch_blocks_saved.end() && saved->second.IsSet(offset))
    {
        value = saved->second.GetByte(offset);
        color = ColorElement::SavedByte;
        return true;
    }
    return false;
}

static bool NextEditedByteRow(const std::map<FileOffset, PatchBlock>& patch_blocks, FileOffset here, FileOffset& there, unsigned hex_width, bool next)
{
    auto f = patch_blocks.lower_bound(here);
    if (next)
    {
        while (f != patch_blocks.end())
        {
            const FileOffset rowofs = f->first & ~(FileOffset(hex_width) - 1);
            if (here < rowofs)
            {
                there = f->first;
                for (unsigned index = 0; index < f->second.c_size; ++index)
                {
                    if (f->second.IsSet(there + index))
                    {
                        there += index;
                        break;
                    }
                }
                return true;
            }
            ++f;
        }
    }
    else
    {
        if (f == patch_blocks.end())
            --f;

        while (true)
        {
            const FileOffset rowofs = f->first & ~(FileOffset(hex_width) - 1);
            if (here > rowofs)
            {
                there = f->first;
                for (unsigned index = f->second.c_size; index--;)
                {
                    if (f->second.IsSet(there + index))
                    {
                        there += index;
                        break;
                    }
                }
                return true;
            }
            if (f == patch_blocks.begin())
                break;
            --f;
        }
    }

    return false;
}

bool ContentCache::NextEditedByteRow(FileOffset here, FileOffset& there, unsigned hex_width, bool next) const
{
    here &= ~(FileOffset(hex_width) - 1);

    FileOffset there1 = -1;
    FileOffset there2 = -1;
    const bool found1 = !m_patch_blocks.empty() && ::NextEditedByteRow(m_patch_blocks, here, there1, hex_width, next);
    const bool found2 = !m_patch_blocks_saved.empty() && ::NextEditedByteRow(m_patch_blocks_saved, here, there2, hex_width, next);

    if (found1 && found2)
    {
        if (next)
            there = min(there1, there2);
        else
            there = max(there1, there2);
    }
    else if (found1)
        there = there1;
    else if (found2)
        there = there2;
    else
        return false;

    return true;
}

#pragma endregion // ContentCache

