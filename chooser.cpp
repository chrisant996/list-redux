// Copyright (c) 2025 by Christopher Antos
// License: http://opensource.org/licenses/MIT

// vim: set et ts=4 sw=4 cino={0s:

#include "pch.h"
#include "chooser.h"
#include "scan.h"
#include "contentcache.h"
#include "filesys.h"
#include "list_format.h"
#include "input.h"
#include "output.h"
#include "popuplist.h"
#include "colors.h"
#include "ellipsify.h"
#include "ecma48.h"
#include "sorting.h"
#include "help.h"
#include "os.h"

#include <algorithm>

constexpr bool c_floating = true;
constexpr scroll_bar_style c_sbstyle = scroll_bar_style::half_line_chars;

static const WCHAR c_no_files_tagged[] = L"*** No Files Tagged ***";
static const WCHAR c_text_not_found[] = L"*** Text Not Found ***";
static const WCHAR c_canceled[] = L"*** Canceled ***";

enum
{
    ID_PATH,
    ID_FILELIST,
    ID_ONE_ATTR,
};

static void ApplyAttr(DWORD& mask, DWORD& attr, bool& minus, DWORD flag)
{
    mask |= flag;
    if (minus)
        attr &= ~flag;
    else
        attr |= flag;
    minus = false;
}

static bool MkDir(const WCHAR* dir, Error& e)
{
    PathW s;
    s.Set(dir);

    // Bail if there is no parent, or the parent is "" (current dir), or the
    // parent exists.
    if (!s.ToParent() || !s.Length())
        return false;
    const DWORD dw = GetFileAttributesW(s.Text());
    if (dw != 0xffffffff && (dw & FILE_ATTRIBUTE_DIRECTORY))
        return false;

    // Recursively make the directory.
    const bool ret = MkDir(s.Text(), e);
    if (e.Test())
        return ret;
    if (CreateDirectoryW(s.Text(), 0))
        return true;
    if (GetLastError() == ERROR_ALREADY_EXISTS)
        return ret;

    e.Sys();
    return false;
}

static bool RunProgram(const WCHAR* commandline, Error& e)
{
    errno = 0;
    return (_wsystem(commandline) >= 0 || !errno);
}

void MarkedList::Mark(intptr_t index, int tag)
{
    if (tag > 0)
        tag = true;
    else if (tag < 0)
        tag = false;
    else
        tag = !IsMarked(index);

    if (m_reverse)
        tag = !tag;

    if (tag)
    {
        m_set.insert(index);
    }
    else
    {
        const auto iter = m_set.find(index);
        if (iter != m_set.end())
            m_set.erase(iter);
    }
}

bool MarkedList::IsMarked(intptr_t index) const
{
    const auto iter = m_set.find(index);
    bool tag = (iter != m_set.end());
    if (m_reverse)
        tag = !tag;
    return tag;
}

bool MarkedList::AnyMarked() const
{
    return (m_set.size() || m_reverse);
}

bool MarkedList::AllMarked() const
{
    return (!m_set.size() && m_reverse);
}

Chooser::Chooser(const Interactive* interactive)
: m_interactive(interactive)
{
}

void Chooser::Navigate(const WCHAR* dir, std::vector<FileInfo>&& files)
{
    Reset();
    m_dir.Set(dir);
    m_files = std::move(files);
    m_count = intptr_t(m_files.size());
}

void Chooser::Navigate(const WCHAR* dir, Error& e)
{
    StrW dir_out;
    std::vector<FileInfo> fileinfos;

    ScanFiles(1, &dir, fileinfos, dir_out, e);
    if (e.Test())
        return;

    std::stable_sort(fileinfos.begin(), fileinfos.end(), CmpFileInfo);
    Navigate(dir_out.Text(), std::move(fileinfos));
}

ChooserOutcome Chooser::Go(Error& e, bool do_search)
{
    ForceUpdateAll();

    AutoMouseConsoleMode mouse(0, g_options.allow_mouse);

    while (true)
    {
        UpdateDisplay();

        if (do_search)
        {
            do_search = false;
            if (g_options.searcher)
            {
                SearchAndTag(g_options.searcher, e);
                if (e.Test())
                {
                    ReportError(e);
                    ForceUpdateAll();
                }
                UpdateDisplay();
            }
        }

        const InputRecord input = SelectInput(INFINITE, &mouse);
        switch (input.type)
        {
        case InputType::None:
        case InputType::Error:
            continue;

        case InputType::Resize:
            Relayout();
            continue;

        case InputType::Key:
        case InputType::Char:
        case InputType::Mouse:
            {
                e.Clear();
                const ChooserOutcome outcome = HandleInput(input, e);
                if (e.Test())
                {
                    ReportError(e);
                    ForceUpdateAll();
                }
                if (outcome != ChooserOutcome::CONTINUE)
                    return outcome;
            }
            break;
        }
    }
}

void Chooser::Reset()
{
    m_terminal_width = 0;
    m_terminal_height = 0;
    m_content_height = 0;

    m_dir.Clear();
    m_files.clear();
    m_col_widths.clear();
    m_max_size_width = 0;
    m_count = 0;
    m_num_rows = 0;
    m_num_per_row = 0;
    m_visible_rows = 0;
    m_vert_scroll_column = 0;
    m_feedback.Clear();

    m_top = 0;
    m_index = 0;
    m_tagged.Clear();
    m_prev_input.type = InputType::None;
    m_prev_latched = false;
    m_can_drag = false;
    m_can_scrollbar = false;

    ForceUpdateAll();
}

void Chooser::ForceUpdateAll()
{
    m_dirty_header = true;
    m_dirty.MarkAll();
#ifdef INCLUDE_MENU_ROW
    m_dirty_menu = true;
#endif
    m_dirty_footer = true;
    m_prev_visible_rows = uintptr_t(-1) >> 1;
    assert(m_prev_visible_rows > 0);
}

