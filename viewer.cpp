// Copyright (c) 2025 by Christopher Antos
// License: http://opensource.org/licenses/MIT

// vim: set et ts=4 sw=4 cino={0s:

#include "pch.h"
#include "viewer.h"
#include "contentcache.h"
#include "input.h"
#include "output.h"
#include "popuplist.h"
#include "list_format.h"
#include "colors.h"
#include "ellipsify.h"
#include "ecma48.h"
#include "wcwidth.h"
#include "wcwidth_iter.h"
#include "filetype.h"
#include "help.h"
#include "os.h"

static const WCHAR c_clreol[] = L"\x1b[K";
static const WCHAR c_no_file_open[] = L"*** No File Open ***";
static const WCHAR c_endoffile_marker[] = L"*** End Of File ***";
static const WCHAR c_text_not_found[] = L"*** Text Not Found ***";
static const WCHAR c_div_char[] = L":"; //L"\u2590"; //L"\u2595"; //L":";

const DWORD c_max_needle = 32;
static_assert(c_max_needle <= c_data_buffer_slop); // Important for searching across word wrapped line breaks.

static unsigned s_max_line_length = c_default_max_line_length;
static ViewerOptions s_options;

constexpr unsigned c_horiz_scroll_amount = 10;

void SetMaxLineLength(const WCHAR* arg)
{
    const unsigned c_max_line_length = std::min<DWORD>(c_data_buffer_slop, 2048);
    unsigned max_line_length = _wtoi(arg);
    if (max_line_length <= 16)
        max_line_length = 16;
    else if (max_line_length > c_max_line_length)
        max_line_length = c_max_line_length;
    s_options.max_line_length = max_line_length;
}

class Viewer
{
public:
                    Viewer(const char* text, const WCHAR* title=L"Text");
                    Viewer(const std::vector<StrW>& files);
                    ~Viewer() = default;

    ViewerOutcome   Go(Error& e);
    StrW            GetCurrentFile() const;

private:
    unsigned        CalcMarginWidth() const;
    void            UpdateDisplay();
    unsigned        LinePercent(size_t line) const;
    ViewerOutcome   HandleInput(const InputRecord& input, Error &e);
    void            EnsureAltFiles();
    void            SetFile(intptr_t index);
    size_t          CountForDisplay() const;
    void            DoSearch(bool next, bool caseless);
    void            FindNext(bool next=true);
    void            Center(const FoundLine& found_line);
    void            GoTo();
    size_t          GetFoundLine(const FoundLine& found_line);
    FileOffset      GetFoundOffset(const FoundLine& found_line);
    void            ShowFileList();
    void            OpenNewFile(Error& e);
    ViewerOutcome   CloseCurrentFile();

private:
    const HANDLE    m_hout;
    unsigned        m_terminal_width = 0;
    unsigned        m_terminal_height = 0;
    unsigned        m_content_height = 0;
    unsigned        m_content_width = 0;
    unsigned        m_margin_width = 0;

    StrW            m_errmsg;

    StrW            m_title;
    const char*     m_text = nullptr;
    const std::vector<StrW>* m_files = nullptr;
    std::vector<StrW> m_alt_files;
    intptr_t        m_index = -1;

    ContentCache     m_context;
    WIN32_FIND_DATAW m_fd = {};
    size_t          m_top = 0;
    unsigned        m_left = 0;
    StrW            m_feedback;
    bool            m_wrap = false;

    bool            m_hex_mode = false;
    unsigned        m_hex_width = 0;
    FileOffset      m_hex_top = 0;

    intptr_t        m_last_index = -1;
    size_t          m_last_top = 0;
    size_t          m_last_left = 0;
    StrW            m_last_feedback = 0;
    FileOffset      m_last_hex_top = 0;
    FileOffset      m_last_processed = FileOffset(-1);
    bool            m_last_completed = false;
    bool            m_force_update = false;

    StrW            m_find;
    bool            m_caseless = false;
    FoundLine       m_found_line;
};

Viewer::Viewer(const char* text, const WCHAR* title)
: m_hout(GetStdHandle(STD_OUTPUT_HANDLE))
, m_title(title)
, m_text(text)
, m_context(s_options)
{
    Error e;
    m_context.SetTextContent(m_text, e);
    // TODO:  Do something with the error?
    m_force_update = true;
}

Viewer::Viewer(const std::vector<StrW>& files)
: m_hout(GetStdHandle(STD_OUTPUT_HANDLE))
, m_files(&files)
, m_context(s_options)
{
}

ViewerOutcome Viewer::Go(Error& e)
{
    SetFile(0);

    while (true)
    {
        e.Clear();

        UpdateDisplay();

        const InputRecord input = SelectInput();
        switch (input.type)
        {
        case InputType::None:
        case InputType::Error:
            continue;

        case InputType::Resize:
            m_force_update = true;
            continue;

        case InputType::Key:
        case InputType::Char:
            {
                const ViewerOutcome outcome = HandleInput(input, e);
                if (outcome != ViewerOutcome::CONTINUE)
                    return outcome;
            }
            break;
        }
    }
}

static void PadToWidth(StrW& s, unsigned min_width)
{
    const auto cells = cell_count(s.Text());
    if (min_width > cells)
        s.AppendSpaces(min_width - cells);
}

