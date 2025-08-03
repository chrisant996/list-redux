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

// static const WCHAR c_eol_marker[] = L"\x1b[0;33;48;2;80;0;80m\u22a6\x1b[m";
static const WCHAR c_eol_marker[] = L"\x1b[36m\u22a6\x1b[m";

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
#pragma region // Utf8Accumulator

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

        // Detect an overlong encoding (0xE0 followed by less than 0xA0, or
        // 0xF0 followed by less than 0x90).
        if (m_ax == 0)
        {
            if (m_expected == 3 && c < 0xA0 && m_length == 1)
                goto InvalidPrecedingData;
            if (m_expected == 4 && c < 0x90 && m_length == 1)
                goto InvalidPrecedingData;
        }

        // 0xC0 followed by 0x80 is an overlong encoding for U+0000, which is
        // accepted so that U+0000 can be encoded without using any NUL bytes.
        // But no other use of 0xC0 is allowed.
        if (m_ax == 0 && m_expected == 2 && m_length == 1 && c != 0x80)
            goto InvalidPrecedingData;

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
#pragma region // FileLineMap

FileLineMap::FileLineMap(const ViewerOptions& options)
: m_options(options)
{
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
    m_lines.clear();
    m_codepage = 0;
    m_encoding_name.Clear();
    m_processed = 0;
    m_binary_file = true;
    m_continue_last_line = false;
    m_last_length = 0;
    m_last_visible_width = 0;

#ifdef DEBUG
    m_skipped_empty_line = false;
#endif
}

void FileLineMap::Next(const BYTE* bytes, const size_t available)
{
#ifdef DEBUG_LINE_PARSING
    dbgprintf(L"Next( %p, %lu )", bytes, available);
#endif

    if (!m_processed)
        m_binary_file = (AnalyzeFileType(bytes, available, &m_codepage, &m_encoding_name) == FileDataType::Binary);

    const size_t count = std::min<size_t>(available, c_data_buffer_main);
    assert(count <= available);
#ifdef DEBUG_LINE_PARSING
    dbgprintf(L"parse %lu of %lu", count, available);
#endif

    DWORD offset_begin = 0;
    unsigned line_length;
    unsigned visible_width;
    if (m_continue_last_line)
    {
        line_length = m_last_length;
        visible_width = m_last_visible_width;
    }
    else
    {
        line_length = 0;
        visible_width = 0;
    }

    const unsigned wrap_width = m_wrap ? m_wrap : m_options.max_line_length;
    DWORD ii = 0;
    while (true)
    {
        BYTE c;
        bool newline;
        if (ii < available)
        {
            c = *bytes;
            newline = (c == '\n');
        }
        else
        {
            c = 0;
            newline = false;
        }

        const bool delay_wrap = (c == '\r' && !m_binary_file && ii + 1 < available && bytes[ii + 1] == '\n');

// TODO:  Smart wrapping after whitespaces or punctuation (a more complex
// variant of delay_wrap, which will naturally eliminate the use of peek
// ahead).  This will likely need different implementations for UTF8 files
// versus other binary or text files.

// TODO:  For UTF8 text files, when wrapping is enabled, then read ahead and
// use Utf8Accumulator to decode the UTF8.  Use an incremental-enabled version
// of calculating wcswidths.  Read until the visible width exceeds the wrap
// width or 2048 bytes, whichever comes first -- being careful not to sever
// any UTF8 byte sequence, nor any run of codepoints that compose a grapheme.
// Then wrap that string into multiple line units.

// TODO:  This is not the right way to measure cell width for wrapping!  It
// needs to parse bytes and respect encoding and measure width of Unicode or
// DBCS codepoints.
        unsigned clen;
        if (c == '\t' && m_options.ctrl_mode != CtrlMode::EXPAND && m_options.tab_mode != TabMode::RAW)
            clen = c_tab_width - (visible_width % c_tab_width);
        else if (c > 0 && c < ' ')
            clen = (m_options.ctrl_mode == CtrlMode::EXPAND) ? 2 : 1;
        else
        {
            // TODO:  This presumes single cell width, which isn't
            // accurate in all OEM codepages (Chinese, for example).
            clen = 1;
        }

        assert(ii <= available);
        const bool end_line = (newline ||                   // Newline.
              line_length >= m_options.max_line_length ||   // Line exceeds max bytes.
              (wrap_width > clen && visible_width + clen > wrap_width)); // Wrapping width reached.
        const bool reached_end = (ii == available || (ii >= count && end_line));
#ifdef DEBUG_LINE_PARSING
        dbgprintf(L"    ii %u, clen %u, count %lu, end_line %u, line_length %u, visible_width %u, offset %lu", ii, clen, count, end_line, line_length, visible_width, m_processed + ii);
#endif
        if (reached_end || (!delay_wrap && end_line))
        {
            const DWORD offset_end = ii + !!newline;
            line_length += !!newline;

            if (m_continue_last_line)
            {
                m_continue_last_line = false;
                m_last_length = 0;
                m_last_visible_width = 0;
            }

            // Add the start of the line that just finished being processed.
            assert(!m_skipped_empty_line);
            if (line_length)
            {
                m_lines.emplace_back(m_processed + offset_begin);
#ifdef DEBUG_LINE_PARSING
                dbgprintf(L"finished line %lu; offset %lu, length %lu", m_lines.size(), m_processed + offset_begin, ii + !!newline - offset_begin);
                dbgprintf(L"        reached_end %u, delay_wrap %u, end_line %u", reached_end, delay_wrap, end_line);
                if (end_line)
                    dbgprintf(L"        newline %u, reached s_max_line_length %u, reached wrap width %u", newline, line_length >= s_max_line_length, wrap_width > clen && visible_width + clen > wrap_width);
                if (wrap_width > clen && visible_width + clen > wrap_width)
                    dbgprintf(L"        wrap_width %u, clen %u, visible_width %u", wrap_width, clen, visible_width);
#endif
            }
#ifdef DEBUG
            else
            {
                m_skipped_empty_line = true;
            }
#endif

            if (reached_end)
            {
                if (end_line)
                {
#ifdef DEBUG_LINE_PARSING
                    dbgprintf(L"reached end of line and end of parse chunk");
#endif
                    m_continue_last_line = false;
                    m_last_length = 0;
                    m_last_visible_width = 0;
                    if (newline)
                        ++ii;
                    // Note:  bytes, line_length, and visible_width are not
                    // used past here, so they don't need to be updated.
                }
                else
                {
#ifdef DEBUG_LINE_PARSING
                    dbgprintf(L"reached end of parse chunk; continue line the next time");
#endif
                    m_continue_last_line = true;
                    m_last_length = line_length;
                    m_last_visible_width = visible_width;
                }
                break;
            }

            offset_begin = ii + !!newline;
            line_length = 0 - !!newline;
            visible_width = 0;
        }

        ++line_length;
        if (!newline)
            visible_width += clen;

        ++ii;
        ++bytes;
    }

    assert(ii <= available);
    m_processed += ii;
}