void Chooser::UpdateDisplay()
{
    if (!m_last_feedback.Equal(m_feedback))
        m_dirty_footer = true;

#ifdef INCLUDE_MENU_ROW
    const bool update_menu = (m_dirty_menu && g_options.show_menu);
    m_dirty_menu = false;
#else
    const bool update_menu = false;
#endif

    if (!m_dirty_header &&
        !m_dirty_footer &&
        !m_dirty.AnyMarked() &&
        !update_menu &&
        m_visible_rows >= m_prev_visible_rows)
    {
        return;
    }

    StrW s;
    StrW tmp;
    const WCHAR* const norm = GetColor(ColorElement::File);

    EnsureColumnWidths();
    EnsureTop();
    if (m_top + m_visible_rows > m_num_rows)
        m_top = m_num_rows - m_visible_rows;
    if (m_top < 0)
        m_top = 0;

    const int32 rows = int32(min<intptr_t>(m_visible_rows, m_num_rows));
    m_vert_scroll_car.set_style(c_sbstyle);
    m_vert_scroll_car.set_extents(rows, m_num_rows);
    m_vert_scroll_car.set_position(m_top);
    m_vert_scroll_column = m_vert_scroll_car.has_car() ? m_terminal_width - 1 : 0;

    // Header.
    if (m_dirty_header)
    {
        m_clickable_header.Init(0, m_terminal_width);

        m_clickable_header.Add(L"LIST - ", -1, 100, false);
        m_clickable_header.Add(L"Path: ", ID_PATH, 100, false);
        m_clickable_header.Add(m_dir.Text(), ID_PATH, 100, false, ellipsify_mode::PATH);

#ifdef DEBUG
        tmp.Clear();
        tmp.Printf(L"    (%lu rows, %lu visible)", m_num_rows, m_visible_rows);
        m_clickable_header.Add(tmp.Text(), -1, 5, true);
#endif

        s.Append(L"\x1b[1H");
        m_clickable_header.BuildOutput(s, GetColor(ColorElement::Header));
        m_dirty_header = false;
    }

    // File list.
    if (m_dirty.AnyMarked())
    {
        s.Append(L"\x1b[2H");

        const intptr_t num_add = m_num_rows;
        for (intptr_t ii = 0; ii < m_visible_rows; ii++)
        {
            intptr_t iItem = m_top + ii;
            if (m_dirty.IsMarked(iItem))
            {
                tmp.Clear();
                unsigned row_width = 0;

                for (intptr_t jj = 0; jj < m_num_per_row && iItem < m_count; jj++, iItem += num_add)
                {
                    const FileInfo* pfi = &m_files[iItem];
                    if (jj)
                    {
                        tmp.AppendSpaces(m_padding);
                        row_width += m_padding;
                    }
                    const bool selected = iItem == m_index;
                    const bool tagged = m_tagged.IsMarked(iItem) && !pfi->IsDirectory();
                    row_width += FormatFileInfo(tmp, pfi, m_col_widths[jj], g_options.details, selected, tagged, m_max_size_width);
                }

                if (m_vert_scroll_car.has_car())
                {
                    const WCHAR* car = m_vert_scroll_car.get_char(int32(ii), c_floating);
                    if (!c_floating || car)
                    {
                        // Space was reserved by update_layout() or col_max.
                        const uint32 pad_to = m_terminal_width - 1;
                        if (pad_to >= row_width)
                        {
                            tmp.AppendSpaces(pad_to - row_width);
                            if (c_floating)
                            {
                                tmp.AppendColor(GetColor(ColorElement::FloatingScrollBar));
                            }
                            else
                            {
                                if (car)
                                    tmp.AppendColor(ConvertColorParams(ColorElement::ScrollBarCar, ColorConversion::TextOnly));
                                else
                                    car = L" ";
                                tmp.AppendColorOverlay(nullptr, ConvertColorParams(ColorElement::ScrollBar, ColorConversion::TextAsBack));
                            }
                            tmp.Append(car);                     // ┃ or etc
                            tmp.AppendColor(norm);
                        }
                        row_width = pad_to + 1;
                    }
                }

                assert(row_width <= m_terminal_width);
                if (row_width < m_terminal_width)
                    tmp.Append(c_clreol);

                s.Append(tmp);
            }

            s.Append(L"\n");
        }

        m_dirty.Clear();
    }

    // Empty area.
    if (m_visible_rows < m_prev_visible_rows)
    {
        s.Printf(L"\x1b[%uH", 2 + m_visible_rows);
        s.AppendColor(norm);

        for (intptr_t ii = m_visible_rows + 2; ii < m_terminal_height; ++ii)
        {
            s.Append(c_clreol);
            s.Append(L"\n");
        }
    }

    // Menu row.
#ifdef INCLUDE_MENU_ROW
    if (update_menu)
    {
        StrW menu;
        unsigned width = 0;
        bool stop = false;

        auto add = [&](const WCHAR* key, const WCHAR* desc, bool delimit=true) {
            if (!stop)
            {
                const unsigned old_len = menu.Length();
                if (!menu.Empty())
                    menu.AppendSpaces(2);
                AppendKeyName(menu, key, ColorElement::MenuRow, delimit ? desc : nullptr);
                if (!delimit && desc)
                    menu.Append(desc);
                if (width + cell_count(menu.Text() + old_len) > m_terminal_width)
                {
                    stop = true;
                    menu.SetLength(old_len);
                }
            }
        };

        add(L"F1", L"Help");
        add(L"Enter", L"View");
        add(L"1-4", L"Details");
        add(L"A", L"ChangeAttr");
        add(L"E", L"Edit");
        add(L"R", L"Rename");
        add(L"S", L"Search");
        add(L"T", L"Tag");
        add(L"U", L"Untag");
        add(L"V", L"ViewTagged");
        add(L"Alt-R", L"Run");

        s.Printf(L"\x1b[%uH", m_terminal_height - 1);
        s.AppendColor(GetColor(ColorElement::MenuRow));
        s.Append(c_clreol);
        s.Append(menu);
        s.Append(c_norm);
    }
#endif

    // Command line.
    if (m_dirty_footer)
    {
        m_clickable_footer.Init(m_terminal_height - 1, m_terminal_width);

        tmp.Clear();
        tmp.Printf(L"Files: %lu of %lu", m_index + 1, m_count);
        m_clickable_footer.Add(tmp.Text(), ID_FILELIST, 25, false);
        const int padding = (20 - tmp.Length());
        if (padding > 0)
            m_clickable_footer.Add(nullptr, padding, 25, false);

        if (m_feedback.Length())
        {
            m_clickable_footer.Add(nullptr, 4, 25, false);
            m_clickable_footer.Add(m_feedback.Text(), -1, 100, false);
        }

        if (size_t(m_index) < m_files.size())
        {
            tmp.Clear();
            FormatFileData(tmp, m_files[m_index], true/*include_size*/);
            const WCHAR* after_last_space = tmp.Text();
            for (const WCHAR* p = tmp.Text(); *p; ++p)
            {
                if (*p == ' ')
                    after_last_space = p + 1;
            }
            StrW attrs(after_last_space);
            tmp.SetLength(tmp.Length() - attrs.Length());
            m_clickable_footer.Add(nullptr, 4, 50, true);
            m_clickable_footer.Add(tmp.Text(), -1, 50, true);
            m_clickable_footer.Add(attrs.Text(), ID_ONE_ATTR, 50, true);
        }

        s.Printf(L"\x1b[%uH", m_terminal_height);
        m_clickable_footer.BuildOutput(s, GetColor(ColorElement::Command));
        m_dirty_footer = false;
    }

    if (s.Length())
    {
        unsigned y = 1/*for zero based to one based*/ + 1/*for header row*/;
        unsigned x = 1/*for zero based to one based*/;
        if (m_num_rows)
        {
            y += unsigned((m_index % m_num_rows) - m_top);
            for (size_t ii = m_index / m_num_rows; ii--;)
                x += m_col_widths[ii] + m_padding;
        }

        OutputConsole(c_hide_cursor);
        s.Printf(L"\x1b[%u;%uH", y, x);
        s.Append(c_show_cursor);
        OutputConsole(s.Text(), s.Length());
    }

    m_prev_visible_rows = m_visible_rows;
    m_last_feedback = std::move(m_feedback);
}

