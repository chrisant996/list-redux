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
#include "filetypeconfig.h"
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
static size_t s_goto_line = size_t(-1);
static uint64 s_goto_offset = uint64(-1);
static UINT s_force_codepage = 0;
ViewerOptions g_options;

constexpr unsigned c_horiz_scroll_amount = 10;

uint32 GetMaxMaxLineLength()
{
    return std::min<uint32>(c_data_buffer_slop, c_default_max_line_length);
}

void SetMaxLineLength(const WCHAR* arg)
{
    uint64 n;
    const uint32 c_max_line_length = std::min<uint32>(c_data_buffer_slop, c_default_max_line_length);
    if (ParseULongLong(arg, n) && n <= 0xffff)
        n = clamp<uint32>(uint32(n), 16, c_max_line_length);
    else
        n = c_max_line_length;
    g_options.max_line_length = uint32(n);
}

void SetWrapping(bool wrapping)
{
    g_options.wrapping = wrapping;
}

void SetViewerScrollbar(bool scrollbar)
{
    g_options.show_scrollbar = scrollbar;
}

void SetViewerGotoLine(size_t line)
{
    s_goto_line = line;
    s_goto_offset = size_t(-1);
}

void SetViewerGotoOffset(uint64 offset)
{
    s_goto_line = uint64(-1);
    s_goto_offset = offset;
}

void SetViewerCodePage(UINT cp)
{
    s_force_codepage = cp;
}

// 1 = yes, 0 = no, -1 = cancel.
static int ConfirmSaveChanges()
{
    const WCHAR* const msg = L"Do you want to save your changes to this file?";
    const WCHAR* const directive = L"Press Y to save, N to discard, or any other key to cancel...";
    // TODO:  ColorElement::Command might not be the most appropriate color.
    const StrW s = MakeMsgBoxText(msg, directive, ColorElement::Command);
    OutputConsole(s.Text(), s.Length());

    while (true)
    {
        const InputRecord input = SelectInput();
        switch (input.type)
        {
        case InputType::None:
        case InputType::Error:
            continue;
        // InputType::Resize falls through to the break and return -1.
        }

        if (input.type == InputType::Char)
        {
            switch (input.key_char)
            {
            case 'y':
            case 'Y':
                return 1;
            case 'n':
            case 'N':
                return 0;
            }
        }

        break;
    }

    return -1;
}

static bool ConfirmDiscardBytes()
{
    const WCHAR* const msg = L"Do you want to discard all unsaved changes to this file?";
    const WCHAR* const directive = L"Press Y to discard, or any other key to cancel...";
    // TODO:  ColorElement::Command might not be the most appropriate color.
    const StrW s = MakeMsgBoxText(msg, directive, ColorElement::Command);
    OutputConsole(s.Text(), s.Length());

    while (true)
    {
        const InputRecord input = SelectInput();
        switch (input.type)
        {
        case InputType::None:
        case InputType::Error:
            continue;
        // InputType::Resize falls through to the break and return false.
        }

        if (input.type == InputType::Char)
        {
            switch (input.key_char)
            {
            case 'y':
            case 'Y':
                return true;
            }
        }

        break;
    }

    return false;
}

static bool ConfirmUndoSave()
{
    const WCHAR* const msg = L"Do you want to undo all saved changes to this file?";
    const WCHAR* const directive = L"Press Y to undo, or any other key to cancel...";
    // TODO:  ColorElement::Command might not be the most appropriate color.
    const StrW s = MakeMsgBoxText(msg, directive, ColorElement::Command);
    OutputConsole(s.Text(), s.Length());

    while (true)
    {
        const InputRecord input = SelectInput();
        switch (input.type)
        {
        case InputType::None:
        case InputType::Error:
            continue;
        // InputType::Resize falls through to the break and return false.
        }

        if (input.type == InputType::Char)
        {
            switch (input.key_char)
            {
            case 'y':
            case 'Y':
                return true;
            }
        }

        break;
    }

    return false;
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

    struct ScrollPosition
    {
        FileOffset  top = 0;
        unsigned    left = 0;
        FileOffset  hex_top = 0;
        FileOffset  hex_pos = 0;
    };

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
    void            SetFile(intptr_t index, ContentCache* context=nullptr, bool force=false);
    size_t          CountForDisplay() const;
    void            DoSearch(bool next, bool caseless);
    void            FindNext(bool next=true);
    void            Center(const FoundOffset& found_line);
    void            GoTo(Error& e);
    size_t          GetFoundLineIndex(const FoundOffset& found_line);
    FileOffset      GetFoundOffset(const FoundOffset& found_line, unsigned* offset_highlight=nullptr);
    void            ShowFileList();
    void            ChooseEncoding();
    void            OpenNewFile(Error& e);
    ViewerOutcome   CloseCurrentFile();
    bool            ToggleHexEditMode(Error& e);

private:
    unsigned        m_terminal_width = 0;
    unsigned        m_terminal_height = 0;
    unsigned        m_content_height = 0;
    unsigned        m_content_width = 0;
    unsigned        m_margin_width = 0;
    scroll_car      m_vert_scroll_car;
    MouseHelper     m_mouse;
    int32           m_vert_scroll_column = 0;

    StrW            m_errmsg;

    StrW            m_title;
    const char*     m_text = nullptr;
    const std::vector<StrW>* m_files = nullptr;
    std::map<const WCHAR*, ScrollPosition> m_file_positions;
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

    bool            m_hex_edit = false;
    FileOffset      m_hex_pos = 0;
    bool            m_hex_high_nybble = true;
    bool            m_hex_characters = false;

    bool            m_can_drag = false;
    bool            m_can_scrollbar = false;

    intptr_t        m_last_index = -1;
    size_t          m_last_top = 0;
    size_t          m_last_left = 0;
    StrW            m_last_feedback = 0;
    FileOffset      m_last_hex_top = 0;
    bool            m_last_hex_edit = false;
    FileOffset      m_last_hex_pos = 0;
    bool            m_last_hex_high_nybble = true;
    bool            m_last_hex_characters = false;
    FileOffset      m_last_processed = FileOffset(-1);
    bool            m_last_completed = false;
    bool            m_force_update = false;
    FileOffset      m_force_update_hex_edit_offset = FileOffset(-1);
    bool            m_force_update_header = false;
    bool            m_force_update_footer = false;
    bool            m_searching = false;
    StrW            m_searching_file;

    std::unique_ptr<Searcher> m_searcher;
    bool            m_multifile_search = false;
    FoundOffset     m_found_line;
};

void ScopedWorkingIndicator::ShowFeedback(bool completed, unsigned __int64 processed, unsigned __int64 target, const Viewer* viewer, bool bytes)
{
    const size_t c_threshold = bytes ? 160000 : 5000; // Based on an average of 32 bytes per line.
    if (!m_needs_cleanup && viewer && !completed && processed + c_threshold < target)
    {
        StrW msg;
        const DWORD colsrows = GetConsoleColsRows();
        msg.Printf(L"\x1b[%uH", HIWORD(colsrows));
        viewer->MakeCommandLine(msg, L"Working...");
        OutputConsole(msg.Text(), msg.Length());
        m_needs_cleanup = true;
    }
}

Viewer::Viewer(const char* text, const WCHAR* title)
: m_title(title)
, m_text(text)
, m_context(g_options)
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
: m_files(&files)
, m_context(g_options)
{
    m_hex_mode = g_options.hex_mode;
}