unsigned Viewer::CalcMarginWidth() const
{
    unsigned margin = 0;
    if (!m_hex_mode)
    {
#ifdef DEBUG
        const unsigned c_min_margin_width = 5;
#else
        const unsigned c_min_margin_width = 8;
#endif

        StrW s;
        if (s_options.show_line_numbers)
        {
            s.Printf(L"%lu", m_context.Count());
            margin = std::max<unsigned>(c_min_margin_width, s.Length() + 2);
        }
        else if (s_options.show_file_offsets)
        {
#ifdef DEBUG
            s.Printf(L"%lx", m_context.Processed());
#else
            s.Printf(L"%lx", m_context.GetFileSize());
#endif
            margin = std::max<unsigned>(c_min_margin_width, s.Length() + 2);
        }

        if (margin > m_terminal_width / 2)
            margin = 0;
    }
    return margin;
}

void Viewer::UpdateDisplay()
{
#ifdef DEBUG
    static bool s_no_accumulate = false;
#endif

    const bool file_changed = (m_last_index != m_index);
    const bool offset_changed = (m_hex_mode ? (m_last_hex_top != m_hex_top) : (m_last_top != m_top || m_last_left != m_left));
    const bool processed_changed = (m_last_processed != m_context.Processed() || m_last_completed != m_context.Completed());
    const bool feedback_changed = (!m_last_feedback.Equal(m_feedback));

    const bool update_header = (m_force_update || file_changed || offset_changed || processed_changed);
    const bool update_content = (m_force_update || offset_changed);
    const bool update_debug_row = (s_options.show_debug_info);
    const bool update_command_line = (m_force_update || feedback_changed);

    if (!update_header && !update_content && !update_debug_row && !update_command_line)
        return;

    StrW s;

    // Remember states that influence optimizing what to redraw.
    m_last_top = m_top;
    m_last_left = m_left;
    m_last_hex_top = m_hex_top;
    m_last_index = m_index;
    m_last_feedback.Set(m_feedback);
    m_last_processed = m_context.Processed();
    m_last_completed = m_context.Completed();
    m_force_update = false;

    // Decide terminal dimensions and content height.  Content width can't be
    // decided yet because it may depend on the margin width (which depends on
    // the highest, i.e. widest, file number or file offset).
    const unsigned debug_row = !!s_options.show_debug_info;
    const DWORD colsrows = GetConsoleColsRows(m_hout);
    m_terminal_width = LOWORD(colsrows);
    m_terminal_height = HIWORD(colsrows);
    if (m_terminal_height > 2 + debug_row)
        m_content_height = m_terminal_height - (2 + debug_row);
    else
        m_content_height = 0;

    // Decide how many hex bytes fit per line.
    m_hex_width = 0;
    if (m_hex_mode)
    {
        const unsigned available = (m_terminal_width - (8/*ofs*/ + 2/*spc*/ + 0/*bytes*/ + 2/*spc*/ + 1/*edge*/ + 0/*bytes*/ + 1/*edge*/ + 2/*margin*/));
        if (available >= 32*3 + 3 + 32)
            m_hex_width = 32;
        else if (available >= 16*3 + 1 + 16)
            m_hex_width = 16;
        else if (available >= 8*3 + 0 + 8)
            m_hex_width = 8;
        else
            m_hex_mode = false;
    }

    // Process enough lines to display the current screenful of lines.  If
    // processing lines causes the margin width to change, then wrapping and
    // processing may need to be redone.
    unsigned autofit_retries = 0;
LAutoFitContentWidth:
    assert(autofit_retries != 2); // Should be impossible to occur...
    m_margin_width = CalcMarginWidth();
    m_content_width = m_terminal_width - m_margin_width;
    {
        Error e;
        m_context.SetWrapWidth(m_wrap ? m_content_width : 0);
        m_context.ProcessThrough(m_top + m_content_height, e);
        const unsigned new_margin_width = CalcMarginWidth();
        if (new_margin_width != m_margin_width)
        {
            // Margin width changed; redo wrapping and processing (processing
            // may be a no-op if wrapping isn't active).
            if (autofit_retries++ < 4)
                goto LAutoFitContentWidth;
        }
    }

    // Fix the top offset.
    if (m_hex_mode)
    {
        const FileOffset max_hex = m_context.GetMaxHexOffset(m_hex_width);
        const FileOffset hex_page = m_content_height * m_hex_width;
        if (m_hex_top + hex_page > max_hex)
        {
            if (m_hex_top > hex_page)
                m_hex_top -= hex_page;
            else
                m_hex_top = 0;
        }
    }
    else
    {
        if (s_options.show_ruler)
        {
            // When the ruler is shown, allow the last line to go all the way
            // to the top, to allow easy measuring.
            if (m_top >= m_context.Count())
                m_top = m_context.Count() ? m_context.Count() - 1 : 0;
        }
        else
        {
            if (m_top + m_content_height - 1 > CountForDisplay())
            {
                if (CountForDisplay() <= m_content_height)
                    m_top = 0;
                else
                    m_top = CountForDisplay() - m_content_height;
            }
        }
    }

    // Header.
    if (update_header)
    {
        StrW left;
        StrW right;
        StrW file;
        StrW details;
        const unsigned c_min_filename_width = 40;

        s.Clear();
        s.Append(L"\x1b[1H");
        s.AppendColor(GetColor(ColorElement::Command));

        if (s_options.show_ruler)
        {
            if (m_hex_mode)
            {
                left.AppendSpaces(8 + 2);
                for (unsigned ii = 0; ii < m_hex_width; ++ii)
                {
                    if (ii)
                        left.AppendSpaces((ii % 8) ? 1 : 2);
                    left.Printf(L"%02x", ii);
                }
                left.AppendSpaces(3);
                for (unsigned ii = 0; ii < m_hex_width; ++ii)
                {
                    left.Printf(L"%x", ii & 0xf);
                }
                PadToWidth(left, m_terminal_width);
                s.Append(left);
            }
            else
            {
                s.AppendSpaces(m_margin_width);
                left.Set(L"\u252c\u252c\u252c\u252c\u253c\u252c\u252c\u252c");
                for (unsigned width = 0; width < m_content_width; width += 10)
                {
                    right.Clear();
                    right.Printf(L"%u", m_left + width + 10);
                    left.SetLength(std::min<unsigned>(10 - right.Length(), m_content_width - width));
                    s.Append(left);
                    s.Append(right);
                }
            }
        }
        else
        {
            left.Printf(L"LIST - ");

            size_t bottom_line_plusone;
            FileOffset bottom_offset;
            if (m_hex_mode)
            {
                bottom_line_plusone = 0;
                bottom_offset = std::min<FileOffset>(m_hex_top + m_content_height * m_hex_width, m_context.GetFileSize());
            }
            else
            {
                bottom_line_plusone = std::min<size_t>(m_top + m_content_height, m_context.Count());
                bottom_offset = !bottom_line_plusone ? 0 : m_context.GetOffset(bottom_line_plusone - 1) + m_context.GetLength(bottom_line_plusone - 1);
            }
            if (m_hex_mode)
                right.Printf(L"    Offset: %06lx-%06lx", m_hex_top, bottom_offset);
            else if (s_options.show_file_offsets)
                right.Printf(L"    Offset: %06lx-%06lx", m_context.GetOffset(m_top), bottom_offset);
            else
                right.Printf(L"    Line: %lu", m_top + 1);
            if (s_options.show_file_offsets || m_hex_mode)
                right.Printf(L" of %06lx", m_context.GetFileSize());
            else if (!m_context.Completed())
                right.Printf(L"   (%u%%)", LinePercent(bottom_line_plusone));
            else
                right.Printf(L" of %lu", m_context.Count());
            if (m_left && !m_hex_mode)
                right.Printf(L"  Col: %u-%u", m_left + 1, m_left + m_content_width);
            PadToWidth(right, 30);
            right.AppendSpaces(4);

            unsigned details_width = 0;
            if (m_fd.cFileName[0])
            {
                details_width = FormatFileData(details, m_fd);
                if (details_width + right.Length() + left.Length() + c_min_filename_width <= m_terminal_width)
                {
                    right.AppendSpaces(std::max<unsigned>(details_width, 16) - details_width);
                    right.Append(details);
                }
                else
                {
                    details_width = 0;
                }
            }
            if (!details_width)
                PadToWidth(right, 50);

            if (left.Length() + right.Length() + c_min_filename_width > m_terminal_width)
                right.Clear();
            const unsigned limit_len = m_terminal_width - (left.Length() + right.Length());
            ellipsify_ex(GetCurrentFile().Text(), limit_len, ellipsify_mode::PATH, file);

            s.Append(left);
            s.Append(file);
            if (right.Length())
            {
                s.AppendSpaces(m_terminal_width - (left.Length() + cell_count(file.Text()) + right.Length()));
                s.Append(right);
            }
            else
            {
                if (m_terminal_width > left.Length() + cell_count(file.Text()))
                    s.Append(c_clreol);
            }
        }

        s.Append(c_norm);

#ifdef DEBUG
        if (s_no_accumulate && s.Length())
        {
            OutputConsole(m_hout, c_hide_cursor);
            s.Append(c_show_cursor);
            OutputConsole(m_hout, s.Text(), s.Length());
            s.Clear();
        }
#endif
    }

    // Content.
    if (update_content)
    {
        s.Printf(L"\x1b[%uH", 2);

        StrW s2;
        Error e;
        const WCHAR* msg_text = nullptr;
        const WCHAR* msg_color = nullptr;

        if (m_errmsg.Length() || !m_context.HasContent())
        {
            msg_text = m_errmsg.Length() ? m_errmsg.Text() : c_no_file_open;
            msg_color = GetColor(ColorElement::EndOfFileLine);
            for (size_t row = 0; row < m_content_height; ++row)
            {
                if (*msg_text)
                {
                    s2.Clear();
                    const unsigned cells = ellipsify_ex(msg_text, m_content_width, ellipsify_mode::RIGHT, s2, L"");
                    if (msg_color)
                        s.AppendColor(msg_color);
                    s.Append(s2);
                    if (msg_color)
                        s.Append(c_norm);
                    if (cells < m_content_width)
                        s.Append(c_clreol);
                    msg_text += s2.Length();
                }
                else
                {
                    s.Append(c_clreol);
                }
                s.Append(L"\n");
            }
        }
        else if (m_hex_mode)
        {
            // Ensure found_line which matches m_hex_mode.
            const FoundLine* found_line = nullptr;
            FoundLine __translated_found_line;
            if (!m_found_line.Empty())
            {
                if (!m_found_line.is_line)
                    found_line = &m_found_line;
                else
                {
                    const FileOffset tmp_file_offset = GetFoundOffset(m_found_line);
                    __translated_found_line.Found(tmp_file_offset, m_found_line.len);
                    found_line = &__translated_found_line;
                }
            }

            for (unsigned row = 0; row < m_content_height; ++row)
            {
                m_context.FormatHexData(m_hex_top, row, m_hex_width, s, e, found_line);
                s.Append(c_clreol);
                s.Append(L"\n");
            }
        }
        else
        {
            // Ensure found_line which matches m_hex_mode.
            const FoundLine* found_line = nullptr;
            FoundLine __translated_found_line;
            if (!m_found_line.Empty())
            {
                if (m_found_line.is_line)
                    found_line = &m_found_line;
                else
                {
                    size_t tmp_line = GetFoundLine(m_found_line);
                    if (tmp_line < m_context.Count())
                    {
                        const FileOffset tmp_file_offset = m_context.GetOffset(tmp_line);
                        const unsigned tmp_line_offset = unsigned(m_found_line.offset - tmp_file_offset);
                        assert(tmp_line_offset < m_context.GetLength(tmp_line));
                        __translated_found_line.Found(tmp_line, tmp_line_offset, m_found_line.len);
                        found_line = &__translated_found_line;
                    }
                }
            }

            for (size_t row = 0; row < m_content_height; ++row)
            {
                if (s_options.show_endoffile_line && m_top + row == m_context.Count())
                {
                    msg_text = c_endoffile_marker;
                    msg_color = GetColor(ColorElement::EndOfFileLine);
                }

                if (msg_text)
                {
                    s2.Clear();
                    const unsigned cells = ellipsify_ex(msg_text, m_content_width, ellipsify_mode::RIGHT, s2, L"");
                    if (msg_color)
                        s.AppendColor(msg_color);
                    s.Append(s2);
                    if (msg_color)
                        s.Append(c_norm);
                    s.Append(c_clreol);
                    if (cells < m_content_width)
                    {
                        msg_text = nullptr;
                        msg_color = nullptr;
                    }
                    else
                    {
                        msg_text += s2.Length();
                    }
                }
                else if (m_top + row < m_context.Count())
                {
                    const WCHAR* color = found_line && (m_top + row == found_line->line) ? GetColor(ColorElement::MarkedLine) : nullptr;
                    if (m_margin_width)
                    {
                        s.AppendColor(GetColor(ColorElement::LineNumber));
                        if (s_options.show_line_numbers)
                            s.Printf(L"%*lu%s", m_margin_width - 2, m_top + row + 1, c_div_char);
                        else if (s_options.show_file_offsets)
                            s.Printf(L"%0*lx%s", m_margin_width - 2, m_context.GetOffset(m_top + row), c_div_char);
                        else
                            assert(!m_margin_width);
                        s.AppendNormalIf(true);
                        s.Append(L" ");
                    }
                    if (color)
                        s.AppendColor(color);
                    const unsigned width = m_context.FormatLineData(m_top + row, m_left, s, m_content_width, e, color, found_line);
                    if (width < m_content_width)
                    {
                        if (!color)
                            s.Append(c_clreol);
                        if (color && width + 1 < m_content_width)
                            s.AppendSpaces(m_content_width - width);
                    }
                    if (color)
                        s.Append(c_norm);
                }
                else
                {
                    s.Append(c_clreol);
                }

                s.Append(L"\n");

#ifdef DEBUG
                if (s_no_accumulate && s.Length())
                {
                    OutputConsole(m_hout, c_hide_cursor);
                    s.Append(c_show_cursor);
                    OutputConsole(m_hout, s.Text(), s.Length());
                    s.Clear();
                }
#endif
            }
        }
    }

// TODO:  Scroll bar.

    // Debug row.
    if (s_options.show_debug_info && update_debug_row)
    {
        s.Printf(L"\x1b[%uH", m_terminal_height - debug_row);
        s.AppendColor(GetColor(ColorElement::DebugRow));

        StrW left;
        StrW right;
        if (s_options.show_file_offsets || m_hex_mode)
            left.Printf(L"Buffer: offset %06lx, %x bytes", m_context.GetBufferOffset(), m_context.GetBufferLength());
        else
            left.Printf(L"Buffer: offset %lu, %u bytes", m_context.GetBufferOffset(), m_context.GetBufferLength());
        if (!m_found_line.Empty())
        {
            if (m_found_line.is_line)
                right.Printf(L"    Found: line %lu, len %u", m_found_line.line, m_found_line.len);
            else
                right.Printf(L"    Found: offset %06lx, len %u", m_found_line.offset, m_found_line.len);
        }
        if (m_context.GetCodePage())
        {
            right.Printf(L"    Encoding: %u", m_context.GetCodePage());
            if (m_context.GetEncodingName(true/*raw*/))
                right.Printf(L", %s", m_context.GetEncodingName(true/*raw*/));
        }
        if (left.Length() + right.Length() > m_terminal_width)
            right.Clear();

        s.Append(left);
        s.AppendSpaces(m_terminal_width - (left.Length() + right.Length()));
        s.Append(right);
        s.Append(c_norm);
    }

    // Command line.
    if (update_command_line)
    {
        static const WCHAR* const c_ctrl_indicator[] =
        {
#ifdef INCLUDE_CTRLMODE_SPACE
            L"C",                       // OEM437
            L"\x1b[7mC\x1b[27m",        // EXPAND
#ifdef INCLUDE_CTRLMODE_PERIOD
            L".",                       // PERIOD
#endif
            L"c",                       // SPACE
#else
            L"c",                       // OEM437
            L"C",                       // EXPAND
#endif
        };
        static const WCHAR* const c_tab_indicator[] =
        {
            L"T",                       // EXPAND
            L"\x1b[7mT\x1b[27m",        // HIGHLIGHT
            L"t",                       // RAW
        };

        s.Printf(L"\x1b[%uH", m_terminal_height);
        s.AppendColor(GetColor(ColorElement::Command));

        const unsigned offset = s.Length();
        StrW left;
        StrW right;
        right.Printf(L"    %-6s", m_context.GetEncodingName());
        right.Append(L"    Options: ");
#ifdef DEBUG
        right.Append(s_options.show_debug_info ? L"D" : L"d");
#endif
        if (!m_text)
            right.Append(m_hex_mode ? L"H" : L"h");
        right.Append(s_options.show_line_numbers ? L"N" : L"n");
        if (!m_text)
        {
            right.Append(s_options.show_file_offsets ? L"O" : L"o");
            right.Append(s_options.show_ruler ? L"R" : L"r");
        }
        right.Append(m_wrap ? L"W" : L"w");
        if (!m_text)
        {
            right.Append(c_tab_indicator[int(s_options.tab_mode)]);
            right.Append(c_ctrl_indicator[int(s_options.ctrl_mode)]);
        }
        if (left.Length() + right.Length() > m_terminal_width)
            right.Clear();

        s.Append(left);
        s.AppendSpaces(m_terminal_width - (left.Length() + cell_count(right.Text())));
        s.Append(right);

        if (m_feedback.Length())
            s.Printf(L"\x1b[10G%s", m_feedback.Text());

        s.Append(c_norm);
    }

    if (s.Length())
    {
        OutputConsole(m_hout, c_hide_cursor);
        s.Printf(L"\x1b[%uH", m_terminal_height);
        s.AppendColor(GetColor(ColorElement::Command));
        s.Append(L"Command: ");
        s.Append(c_norm);
        s.Append(c_show_cursor);
        OutputConsole(m_hout, s.Text(), s.Length());
    }

    m_feedback.Clear();
}