void Chooser::Relayout()
{
    m_terminal_width = 0;
    m_terminal_height = 0;
    m_content_height = 0;
    m_vert_scroll_column = 0;
    ForceUpdateAll();
}

void Chooser::EnsureColumnWidths()
{
    const DWORD colsrows = GetConsoleColsRows();
    const unsigned terminal_width = LOWORD(colsrows);
    const unsigned terminal_height = HIWORD(colsrows);
    if (!m_terminal_width || terminal_width != m_terminal_width ||
        !m_terminal_height || terminal_height != m_terminal_height ||
        !m_num_per_row || !m_num_rows || !m_visible_rows)
    {
        unsigned target_width = terminal_width;
        m_terminal_width = terminal_width;
        m_terminal_height = terminal_height;
        m_content_height = terminal_height - 2;
#ifdef INCLUDE_MENU_ROW
        if (g_options.show_menu)
            --m_content_height;
#endif

        m_max_size_width = 0;
        if (g_options.details >= 3 && m_files.size())
        {
            if (m_files[0].IsDirectory())
                m_max_size_width = WidthForDirectorySize(g_options.details);

            for (size_t index = 0; index < m_files.size(); ++index)
            {
                const FileInfo* pfi = &m_files[index];
                const unsigned size_width = WidthForFileInfoSize(pfi, g_options.details, -1);
                m_max_size_width = std::max<unsigned>(m_max_size_width, size_width);
            }
        }

        // First try columns that are the height of the terminal and don't
        // need to scroll.
        {
            size_t rows = m_content_height;
            unsigned width = 0;
            unsigned total_width = 0;
            const size_t last = m_files.size() - 1;

            m_col_widths.clear();
            for (size_t index = 0; index < m_files.size(); ++index)
            {
                width = max<unsigned>(width, WidthForFileInfo(&m_files[index], g_options.details, m_max_size_width));
                if (!--rows || index == last)
                {
                    rows = m_content_height;
                    m_col_widths.emplace_back(width);
                    total_width += width + m_padding;
                    width = 0;
                    if (total_width > target_width)
                    {
                        m_col_widths.clear();
                        break;
                    }
                }
            }

            if (!m_col_widths.empty())
            {
                m_num_per_row = int32(std::max<intptr_t>(1, m_col_widths.size()));
                m_num_rows = std::min<intptr_t>(m_content_height, m_files.size());
                m_visible_rows = int32((terminal_height > 2) ? m_num_rows : 0);
            }
        }

        // If the files didn't all fit, then fit as many columns as possible
        // into the terminal width.
        if (m_col_widths.empty())
        {
            target_width -= 2; // Reserve space for scrollbar.
            m_col_widths = CalculateColumns([this](size_t index){
                return WidthForFileInfo(&m_files[index], g_options.details, m_max_size_width);
            }, m_files.size(), true, m_padding, target_width, target_width / 4);

            m_num_per_row = int32(std::max<intptr_t>(1, m_col_widths.size()));
            m_num_rows = (m_count + m_num_per_row - 1) / m_num_per_row;
            m_visible_rows = int32(std::min<intptr_t>(m_num_rows, (terminal_height > 2) ? terminal_height - 2 : 0));
        }

        if (m_col_widths.size() == 1 && m_col_widths[0] > target_width)
            m_col_widths[0] = target_width;

        ForceUpdateAll();
    }
}