FileOffset FileLineMap::GetOffset(size_t index) const
{
    assert(!index || index < m_lines.size());
    assert(!m_lines.size() || !m_lines[0]);
    return index ? m_lines[index] : 0;
}

bool FileLineMap::IsUTF8Compatible() const
{
    switch (GetCodePage())
    {
    case 38:            // USA ASCII (IBM)
    case 367:           // 7-bit US-ASCII (IBM)
    case 20127:         // 7-bit US-ASCII (Windows)
    case CP_UTF8:       // UTF-8
        return true;
    }
    return false;
}

const WCHAR* FileLineMap::GetEncodingName(bool raw) const
{
    if (IsBinaryFile())
        return L"Binary";
    if (raw && m_codepage && !m_encoding_name.Empty())
        return m_encoding_name.Text();
    return L"Text";
}

#pragma endregion // FileLineMap
#pragma region // ContentCache

ContentCache::ContentCache(const ViewerOptions& options)
: m_options(options)
, m_map(options)
{
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
        m_file = 0;
    }

    m_size = 0;
    m_chunks.swap(PipeChunks {});
    m_text = nullptr;
    m_redirected = false;
    m_eof = false;

    Reset();

    m_data_offset = 0;
    m_data_length = 0;
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

unsigned ContentCache::FormatLineData(size_t line, unsigned left_offset, StrW& s, unsigned max_width, Error& e, const WCHAR* color, const FoundLine* found_line)
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