unsigned Viewer::LinePercent(size_t line) const
{
    const FileOffset offset = ((line < m_context.Count()) ? m_context.GetOffset(line) :
                               (!line) ? 0 : m_context.GetOffset(line - 1) + m_context.GetLength(line - 1));
    const FileOffset size = m_context.GetFileSize();
    const double percent = size ? double(offset) / double(size) * 100.0 : 0.0;
    assert(percent >= 0);
    assert(percent <= 100.0);
    return unsigned(std::min<double>(100.0, percent));
}

ViewerOutcome Viewer::HandleInput(const InputRecord& input, Error& e)
{
    if (input.type == InputType::Key)
    {
        switch (input.key)
        {
        case Key::F1:
            if ((input.modifier & ~(Modifier::SHIFT)) == Modifier::None)
            {
                if (!m_text && ViewHelp(Help::VIEWER, e) == ViewerOutcome::EXITAPP)
                    return ViewerOutcome::EXITAPP;
                m_force_update = true;
            }
            break;

        case Key::ESC:
            return ViewerOutcome::RETURN;

        case Key::HOME:
            if (m_hex_mode)
                m_hex_top = 0;
            else
                m_top = 0;
            break;
        case Key::END:
            if (m_hex_mode)
            {
                const FileOffset partial = (m_context.GetFileSize() % m_hex_width);
                m_hex_top = m_context.GetFileSize() + (partial ? m_hex_width - partial : 0);
                if (m_hex_top >= m_content_height * m_hex_width)
                    m_hex_top -= m_content_height * m_hex_width;
                else
                    m_hex_top = 0;
            }
            else
            {
                if (m_context.ProcessToEnd(e))
                {
                    m_top = CountForDisplay();
                    if (m_top > m_content_height)
                        m_top -= m_content_height;
                    else
                        m_top = 0;
                }
            }
            break;
        case Key::UP:
            if (m_hex_mode)
            {
                if (m_hex_top)
                    m_hex_top -= m_hex_width;
            }
            else
            {
                if (m_top)
                    --m_top;
            }
            break;
        case Key::DOWN:
            if (m_hex_mode)
            {
                if (m_hex_top + m_content_height * m_hex_width < m_context.GetFileSize())
                    m_hex_top += m_hex_width;
            }
            else
            {
                if (!m_context.Completed() || m_top + (s_options.show_ruler ? 0 : m_content_height) < CountForDisplay())
                    ++m_top;
            }
            break;
        case Key::PGUP:
            if (m_hex_mode)
            {
                const FileOffset hex_page = (m_content_height - 1) * m_hex_width;
                if (m_hex_top > hex_page)
                    m_hex_top -= hex_page;
                else
                    m_hex_top = 0;
            }
            else
            {
                if (m_top >= m_content_height - 1)
                    m_top -= m_content_height - 1;
                else
                    m_top = 0;
            }
            break;
        case Key::PGDN:
            if (m_hex_mode)
            {
                const FileOffset hex_page = m_content_height * m_hex_width;
                if (m_hex_top + hex_page + hex_page - m_hex_width < m_context.GetMaxHexOffset(m_hex_width))
                    m_hex_top += hex_page - m_hex_width;
                else if (m_context.GetMaxHexOffset(m_hex_width) >= m_hex_top)
                    m_hex_top = m_context.GetMaxHexOffset(m_hex_width) - hex_page;
                else
                    m_hex_top = 0;
            }
            else
            {
                if (!m_context.Completed() || m_top + m_content_height + m_content_height - 1 < CountForDisplay())
                    m_top += m_content_height - 1;
                else if (CountForDisplay() >= m_content_height)
                    m_top = CountForDisplay() - m_content_height;
                else
                    m_top = 0;
            }
            break;

        case Key::LEFT:
            if (m_hex_mode)
            {
                // TODO:  TBD.
            }
            else
            {
                if (m_left <= c_horiz_scroll_amount)
                    m_left = 0;
                else
                    m_left -= c_horiz_scroll_amount;
            }
            break;
        case Key::RIGHT:
            if (m_hex_mode)
            {
                // TODO:  TBD.
            }
            else
            {
                if (s_max_line_length <= m_content_width)
                    m_left = 0;
                else if (m_left + m_content_width <= s_max_line_length)
                    m_left += c_horiz_scroll_amount;
            }
            break;

        case Key::F2:
            if (input.modifier == Modifier::None)
            {
                ShowFileList();
            }
            break;
        case Key::F3:
            {
                // F3 = forward, Shift-F3 = backward.
                const bool next = (input.modifier & Modifier::SHIFT) == Modifier::None;
                if (m_find.Empty())
                {
                    if (!next && m_found_line.Empty())
                    {
                        // Mark where to start searching.
                        if (m_hex_mode)
                            m_found_line.MarkOffset(m_context.GetFileSize());
                        else
                            m_found_line.MarkLine(m_context.Count());
                    }
                    DoSearch(next, true/*caseless*/);
                }
                else
                {
                    FindNext(next);
                }
            }
            break;
        }
    }
    else if (input.type == InputType::Char)
    {
        switch (input.key_char)
        {
        case '?':
            if ((input.modifier & ~(Modifier::SHIFT)) == Modifier::None)
            {
                if (!m_text && ViewHelp(Help::VIEWER, e) == ViewerOutcome::EXITAPP)
                    return ViewerOutcome::EXITAPP;
                m_force_update = true;
            }
            break;

        case 'N'-'@':   // CTRL-N
            if (input.modifier == Modifier::CTRL)
            {
                SetFile(m_index + 1);
            }
            break;
        case 'P'-'@':   // CTRL-P
            if (input.modifier == Modifier::CTRL)
            {
                SetFile(m_index - 1);
            }
            break;

        case '@':
            if ((input.modifier & ~(Modifier::SHIFT)) == Modifier::None)
            {
                ShowFileList();
            }
            break;

        case 'c':
            if (input.modifier == Modifier::ALT)
            {
                return CloseCurrentFile();
            }
            __fallthrough;
        case '^':
            if ((input.modifier & ~Modifier::SHIFT) == Modifier::None)
            {
                if (!m_hex_mode && !m_text)
                {
                    s_options.ctrl_mode = CtrlMode((int(s_options.ctrl_mode) + 1) % int(CtrlMode::__MAX));
                    m_context.Reset();
                    m_force_update = true;
                }
            }
            break;
        case 'd':
            if (input.modifier == Modifier::ALT)
            {
                s_options.show_debug_info = !s_options.show_debug_info;
                m_force_update = true;
            }
            break;
        case 'g':
            if (input.modifier == Modifier::None)
            {
                GoTo();
            }
            break;
        case 'h':
            if (input.modifier == Modifier::None)
            {
                if (!m_text)
                {
                    m_hex_mode = !m_hex_mode;
                    m_hex_top = m_context.GetOffset(m_top) & ~FileOffset(0xf);
                    m_force_update = true;
                }
            }
            break;
        case 'j':
            if (input.modifier == Modifier::None)
            {
                if (!m_found_line.Empty())
                    Center(m_found_line);
            }
            break;
        case 'm':
            if (input.modifier == Modifier::None)
            {
                if (m_hex_mode)
                    m_found_line.MarkOffset(std::min<FileOffset>(m_hex_top + (m_content_height / 2) * m_hex_width, m_context.GetFileSize() / 2));
                else
                    m_found_line.MarkLine(m_top + (std::min<size_t>(m_content_height, m_context.Count()) / 2));
                m_force_update = true;
            }
            break;
        case 'n':
            if (input.modifier == Modifier::None)
            {
                if (!m_hex_mode)
                {
                    s_options.show_line_numbers = !s_options.show_line_numbers;
                    s_options.show_file_offsets = false;
                    m_force_update = true;
                }
            }
            break;
        case 'o':
            if (input.modifier == Modifier::None)
            {
                if (!m_hex_mode && !m_text)
                {
                    s_options.show_file_offsets = !s_options.show_file_offsets;
                    s_options.show_line_numbers = false;
                    m_force_update = true;
                }
            }
            else if (input.modifier == Modifier::ALT)
            {
                if (!m_text) // Can't open files in ViewText() mode.
                    OpenNewFile(e);
            }
            break;
        case 'r':
            if (input.modifier == Modifier::None)
            {
                if (!m_text)
                {
                    s_options.show_ruler = !s_options.show_ruler;
                    // TODO:  Only the header needs to redraw.
                    m_force_update = true;
                }
            }
            break;
        case 't':
            if (input.modifier == Modifier::None)
            {
                if (!m_hex_mode && !m_text)
                {
                    s_options.tab_mode = TabMode((int(s_options.tab_mode) + 1) % int(TabMode::__MAX));
                    m_context.Reset();
                    m_force_update = true;
                }
            }
            break;
        case 'u':
            if (input.modifier == Modifier::None)
            {
                if (!m_found_line.Empty())
                {
                    m_found_line.Clear();
                    m_force_update = true;
                }
            }
            break;
        case 'w':
            if (input.modifier == Modifier::None)
            {
                if (!m_hex_mode)
                {
                    m_wrap = !m_wrap;
                    m_force_update = true;
                }
            }
            break;

        case '/':
        case 's':
            if (input.modifier == Modifier::None)
            {
                // TODO:  What should it do in hex mode?
                DoSearch(true, false/*caseless*/);
            }
            break;
        case '\\':
        case 'f':
            if (input.modifier == Modifier::None)
            {
                // TODO:  What should it do in hex mode?
                DoSearch(true, true/*caseless*/);
            }
            break;
        }
    }

    return ViewerOutcome::CONTINUE;
}