ChooserOutcome Chooser::HandleInput(const InputRecord& input, Error& e)
{
    const InputRecord prev_input = m_prev_input;
    if (prev_input != input)
    {
        if (input.type != InputType::Key || (input.key != Key::DOWN && input.key != Key::RIGHT))
            m_prev_latched = false;
        m_prev_input = input;
    }

    int32 amount = 1;
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
            if (input.modifier == Modifier::None)
            {
                if (ViewHelp(Help::CHOOSER, e) == ViewerOutcome::EXITAPP)
                    return ChooserOutcome::EXITAPP;
                ForceUpdateAll();
            }
            break;
        case Key::F2:
            if (input.modifier == Modifier::None)
            {
                ShowFileList();
            }
            break;
#ifdef INCLUDE_MENU_ROW
        case Key::F10:
            if (input.modifier == Modifier::None)
            {
                g_options.show_menu = !g_options.show_menu;
                Relayout();
            }
            break;
#endif
        case Key::F12:
            ShowOriginalScreen();
            ForceUpdateAll();
            break;

        case Key::ESC:
            if (m_can_drag || m_can_scrollbar)
                break;
            return ChooserOutcome::EXITAPP;

        case Key::ENTER:
viewone:
            if (m_index >= 0 && m_index < m_count)
            {
                const auto& info = m_files[m_index];
                if (info.IsDirectory())
                {
                    PathW dir;
                    info.GetPathName(dir);
                    if (info.IsPseudoDirectory())
                    {
                        const WCHAR* mask = FindName(m_dir.Text());
                        dir.ToParent(); // Strip "..".
                        dir.ToParent(); // Go up to parent.
                        dir.JoinComponent(mask);
                    }

                    Navigate(dir.Text(), e);
                    if (e.Test())
                    {
                        ReportError(e);
                        ForceUpdateAll();
                    }
                }
                else
                {
                    return ChooserOutcome::VIEWONE;
                }
            }
            break;

        case Key::HOME:
            SetIndex(0);
            EnsureTop();
            break;
        case Key::END:
LEnd:
            SetIndex(m_count - 1);
            if (m_num_rows >= m_visible_rows)
                SetTop(m_num_rows - m_visible_rows);
            EnsureTop();
            break;

        case Key::UP:
DoKeyUp:
            for (; amount > 0; --amount)
            {
                if (m_index)
                    SetIndex(m_index - 1);
            }
            EnsureTop();
            break;
        case Key::DOWN:
DoKeyDown:
            for (; amount > 0; --amount)
            {
                if (m_index == m_count - 1)
                    m_prev_latched = true;
LNext:
                if (m_count && m_index < m_count - 1)
                    SetIndex(m_index + 1);
            }
            EnsureTop();
            break;

        case Key::LEFT:
            if (m_count && m_index)
            {
                intptr_t index = m_index - m_num_rows;
                if (index < 0)
                {
                    --index;
                    index += m_num_rows * m_num_per_row;
                    while (index >= m_count)
                        index -= m_num_rows;
                }
                SetIndex(index);
                EnsureTop();
            }
            break;
        case Key::RIGHT:
            if (m_count && !m_prev_latched)
            {
                intptr_t index = m_index;
                if (index + m_num_rows >= m_count && (index + 1) % m_num_rows == 0)
                {
                    index = m_count - 1;
                    m_prev_latched = true;
                }
                else
                {
                    index += m_num_rows;
                    if (index >= m_count)
                        index = (index + 1) % m_num_rows;
                }
                SetIndex(index);
                EnsureTop();
            }
            break;

        case Key::PGUP:
        case Key::PGDN:
            if (m_count)
            {
                const intptr_t y = m_index % m_num_rows;
                const intptr_t rows = m_visible_rows;
                const intptr_t scroll_rows = m_visible_rows - 1;
                if (input.key == Key::PGUP)
                {
                    if (!y)
                    {
                        SetIndex(0);
                    }
                    else
                    {
                        const intptr_t new_y = max<intptr_t>(0, (y <= m_top) ? y - scroll_rows : m_top);
                        SetIndex(m_index + (new_y - y));
                    }
                    EnsureTop();
                }
                else if (input.key == Key::PGDN)
                {
                    if (y == m_num_rows - 1)
                    {
                        SetIndex(m_count - 1);
                    }
                    else if (m_index == m_count - 1)
                    {
                        goto LEnd;
                    }
                    else
                    {
                        const intptr_t new_y = min<intptr_t>(m_num_rows - 1, (y >= m_top + scroll_rows) ? y + scroll_rows : m_top + scroll_rows);
                        intptr_t new_index = m_index + (new_y - y);
                        intptr_t new_top = m_top;
                        if (new_index >= m_count)
                        {
                            new_index = m_count - 1;
                            if (new_index % m_num_rows >= m_top + rows)
                                new_top = min<intptr_t>(new_index % m_num_rows, (m_num_rows - rows) % m_num_rows);
                        }
                        SetIndex(new_index);
                        SetTop(max<intptr_t>(0, new_top));
                    }
                    EnsureTop();
                }
            }
            break;

        case Key::F5:
            if (input.modifier == Modifier::None)
            {
                RefreshDirectoryListing(e);
            }
            break;

        case Key::DEL:
            if (input.modifier == Modifier::None ||
                input.modifier == Modifier::SHIFT)
            {
                const bool recycle = (input.modifier == Modifier::None);
                DeleteEntries(e, recycle);
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

        switch (input.key_char)
        {
        case '?':
            if ((input.modifier & ~(Modifier::SHIFT)) == Modifier::None)
            {
                if (ViewHelp(Help::CHOOSER, e) == ViewerOutcome::EXITAPP)
                    return ChooserOutcome::EXITAPP;
                ForceUpdateAll();
            }
            break;

        case '1':
        case '2':
        case '3':
        case '4':
            if (input.modifier == Modifier::None)
            {
                g_options.details = input.key_char - '1';
                Relayout();
            }
            break;

        case '\'':
        case '@':
            if ((input.modifier & ~(Modifier::SHIFT)) == Modifier::None)
            {
                ShowFileList();
            }
            break;

        case 's':
        case 'S':
            if ((input.modifier & ~(Modifier::SHIFT)) == Modifier::None)
            {
                // TODO:  What should it do in hex mode?
                SearchAndTag(e, input.modifier == Modifier::None/*caseless*/);
            }
            break;
        case '/':
        case '\\':
            if ((input.modifier & ~(Modifier::SHIFT)) == Modifier::None)
            {
                SearchAndTag(e, input.key_char == '\\'/*caseless*/);
            }
            break;

        case '*':
            if ((input.modifier & (Modifier::ALT|Modifier::CTRL)) == Modifier::None)
            {
                RefreshDirectoryListing(e);
            }
            break;
        case 'p':
            if (input.modifier == Modifier::None)
            {
                NewFileMask(e);
            }
            break;
        case '.':
            if (input.modifier == Modifier::None)
            {
                PathW dir(m_dir);
                EnsureTrailingSlash(dir);   // Guarantee trailing slash (just in case).
                dir.ToParent();             // Eats trailing slash.
                dir.ToParent();             // Actually goes up to parent.
                Navigate(dir.Text(), e);
                if (e.Test())
                {
                    ReportError(e);
                    ForceUpdateAll();
                }
            }
            break;

        case 'l':
        case 'v':
            if (input.modifier == Modifier::None)
            {
                if (m_tagged.AnyMarked())
                    return ChooserOutcome::VIEWTAGGED;
                else
                    m_feedback.Set(c_no_files_tagged);
            }
            break;

        case 'a':
            if (input.modifier == Modifier::None)
            {
                ChangeAttributes(e);
            }
            break;
        case 'n':
            if (input.modifier == Modifier::None)
            {
                NewDirectory(e);
            }
            break;
        case 'r':
            if (input.modifier == Modifier::None)
            {
                RenameEntry(e);
            }
            else if (input.modifier == Modifier::ALT)
            {
                RunFile(false/*edit*/, e);
            }
            break;
        case 'w':
            if (input.modifier == Modifier::None)
            {
                SweepFiles(e);
            }
            break;

        case 'e':
            if (input.modifier == Modifier::None)
            {
                RunFile(true/*edit*/, e);
            }
            break;

        case ' ':
            if (input.modifier == Modifier::None)
            {
                if (m_index < m_count && !m_files[m_index].IsDirectory())
                {
                    m_tagged.Mark(m_index, 0);      // Toggle.
                    m_dirty.Mark(m_index % m_num_rows, 1);
                }
                goto LNext;
            }
            break;
        case 't':
            if (input.modifier == Modifier::None)
            {
                if (m_index < m_count && !m_files[m_index].IsDirectory())
                {
                    m_tagged.Mark(m_index, 1);      // Mark.
                    m_dirty.Mark(m_index % m_num_rows, 1);
                }
                goto LNext;
            }
            break;
        case 'u':
            if (input.modifier == Modifier::None)
            {
                if (m_index < m_count && !m_files[m_index].IsDirectory())
                {
                    m_tagged.Mark(m_index, -1);     // Unmark.
                    m_dirty.Mark(m_index % m_num_rows, 1);
                }
                goto LNext;
            }
            break;
        case '\x01':    // CTRL-A
        case '\x14':    // CTRL-T
            if (!m_tagged.AllMarked())
            {
                m_tagged.MarkAll();
                m_dirty.MarkAll();
            }
            break;
        case '\x0e':    // CTRL-N
            m_tagged.Reverse();
            m_dirty.MarkAll();
            break;
        case '\x15':    // CTRL-U
            if (m_tagged.AnyMarked())
            {
                m_tagged.Clear();
                m_dirty.MarkAll();
            }
            break;
        }
    }
    else if (input.type == InputType::Mouse)
    {
        switch (input.key)
        {
        case Key::MouseWheel:
            amount = abs(m_mouse.LinesFromRecord(input));
            if (input.mouse_wheel_amount < 0)
                goto DoKeyUp;
            else
                goto DoKeyDown;
            break;
        case Key::MouseLeftClick:
            m_can_drag = true;
            m_can_scrollbar = (m_vert_scroll_column && input.mouse_pos.X == m_vert_scroll_column && input.mouse_pos.Y >= 1 && unsigned(input.mouse_pos.Y) < 1 + m_content_height);
            __fallthrough;
        case Key::MouseDrag:
        case Key::MouseLeftDblClick:
            if (OnLeftClick(input, e))
                goto viewone;
            break;
        default:
            m_can_drag = false;
            m_can_scrollbar = false;
            break;
        }
    }

    return ChooserOutcome::CONTINUE;
}