ViewerOutcome Viewer::Go(Error& e)
{
    SetFile(0);

    AutoMouseConsoleMode mouse(0, g_options.allow_mouse);

    while (true)
    {
        e.Clear();

        UpdateDisplay();

        const InputRecord input = SelectInput(INFINITE, &mouse);
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
                if (e.Test())
                {
                    ReportError(e);
                    m_force_update = true;
                }
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
        if (g_options.show_line_numbers)
        {
            s.Printf(L"%lu", m_context.CountFriendlyLines());
            margin = std::max<unsigned>(c_min_margin_width, s.Length() + 2);
        }
        else if (g_options.show_file_offsets)
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

    bool update_command_line = false;

    // Decide terminal dimensions and content height.  Content width can't be
    // decided yet because it may depend on the margin width (which depends on
    // the highest, i.e. widest, file number or file offset).
    const unsigned debug_row = !!g_options.show_debug_info;
#ifdef INCLUDE_MENU_ROW
    const unsigned menu_row = !!g_options.show_menu;
#else
    const unsigned menu_row = false;
#endif
    const unsigned hex_ruler = !!m_hex_mode;
    const DWORD colsrows = GetConsoleColsRows();
    m_terminal_width = LOWORD(colsrows);
    m_terminal_height = HIWORD(colsrows);
    m_content_height = max(int32(m_terminal_height - (1 + hex_ruler + debug_row + menu_row + 1)), 0);
    const bool show_scrollbar = (g_options.show_scrollbar &&
                                 m_content_height >= 4 &&
                                 !(m_errmsg.Length() || !m_context.HasContent()) &&
                                 m_context.GetFileSize() > 0);
    m_vert_scroll_column = (show_scrollbar ? m_terminal_width - 1 : 0);

    // Decide how many hex bytes fit per line.
    InitHexWidth();

    // Honor command line flag to goto line or offset.
    if (s_goto_line != size_t(-1))
    {
        Error dummy;
        if (m_context.ProcessThrough(s_goto_line, dummy))
        {
            const size_t index = m_context.FriendlyLineNumberToIndex(s_goto_line);
            m_found_line.MarkOffset(m_context.GetOffset(index));
            Center(m_found_line);
        }
    }
    else if (s_goto_offset != FileOffset(-1))
    {
        m_found_line.MarkOffset(s_goto_offset);
        Center(m_found_line);
    }
    s_goto_line = size_t(-1);
    s_goto_offset = FileOffset(-1);

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
        if (m_hex_edit)
        {
            if (m_hex_top > m_hex_pos)
                m_hex_top = m_hex_pos - (m_hex_pos % m_hex_width);
            if (m_hex_top + hex_page <= m_hex_pos)
            {
                if (m_hex_pos >= (hex_page - m_hex_width))
                    m_hex_top = m_hex_pos - (m_hex_pos % m_hex_width) - (hex_page - m_hex_width);
                else
                    m_hex_top = 0;
            }
        }
        if (m_hex_top + hex_page > max_hex)
        {
            if (max_hex > hex_page)
                m_hex_top = max_hex - hex_page;
            else
                m_hex_top = 0;
        }
    }
    else
    {
        if (g_options.show_ruler)
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

    // Decide what changed.
    const bool file_changed = (m_last_index != m_index);
    const bool top_changed = (m_hex_mode ? (m_last_hex_top != m_hex_top) : (m_last_top != m_top || m_last_left != m_left));
    const bool pos_changed = (m_hex_edit && m_last_hex_pos != m_hex_pos);
    const bool processed_changed = (m_last_processed != m_context.Processed() || m_last_completed != m_context.Completed());
    const bool feedback_changed = (!m_last_feedback.Equal(m_feedback));
    const bool hex_meta_pos_changed = (m_last_hex_characters != m_hex_characters || m_last_hex_high_nybble != m_hex_high_nybble);

    // Decide what needs to be updated.
    const bool update_header = (m_force_update || m_force_update_header || file_changed || top_changed || pos_changed || processed_changed);
    const bool update_content = (m_force_update || top_changed);
    const bool update_hex_edit = (m_force_update_hex_edit_offset != FileOffset(-1));
    const FileOffset update_hex_edit_offset = m_force_update_hex_edit_offset;
    update_command_line |= (m_force_update || m_force_update_footer || feedback_changed);
    if (!update_header && !update_content && !update_command_line && !hex_meta_pos_changed)
        return;
    const bool update_debug_row = debug_row;
#ifdef INCLUDE_MENU_ROW
    const bool update_menu_row = (menu_row && m_force_update);
#endif

    StrW s;

    // Remember states that influence optimizing what to redraw.
    m_last_top = m_top;
    m_last_left = m_left;
    m_last_hex_top = m_hex_top;
    m_last_hex_edit = m_hex_edit;
    m_last_hex_pos = m_hex_pos;
    m_last_hex_high_nybble = m_hex_high_nybble;
    m_last_hex_characters = m_hex_characters;
    m_last_index = m_index;
    m_last_feedback.Set(m_feedback);
    m_last_processed = m_context.Processed();
    m_last_completed = m_context.Completed();
    m_force_update = false;
    m_force_update_hex_edit_offset = FileOffset(-1);
    m_force_update_header = false;
    m_force_update_footer = false;

    // Compute scrollbar metrics.
    if (show_scrollbar)
    {
        m_vert_scroll_car.set_style(c_sbstyle);
        if (m_hex_mode)
        {
            // Use hex line based metrics.
            m_vert_scroll_car.set_extents(m_content_height, ((m_context.GetFileSize() - 1) / m_hex_width) + 1);
            m_vert_scroll_car.set_position(m_hex_top / m_hex_width);
        }
        else if (m_context.Completed())
        {
            // Use line based metrics.
            m_vert_scroll_car.set_extents(m_content_height, CountForDisplay());
            m_vert_scroll_car.set_position(m_top);
        }
        else
        {
            // Otherwise approximate with percentage.
            const double total = double(m_context.GetFileSize());
            const intptr_t i_bottom = m_top + m_content_height - 1;
            const FileOffset offset_bottom = m_context.GetOffset(i_bottom) + m_context.GetLength(i_bottom);
            const FileOffset bytes_per_line = max<FileOffset>(1, offset_bottom / (i_bottom + 1));
            m_vert_scroll_car.set_extents(m_content_height, intptr_t(total / bytes_per_line));
            m_vert_scroll_car.set_position(m_top);
        }
    }

    // Header.
    if (update_header)
    {
        StrW left;
        StrW right;
        StrW file;
        StrW details;
        const unsigned c_min_filename_width = 16;

        s.Clear();
        s.Append(L"\x1b[1H");
        s.AppendColor(GetColor(ColorElement::Header));

        if (g_options.show_ruler && !m_hex_mode)
        {
            s.AppendSpaces(m_margin_width);
            left.Set(L"\u252c\u252c\u252c\u252c\u253c\u252c\u252c\u252c");
            for (unsigned width = 0; width < m_content_width; width += 10)
            {
                right.Clear();
                right.Printf(L"%u", m_left + width + 10);
                left.SetLength(std::min<unsigned>(10 - right.Length(), m_content_width - width));
                left.Append(right);
                if (width + left.Length() > m_content_width)
                    left.SetLength(m_content_width - width);
                s.Append(left);
            }
            if (m_terminal_width > m_content_width)
                s.Append(c_clreol);
        }
        else
        {
            StrW pos;
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
            if (m_hex_edit)
                pos.Printf(L"    Pos: %06lx (%lu)", m_hex_pos, m_hex_pos);
            if (m_hex_mode)
                right.Printf(L"    Offset: %06lx-%06lx", m_hex_top, bottom_offset);
            else if (g_options.show_file_offsets)
                right.Printf(L"    Offset: %06lx-%06lx", m_context.GetOffset(m_top), bottom_offset);
            else
                right.Printf(L"    Line: %lu", m_top + 1);
            if (g_options.show_file_offsets || m_hex_mode)
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
                if (details_width + right.Length() + pos.Length() + left.Length() + c_min_filename_width <= m_terminal_width)
                {
                    right.AppendSpaces(std::max<unsigned>(details_width, 16) - details_width);
                    right.Append(details);
                }
                else
                {
                    details_width = 0;
                }
            }
#if 0
            if (!details_width)
                PadToWidth(right, 50);
#endif

            if (left.Length() + pos.Length() + right.Length() + c_min_filename_width > m_terminal_width)
                right.Clear();
            if (left.Length() + pos.Length() + c_min_filename_width > m_terminal_width)
                pos.Clear();
            const unsigned limit_len = m_terminal_width - (left.Length() + pos.Length() + right.Length());
            ellipsify_ex(GetCurrentFile().Text(), limit_len, ellipsify_mode::PATH, file);

            s.Append(left);
            s.Append(file);
            if (pos.Length() + right.Length())
            {
                s.AppendSpaces(m_terminal_width - (left.Length() + cell_count(file.Text()) + pos.Length() + right.Length()));
                s.Append(pos);
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
            OutputConsole(c_hide_cursor);
            s.Append(c_show_cursor);
            OutputConsole(s.Text(), s.Length());
            s.Clear();
        }
#endif
    }

    // Content.
    if (update_content || update_hex_edit)
    {
        s.Printf(L"\x1b[%uH", 2);

        StrW s2;
        Error e;
        const WCHAR* msg_text = nullptr;
        const WCHAR* msg_color = nullptr;

        if (m_errmsg.Length() || !m_context.HasContent())
        {
            // There's no scrollbar when showing an error message.
            m_vert_scroll_car.set_extents(0, 0);

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
            if (update_content)
            {
                StrW ruler;
                ruler.AppendColor(GetColor(ColorElement::Header));
                ruler.AppendSpaces(m_context.GetHexOffsetColumnWidth());
                for (unsigned ii = 0; ii < m_hex_width; ++ii)
                {
                    if (ii % (1 << g_options.hex_grouping) == 0)
                        ruler.Append(L"  ", ((ii % 8) == 0) ? 2 : 1);
                    ruler.Printf(L"%02x", ii);
                }
                ruler.AppendSpaces(3);
                for (unsigned ii = 0; ii < m_hex_width; ++ii)
                {
                    ruler.Printf(L"%x", ii & 0xf);
                }
                PadToWidth(ruler, m_terminal_width);
                ruler.Append(c_norm);
                s.Append(ruler);
            }
            else
            {
                s.Append(L"\n");
            }

            const FoundOffset* found_line = m_found_line.Empty() ? nullptr : &m_found_line;
            for (unsigned row = 0; row < m_content_height; ++row)
            {
                if (update_content || (update_hex_edit && m_hex_top + (row * m_hex_width) == update_hex_edit_offset))
                {
                    const uint32 orig_length = s.Length();
                    m_context.FormatHexData(m_hex_top, row, m_hex_width, s, e, found_line);

                    if (m_vert_scroll_car.has_car())
                    {
                        s.AppendSpaces(m_content_width - cell_count(s.Text() + orig_length));
                        const WCHAR* car = m_vert_scroll_car.get_char(int32(row), c_floating);
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
                }

                s.Append(L"\n");
            }
        }
        else
        {
            const FoundOffset* found_line = m_found_line.Empty() ? nullptr : &m_found_line;
            for (size_t row = 0; row < m_content_height; ++row)
            {
                if (g_options.show_endoffile_line && m_top + row == m_context.Count())
                {
                    msg_text = c_endoffile_marker;
                    msg_color = GetColor(ColorElement::EndOfFileLine);
                }

                if (msg_text)
                {
                    s2.Clear();
                    const uint32 content_width = m_terminal_width - !!show_scrollbar;
                    const unsigned cells = ellipsify_ex(msg_text, content_width, ellipsify_mode::RIGHT, s2, L"");
                    if (msg_color)
                        s.AppendColor(msg_color);
                    s.Append(s2);
                    if (msg_color)
                        s.Append(c_norm);
                    if (cells < content_width || show_scrollbar)
                        s.Append(c_clreol);
                    msg_text = nullptr;
                    msg_color = nullptr;
                }
                else if (m_top + row < m_context.Count())
                {
                    const WCHAR* color = nullptr;
                    if (found_line)
                    {
                        const FileOffset row_offset = m_context.GetOffset(m_top + row);
                        const size_t row_length = m_context.GetLength(m_top + row);
                        if (row_offset <= found_line->offset && found_line->offset < row_offset + max<size_t>(1, row_length))
                            color = GetColor(ColorElement::MarkedLine);
                    }
                    if (m_margin_width)
                    {
                        s.AppendColor(GetColor(ColorElement::LineNumber));
                        if (g_options.show_line_numbers)
                        {
                            const size_t prev_num = (m_top + row > 0) ? m_context.GetLineNunber(m_top + row - 1) : 0;
                            const size_t num = m_context.GetLineNunber(m_top + row);
                            if (num > prev_num)
                                s.Printf(L"%*lu%s", m_margin_width - 2, m_context.GetLineNunber(m_top + row), c_div_char);
                            else
                                s.Printf(L"%*s%s", m_margin_width - 2, L"", c_div_char);
                        }
                        else if (g_options.show_file_offsets)
                            s.Printf(L"%0*lx%s", m_margin_width - 2, m_context.GetOffset(m_top + row), c_div_char);
                        else
                            assert(!m_margin_width);
                        s.AppendNormalIf(true);
                        s.Append(L" ");
                    }
                    if (color)
                        s.AppendColor(color);
                    const unsigned width = m_context.FormatLineData(m_top + row, m_left, s, m_content_width, e, color, found_line);
                    if (width < m_content_width || show_scrollbar)
                        s.Append(c_clreol);
                    if (color)
                        s.Append(c_norm);
                }
                else
                {
                    s.Append(c_clreol);
                }

                if (show_scrollbar)
                {
                    const WCHAR* car;
                    s.Printf(L"\x1b[%u;%uH", 2 + row, m_terminal_width);
                    if (m_vert_scroll_car.has_car())
                    {
                        car = m_vert_scroll_car.get_char(int32(row), c_floating);
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
                    }
                    else
                    {
                        car = c_floating ? L" " : L"\u2592";             // ▒
                        s.AppendColor(ConvertColorParams(ColorElement::PopupBorder, ColorConversion::TextOnly));
                    }
                    s.Append(car ? car : L" ");
                    s.Append(c_norm);
                }

                s.Append(L"\n");

#ifdef DEBUG
                if (s_no_accumulate && s.Length())
                {
                    OutputConsole(c_hide_cursor);
                    s.Append(c_show_cursor);
                    OutputConsole(s.Text(), s.Length());
                    s.Clear();
                }
#endif
            }
        }
    }

    // Debug row.
    if (g_options.show_debug_info && update_debug_row)
    {
        s.Printf(L"\x1b[%uH", m_terminal_height - menu_row - debug_row);
        s.AppendColor(GetColor(ColorElement::DebugRow));

        StrW left;
        StrW right;
        if (g_options.show_file_offsets || m_hex_mode)
            left.Printf(L"Buffer: offset %06lx, %x bytes", m_context.GetBufferOffset(), m_context.GetBufferLength());
        else
            left.Printf(L"Buffer: offset %lu, %u bytes", m_context.GetBufferOffset(), m_context.GetBufferLength());
        if (!m_found_line.Empty())
        {
            const size_t index = GetFoundLineIndex(m_found_line);
            const size_t lineno = m_context.GetLineNunber(index);
            right.Printf(L"    Found: ln %lu(%lu), offset %06lx, len %u", lineno, index, m_found_line.offset, m_found_line.len);
        }
        if (m_context.GetCodePage())
            right.Printf(L"    Encoding: %u, %s", m_context.GetCodePage(), m_context.GetEncodingName());
        if (left.Length() + right.Length() > m_terminal_width)
            right.Clear();
        if (left.Length() > m_terminal_width)
        {
            ellipsify(left.Text(), m_terminal_width, right, false);
            left = std::move(right);
        }

        s.Append(left);
        s.AppendSpaces(m_terminal_width - (left.Length() + right.Length()));
        s.Append(right);
        s.Append(c_norm);
    }

    // Menu row.
#ifdef INCLUDE_MENU_ROW
    if (menu_row && update_menu_row)
    {
        StrW menu;
        unsigned width = 0;
        bool stop = false;

        auto add = [&](const WCHAR* key, const WCHAR* desc) {
            if (!stop)
            {
                const unsigned old_len = menu.Length();
                if (!menu.Empty())
                    menu.AppendSpaces(2);
                AppendKeyName(menu, key, ColorElement::MenuRow, desc);
                if (width + cell_count(menu.Text() + old_len) > m_terminal_width)
                {
                    stop = true;
                    menu.SetLength(old_len);
                }
            }
        };

        add(L"F1", L"Help");
        add(L"F3", L"FindNext");
        add(L"Alt-G", L"GoTo");
        if (m_hex_edit)
        {
            add(L"F7/F8", L"Prev/Next");
            add(L"^S", L"Save");
            add(L"^Z", L"Undo");
        }

        s.Printf(L"\x1b[%uH", m_terminal_height - menu_row);
        s.AppendColor(GetColor(ColorElement::MenuRow));
        s.Append(c_clreol);
        s.Append(menu);
        s.Append(c_norm);
    }
#endif

    // Command line.
    StrW left;
    if (m_searching)
        left.Append(L"Searching... (Ctrl-Break to cancel)");
    else
        left.Printf(L"Command%s %s", c_prompt_char, m_feedback.Text());
    if (update_command_line)
    {
        if (m_searching && !m_searching_file.Empty())
        {
            StrW tmp;
            left.AppendSpaces(4);
            // -1 because of how MakeCommandLine works inside.
            const WCHAR* name = FindName(m_searching_file.Text());
            int32 limit = m_terminal_width - 21 - left.Length();
            if (cell_count(name) <= 20 && limit >= 20)
            {
                StrW only_path;
                only_path.Set(m_searching_file.Text(), name - m_searching_file.Text());
                ellipsify_ex(only_path.Text(), limit, ellipsify_mode::PATH, tmp);
                tmp.Append(name);
            }
            else
            {
                limit = m_terminal_width - 1 - left.Length();
                ellipsify_ex(m_searching_file.Text(), limit, ellipsify_mode::PATH, tmp);
            }
            left.Append(tmp.Text());
        }
        MakeCommandLine(s, left.Text());
    }

    if (s.Length() || hex_meta_pos_changed)
    {
        unsigned cursor_y;
        unsigned cursor_x;
        if (m_hex_edit)
        {
            unsigned pos_in_row = (m_hex_pos % m_hex_width);
            cursor_y = 1;                               // One-based.
            cursor_y += 1;                              // Header row.
            cursor_y += 1;                              // Hex ruler.
            cursor_y += unsigned(m_hex_pos - m_hex_top) / m_hex_width;
            cursor_x = 1;                               // One-based.
            cursor_x += m_context.GetHexOffsetColumnWidth();
            cursor_x += 2;                              // Padding.
            if (m_hex_characters)
            {
                cursor_x += m_hex_width * 2;
                cursor_x += (1 << (3 - g_options.hex_grouping)) * (m_hex_width / 8);
                cursor_x += m_hex_width / 8;
                cursor_x += 1;
                cursor_x += pos_in_row;
            }
            else
            {
                cursor_x += pos_in_row * 2;
                cursor_x += pos_in_row / (1 << g_options.hex_grouping);
                cursor_x += pos_in_row / 8;
                cursor_x += !m_hex_high_nybble;
            }
        }
        else
        {
            cursor_y = m_terminal_height;
            cursor_x = cell_count(left.Text()) + 1;
        }

        OutputConsole(c_hide_cursor);
        s.Printf(L"\x1b[%u;%uH", cursor_y, cursor_x);
        s.Append(c_norm);
        s.Append(c_show_cursor);
        OutputConsole(s.Text(), s.Length());
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

    s.Printf(L"\x1b[%uH", m_terminal_height);
    s.AppendColor(GetColor(ColorElement::Command));

    const unsigned offset = s.Length();
    StrW right;
    if (m_multifile_search)
        right.Append(L"    MultiFile");
    right.Printf(L"    %-6s", m_context.GetEncodingName(m_hex_mode));
    if (m_hex_mode)
    {
        right.AppendSpaces(4);
        AppendKeyName(right, L"Alt-E", ColorElement::Command, m_hex_edit ? L"EDITING " : L"EditMode");
    }
    else
    {
        right.Append(L"    Options: ");
        if (!m_text /*&& !m_context.IsBinaryFile()*/)
            right.Append(g_options.show_line_endings ? L"E" : L"e");
        right.Append(g_options.show_line_numbers ? L"N" : L"n");
        if (!m_text)
            right.Append(g_options.show_file_offsets ? L"O" : L"o");
        right.Append(g_options.show_whitespace ? L"S" : L"s");
        right.Append(m_wrap ? L"W" : L"w");
        if (!m_text)
        {
            right.Append(g_options.expand_tabs ? L"T" : L"t");
            right.Append(c_ctrl_indicator[int(g_options.ctrl_mode)]);
        }
        if (!m_text)
           right.Append(g_options.show_ruler ? L"R" : L"r");
#ifdef DEBUG
        right.Append(g_options.show_debug_info ? L"D" : L"d");
#endif
    }
    uint32 right_width = cell_count(right.Text());

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
    {
        right.Clear();
        right_width = 0;
    }

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
        {
            m_hex_mode = false;
// TODO:  This can lead to losing unsaved edits!
            m_hex_edit = false;
        }
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
    int amount = 1;
    AutoCleanup cleanup;

    if (input.type == InputType::Key)
    {
        cleanup.Set([&]() {
            m_can_drag = false;
            m_can_scrollbar = false;
        });

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
#ifdef INCLUDE_MENU_ROW
        case Key::F10:
            if (input.modifier == Modifier::None)
            {
                g_options.show_menu = !g_options.show_menu;
                m_force_update = true;
            }
            break;
#endif

        case Key::ESC:
            if (m_can_drag || m_can_scrollbar)
                break;
            if (m_hex_edit)
            {
                ToggleHexEditMode(e);
                break;
            }
            return ViewerOutcome::RETURN;

        case Key::HOME:
            if (!m_hex_mode)
                m_top = 0;
            else if (input.modifier == Modifier::CTRL)
            {
hex_top:
                m_hex_top = 0;
                m_hex_pos = 0;
                m_hex_high_nybble = true;
            }
            else if (input.modifier == Modifier::None)
            {
                if (!m_hex_edit)
                    goto hex_top;
                m_hex_pos -= m_hex_pos % m_hex_width;
                m_hex_high_nybble = true;
            }
            break;
        case Key::END:
            if (!m_hex_mode)
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
            else if (input.modifier == Modifier::CTRL)
            {
hex_bottom:
                const FileOffset partial = (m_context.GetFileSize() % m_hex_width);
                m_hex_top = m_context.GetFileSize() + (partial ? m_hex_width - partial : 0);
                if (m_hex_top >= m_content_height * m_hex_width)
                    m_hex_top -= m_content_height * m_hex_width;
                else
                    m_hex_top = 0;
                if (m_context.GetFileSize() > 0)
                {
                    m_hex_pos = m_context.GetFileSize() - 1;
                    m_hex_high_nybble = false;
                }
                else
                {
                    m_hex_pos = 0;
                    m_hex_high_nybble = true;
                }
            }
            else if (input.modifier == Modifier::None)
            {
                if (!m_hex_edit)
                    goto hex_bottom;
                if (m_context.GetFileSize() > 0)
                {
                    m_hex_pos -= m_hex_pos % m_hex_width;
                    m_hex_pos += m_hex_width - 1;
                    m_hex_high_nybble = false;
                    if (m_hex_pos >= m_context.GetFileSize())
                        m_hex_pos = m_context.GetFileSize() - 1;
                }
                else
                {
                    m_hex_pos = 0;
                    m_hex_high_nybble = true;
                }
            }
            break;
        case Key::UP:
key_up:
            while (amount-- > 0)
            {
                if (!m_hex_mode)
                {
                    if (m_top)
                        --m_top;
                }
                else if (!m_hex_edit)
                {
                    if (m_hex_top)
                        m_hex_top -= m_hex_width;
                }
                else
                {
                    if (m_hex_pos >= m_hex_width)
                        m_hex_pos -= m_hex_width;
                }
            }
            break;
        case Key::DOWN:
key_down:
            while (amount-- > 0)
            {
                if (!m_hex_mode)
                {
                    if (!m_context.Completed() || m_top + (g_options.show_ruler ? 0 : m_content_height) < CountForDisplay())
                        ++m_top;
                }
                else if (!m_hex_edit)
                {
                    if (m_hex_top + m_content_height * m_hex_width < m_context.GetFileSize())
                        m_hex_top += m_hex_width;
                }
                else
                {
                    if (m_hex_pos + m_hex_width < m_context.GetFileSize())
                        m_hex_pos += m_hex_width;
                    else if (m_context.GetFileSize() > 0)
                        m_hex_pos = m_context.GetFileSize() - 1;
                    else
                    {
                        m_hex_pos = 0;
                        m_hex_high_nybble = true;
                    }
                }
            }
            break;
        case Key::PGUP:
            if (m_hex_mode)
            {
                const FileOffset hex_page = (m_content_height - 1) * m_hex_width;
                if (m_hex_edit)
                {
                    if (m_hex_pos > hex_page)
                        m_hex_pos -= hex_page;
                    else
                    {
                        m_hex_pos = 0;
                        m_hex_high_nybble = true;
                    }
                }
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
                if (m_hex_edit)
                {
                    if (m_hex_pos + hex_page - m_hex_width < m_context.GetFileSize())
                        m_hex_pos += hex_page - m_hex_width;
                    else if (m_context.GetFileSize() > 0)
                        m_hex_pos = m_context.GetFileSize() - 1;
                    else
                    {
                        m_hex_pos = 0;
                        m_hex_high_nybble = true;
                    }
                }
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
            if (!m_hex_mode)
            {
                if (m_left <= c_horiz_scroll_amount)
                    m_left = 0;
                else
                    m_left -= c_horiz_scroll_amount;
            }
            else if (m_hex_edit)
            {
                if (m_hex_characters)
                {
                    if (m_hex_pos > 0)
                        --m_hex_pos;
                }
                else if (m_hex_pos > 0 || !m_hex_high_nybble)
                {
                    m_hex_high_nybble = !m_hex_high_nybble;
                    if (!m_hex_high_nybble)
                        --m_hex_pos;
                }
            }
            break;
        case Key::RIGHT:
            if (!m_hex_mode)
            {
                if (s_max_line_length <= m_content_width)
                    m_left = 0;
                else if (m_left + m_content_width <= s_max_line_length)
                    m_left += c_horiz_scroll_amount;
            }
            else if (m_hex_edit)
            {
hex_edit_right:
                if (m_hex_characters)
                {
                    if (m_hex_pos + 1 < m_context.GetFileSize())
                        ++m_hex_pos;
                }
                else if ((m_hex_pos + 1 < m_context.GetFileSize()) ||
                         (m_hex_high_nybble && m_context.GetFileSize()))
                {
                    m_hex_high_nybble = !m_hex_high_nybble;
                    if (m_hex_high_nybble)
                        ++m_hex_pos;
                }
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
                if (!m_searcher)
                {
                    if (!next && m_found_line.Empty())
                    {
                        // Mark where to start searching.
                        m_found_line.MarkOffset(m_context.GetFileSize());
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
            if (input.modifier == Modifier::None)
            {
                m_multifile_search = !m_multifile_search;
                m_force_update_footer = true;
            }
            break;
        case Key::F5:
            if (input.modifier == Modifier::None)
            {
                if (m_text || m_context.IsPipe())
                {
                    m_context.ClearProcessed();
                    m_force_update = true;
                }
                else
                {
                    SetFile(m_index, nullptr, true/*force*/);
                }
            }
            break;
        case Key::F7:
        case Key::F8:
            if (m_hex_mode && input.modifier == Modifier::None)
            {
                FileOffset offset;
                const bool next = (input.key == Key::F8);
                if (m_context.NextEditedByteRow(m_hex_pos, offset, m_hex_width, next))
                {
                    m_hex_pos = offset;
                    m_hex_high_nybble = next;
                }
            }
            break;

        case Key::TAB:
            if (m_hex_edit)
            {
                m_hex_characters = !m_hex_characters;
                m_hex_high_nybble = true;
            }
            break;
        case Key::BACK:
            if (input.modifier == Modifier::None)
            {
                if (m_hex_edit && m_hex_characters && m_hex_pos)
                {
                    --m_hex_pos;
                    if (m_context.RevertByte(m_hex_pos))
                        m_force_update_hex_edit_offset = m_hex_pos & ~FileOffset(m_hex_width - 1);
                }
            }
            break;
        }
    }
    else if (input.type == InputType::Char)
    {
        cleanup.Set([&]() {
            m_can_drag = false;
            m_can_scrollbar = false;
        });

        if (m_hex_edit)
        {
            if (!m_hex_characters)
            {
                // Interpret hex digits as input.
                switch (input.key_char)
                {
                case '0':   case '1':   case '2':   case '3':   case '4':
                case '5':   case '6':   case '7':   case '8':   case '9':
                    if (input.modifier == Modifier::None)
                    {
                        const BYTE value = input.key_char - '0';
                        m_context.SetByte(m_hex_pos, value, m_hex_high_nybble);
                        m_force_update_hex_edit_offset = m_hex_pos & ~FileOffset(m_hex_width - 1);
                        goto hex_edit_right;
                    }
                    break;
                case 'a':   case 'b':   case 'c':   case 'd':   case 'e':   case 'f':
                case 'A':   case 'B':   case 'C':   case 'D':   case 'E':   case 'F':
                    if ((input.modifier & ~Modifier::SHIFT) == Modifier::None)
                    {
                        const BYTE ten_char = (input.key_char >= 'a' && input.key_char <= 'f') ? 'a' : 'A';
                        const BYTE value = input.key_char - ten_char + 10;
                        m_context.SetByte(m_hex_pos, value, m_hex_high_nybble);
                        m_force_update_hex_edit_offset = m_hex_pos & ~FileOffset(m_hex_width - 1);
                        goto hex_edit_right;
                    }
                    break;
                }
            }
            else if ((input.modifier & ~Modifier::SHIFT) == Modifier::None)
            {
                // Interpret typeable characters as input.
                char multibyte[32];
                BOOL used_default = false;
                const UINT cp = m_context.GetCodePage(m_hex_mode);
                const int len = WideCharToMultiByte(cp, 0, &input.key_char, input.key_char2 ? 2 : 1, multibyte, _countof(multibyte), nullptr, &used_default);
                if (!used_default && len == 1)
                {
                    m_context.SetByte(m_hex_pos, multibyte[0] >> 4, true);
                    m_context.SetByte(m_hex_pos, multibyte[0] & 0xf, false);
                    m_force_update_hex_edit_offset = m_hex_pos & ~FileOffset(m_hex_width - 1);
                    goto hex_edit_right;
                }
            }
        }

        switch (input.key_char)
        {
        case '?':
            if ((input.modifier & ~(Modifier::SHIFT)) == Modifier::None)
            {
                if (!m_text && ViewHelp(Help::VIEWER, e) == ViewerOutcome::EXITAPP)
                {
                    if (!m_hex_edit || ToggleHexEditMode(e))
                        return ViewerOutcome::EXITAPP;
                }
                m_force_update = true;
            }
            break;

        case 'E'-'@':
            if (input.modifier == Modifier::CTRL)
            {
                if (!m_hex_mode && !m_text)
                    ChooseEncoding();
            }
            break;
        case 'N'-'@':   // CTRL-N
            if (input.modifier == Modifier::CTRL)
            {
                if (!m_hex_edit || ToggleHexEditMode(e))
                    SetFile(m_index + 1);
            }
            break;
        case 'P'-'@':   // CTRL-P
            if (input.modifier == Modifier::CTRL)
            {
                if (!m_hex_edit || ToggleHexEditMode(e))
                    SetFile(m_index - 1);
            }
            break;
        case 'S'-'@':   // CTRL-S
            if (input.modifier == Modifier::CTRL)
            {
                if (m_hex_edit && m_context.IsDirty())
                {
                    m_context.SaveBytes(e);
                    m_force_update = true;
                }
            }
            break;
        case 'U'-'@':   // CTRL-U
            if (input.modifier == Modifier::CTRL)
            {
                if (m_hex_edit)
                {
                    if (m_context.RevertByte(m_hex_pos))
                        m_force_update_hex_edit_offset = m_hex_pos & ~FileOffset(m_hex_width - 1);
                    goto hex_edit_right;
                }
            }
            break;
        case 'Z'-'@':   // CTRL-Z
            if (input.modifier == Modifier::CTRL)
            {
                if (m_hex_edit)
                {
                    if (m_context.IsDirty())
                    {
                        m_force_update = true;
                        if (ConfirmDiscardBytes())
                            m_context.DiscardBytes();
                    }
                    else if (m_context.IsSaved())
                    {
                        m_force_update = true;
                        if (ConfirmUndoSave())
                            m_context.UndoSave(e);
                    }
                }
            }
            break;

        case '\'':
        case '@':
            if ((input.modifier & ~(Modifier::SHIFT|Modifier::ALT)) == Modifier::None)
            {
                ShowFileList();
            }
            break;

        case 'a':
            if (input.modifier == Modifier::ALT)
            {
                if (m_hex_mode)
                {
                    g_options.ascii_filter = !g_options.ascii_filter;
                    m_force_update = true;
                }
            }
            break;
        case 'c':
            if (input.modifier == Modifier::ALT)
            {
                if (!m_hex_edit || ToggleHexEditMode(e))
                    return CloseCurrentFile();
            }
            else if (input.modifier != Modifier::None)
            {
                break;
            }
            __fallthrough;
        case '^':
            if ((input.modifier & ~Modifier::SHIFT) == Modifier::None)
            {
                if (!m_hex_mode && !m_text)
                {
                    g_options.ctrl_mode = CtrlMode((int(g_options.ctrl_mode) + 1) % int(CtrlMode::__MAX));
                    m_context.ClearProcessed();
                    m_force_update = true;
                }
            }
            break;
        case 'd':
            if (input.modifier == Modifier::ALT)
            {
                g_options.show_debug_info = !g_options.show_debug_info;
                m_force_update = true;
            }
            break;
        case 'e':
            if (input.modifier == Modifier::ALT)
            {
                ToggleHexEditMode(e);
            }
            else if (input.modifier == Modifier::None)
            {
                if (!m_hex_mode)
                {
                    g_options.show_line_endings = !g_options.show_line_endings;
                    m_force_update = true;
                }
            }
            break;
        case 'g':
            if ((input.modifier & ~Modifier::ALT) == Modifier::None)
            {
                GoTo(e);
            }
            break;
        case 'h':
            if (input.modifier == Modifier::None)
            {
                if (!m_text && !m_hex_edit)
                {
                    m_hex_mode = !m_hex_mode;
                    g_options.hex_mode = m_hex_mode;
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
                    ++g_options.hex_grouping;
                    if (unsigned(1) << g_options.hex_grouping >= m_hex_width)
                        g_options.hex_grouping = 0;
                    m_force_update = true;
                }
            }
            break;
        case 'j':
            if ((input.modifier & ~Modifier::ALT) == Modifier::None)
            {
                if (!m_found_line.Empty())
                    Center(m_found_line);
            }
            break;
        case 'm':
            if ((input.modifier & ~Modifier::ALT) == Modifier::None)
            {
                if (!m_hex_mode)
                    m_found_line.MarkOffset(m_context.GetOffset(m_top + (std::min<size_t>(m_content_height, m_context.Count()) / 2)));
                else if (!m_hex_edit)
                    m_found_line.MarkOffset(std::min<FileOffset>(m_hex_top + (m_content_height / 2) * m_hex_width, m_context.GetFileSize() / 2));
                else
                    m_found_line.MarkOffset(m_hex_pos);
                m_force_update = true;
            }
            break;
        case 'n':
            if (input.modifier == Modifier::None)
            {
                if (!m_hex_mode)
                {
                    g_options.show_line_numbers = !g_options.show_line_numbers;
                    g_options.show_file_offsets = false;
                    m_force_update = true;
                }
            }
            break;
        case 'o':
            if (input.modifier == Modifier::None)
            {
                if (!m_hex_mode && !m_text)
                {
                    g_options.show_file_offsets = !g_options.show_file_offsets;
                    g_options.show_line_numbers = false;
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
                    g_options.show_ruler = !g_options.show_ruler;
                    m_force_update_header = true;
                }
            }
            break;
        case ' ':
            if (input.modifier == Modifier::None)
            {
                if (!m_hex_mode)
                {
                    g_options.show_whitespace = !g_options.show_whitespace;
                    m_force_update = true;
                }
            }
            break;
        case 't':
            if (input.modifier == Modifier::None)
            {
                if (!m_hex_mode && !m_text)
                {
                    g_options.expand_tabs = !g_options.expand_tabs;
                    m_context.ClearProcessed();
                    m_force_update = true;
                }
            }
            break;
        case 'u':
            if ((input.modifier & ~Modifier::ALT) == Modifier::None)
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
                    g_options.wrapping = !g_options.wrapping;
                    m_wrap = g_options.wrapping;
                    m_force_update = true;
                }
            }
            break;

        case 's':
        case 'S':
            if ((input.modifier & ~(Modifier::SHIFT|Modifier::ALT)) == Modifier::None)
            {
                // TODO:  What should it do in hex mode?
                DoSearch(true, (input.modifier & Modifier::SHIFT) == Modifier::None/*caseless*/);
            }
            break;
        case '/':
        case '\\':
            if ((input.modifier & ~(Modifier::SHIFT|Modifier::ALT)) == Modifier::None)
            {
                // TODO:  What should it do in hex mode?
                DoSearch(true, input.key_char == '\\'/*caseless*/);
            }
            break;
        }
    }
    else if (input.type == InputType::Mouse)
    {
        switch (input.key)
        {
        case Key::MouseWheel:
            if (input.mouse_wheel_amount < 0)
            {
                amount = -m_mouse.LinesFromRecord(input);
                goto key_up;
            }
            else if (input.mouse_wheel_amount > 0)
            {
                amount = m_mouse.LinesFromRecord(input);
                goto key_down;
            }
            break;
        case Key::MouseLeftClick:
        case Key::MouseLeftDblClick:
            m_can_drag = true;
            m_can_scrollbar = (m_vert_scroll_column && input.mouse_pos.X == m_vert_scroll_column && input.mouse_pos.Y >= 1 + !!m_hex_mode && unsigned(input.mouse_pos.Y) < 1 + !!m_hex_mode + m_content_height);
            __fallthrough;
        case Key::MouseDrag:
            OnLeftClick(input, e);
            break;
        case Key::MouseRightClick:
            m_can_drag = false;
            m_can_scrollbar = false;
            break;
        }
    }

    return ViewerOutcome::CONTINUE;
}

void Viewer::OnLeftClick(const InputRecord& input, Error& e)
{
    const uint32 content_top = 1 + !!m_hex_mode;

    // Clink in scrollbar.
    if (m_can_scrollbar)
    {
        if (m_can_scrollbar)
        {
            const intptr_t scroll_pos = m_vert_scroll_car.hittest_scrollbar(input, content_top);
            if (scroll_pos >= 0)
            {
                FoundOffset found;
                if (m_hex_mode)
                {
                    found.MarkOffset(scroll_pos * m_hex_width);
                }
                else
                {
                    if (!m_context.ProcessThrough(scroll_pos, e))
                        return;
                    if (m_context.Count() > 0)
                        found.MarkOffset(m_context.GetOffset(min<size_t>(scroll_pos, m_context.Count() - 1)));
                }
                Center(found);
            }
        }
        return;
    }

    // Click in content area.
    if (unsigned(input.mouse_pos.Y) >= content_top && unsigned(input.mouse_pos.Y) < content_top + m_content_height)
    {
        if (m_can_drag && m_hex_edit)
        {
            const FileOffset y_ofs = m_hex_top + ((input.mouse_pos.Y - content_top) * m_hex_width);

            const uint32 hex_left = m_context.GetHexOffsetColumnWidth() + 2;
            uint32 chars_left = hex_left;
            chars_left += m_hex_width * 2;
            chars_left += (1 << (3 - g_options.hex_grouping)) * (m_hex_width / 8);
            chars_left += m_hex_width / 8;
            chars_left += 1;

            if (uint32(input.mouse_pos.X) >= chars_left && uint32(input.mouse_pos.X) < chars_left + m_hex_width)
            {
                const FileOffset pos = y_ofs + input.mouse_pos.X - chars_left;
                if (pos >= 0 && pos < m_context.GetFileSize())
                {
                    m_hex_pos = pos;
                    m_hex_characters = true;
                }
            }
            else if (uint32(input.mouse_pos.X) >= hex_left && uint32(input.mouse_pos.X) < chars_left)
            {
                FileOffset pos = y_ofs;
                int32 x = input.mouse_pos.X - hex_left;
                for (uint32 ii = 0; ii < m_hex_width;)
                {
                    if (x == 0 || x == 1)
                    {
                        if (pos < m_context.GetFileSize())
                        {
                            m_hex_pos = pos;
                            m_hex_high_nybble = (x == 0);
                            m_hex_characters = false;
                            break;
                        }
                    }

                    ++ii;
                    x -= 2;
                    if (ii % (1 << g_options.hex_grouping) == 0)
                        x -= (((ii % 8) == 0) ? 2 : 1);

                    ++pos;
                }
            }
        }
        return;
    }
    else if (input.key == Key::MouseDrag)
    {
        if (m_can_drag && m_hex_edit)
        {
            // TODO:  autoscroll
        }
    }

    // TODO:  Click on file path in header?
    // TODO:  Click on line number (or offset) in header?
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

        // Rebuild the file positions list.
        std::map<const WCHAR*, ScrollPosition> alt_positions;
        for (size_t i = 0; i < m_files->size(); ++i)
        {
            auto& fpos = m_file_positions.find((*m_files)[i].Text());
            if (fpos != m_file_positions.end())
                alt_positions.emplace(m_alt_files[i].Text(), fpos->second);
        }

        // Switch to using the modifiable list.
        m_files = &m_alt_files;
        m_file_positions = std::move(alt_positions);
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

void Viewer::SetFile(intptr_t index, ContentCache* context, bool force)
{
    assert(context != &m_context);

    if (m_text)
        return;

    assert(m_files);
    if (index > 0 && size_t(index) >= m_files->size())
        index = m_files->size() - 1;
    if (index < 0)
        index = 0;

    if (index == m_index && !force)
        return;

    if (m_index >= 0)
    {
        auto& oldfpos = m_file_positions.find((*m_files)[m_index].Text());
        if (oldfpos != m_file_positions.end())
        {
            oldfpos->second.top = m_top;
            oldfpos->second.left = m_left;
            oldfpos->second.hex_top = m_hex_top;
            oldfpos->second.hex_pos = m_hex_pos;
        }
    }

    auto& newfpos = m_file_positions.find((*m_files)[index].Text());
    if (newfpos != m_file_positions.end())
    {
        m_top = newfpos->second.top;
        m_left = newfpos->second.left;
        m_hex_top = newfpos->second.hex_top;
        m_hex_pos = newfpos->second.hex_pos;
    }
    else
    {
        m_file_positions.emplace((*m_files)[index].Text(), ScrollPosition());
        m_top = 0;
        m_left = 0;
        m_hex_top = 0;
        m_hex_pos = 0;
    }

    m_errmsg.Clear();
    m_index = index;
    m_hex_edit = false;
    m_hex_high_nybble = true;
    m_can_drag = false;
    m_can_scrollbar = false;
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

        if (s_force_codepage)
        {
            m_context.SetEncoding(s_force_codepage);
            s_force_codepage = 0;
        }

        if (!m_context.IsPipe())
        {
            SHFind sh = FindFirstFileW((*m_files)[m_index].Text(), &m_fd);
            if (sh.Empty())
                ZeroMemory(&m_fd, sizeof(m_fd));
        }

        ApplyFileTypeConfig((*m_files)[m_index].Text(), g_options);
        m_mouse.AllowAcceleration(true);

        m_wrap = g_options.wrapping;
    }
}

size_t Viewer::CountForDisplay() const
{
    return m_context.Count() + g_options.show_endoffile_line;
}

void Viewer::DoSearch(bool next, bool caseless)
{
    StrW s;
    StrW tmp;
    tmp.Printf(L"Search%s ", c_prompt_char);
    MakeCommandLine(s, tmp.Text());
    OutputConsole(s.Text(), s.Length());

    Error e;
    auto searcher = ReadSearchInput(m_terminal_width, caseless, false, e);

    OutputConsole(c_norm);
    m_force_update = true;

    if (e.Test())
    {
        ReportError(e);
        return;
    }

    if (!searcher)
        return;

    m_searcher = std::move(searcher);
    m_found_line.Clear();
    FindNext(next);
}

void Viewer::FindNext(bool next)
{
    assert(m_searcher);

    // TODO:  When should a search start over at the top of the file?

    ClearSignaled();

    assert(!m_searching);
    m_searching = true;
    m_searching_file.Clear();
    m_force_update_footer = true;
    UpdateDisplay();

    Error e;
    unsigned left_offset = m_left;
    bool found = (m_hex_mode ?
            m_context.Find(next, m_searcher, m_hex_width, m_found_line, e) :
            m_context.Find(next, m_searcher, m_content_width, m_found_line, left_offset, e));
    bool canceled = (e.Code() == E_ABORT);

    if (!found && !canceled && !m_text && m_multifile_search && m_files)
    {
        size_t index = m_index;
        ContentCache ctx(g_options);
        while (!found)
        {
            if (next)
                ++index;
            else
                --index;
            if (index >= m_files->size())
                break;

            m_searching_file = (*m_files)[index].Text();
            m_force_update_footer = true;
            UpdateDisplay();

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

            FoundOffset found_line;
            ctx.SetWrapWidth(m_wrap ? m_content_width : 0);
            found = (m_hex_mode ?
                    ctx.Find(next, m_searcher, m_hex_width, found_line, e) :
                    ctx.Find(next, m_searcher, m_content_width, found_line, left_offset, e));
            if (e.Code() == E_ABORT)
            {
                SetFile(index, &ctx);
                Center(found_line);
                if (!m_hex_mode)
                    m_left = left_offset;
                canceled = true;
                assert(!found);
                found = false;
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
    m_searching_file.Clear();
    m_force_update_footer = true;
    m_feedback.Clear();

    if (!found)
    {
        m_feedback.Set(canceled ? c_canceled : c_text_not_found);
    }
    else
    {
        Center(m_found_line);
        if (!m_hex_mode)
            m_left = left_offset;
        m_force_update = true;
    }
}

void Viewer::Center(const FoundOffset& found_line)
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
        m_hex_pos = found_line.offset;
        m_hex_high_nybble = true;
    }
    else
    {
        const size_t line = GetFoundLineIndex(found_line);
        const unsigned center = m_content_height / 2;
        if (line >= center)
            m_top = line - center;
        else
            m_top = 0;
    }
}

void Viewer::GoTo(Error& e)
{
    StrW s;
    bool lineno = !m_hex_mode;
    bool done = false;

    auto callback = [&](const InputRecord& input)
    {
        if (input.type != InputType::Char)
            return 0; // Accept.
        if ((input.modifier & ~Modifier::SHIFT) == Modifier::None)
        {
            if (input.key_char >= '0' && input.key_char <= '9')
                return 0; // Accept decimal digits for both line number and offset.
            if ((input.key_char >= 'A' && input.key_char <= 'F') || (input.key_char >= 'a' && input.key_char <= 'f'))
                return lineno ? 1 : 0; // Accept hexadecimal digits only for offset.
            if (input.key_char == 'x' || input.key_char == 'X')
                return s.Equal(L"0") ? 0 : 1; // Accept '0x' or '0X' prefix.
            if (input.key_char == '$' || input.key_char == '#')
                return s.Empty() ? 0 : 1; // Accept '$' or '#' prefix.
        }
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
        OutputConsole(s.Text(), s.Length());

        done = true;
        ReadInput(s, History::Goto, 32, 32, callback);

        OutputConsole(c_norm);
        if (done)
            m_force_update = true;
    }

    if (s.Length())
    {
        ULONGLONG n;
        const unsigned radix = lineno ? 10 : 16;
        if (ParseULongLong(s.Text(), n, radix))
        {
            if (!lineno)
            {
                m_found_line.MarkOffset(n);
            }
            else
            {
                m_context.ProcessThrough(n, e);
                if (e.Test())
                    return;
                size_t line = m_context.FriendlyLineNumberToIndex(n);
                m_found_line.MarkOffset(m_context.GetOffset(line));
            }
            Center(m_found_line);
            m_force_update = true;
        }
    }
}

size_t Viewer::GetFoundLineIndex(const FoundOffset& found_line)
{
    assert(!found_line.Empty());
    size_t line = 0;
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
    return line;
}

FileOffset Viewer::GetFoundOffset(const FoundOffset& found_line, unsigned* offset_highlight)
{
    assert(m_hex_mode);
    assert(!found_line.Empty());
    FileOffset offset = found_line.offset;
    const FileOffset highlight = found_line.offset;
    offset = highlight & ~FileOffset(m_hex_width - 1);
    if (offset_highlight)
        *offset_highlight = unsigned(highlight - offset);
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

    encodings.insert(encodings.begin(), { 0, L"Binary File" });

    uint32 longest = c_min_popuplist_content_width - (2 + 9);
    for (size_t i = 0; i < encodings.size(); ++i)
    {
        const auto& enc = encodings[i];
        if (index < 0 && (enc.codepage == m_context.GetCodePage() ||
                          !enc.codepage && m_context.IsBinaryFile()))
            index = i;
        names.emplace_back(enc.encoding_name);
        longest = max<>(longest, cell_count(enc.encoding_name.Text()));
    }
    assert(names.size() == encodings.size());
    if (names.size() == encodings.size())
    {
        StrW tmp;
        for (size_t i = 0; i < names.size(); ++i)
        {
            const auto& enc = encodings[i];
            auto& name = names[i];
            const bool star = (enc.codepage == m_context.GetDetectedCodePage() ||
                               !enc.codepage && m_context.IsDetectedBinaryFile());
            tmp.Set(i == index ? L"> " : (star ? L"* " : L"  "));       // 2
            tmp.Append(name);
            tmp.AppendSpaces(2 + longest - cell_count(tmp.Text()));
            name = std::move(tmp);
            assert(tmp.Empty());
            tmp.Printf(L"(%u)", enc.codepage);
            name.Printf(L"  %7s", tmp.Text());                          // 9
        }
    }

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
    OutputConsole(s.Text(), s.Length());

    ReadInput(s, History::OpenFile, m_terminal_width - 1 - tmp.Length());

    OutputConsole(c_norm);

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

    m_file_positions.erase((*m_files)[m_index].Text());

    EnsureAltFiles();
    m_alt_files.erase(m_alt_files.begin() + m_index);

    const auto index = m_index;
    m_index = -2;
    SetFile(index);
    return ViewerOutcome::CONTINUE;
}

bool Viewer::ToggleHexEditMode(Error& e)
{
    if (!m_hex_mode || m_text || m_context.IsPipe())
        return false;

    if (m_hex_edit && m_context.IsDirty())
    {
        const int confirm = ConfirmSaveChanges();
        m_force_update = true;
        if (confirm < 0)
            return false;
        else if (confirm == 0)
            m_context.DiscardBytes();
        else if (!m_context.SaveBytes(e))
            return false;
    }

    m_hex_edit = !m_hex_edit;
    m_force_update_footer = true;
    return true;
}

ViewerOutcome ViewFiles(const std::vector<StrW>& files, StrW& dir, Error& e)
{
    Viewer viewer(files);

    const ViewerOutcome outcome = viewer.Go(e);

    dir = viewer.GetCurrentFile();
    dir.SetEnd(FindName(dir.Text()));

    return outcome;
}

ViewerOutcome ViewText(const char* text, Error& e, const WCHAR* title, bool help)
{
    ViewerOptions old = g_options;
    g_options = ViewerOptions();
    g_options.ctrl_mode = CtrlMode::OEM437;
    g_options.expand_tabs = true;
    g_options.show_whitespace = false;
    g_options.show_line_numbers = false;
    g_options.show_file_offsets = false;
    g_options.show_ruler = false;
    g_options.show_endoffile_line = true;
    g_options.show_debug_info = false;

    if (help)
    {
        g_options.internal_help_mode = true;
        g_options.hanging_extra = 0;
    }

    Viewer viewer(text, title);
    ViewerOutcome ret = viewer.Go(e);

    g_options = old;
    return ret;
}