void Viewer::EnsureAltFiles()
{
    if (m_files != &m_alt_files)
    {
        // Copy the list so it can be modified.
        m_alt_files.clear();
        for (const auto& file : *m_files)
            m_alt_files.emplace_back(file);
        m_files = &m_alt_files;
    }
}

StrW Viewer::GetCurrentFile() const
{
    StrW s;
    if (m_files)
    {
        if (m_index >= 0 && size_t(m_index) < m_files->size())
            s.Set((*m_files)[m_index]);
    }
    else
    {
        s.Set(m_title);
    }
    return s;
}

void Viewer::SetFile(intptr_t index)
{
    if (m_text)
        return;

    assert(m_files);
    if (index > 0 && size_t(index) >= m_files->size())
        index = m_files->size() - 1;
    if (index < 0)
        index = 0;

    if (index == m_index)
        return;

    m_errmsg.Clear();
    m_index = index;
    m_top = 0;
    m_left = 0;
    m_hex_top = 0;
    m_force_update = true;

    m_found_line.Clear();

    m_context.Close();
    ZeroMemory(&m_fd, sizeof(m_fd));

    if (m_files && size_t(m_index) < m_files->size())
    {
        Error e;
        m_context.Open((*m_files)[m_index].Text(), e);

        if (e.Test())
        {
            e.Format(m_errmsg);
            m_errmsg.TrimRight();
        }

        if (!m_context.IsPipe() && !m_text)
        {
            SHFind sh = FindFirstFileW((*m_files)[m_index].Text(), &m_fd);
            if (sh.Empty())
                ZeroMemory(&m_fd, sizeof(m_fd));
        }
    }
}