StrW Chooser::GetSelectedFile() const
{
    StrW s;
    if (m_index >= 0 && size_t(m_index) < m_files.size())
        m_files[m_index].GetPathName(s);
    return s;
}

std::vector<StrW> Chooser::GetTaggedFiles(intptr_t* num_before_index) const
{
    StrW s;
    std::vector<StrW> files;
    if (num_before_index)
        (*num_before_index) = 0;
    if (m_index < 0)
        num_before_index = nullptr;
    for (size_t i = 0; i < m_files.size(); ++i)
    {
        if (m_tagged.IsMarked(i))
        {
            const auto& file = m_files[i];
            if (!file.IsDirectory())
            {
                m_files[i].GetPathName(s);
                files.emplace_back(std::move(s));
                if (num_before_index && i < size_t(m_index))
                    ++(*num_before_index);
            }
        }
    }
    return files;
}

std::vector<intptr_t> Chooser::GetTaggedIndices(intptr_t* num_before_index) const
{
    StrW s;
    std::vector<intptr_t> indices;
    if (num_before_index)
        (*num_before_index) = 0;
    if (m_index < 0)
        num_before_index = nullptr;
    for (size_t i = 0; i < m_files.size(); ++i)
    {
        if (m_tagged.IsMarked(i))
        {
            const auto& file = m_files[i];
            if (!file.IsDirectory())
            {
                indices.emplace_back(i);
                if (num_before_index && i < size_t(m_index))
                    ++(*num_before_index);
            }
        }
    }
    return indices;
}

void Chooser::SetIndex(intptr_t index)
{
    assert(index >= -1); // Accept -1 because of m_count-1 when m_count==0.
    assert(!index || index < m_count);
    if (index >= m_count)
        index = m_count - 1;
    if (index < 0)
        index = 0;
    if (m_count)
        m_dirty.Mark(m_index % m_num_rows, 1);
    m_index = index;
    if (m_count)
        m_dirty.Mark(m_index % m_num_rows, 1);
    m_dirty_footer = true;
}

void Chooser::SetTop(intptr_t top)
{
    assert(top >= 0);
    assert(!top || top < m_num_rows);
    assert(m_num_rows >= m_visible_rows);

    if (!m_count)
        return;

    if (top != m_top)
    {
        if (top <= m_num_rows - m_visible_rows)
        {
            m_top = top;
            m_dirty.MarkAll();
        }
        else if (m_num_rows > m_visible_rows)
        {
            m_top = m_num_rows - m_visible_rows;
            m_dirty.MarkAll();
        }
    }
    assert(m_top >= 0);
    assert(m_top <= m_num_rows - m_visible_rows);
}

void Chooser::EnsureTop()
{
    if (!m_count)
        return;

    const intptr_t row = (m_index % m_num_rows);
    if (m_top > row)
    {
        SetTop(row);
    }
    else if (m_visible_rows)
    {
        const intptr_t top = max<intptr_t>(0, row - (m_visible_rows - 1));
        if (m_top < top)
            SetTop(top);
    }
}

void Chooser::RefreshDirectoryListing(Error& e)
{
    StrW dir(m_dir);
    Navigate(dir.Text(), e);
}

bool Chooser::AskForConfirmation(const WCHAR* msg)
{
    const WCHAR* const directive = L"Press Y to confirm, or any other key to cancel...";
    // TODO:  ColorElement::Command might not be the most appropriate color.
    const StrW s = MakeMsgBoxText(msg, directive, ColorElement::Command);
    OutputConsole(s.Text(), s.Length());

    bool confirmed = false;
    while (true)
    {
        const InputRecord input = SelectInput();
        switch (input.type)
        {
        case InputType::None:
        case InputType::Error:
            continue;
        case InputType::Resize:
            goto LDone;
        }

        if (input.type == InputType::Char)
        {
            switch (input.key_char)
            {
            case 'y':
            case 'Y':
                confirmed = true;
                goto LDone;
            }
        }

        break;
    }

LDone:
    ForceUpdateAll();
    return confirmed;
}

void Chooser::WaitToContinue(bool erase_after, bool new_line)
{
    StrW msg;
    if (new_line)
        msg.Append(L"\r\n");
    msg.Append(L"Press SPACE or ENTER or ESC to continue...");

    StrW s;
    WrapText(msg.Text(), s);
    s.TrimRight();

    size_t lines = 1;
    for (const WCHAR* walk = s.Text(); *walk;)
    {
        walk = wcschr(walk, '\n');
        if (!walk)
            break;
        ++walk;
        ++lines;
    }

    OutputConsole(s.Text(), s.Length());

    while (true)
    {
        const InputRecord input = SelectInput();
        switch (input.type)
        {
        case InputType::None:
        case InputType::Error:
        case InputType::Resize:
            continue;
        }

        if (input.type == InputType::Key)
        {
            switch (input.key)
            {
            case Key::ENTER:
            case Key::ESC:
                goto LDone;
            }
        }
        else if (input.type == InputType::Char)
        {
            switch (input.key_char)
            {
            case ' ':
                goto LDone;
            }
        }
    }

LDone:
    if (erase_after)
    {
        s.Clear();
        while (lines--)
        {
            s.Append(L"\r\x1b[K");
            if (lines)
                s.Append(L"\x1b[A");
        }
        OutputConsole(s.Text(), s.Length());
    }
    else
    {
        OutputConsole(L"\r\n");
    }
}

