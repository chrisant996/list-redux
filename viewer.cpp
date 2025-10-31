// Copyright (c) 2025 by Christopher Antos
// License: http://opensource.org/licenses/MIT

// vim: set et ts=4 sw=4 cino={0s:

#include "pch.h"
#include "viewer.h"
#include "contentcache.h"
#include "input.h"
#include "output.h"
#include "signaled.h"
#include "popuplist.h"
#include "list_format.h"
#include "colors.h"
#include "ellipsify.h"
#include "ecma48.h"
#include "wcwidth.h"
#include "wcwidth_iter.h"
#include "encodings.h"
#include "help.h"
#include "os.h"

constexpr bool c_floating = false;
constexpr scroll_bar_style c_sbstyle = scroll_bar_style::eighths_block_chars;

static const WCHAR c_clreol[] = L"\x1b[K";
static const WCHAR c_no_file_open[] = L"*** No File Open ***";
static const WCHAR c_endoffile_marker[] = L"*** End Of File ***";
static const WCHAR c_text_not_found[] = L"*** Text Not Found ***";
static const WCHAR c_canceled[] = L"*** Canceled ***";
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

void SetViewerScrollbar(bool scrollbar)
{
    s_options.show_scrollbar = scrollbar;
}

class Viewer;
class ScopedWorkingIndicator;

class ScopedWorkingIndicator
{
public:
                    ScopedWorkingIndicator() = default;
    void            ShowFeedback(bool completed, unsigned __int64 processed, unsigned __int64 target, const Viewer* viewer, bool bytes);
    bool            NeedsCleanup() const { return m_needs_cleanup; }
private:
    bool            m_needs_cleanup = false;
};

class Viewer
{
    friend class ScopedWorkingIndicator;

public:
                    Viewer(const char* text, const WCHAR* title=L"Text");
                    Viewer(const std::vector<StrW>& files);
                    ~Viewer() = default;

    ViewerOutcome   Go(Error& e);
    StrW            GetCurrentFile() const;

private:
    unsigned        CalcMarginWidth() const;
    void            UpdateDisplay();
    void            MakeCommandLine(StrW& s, const WCHAR* msg=nullptr) const;
    void            InitHexWidth();
    unsigned        LinePercent(size_t line) const;
    ViewerOutcome   HandleInput(const InputRecord& input, Error &e);
    void            OnLeftClick(const InputRecord& input, Error &e);
    void            EnsureAltFiles();
    void            SetFile(intptr_t index, ContentCache* context=nullptr);
    size_t          CountForDisplay() const;
    void            DoSearch(bool next, bool caseless);
    void            FindNext(bool next=true);
    void            Center(const FoundLine& found_line);
    void            GoTo();
    size_t          GetFoundLine(const FoundLine& found_line);
    FileOffset      GetFoundOffset(const FoundLine& found_line, unsigned* offset_highlight=nullptr);
    void            ShowFileList();
    void            ChooseEncoding();
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
    bool            m_force_update_footer = false;
    bool            m_searching = false;

    StrW            m_find;
    bool            m_caseless = false;
    bool            m_multifile_search = false;
    FoundLine       m_found_line;

    bool            m_allow_mouse = false;
};

void ScopedWorkingIndicator::ShowFeedback(bool completed, unsigned __int64 processed, unsigned __int64 target, const Viewer* viewer, bool bytes)
{
    const size_t c_threshold = bytes ? 160000 : 5000; // Based on an average of 32 bytes per line.
    if (!m_needs_cleanup && viewer && !completed && processed + c_threshold < target)
    {
        StrW msg;
        HANDLE hout = GetStdHandle(STD_OUTPUT_HANDLE);
        const DWORD colsrows = GetConsoleColsRows(hout);
        msg.Printf(L"\x1b[%uH", HIWORD(colsrows));
        viewer->MakeCommandLine(msg, L"Working...");
        OutputConsole(hout, msg.Text(), msg.Length());
        m_needs_cleanup = true;
    }
}