size_t Viewer::CountForDisplay() const
{
    return m_context.Count() + s_options.show_endoffile_line;
}

void Viewer::DoSearch(bool next, bool caseless)
{
    StrW s;
    s.AppendColor(GetColor(ColorElement::Command));
    s.Append(L"\rSearch> ");
    OutputConsole(m_hout, s.Text(), s.Length());

    ReadInput(s);

    OutputConsole(m_hout, c_norm);
    m_force_update = true;

    if (s.Length())
    {
        m_find.Set(std::move(s));
        m_caseless = caseless;
        m_found_line.Clear();
        FindNext(next);
    }
}

void Viewer::FindNext(bool next)
{
    assert(m_find.Length());

    // TODO:  Print feedback saying it's searching.
    // TODO:  When should a search start over at the top of the file?

    if (!(m_hex_mode ?
          m_context.Find(next, m_find.Text(), m_hex_width, m_found_line, m_caseless) :
          m_context.Find(next, m_find.Text(), m_found_line, m_caseless)))
    {
        m_feedback.Set(c_text_not_found);
    }
    else
    {
        Center(m_found_line);
        m_force_update = true;
    }
}

void Viewer::Center(const FoundLine& found_line)
{
    assert(!found_line.Empty());
    if (found_line.Empty())
        return;

    if (m_hex_mode)
    {
        const FileOffset offset = GetFoundOffset(found_line);
        const unsigned center = (m_content_height / 2) * m_hex_width;
        if (offset >= center)
            m_hex_top = offset - center;
        else
            m_hex_top = 0;
    }
    else
    {
        const size_t line = GetFoundLine(found_line);
        const unsigned center = m_content_height / 2;
        if (line >= center)
            m_top = line - center;
        else
            m_top = 0;
    }
}