bool Chooser::OnLeftClick(const InputRecord& input, Error& e)
{
    // Check for clicks in scrollbar.
    if (m_can_scrollbar)
    {
        if (unsigned(input.mouse_pos.Y) >= 1 && unsigned(input.mouse_pos.Y) < 1 + m_content_height)
        {
            const intptr_t scroll_pos = m_vert_scroll_car.hittest_scrollbar(input, 1);
            if (scroll_pos >= 0)
            {
                SetIndex(scroll_pos);
                SetTop(clamp<intptr_t>(scroll_pos - (m_content_height / 2), 0, m_num_rows));
                return false;
            }
        }
        return false;
    }

    // Check for clicks in file list area.
    if (m_visible_rows > 0 && unsigned(input.mouse_pos.Y - 1) < unsigned(m_visible_rows))
    {
        if (m_can_drag)
        {
            intptr_t index = -1;
            SHORT left = 0;
            for (unsigned i = 0; i < m_col_widths.size(); ++i)
            {
                SHORT width = m_col_widths[i];
                if (input.mouse_pos.X >= left && input.mouse_pos.X < left + width)
                {
                    const int y = input.mouse_pos.Y - 1;
                    index = (i * m_num_rows) + m_top + y;
                    if (size_t(index) < m_files.size())
                    {
                        SetIndex(index);
                        if (input.key == Key::MouseLeftDblClick)
                        {
                            m_can_drag = false;
                            return true;
                        }
                        return false;
                    }
                    break;
                }
                left += m_col_widths[i] + m_padding;
            }
            m_can_drag = false;
        }
        return false;
    }

    // Check for autoscroll
    if (input.key == Key::MouseDrag)
    {
        if (m_can_drag)
        {
            // TODO:  autoscroll
        }
        return false;
    }

    m_can_drag = false;

    // TODO:  Could hover effects be feasible/useful?  (To show clickable spots and tooltips?)

    // Click in header.
    if (input.mouse_pos.Y == 0)
    {
        switch (m_clickable_header.InterpretInput(input))
        {
        case ID_PATH:
            NewFileMask(e);
            break;
        }
        return false;
    }

    // Click in footer.
    if (input.mouse_pos.Y == m_terminal_height - 1)
    {
        switch (m_clickable_footer.InterpretInput(input))
        {
        case ID_FILELIST:
            ShowFileList();
            break;
        case ID_ONE_ATTR:
            ChangeAttributes(e, true/*only_current*/);
            break;
        }
        return false;
    }

    return false;
}

void Chooser::NewFileMask(Error& e)
{
    StrW s;
    s.Printf(L"\x1b[%uH", m_terminal_height);
    s.AppendColor(GetColor(ColorElement::Command));
    s.Printf(L"\r\x1b[KEnter new file mask or path%s ", c_prompt_char);
    OutputConsole(s.Text(), s.Length());

    ReadInput(s, History::FileMask);

    OutputConsole(c_norm);
    ForceUpdateAll();

    s.TrimRight();
    if (s.Empty())
        return;

    const WCHAR* mask = s.Text();
    while (IsSpace(*mask))
        ++mask;

    PathW path(m_dir);
    path.EnsureTrailingSlash(); // Guarantee trailing slash (just in case).
    path.ToParent();            // Eats trailing slash and mask.
    path.JoinComponent(mask);

    const DWORD dwAttr = GetFileAttributesW(path.Text());
// REVIEW:  If the file system is FAT, is it necessary to append "*.*" instead of just "*"?
    if (dwAttr != DWORD(-1) && (dwAttr & FILE_ATTRIBUTE_DIRECTORY))
        path.JoinComponent(L"*");
    else if (!StrChr(mask, '*') && !StrChr(mask, '?'))
        path.JoinComponent(L"*");

    Navigate(path.Text(), e);
}

void Chooser::ChangeAttributes(Error& e, bool only_current)
{
    std::vector<intptr_t> indices;
    const WCHAR* scope = nullptr;
    if (!only_current && m_tagged.AnyMarked())
    {
        indices = GetTaggedIndices();
        scope = L"tagged entries";
    }
    else if (size_t(m_index) < m_files.size() && !m_files[m_index].IsPseudoDirectory())
    {
        indices.emplace_back(m_index);
        scope = L"current entry";
    }
    assert(indices.empty() == !scope);
    if (indices.empty())
        return;

    StrW right;
    right = L"('ashr' to set or '-a-s-h-r' to clear)";

    StrW s;
    s.Printf(L"\x1b[%uH", m_terminal_height);
    s.AppendColor(GetColor(ColorElement::Command));
    s.Printf(L"\r%s\x1b[%uG%s\r%s (%s)%s ", c_clreol, m_terminal_width + 1 - right.Length(), right.Text(), L"Change attributes", scope, c_prompt_char);
    OutputConsole(s.Text(), s.Length());

    ReadInput(s, History::ChangeAttr);

    OutputConsole(c_norm);
    ForceUpdateAll();

    DWORD mask = 0;
    DWORD attr = 0;
    bool minus = false;
    for (const WCHAR* walk = s.Text(); *walk; ++walk)
    {
        switch (*walk)
        {
        case '-':
            minus = true;
            break;
        case '+':
        case ' ':
        case ',':
        case ';':
            minus = false;
            break;
        case 'a':
        case 'A':
            ApplyAttr(mask, attr, minus, FILE_ATTRIBUTE_ARCHIVE);
            break;
        case 's':
        case 'S':
            ApplyAttr(mask, attr, minus, FILE_ATTRIBUTE_SYSTEM);
            break;
        case 'h':
        case 'H':
            ApplyAttr(mask, attr, minus, FILE_ATTRIBUTE_HIDDEN);
            break;
        case 'r':
        case 'R':
            ApplyAttr(mask, attr, minus, FILE_ATTRIBUTE_READONLY);
            break;
        default:
            e.Set(L"Unrecognized input '%1'.") << *walk;
            return;
        }
    }

    if (!mask)
        return;

    StrW path;
    for (const auto& i : indices)
    {
        m_files[i].GetPathName(path);
        const DWORD current = GetFileAttributesW(path.Text());
        if (current == 0xffffffff)
        {
LError:
            e.Sys();
            return;
        }

        const DWORD update = (current & ~mask) | attr;
        if (!SetFileAttributesW(path.Text(), update))
            goto LError;

        m_files[i].UpdateAttributes(update);
    }
}