// TODO:  Encodings.  This currently assumes ACP.
// TODO:  Non-convertible characters will make conversion go haywire.
    StrW tmp;
    // tmp.SetA(reinterpret_cast<const char*>(ptr), len);
    tmp.SetFromCodepage(CP_OEMCP, reinterpret_cast<const char*>(ptr), len);

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
                    if (m_options.tab_mode == TabMode::HIGHLIGHT && something_fits)
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
                    if (m_options.tab_mode == TabMode::HIGHLIGHT && something_fits)
                        s.Append(c_norm);
                }
                else if (c >= 0 && c < ' ')
                {
                    if (m_options.ctrl_mode == CtrlMode::EXPAND)
                    {
                        const bool something_fits = (visible_len + 2 > left_offset);
                        if (something_fits && !color)
                            s.AppendColor(GetColor(ColorElement::CtrlCode));
                        append_text(L"^", 1);
                        if (left_offset || visible_len < max_width)
                        {
                            WCHAR text[2] = { WCHAR('@' + c) };
                            append_text(text, 1);
                        }
                        if (something_fits && !color)
                            s.Append(c_norm);
                    }
#ifdef INCLUDE_CTRLMODE_PERIOD
                    else if (m_options.ctrl_mode == CtrlMode::PERIOD)
                    {
                        assert(left_offset || visible_len < max_width);
                        if (!left_offset)
                            s.AppendColor(GetColor(ColorElement::CtrlCode));
                        append_text(L".", 1);
                        if (!left_offset)
                            s.Append(c_norm);
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
                        assert(clen == 1);
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
    const unsigned len = unsigned(std::min<FileOffset>(hex_bytes, GetFileSize() - offset));
    assert(ptr + len <= m_data + m_data_length);

// TODO:  Encodings.  This currently assumes ACP.
// TODO:  Non-convertible characters will make conversion go haywire.
    StrW tmp;
    // tmp.SetA(reinterpret_cast<const char*>(ptr), len);
    tmp.SetFromCodepage(CP_OEMCP, reinterpret_cast<const char*>(ptr), len);
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
            s.Append(L"  ", ((ii % 8) == 0) ? 2 : 1);
        if (marked_color && found_line->len && offset + ii == found_line->offset)
        {
            highlighting_found_text = true;
            s.AppendColor(GetColor(ColorElement::SearchFound));
        }
        if (ii < len)
            s.Printf(L"%02X", ptr[ii]);
        else
            s.Append(L"  ", 2);
    }
    if (marked_color)
        s.Append(c_norm);

    // Format the text characters.
    s.Printf(L"  ", 2);
    s.AppendColor(L"38;2;80;80;80");
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
            s.Append(c_oem437[c], 1);
        else if (!c || wcwidth(tmp.Text()[ii]) != 1)
            // TODO:  Maybe apply color?
            s.Append(L".", 1);
        else
            s.Append(tmp.Text() + ii, 1);
    }
    if (marked_color)
        s.Append(c_norm);
    s.AppendColor(L"38;2;80;80;80");
    // s.Append(L"\u2502", 1);
    s.Append(L"*", 1);
    s.Append(c_norm);

    return true;
}

bool ContentCache::ProcessThrough(size_t line, Error& e)
{
    assert(!e.Test());

    bool ret = true;
    if (HasContent())
    {
        while (line >= m_map.Count() && !m_completed)
        {
            const FileOffset offset = m_map.Processed();
            if (!LoadData(offset, e))
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
        }

        if (m_map.Processed() >= m_size)
            m_completed = true;
    }
    else
    {
        m_completed = true;
    }

    return ret;
}

bool ContentCache::ProcessToEnd(Error& e)
{
    assert(!e.Test());
    if (!m_completed)
    {
        ProcessThrough(size_t(-1), e);
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

bool ContentCache::Find(bool next, const WCHAR* needle, FoundLine& found_line, bool caseless)
{
    Error e;
    StrW tmp;
    const unsigned needle_len = unsigned(wcslen(needle));
    assert(needle_len);

    if (!found_line.is_line || found_line.Empty())
    {
        // TODO-HEX:  Translate offset to line, instead of reseting?
        found_line.Clear();
        if (!next)
        {
            ProcessToEnd(e);
            // TODO:  Do something with the error?
            e.Clear();
            found_line.line = Count();
        }
    }

    size_t index = found_line.line;
    while (true)
    {
        if (next)
        {
            if (index + 1 >= Count())
            {
                ProcessThrough(index + 1, e);
                // TODO:  Do something with the error?
                e.Clear();
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

// TODO:  Encodings.  This currently assumes ACP.
// TODO:  Non-convertible characters will make conversion go haywire.
        StrW tmp;
        tmp.SetFromCodepage(CP_OEMCP, reinterpret_cast<const char*>(ptr), len);

// TODO:  Optional regex search.
// TODO:  Boyer-Moore search.
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

bool ContentCache::Find(bool next, const WCHAR* needle, unsigned hex_width, FoundLine& found_line, bool caseless)
{
    Error e;
    StrW tmp;
    const unsigned needle_len = unsigned(wcslen(needle));
    assert(needle_len);

    if (found_line.is_line || found_line.Empty())
    {
        // TODO-HEX:  Translate line to offset, instead of reseting?
        if (next)
            found_line.Found(FileOffset(-1), 0);
        else
            found_line.Found(GetFileSize(), 0);
    }

    FileOffset offset = found_line.offset;
    while (true)
    {
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
        tmp.SetFromCodepage(CP_OEMCP, reinterpret_cast<const char*>(ptr), len);

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

    if (offset < m_data_offset || offset + length > m_data_offset + std::max<intptr_t>(0, intptr_t(m_data_length) - c_data_buffer_slop))
    {
        if (!LoadData(offset, e))
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

    if (offset < m_data_offset || offset + length > m_data_offset + std::max<intptr_t>(0, intptr_t(m_data_length) - c_data_buffer_slop))
    {
        if (!LoadData(offset, e))
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

bool ContentCache::LoadData(const FileOffset offset, Error& e)
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
    if (bytes_read < to_read)
        m_eof = true;
    return true;
}

#pragma endregion // ContentCache