static bool wcstonum(const WCHAR* text, unsigned radix, unsigned __int64& out)
{
    assert(radix == 10 || radix == 16);

    if (!*text)
        return false;

    unsigned __int64 num = 0;
    while (*text)
    {
        if (*text >= '0' && *text <= '9')
            num = (num * radix) + (*text - '0');
        else if (radix != 16)
            return false;
        else if (*text >= 'A' && *text <= 'F')
            num = (num * radix) + (10 + *text - 'A');
        else if (*text >= 'a' && *text <= 'f')
            num = (num * radix) + (10 + *text - 'a');
        ++text;
    }

    out = num;
    return true;
}

void Viewer::GoTo()
{
    StrW s;
    s.AppendColor(GetColor(ColorElement::Command));
    s.Printf(L"\r%s> ", m_hex_mode ? L"Offset" : L"Line #");
    OutputConsole(m_hout, s.Text(), s.Length());

    ReadInput(s);

    OutputConsole(m_hout, c_norm);
    m_force_update = true;

    if (s.Length())
    {
        if (m_hex_mode)
        {
            FileOffset offset;
            if (wcstonum(s.Text(), 16, offset))
            {
                offset &= ~FileOffset(m_hex_width - 1);
                m_found_line.MarkOffset(offset);
                Center(m_found_line);
                m_force_update = true;
            }
        }
        else
        {
            unsigned __int64 line;
            if (wcstonum(s.Text(), 10, line) && line > 0)
            {
                m_found_line.MarkLine(line - 1);
                Center(m_found_line);
                m_force_update = true;
            }
        }
    }
}