void Chooser::NewDirectory(Error& e)
{
    StrW s;
    s.Printf(L"\x1b[%uH", m_terminal_height);
    s.AppendColor(GetColor(ColorElement::Command));
    s.Printf(L"\rEnter new directory name%s ", c_prompt_char);
    OutputConsole(s.Text(), s.Length());

    ReadInput(s, History::NewDirectory);

    OutputConsole(c_norm);
    ForceUpdateAll();

    if (!s.Length())
        return;

    PathW dir;
    dir.Set(m_dir);
    dir.ToParent();                     // Strip file mask.
    dir.JoinComponent(s.Text());
    dir.Append(L"\\__dummy__");         // MkDir() makes dirs above filename.

    if (!MkDir(dir.Text(), e))
        return;

    RefreshDirectoryListing(e);
}

void Chooser::RenameEntry(Error& e)
{
    if (size_t(m_index) >= m_files.size())
        return;
    if (m_files[m_index].IsPseudoDirectory())
        return;

    StrW old_name = GetSelectedFile();
    if (old_name.Empty())
        return;

    StrW s;
    s.Printf(L"\x1b[%uH", m_terminal_height);
    s.AppendColor(GetColor(ColorElement::Command));
    s.Printf(L"\rEnter new name%s ", c_prompt_char);
    OutputConsole(s.Text(), s.Length());

    ReadInput(s, History::RenameEntry);

    OutputConsole(c_norm);
    ForceUpdateAll();

    if (!s.Length())
        return;

    const WCHAR* invalid = wcspbrk(s.Text(), L"\\<>|:*?\"");
    if (invalid)
    {
        e.Set(L"Invalid character '%1' in new name.") << *invalid;
        return;
    }

    PathW new_name;
    new_name.Set(m_dir);
    EnsureTrailingSlash(new_name);  // Guarantee trailing slash (just in case).
    new_name.ToParent();            // Remove trailing slash and file mask.
    new_name.JoinComponent(s.Text());

    if (!MoveFileW(old_name.Text(), new_name.Text()))
    {
        e.Sys();
        return;
    }

    RefreshDirectoryListing(e);
}

void Chooser::DeleteEntries(Error& e, bool recycle)
{
    std::vector<StrW> files;
    bool is_dir = false;
    intptr_t num_before_index = 0;

    if (m_tagged.AnyMarked())
    {
        files = GetTaggedFiles(&num_before_index);
    }
    else if (size_t(m_index) < m_files.size() && !m_files[m_index].IsPseudoDirectory())
    {
        files.emplace_back(GetSelectedFile());
        is_dir = m_files[m_index].IsDirectory();
    }

    if (files.empty())
        return;

    StrW msg;
    const WCHAR* opname = recycle ? L"recycle" : L"PERMANENTLY DELETE";
    if (files.size() == 1)
    {
        const WCHAR* file = files[0].Text();
        const WCHAR* name_part = FindName(file);
        if (name_part && *name_part)
            msg.Printf(L"Confirm %s '%s'?", opname, name_part);
    }
    if (msg.Empty())
    {
        const size_t n = files.size();
        msg.Printf(L"Confirm %s %zu item%s?", opname, n, (n == 1) ? L"" : L"s");
    }
    if (!AskForConfirmation(msg.Text()))
        return;

    UpdateDisplay();

    bool any = false;
#ifdef DISALLOW_DESTRUCTIVE_OPERATIONS
    SetLastError(ERROR_ACCESS_DENIED);
    e.Sys(L"(Destructive operations are disallowed.)");
#else
    BOOL ok = true;
    if (recycle)
    {
        const int r = Recycle(files, e);
        if (r >= 0)
        {
            files.clear();
            any = true;
        }
    }
    for (const auto& file : files)
    {
        if (is_dir)
            ok = RemoveDirectoryW(file.Text());
        else
            ok = DeleteFileW(file.Text());
        if (!ok)
        {
            e.Sys();
            const WCHAR* name_part = FindName(file.Text());
            if (name_part && *name_part)
                e.Set(L"Unable to delete '%1'.") << name_part;
            break;
        }
        any = true;
    }
#endif

    if (any)
    {
        Error dummy;
        Error* err = e.Test() ? &dummy : &e; // Don't overwrite e!
        const auto top = m_top;
        const auto index = m_index;
        RefreshDirectoryListing(*err);
        m_top = top;
        m_index = index - num_before_index;
    }
}

void Chooser::RunFile(bool edit, Error& e)
{
    const StrW file = GetSelectedFile().Text();
    if (file.Empty())
        return;

    StrW s;
    if (edit)
    {
        StrW editor;
        if (!OS::GetEnv(L"EDITOR", editor))
            editor = L"notepad.exe";
        s.AppendMaybeQuoted(editor.Text());
        s.Append(L" ");
        s.AppendMaybeQuoted(file.Text());
    }
    else
    {
        bool ok = false;
#ifdef DISALLOW_DESTRUCTIVE_OPERATIONS
        SetLastError(ERROR_ACCESS_DENIED);
        e.Sys(L"(Destructive operations are disallowed.)");
#else
        s.AppendMaybeQuoted(file.Text());
#endif
    }

    if (s.Empty())
        return

    // Clear the current (alternate) screen in case programs switch to it.
    OutputConsole(L"\x1b[J");

    // Swap back to original screen and console modes.
    std::unique_ptr<Interactive> inverted = m_interactive->MakeReverseInteractive();

    StrW msg;
    msg.Printf(L"\r\n%s '%s'...\r\n", edit ? L"Editing" : L"Running", file.Text());
    OutputConsole(msg.Text(), msg.Length());

    RunProgram(s.Text(), e);

    if (!edit)
        WaitToContinue(true/*erase_after*/, true/*new_line*/);

    // Swap back to alternate screen and console modes.
    inverted.reset();

    ForceUpdateAll();
    e.Clear();
}