Viewer::Viewer(const char* text, const WCHAR* title)
: m_hout(GetStdHandle(STD_OUTPUT_HANDLE))
, m_title(title)
, m_text(text)
, m_context(s_options)
{
    Error e;
    m_context.SetTextContent(m_text, e);
    if (e.Test())
    {
        e.Format(m_errmsg);
        m_errmsg.TrimRight();
    }
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

    AutoMouseConsoleMode mouse_mode;

    while (true)
    {
        e.Clear();

        UpdateDisplay();

        mouse_mode.EnableMouseInput(m_allow_mouse);

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
        case InputType::Mouse:
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
            s.Printf(L"%lu", m_context.CountFriendlyLines());
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
    bool update_command_line = (m_force_update || m_force_update_footer || feedback_changed);

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
    m_force_update_footer = false;

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
    const bool show_scrollbar = (s_options.show_scrollbar &&
                                 m_content_height >= 4 &&
                                 !(m_errmsg.Length() || !m_context.HasContent()) &&
                                 m_context.GetFileSize() > 0);

    // Decide how many hex bytes fit per line.
    InitHexWidth();

    // Process enough lines to display the current screenful of lines.  If
    // processing lines causes the margin width to change, then wrapping and
    // processing may need to be redone.
    ScopedWorkingIndicator working;
    unsigned autofit_retries = 0;
LAutoFitContentWidth:
    assert(autofit_retries != 2); // Should be impossible to occur...
    m_margin_width = CalcMarginWidth();
    m_content_width = m_terminal_width - m_margin_width - show_scrollbar;
    {
        Error e;
        m_context.SetWrapWidth(m_wrap ? m_content_width : 0);
        working.ShowFeedback(m_context.Completed(), m_context.Count(), m_top + m_content_height, this, false/*bytes*/);
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
    update_command_line |= working.NeedsCleanup();

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

    // Compute scrollbar metrics.
    scroll_car scroll_car;
    if (show_scrollbar)
    {
        scroll_car.set_style(c_sbstyle);
        if (m_hex_mode)
        {
            // Use hex line based metrics.
            scroll_car.set_extents(m_content_height, ((m_context.GetFileSize() - 1) / m_hex_width) + 1);
            scroll_car.set_position(m_hex_top / m_hex_width);
        }
        else if (m_context.Completed())
        {
            // Use line based metrics.
            scroll_car.set_extents(m_content_height, m_context.Count());
            scroll_car.set_position(m_top);
        }
        else
        {
            // Otherwise approximate with percentage.
            const double total = double(m_context.GetFileSize());
            const intptr_t i_bottom = m_top + m_content_height - 1;
            const FileOffset offset_bottom = m_context.GetOffset(i_bottom) + m_context.GetLength(i_bottom);
            const FileOffset bytes_per_line = max<FileOffset>(1, offset_bottom / (i_bottom + 1));
            scroll_car.set_extents(m_content_height, intptr_t(total / bytes_per_line));
            scroll_car.set_position(m_top);
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
        s.AppendColor(GetColor(ColorElement::Header));

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
            // There's no scrollbar when showing an error message.
            scroll_car.set_extents(0, 0);

            msg_text = m_errmsg.Length() ? m_errmsg.Text() : c_no_file_open;
            WrapText(msg_text, s2, m_terminal_width);
            msg_text = s2.Text();
            msg_color = GetColor(ColorElement::EndOfFileLine);
            for (size_t row = 0; row < m_content_height; ++row)
            {
                if (*msg_text)
                {
                    const WCHAR* end = StrChr(msg_text, '\n');
                    if (end)
                        ++end;
                    else
                        end = msg_text + StrLen(msg_text);
                    uint32 len_row = uint32(end - msg_text);
                    while (len_row)
                    {
                        if (!IsSpace(msg_text[len_row - 1]))
                            break;
                        --len_row;
                    }
                    const uint32 cells = __wcswidth(msg_text, len_row);
                    if (msg_color)
                        s.AppendColor(msg_color);
                    s.Append(msg_text, len_row);
                    s.AppendNormalIf(msg_color && !*end);
                    if (cells < m_terminal_width)
                        s.Append(c_clreol);
                    s.AppendNormalIf(msg_color && *end);
                    msg_text = end;
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
                    unsigned offset_highlight;
                    const FileOffset tmp_file_offset = GetFoundOffset(m_found_line, &offset_highlight);
                    __translated_found_line.Found(tmp_file_offset + offset_highlight, m_found_line.len);
                    found_line = &__translated_found_line;
                }
            }

            for (unsigned row = 0; row < m_content_height; ++row)
            {
                const uint32 orig_length = s.Length();
                m_context.FormatHexData(m_hex_top, row, m_hex_width, s, e, found_line);

                if (scroll_car.has_car())
                {
                    s.AppendSpaces(m_content_width - cell_count(s.Text() + orig_length));
                    const WCHAR* car = scroll_car.get_char(int32(row), c_floating);
                    if (c_floating)
                    {
                        s.AppendColor(GetColor(ColorElement::FloatingScrollBar));
                    }
                    else
                    {
                        if (car)
                            s.AppendColor(ConvertColorParams(ColorElement::PopupScrollCar, ColorConversion::TextOnly));
                        s.AppendColorOverlay(nullptr, ConvertColorParams(ColorElement::PopupBorder, ColorConversion::TextAsBack));
                    }
                    s.Append(car ? car : L" ");
                    s.Append(c_norm);
                }
                else
                {
                    s.Append(c_clreol);
                }

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
                    const uint32 content_width = m_terminal_width - 1;
                    const unsigned cells = ellipsify_ex(msg_text, content_width, ellipsify_mode::RIGHT, s2, L"");
                    if (msg_color)
                        s.AppendColor(msg_color);
                    s.Append(s2);
                    if (msg_color)
                        s.Append(c_norm);
                    if (cells < content_width)
                    {
                        if (scroll_car.has_car())
                            s.AppendSpaces(content_width - cells);
                        else
                            s.Append(c_clreol);
                    }
                    msg_text = nullptr;
                    msg_color = nullptr;
                }
                else if (m_top + row < m_context.Count())
                {
                    const WCHAR* color = found_line && (m_top + row == found_line->line) ? GetColor(ColorElement::MarkedLine) : nullptr;
                    if (m_margin_width)
                    {
                        s.AppendColor(GetColor(ColorElement::LineNumber));
                        if (s_options.show_line_numbers)
                        {
                            const size_t prev_num = (m_top + row > 0) ? m_context.GetLineNunber(m_top + row - 1) : 0;
                            const size_t num = m_context.GetLineNunber(m_top + row);
                            if (num > prev_num)
                                s.Printf(L"%*lu%s", m_margin_width - 2, m_context.GetLineNunber(m_top + row), c_div_char);
                            else
                                s.Printf(L"%*s%s", m_margin_width - 2, L"", c_div_char);
                        }
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
                        if (!color && !scroll_car.has_car())
                            s.Append(c_clreol);
                        else if (width < m_content_width)
                            s.AppendSpaces(m_content_width - width);
                    }
                    if (color)
                        s.Append(c_norm);
                }
                else if (scroll_car.has_car())
                {
                    s.AppendSpaces(m_content_width);
                }
                else
                {
                    s.Append(c_clreol);
                }

                if (scroll_car.has_car())
                {
                    const WCHAR* car = scroll_car.get_char(int32(row), c_floating);
                    if (c_floating)
                    {
                        s.AppendColor(GetColor(ColorElement::FloatingScrollBar));
                    }
                    else
                    {
                        if (car)
                            s.AppendColor(ConvertColorParams(ColorElement::PopupScrollCar, ColorConversion::TextOnly));
                        s.AppendColorOverlay(nullptr, ConvertColorParams(ColorElement::PopupBorder, ColorConversion::TextAsBack));
                    }
                    s.Append(car ? car : L" ");
                    s.Append(c_norm);
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
                right.Printf(L"    Found: line %lu, len %u", m_found_line.line + 1, m_found_line.len);
            else
                right.Printf(L"    Found: offset %06lx, len %u", m_found_line.offset, m_found_line.len);
        }
        if (m_context.GetCodePage())
            right.Printf(L"    Encoding: %u, %s", m_context.GetCodePage(), m_context.GetEncodingName());
        if (left.Length() + right.Length() > m_terminal_width)
            right.Clear();

        s.Append(left);
        s.AppendSpaces(m_terminal_width - (left.Length() + right.Length()));
        s.Append(right);
        s.Append(c_norm);
    }

    // Command line.
    StrW left;
    if (m_searching)
        left.Append(L"Searching... (Ctrl-Break to cancel)");
    else
        left.Printf(L"Command%s %s", c_prompt_char, m_feedback.Text());
    if (update_command_line)
        MakeCommandLine(s, left.Text());

    if (s.Length())
    {
        OutputConsole(m_hout, c_hide_cursor);
        s.Printf(L"\x1b[%u;%uH", m_terminal_height, cell_count(left.Text()) + 1);
        s.Append(c_norm);
        s.Append(c_show_cursor);
        OutputConsole(m_hout, s.Text(), s.Length());
    }

    m_feedback.Clear();
}

void Viewer::MakeCommandLine(StrW& s, const WCHAR* msg) const
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
    StrW right;
    if (m_multifile_search)
        right.Append(L"    MultiFile");
    right.Printf(L"    %-6s", m_context.GetEncodingName(m_hex_mode));
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
    const uint32 right_width = cell_count(right.Text());

    StrW tmp;
    uint32 msg_width = cell_count(msg);
    if (msg_width >= m_terminal_width)
    {
        bool truncated = false;
        msg_width = ellipsify_ex(msg, m_terminal_width - 1, ellipsify_mode::LEFT, tmp, L"", false, &truncated);
        if (truncated)
            msg = tmp.Text();
    }

    if (msg_width + 3 + right_width > m_terminal_width)
        right.Clear();

    s.Append(msg);
    s.AppendSpaces(m_terminal_width - (msg_width + right_width));
    s.Append(right);

    s.Printf(L"\x1b[%uG", msg_width + 1);
}

void Viewer::InitHexWidth()
{
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
    if (input.type == InputType::Key || input.type == InputType::Mouse)
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
                ScopedWorkingIndicator working;
                working.ShowFeedback(m_context.Completed(), m_context.Processed(), m_context.GetFileSize(), this, true/*bytes*/);
                if (m_context.ProcessToEnd(e))
                {
                    m_top = CountForDisplay();
                    if (m_top > m_content_height)
                        m_top -= m_content_height;
                    else
                        m_top = 0;
                }
                m_force_update_footer |= working.NeedsCleanup();
            }
            break;
        case Key::UP:
key_up:
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
key_down:
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
        case Key::F4:
            m_multifile_search = !m_multifile_search;
            m_force_update_footer = true;
            break;

        case Key::MouseWheel:
            // FUTURE:  Respect the amount (in addition to the direction)?
            // FUTURE:  Acceleration?
            if (input.mouse_wheel_amount < 0)
                goto key_up;
            else if (input.mouse_wheel_amount > 0)
                goto key_down;
            break;
        case Key::MouseLeftClick:
        case Key::MouseLeftDblClick:
            OnLeftClick(input, e);
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

        case 'E'-'@':
            if (input.modifier == Modifier::CTRL)
            {
                ChooseEncoding();
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
                    InitHexWidth();
                    if (m_hex_width)
                    {
                        if (m_found_line.Empty())
                            m_hex_top = m_context.GetOffset(m_top) & ~FileOffset(m_hex_width - 1);
                        else
                            Center(m_found_line);
                    }
                    m_force_update = true;
                }
            }
            else if (input.modifier == Modifier::ALT)
            {
                if (!m_text && m_hex_mode)
                {
                    ++s_options.hex_grouping;
                    if (unsigned(1) << s_options.hex_grouping >= m_hex_width)
                        s_options.hex_grouping = 0;
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

void Viewer::OnLeftClick(const InputRecord& input, Error& e)
{
    // TODO:  Mouse clicks...

    // TODO:  Click on scrollbar.

    // TODO:  Click on file path in header?
    // TODO:  Click on line number (or offset) in header?
    // TODO:  Click on content line?  (Maybe to mark/unmark?)
    // TODO:  Click on Command in footer?
    // TODO:  Click on encoding in footer?
    // TODO:  Click on options in footer?

    // TODO:  Could hover effects be feasible/useful?  (To show clickable spots and tooltips?)
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

void Viewer::SetFile(intptr_t index, ContentCache* context)
{
    assert(context != &m_context);

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
        if (context)
        {
            m_context = std::move(*context);
        }
        else
        {
            Error e;
            m_context.Open((*m_files)[m_index].Text(), e);

            if (e.Test())
            {
                e.Format(m_errmsg);
                m_errmsg.TrimRight();
            }
        }

        if (!m_context.IsPipe())
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
    StrW tmp;
    tmp.Printf(L"Search%s ", c_prompt_char);
    MakeCommandLine(s, tmp.Text());
    OutputConsole(m_hout, s.Text(), s.Length());

// TODO:  make a variant of ReadInput that plays nicely with the Command line.
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

    // TODO:  When should a search start over at the top of the file?

    ClearSignaled();

    assert(!m_searching);
    m_searching = true;
    m_force_update_footer = true;
    UpdateDisplay();

    Error e;
    bool found = (m_hex_mode ?
            m_context.Find(next, m_find.Text(), m_hex_width, m_found_line, m_caseless, e) :
            m_context.Find(next, m_find.Text(), m_found_line, m_caseless, e));
    bool canceled = (e.Code() == E_ABORT);

    if (!found && !canceled && !m_text && m_multifile_search && !m_hex_mode && m_files)
    {
        size_t index = m_index;
        ContentCache ctx(s_options);
        while (!found)
        {
            if (next)
                ++index;
            else
                --index;
            if (index >= m_files->size())
                break;

            Error e;
            ctx.Open((*m_files)[index].Text(), e);

            if (e.Test())
            {
                SetFile(index, &ctx);
                e.Format(m_errmsg);
                ReportError(e);
                m_force_update = true;
                break;
            }

            FoundLine found_line;
            found = ctx.Find(next, m_find.Text(), found_line, m_caseless, e);
            if (e.Code() == E_ABORT)
            {
                SetFile(index, &ctx);
                Center(found_line);
                canceled = true;
                break;
            }

            if (found)
            {
                SetFile(index, &ctx);
                m_found_line = found_line;
            }
        }
    }

    m_searching = false;
    m_force_update_footer = true;
    m_feedback.Clear();

    if (!found)
    {
        m_feedback.Set(canceled ? c_canceled : c_text_not_found);
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
    bool lineno = !m_hex_mode;
    bool done = false;

    auto callback = [&](const InputRecord& input)
    {
        if (input.type != InputType::Char)
            return 0; // Accept.
        if ((input.modifier & ~Modifier::SHIFT) != Modifier::None)
            return 1; // Eat.

        if (input.key_char >= '0' && input.key_char <= '9')
            return 0; // Accept decimal digits for both line number and offset.
        if ((input.key_char >= 'A' && input.key_char <= 'F') || (input.key_char >= 'a' && input.key_char <= 'f'))
            return lineno ? 1 : 0; // Accept hexadecimal digits only for offset.
        if (input.key_char == 'x' || input.key_char == 'X')
            return s.Equal(L"0") ? 0 : 1; // Accept '0x' or '0X' prefix.
        if (input.key_char == '$' || input.key_char == '#')
            return s.Empty() ? 0 : 1; // Accept '$' or '#' prefix.

        if (input.key_char == 'g')
        {
            // 'G' toggles between line number and offset.
            lineno = !lineno;
            done = false;
            s.Clear();
            return -1;
        }

        return 1; // Eat other characters.
    };

    StrW right;
    while (!done)
    {
        if (lineno)
            right = L"Base 10 (use $ or 0x prefix for base 16)";
        else
            right = L"Base 16 (use # prefix for base 10)";

        s.Clear();
        s.AppendColor(GetColor(ColorElement::Command));
        s.Printf(L"\r%s\x1b[%uG%s\r%s%s ", c_clreol, m_terminal_width + 1 - right.Length(), right.Text(), !lineno ? L"Offset" : L"Line #", c_prompt_char);
        OutputConsole(m_hout, s.Text(), s.Length());

        done = true;
        ReadInput(s, 32, callback);

        OutputConsole(m_hout, c_norm);
        if (done)
            m_force_update = true;
    }

    if (s.Length())
    {
        unsigned radix = lineno ? 10 : 16;
        const WCHAR* p = s.Text();
        if (p[0] == '$')
        {
            radix = 16;
            ++p;
        }
        else if (p[0] == '#')
        {
            radix = 10;
            ++p;
        }
        else if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
        {
            radix = 16;
            p += 2;
        }

        if (!lineno)
        {
            FileOffset offset;
            if (wcstonum(p, radix, offset))
            {
                if (m_hex_mode)
                    offset &= ~FileOffset(m_hex_width - 1);
                m_found_line.MarkOffset(offset);
                Center(m_found_line);
                m_force_update = true;
            }
        }
        else
        {
            unsigned __int64 line;
            if (wcstonum(p, radix, line) && line > 0)
            {
                line = m_context.FriendlyLineNumberToIndex(line);
                m_found_line.MarkLine(line);
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

FileOffset Viewer::GetFoundOffset(const FoundLine& found_line, unsigned* offset_highlight)
{
    assert(m_hex_mode);
    assert(!found_line.Empty());
    FileOffset offset = found_line.offset;
    if (found_line.is_line)
    {
        Error e;
        m_context.ProcessThrough(found_line.line, e);
        // TODO:  Do something with the error?
        if (found_line.line >= m_context.Count())
        {
            offset = m_context.GetFileSize();
            if (offset_highlight)
                *offset_highlight = 0;
        }
        else
        {
            const FileOffset highlight = m_context.GetOffset(found_line.line) + found_line.offset;
            offset = highlight & ~FileOffset(m_hex_width - 1);
            if (offset_highlight)
                *offset_highlight = unsigned(highlight - offset);
        }
    }
    else
    {
        const FileOffset highlight = found_line.offset;
        offset = highlight & ~FileOffset(m_hex_width - 1);
        if (offset_highlight)
            *offset_highlight = unsigned(highlight - offset);
    }
    return offset;
}

void Viewer::ShowFileList()
{
    const PopupResult result = ShowPopupList(*m_files, L"Jump to Chosen File", m_index, PopupListFlags::DimPaths);
    m_force_update = true;
    if (!result.canceled)
        SetFile(result.selected);
}

void Viewer::ChooseEncoding()
{
    std::vector<EncodingDefinition> encodings = GetAvailableEncodings();
    std::vector<StrW> names;

    intptr_t index = -1;

    uint32 longest = c_min_popuplist_content_width - 9;
    for (size_t i = 0; i < encodings.size(); ++i)
    {
        const auto& def = encodings[i];
        if (index < 0 && def.codepage == m_context.GetDetectedCodePage())
            index = i;
        names.emplace_back(def.encoding_name);
        longest = max<>(longest, cell_count(def.encoding_name.Text()));
    }
    assert(names.size() == encodings.size());
    if (names.size() == encodings.size())
    {
        StrW tmp;
        for (size_t i = 0; i < names.size(); ++i)
        {
            tmp.Clear();
            tmp.Printf(L"(%u)", encodings[i].codepage);
            names[i].AppendSpaces(longest - cell_count(names[i].Text()));
            names[i].Printf(L"  %7s", tmp.Text());
        }
    }

    encodings.insert(encodings.begin(), { 0, L"Binary File" });
    names.insert(names.begin(), L"Binary File");
    if (index >= 0)
        ++index;

    const PopupResult result = ShowPopupList(names, L"Choose Encoding", index);
    m_force_update = true;
    if (!result.canceled)
        m_context.SetEncoding(encodings[result.selected].codepage);
}

void Viewer::OpenNewFile(Error& e)
{
    StrW s;
    StrW tmp;
    tmp.Printf(L"Enter file to open%s ", c_prompt_char);
    s.AppendColor(GetColor(ColorElement::Command));
    s.Printf(L"\r%s", tmp.Text());
    OutputConsole(m_hout, s.Text(), s.Length());

    ReadInput(s, m_terminal_width - 1 - tmp.Length());

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