size_t Viewer::GetFoundLine(const FoundLine& found_line)
{
    assert(!found_line.Empty());
    size_t line = found_line.line;
    if (!found_line.is_line)
    {
        line = 0;
// TODO:  Use binary search over the lines processed so far.
        for (size_t ii = 0; true; ++ii)
        {
            if (ii >= m_context.Count())
            {
                Error e;
                m_context.ProcessThrough(ii + 1, e);
                // TODO:  Do something with the error?
                if (ii >= m_context.Count())
                    break;
            }
            if (m_context.GetOffset(ii) <= found_line.offset)
                line = ii;
            else
                break;
        }
    }
    return line;
}

FileOffset Viewer::GetFoundOffset(const FoundLine& found_line)
{
    assert(!found_line.Empty());
    FileOffset offset = found_line.offset;
    if (found_line.is_line)
    {
        Error e;
        m_context.ProcessThrough(found_line.line, e);
        // TODO:  Do something with the error?
        if (found_line.line >= m_context.Count())
            offset = m_context.GetFileSize();
        else
            offset = m_context.GetOffset(found_line.line);
    }
    offset &= ~FileOffset(m_hex_width - 1);
    return offset;
}

void Viewer::ShowFileList()
{
    const PopupResult result = ShowPopupList(*m_files, L"Choose File", m_index, PopupListFlags::DimPaths);
    m_force_update = true;
    if (!result.canceled)
        SetFile(result.selected);
}