void Chooser::SweepFiles(Error& e)
{
    std::vector<StrW> files;
    StrW name;
    bool is_dir = false;

    if (m_tagged.AnyMarked())
    {
        files = GetTaggedFiles();
    }
    else if (size_t(m_index) < m_files.size() && !m_files[m_index].IsDirectory())
    {
        name = GetSelectedFile();
        if (name.Empty())
            return;
        files.emplace_back(std::move(name));
    }

    if (files.empty())
        return;

    StrW s;
    StrW program;
    StrW args_before;
    StrW args_after;
    bool ok = true;

    s.Clear();
    s.Printf(L"\x1b[%uH", m_terminal_height);
    s.AppendColor(GetColor(ColorElement::Command));
    s.Printf(L"\rEnter program to run%s ", c_prompt_char);
    OutputConsole(s.Text(), s.Length());
    ReadInput(program, History::SweepProgram);
    OutputConsole(c_norm);
    ForceUpdateAll();

    program.TrimRight();
    if (!program.Length())
        return;

    UpdateDisplay();

    s.Clear();
    s.Printf(L"\x1b[%uH", m_terminal_height);
    s.AppendColor(GetColor(ColorElement::Command));
    s.Printf(L"\rArguments before file name%s ", c_prompt_char);
    OutputConsole(s.Text(), s.Length());
    ok = ReadInput(args_before, History::SweepArgsBefore);
    OutputConsole(c_norm);
    ForceUpdateAll();
    if (!ok)
        return;

    UpdateDisplay();

    s.Clear();
    s.Printf(L"\x1b[%uH", m_terminal_height);
    s.AppendColor(GetColor(ColorElement::Command));
    s.Printf(L"\rArguments after file name%s ", c_prompt_char);
    OutputConsole(s.Text(), s.Length());
    ok = ReadInput(args_after, History::SweepArgsAfter);
    OutputConsole(c_norm);
    ForceUpdateAll();
    if (!ok)
        return;

    // Clear the current (alternate) screen in case programs switch to it.
    OutputConsole(L"\x1b[J");

    // Swap back to original screen and console modes.
    std::unique_ptr<Interactive> inverted = m_interactive->MakeReverseInteractive();

    // Report that it will run commands.
    const StrW sweepdivider = MakeColor(ColorElement::SweepDivider);
    const StrW sweepfile = MakeColor(ColorElement::SweepFile);
    const WCHAR* const c_div = sweepdivider.Text();
    s.Clear();
    s.Printf(L"\r\n%s---- Sweep %zu File(s) ----%s\r\n", c_div, files.size(), c_norm);
    OutputConsole(s.Text(), s.Length());

    bool completed = true;
    size_t errors = 0;
    for (const auto& file : files)
    {
        // Report each file.
        s.Clear();
        s.Printf(L"%s%s%s\r\n", sweepfile.Text(), file.Text(), c_norm);
        OutputConsole(s.Text(), s.Length());

        bool ok = false;
#ifdef DISALLOW_DESTRUCTIVE_OPERATIONS
        SetLastError(ERROR_ACCESS_DENIED);
        e.Sys(L"(Destructive operations are disallowed.)");
#else
        s.Clear();
        s.AppendMaybeQuoted(program.Text());
        if (args_before.Length() > 0)
        {
            s.Append(L" ");
            s.Append(args_before.Text());
        }
        s.Append(L" ");
        s.AppendMaybeQuoted(file.Text());
        if (args_after.Length() > 0)
        {
            s.Append(L" ");
            s.Append(args_after.Text());
        }
        ok = RunProgram(s.Text(), e);
#endif
        if (!ok)
        {
            ++errors;
            e.Set(L"Error running program for '%1'.") << file.Text();
            ok = ReportError(e, ReportErrorFlags::CANABORT|ReportErrorFlags::INLINE);
            e.Clear();
            OutputConsole(L"\r\n");
            if (!ok)
            {
                completed = false;
                break;
            }
        }
    }

    // Report that it finished.
    s.Clear();
    if (!errors)
        s.Printf(L"%s---- Completed ----%s\r\n", c_div, c_norm);
    else if (completed)
        s.Printf(L"%s---- Completed with %zu error(s) ----%s\r\n", c_div, errors, c_norm);
    else
        s.Printf(L"%s---- %zu error(s) ----%s\r\n", c_div, errors, c_norm);
    OutputConsole(s.Text(), s.Length());

    // Wait for ENTER, SPACE, or ESC.
    WaitToContinue(true/*erase_after*/, true/*new_line*/);

    // Swap back to alternate screen and console modes.
    inverted.reset();

    ForceUpdateAll();
    e.Clear();
}

void Chooser::ShowFileList()
{
    StrW tmp;
    std::vector<StrW> files;
    for (const auto& file : m_files)
    {
        FormatFilename(tmp, &file, 0);
        files.emplace_back(std::move(tmp));
    }

    const PopupResult result = ShowPopupList(files, L"Jump to Chosen File", m_index);
    ForceUpdateAll();
    if (!result.canceled)
        SetIndex(result.selected);
}

void Chooser::SearchAndTag(Error& e, bool caseless)
{
    StrW s;
    s.Printf(L"\x1b[%uH", m_terminal_height);
    s.AppendColor(GetColor(ColorElement::Command));
    s.Printf(L"\r\x1b[KSearch%s ", c_prompt_char);
    OutputConsole(s.Text(), s.Length());

    auto searcher = ReadSearchInput(m_terminal_height - 1, m_terminal_width, caseless, false, e);

    OutputConsole(c_norm);
    m_dirty_footer = true;

    if (e.Test())
    {
        ReportError(e);
        ForceUpdateAll();
        return;
    }

    if (searcher)
        SearchAndTag(searcher, e);
}

void Chooser::SearchAndTag(std::shared_ptr<Searcher> searcher, Error& e)
{
    g_options.searcher = searcher;

    StrW s;
    bool canceled = false;
    size_t num_found = 0;
    FoundOffset found_line;
    ContentCache ctx(g_options);
    for (size_t index = 0; index < m_files.size(); ++index)
    {
        if (m_files[index].IsDirectory())
            continue;

        m_files[index].GetPathName(s);
#if 0
        m_searching_file = s.Text();
        m_dirty_footer = true;
        UpdateDisplay();
#endif

        ctx.Open(s.Text(), e);

        if (e.Test())
        {
            ReportError(e);
            ForceUpdateAll();
            break;
        }

        unsigned left_offset = 0;
        const bool found = ctx.Find(true, searcher, 999, found_line, left_offset, e);
        if (e.Code() == E_ABORT)
        {
            canceled = true;
            break;
        }

        if (found)
        {
            ++num_found;
            m_tagged.Mark(index, 1);
            m_dirty.Mark(index % m_num_rows, 1);
        }
    }

    m_dirty_footer = true;

    m_feedback.Clear();
    if (canceled)
        m_feedback = c_canceled;
    else if (e.Test())
        return;
    else if (!num_found)
        m_feedback = c_text_not_found;
    else
    {
        m_feedback.Printf(L"*** Tagged %zu file(s) ***", num_found);
        ForceUpdateAll();
    }
}