void Viewer::OpenNewFile(Error& e)
{
    StrW s;
    s.AppendColor(GetColor(ColorElement::Command));
    s.Append(L"\rEnter file to open> ");
    OutputConsole(m_hout, s.Text(), s.Length());

    ReadInput(s);

    OutputConsole(m_hout, c_norm);

    StrW full;
    if (!OS::GetFullPathName(s.Text(), full, e))
        return;

    for (size_t i = 0; i < m_files->size(); ++i)
    {
        if (full.EqualI((*m_files)[i]))
        {
            SetFile(i);
            return;
        }
    }

    EnsureAltFiles();
    m_alt_files.insert(m_alt_files.begin() + m_index + 1, std::move(full));
    SetFile(m_index + 1);
}

ViewerOutcome Viewer::CloseCurrentFile()
{
    if (m_text || m_files->size() <= 1)
        return ViewerOutcome::RETURN;

    EnsureAltFiles();
    m_alt_files.erase(m_alt_files.begin() + m_index);

    const auto index = m_index;
    m_index = -2;
    SetFile(index);
    return ViewerOutcome::CONTINUE;
}

ViewerOutcome ViewFiles(const std::vector<StrW>& files, StrW& dir, Error& e)
{
    Viewer viewer(files);

    const ViewerOutcome outcome = viewer.Go(e);

    dir = viewer.GetCurrentFile();
    dir.SetEnd(FindName(dir.Text()));

    return outcome;
}

ViewerOutcome ViewText(const char* text, Error& e, const WCHAR* title)
{
    ViewerOptions old = s_options;
    s_options = ViewerOptions();
    s_options.ctrl_mode = CtrlMode::OEM437;
    s_options.tab_mode = TabMode::EXPAND;
    s_options.show_line_numbers = false;
    s_options.show_file_offsets = false;
    s_options.show_ruler = false;
    s_options.show_endoffile_line = true;
    s_options.show_debug_info = false;

    Viewer viewer(text, title);
    ViewerOutcome ret = viewer.Go(e);

    s_options = old;
    return ret;
}

