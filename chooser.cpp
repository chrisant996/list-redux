// Copyright (c) 2025 by Christopher Antos
// License: http://opensource.org/licenses/MIT

// vim: set et ts=4 sw=4 cino={0s:

#include "pch.h"
#include "chooser.h"
#include "scan.h"
#include "list_format.h"
#include "input.h"
#include "output.h"
#include "popuplist.h"
#include "colors.h"
#include "ellipsify.h"
#include "ecma48.h"
#include "sorting.h"
#include "scroll_car.h"
#include "help.h"
#include "os.h"

#include <algorithm>

constexpr bool c_floating = true;
constexpr scroll_bar_style c_sbstyle = scroll_bar_style::half_line_chars;

static const WCHAR c_clreol[] = L"\x1b[K";

static const WCHAR c_no_files_tagged[] = L"*** No Files Tagged ***";

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
: m_hout(GetStdHandle(STD_OUTPUT_HANDLE))
, m_interactive(interactive)
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

ChooserOutcome Chooser::Go(Error& e)
{
    ForceUpdateAll();

    while (true)
    {
        UpdateDisplay();

        const InputRecord input = SelectInput();
        switch (input.type)
        {
        case InputType::None:
        case InputType::Error:
        case InputType::Resize:
            continue;

        case InputType::Key:
        case InputType::Char:
            {
                e.Clear();
                const ChooserOutcome outcome = HandleInput(input, e);
                if (outcome != ChooserOutcome::CONTINUE)
                    return outcome;
                if (e.Test())
                    ReportError(e);
            }
            break;
        }
    }
}

void Chooser::Reset()
{
    m_terminal_width = 0;
    m_terminal_height = 0;

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

    ForceUpdateAll();
}

void Chooser::ForceUpdateAll()
{
    m_dirty_header = true;
    m_dirty.MarkAll();
    m_dirty_footer = true;
    m_prev_visible_rows = uintptr_t(-1) >> 1;
    assert(m_prev_visible_rows > 0);
}

void Chooser::UpdateDisplay()
{
// BUGBUG:  in the @withfig/autocomplete/build directory this gets the max
// scroll height all wrong.
    if (!m_last_feedback.Equal(m_feedback))
        m_dirty_footer = true;

    if (!m_dirty_header &&
        !m_dirty_footer &&
        !m_dirty.AnyMarked() &&
        m_visible_rows >= m_prev_visible_rows)
    {
        return;
    }

    StrW s;

    EnsureColumnWidths();

    scroll_car scroll_car;
    const int32 rows = int32(min<intptr_t>(m_visible_rows, m_num_rows));
    scroll_car.set_style(c_sbstyle);
    scroll_car.set_extents(rows, m_num_rows);
    scroll_car.set_position(m_top);
    m_vert_scroll_column = scroll_car.has_car() ? m_terminal_width - 2 : 0;

    // Header.
    if (m_dirty_header)
    {
        StrW left;
        StrW right;
        StrW dir;

        s.Append(L"\x1b[1H");
        s.AppendColor(GetColor(ColorElement::Header));

        left.Printf(L"LIST - Path: ");
#ifdef DEBUG
        right.Printf(L"    (%lu rows, %lu visible)", m_num_rows, m_visible_rows);
#endif
        if (left.Length() + right.Length() + 40 > m_terminal_width)
            right.Clear();
        const unsigned limit_len = m_terminal_width - (left.Length() + right.Length());
        ellipsify_ex(m_dir.Text(), limit_len, ellipsify_mode::PATH, dir);

        s.Append(left);
        s.Append(dir);
        if (right.Length())
        {
            s.AppendSpaces(m_terminal_width - (left.Length() + cell_count(dir.Text()) + right.Length()));
            s.Append(right);
        }
        else
        {
            if (m_terminal_width > left.Length() + cell_count(dir.Text()))
                s.Append(c_clreol);
        }
        s.Append(c_norm);
        m_dirty_header = false;
    }

    // File list.
    if (m_dirty.AnyMarked())
    {
        s.Append(L"\x1b[2H");

        StrW s2;
        const intptr_t num_add = m_num_rows;
        for (intptr_t ii = 0; ii < m_visible_rows; ii++)
        {
            intptr_t iItem = m_top + ii;
            if (m_dirty.IsMarked(iItem))
            {
                s2.Clear();
                unsigned row_width = 0;

                for (intptr_t jj = 0; jj < m_num_per_row && iItem < m_count; jj++, iItem += num_add)
                {
                    const FileInfo* pfi = &m_files[iItem];
                    if (jj)
                    {
                        s2.AppendSpaces(m_padding);
                        row_width += m_padding;
                    }
                    const bool selected = iItem == m_index;
                    const bool tagged = m_tagged.IsMarked(iItem) && !pfi->IsDirectory();
                    row_width += FormatFileInfo(s2, pfi, m_col_widths[jj], m_details, selected, tagged, m_max_size_width);
                }

                if (scroll_car.has_car())
                {
                    const WCHAR* car = scroll_car.get_char(int32(ii), c_floating);
                    if (!c_floating || car)
                    {
                        // Space was reserved by update_layout() or col_max.
                        const uint32 pad_to = m_terminal_width - 2;
                        if (pad_to >= row_width)
                        {
                            s2.AppendSpaces(pad_to - row_width);
                            if (c_floating)
                            {
                                s2.AppendColor(GetColor(ColorElement::FloatingScrollBar));
                            }
                            else
                            {
                                if (car)
                                    s2.AppendColor(ConvertColorParams(ColorElement::PopupScrollCar, ColorConversion::TextOnly));
                                else
                                    car = L" ";
                                s2.AppendColorOverlay(nullptr, ConvertColorParams(ColorElement::PopupBorder, ColorConversion::TextAsBack));
                            }
                            s2.Append(car);                     // ┃ or etc
                            s2.Append(c_norm);
                        }
                        row_width = pad_to + 1;
                    }
                }

                assert(row_width < m_terminal_width);
                if (row_width < m_terminal_width)
                    s2.Append(c_clreol);

                s.Append(s2);
            }

            s.Append(L"\n");
        }

        m_dirty.Clear();
    }

    // Empty area.
    if (m_visible_rows < m_prev_visible_rows)
    {
        s.Printf(L"\x1b[%uH", 2 + m_visible_rows);

        for (intptr_t ii = m_visible_rows + 2; ii < m_terminal_height; ++ii)
        {
            s.Append(c_clreol);
            s.Append(L"\n");
        }
    }

    // Command line.
    if (m_dirty_footer)
    {
        s.Printf(L"\x1b[%uH", m_terminal_height);
        s.AppendColor(GetColor(ColorElement::Command));

        const unsigned offset = s.Length();
        StrW left;
        StrW right;
        left.Printf(L"Files: %lu of %lu", m_index + 1, m_count);
        if (m_feedback.Length())
        {
            if (left.Length() < 20)
                left.AppendSpaces(20 - left.Length());
            left.AppendSpaces(4);
            left.Append(m_feedback);
        }
        if (left.Length() + right.Length() > m_terminal_width)
            right.Clear();

        s.Append(left);
        s.AppendSpaces(m_terminal_width - (left.Length() + right.Length()));
        s.Append(right);

        s.Append(c_norm);
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

        OutputConsole(m_hout, c_hide_cursor);
        s.Printf(L"\x1b[%u;%uH", y, x);
        s.Append(c_show_cursor);
        OutputConsole(m_hout, s.Text(), s.Length());
    }

    m_prev_visible_rows = m_visible_rows;
    m_last_feedback = std::move(m_feedback);
}

void Chooser::Relayout()
{
    m_terminal_width = 0;
    m_terminal_height = 0;
    m_vert_scroll_column = 0;
    ForceUpdateAll();
}

void Chooser::EnsureColumnWidths()
{
    const DWORD colsrows = GetConsoleColsRows(m_hout);
    const unsigned terminal_width = LOWORD(colsrows);
    const unsigned terminal_height = HIWORD(colsrows);
    if (!m_terminal_width || terminal_width != m_terminal_width ||
        !m_terminal_height || terminal_height != m_terminal_height ||
        !m_num_per_row || !m_num_rows || !m_visible_rows)
    {
        m_terminal_width = terminal_width;
        m_terminal_height = terminal_height;

        m_max_size_width = 0;
        if (m_details >= 3 && m_files.size())
        {
            if (m_files[0].IsDirectory())
                m_max_size_width = WidthForDirectorySize(m_details);

            for (size_t index = 0; index < m_files.size(); ++index)
            {
                const FileInfo* pfi = &m_files[index];
                const unsigned size_width = WidthForFileInfoSize(pfi, m_details, -1);
                m_max_size_width = std::max<unsigned>(m_max_size_width, size_width);
            }
        }

        // First try columns that are the height of the terminal and don't
        // need to scroll.
        {
            size_t rows = m_terminal_height - 2;
            unsigned width = 0;
            unsigned total_width = 0;
            const size_t last = m_files.size() - 1;

            m_col_widths.clear();
            for (size_t index = 0; index < m_files.size(); ++index)
            {
                width = max<unsigned>(width, WidthForFileInfo(&m_files[index], m_details, m_max_size_width));
                if (!--rows || index == last)
                {
                    rows = m_terminal_height - 2;
                    m_col_widths.emplace_back(width);
                    total_width += width + m_padding;
                    width = 0;
                    if (total_width > m_terminal_width)
                    {
                        m_col_widths.clear();
                        break;
                    }
                }
            }

            if (!m_col_widths.empty())
            {
                m_num_per_row = int32(std::max<intptr_t>(1, m_col_widths.size()));
                m_num_rows = std::min<intptr_t>(m_terminal_height - 2, m_files.size());
                m_visible_rows = int32((terminal_height > 2) ? m_num_rows : 0);
            }
        }

        // If the files didn't all fit, then fit as many columns as possible
        // into the terminal width.
        if (m_col_widths.empty())
        {
            m_col_widths = CalculateColumns([this](size_t index){
                return WidthForFileInfo(&m_files[index], m_details, m_max_size_width);
            }, m_files.size(), true, m_padding, terminal_width, terminal_width / 4);

            m_num_per_row = int32(std::max<intptr_t>(1, m_col_widths.size()));
            m_num_rows = (m_count + m_num_per_row - 1) / m_num_per_row;
            m_visible_rows = int32(std::min<intptr_t>(m_num_rows, (terminal_height > 2) ? terminal_height - 2 : 0));
        }

        if (m_col_widths.size() == 1 && m_col_widths[0] > m_terminal_width)
            m_col_widths[0] = m_terminal_width;

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

    if (input.type == InputType::Key)
    {
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

        case Key::ESC:
            return ChooserOutcome::EXITAPP;

        case Key::ENTER:
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
                        ReportError(e);
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
            if (m_index)
                SetIndex(m_index - 1);
            EnsureTop();
            break;
        case Key::DOWN:
            if (m_index == m_count - 1)
                m_prev_latched = true;
LNext:
            if (m_count && m_index < m_count - 1)
                SetIndex(m_index + 1);
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
            if (input.modifier == Modifier::None)
            {
                DeleteEntries(e);
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
                m_details = input.key_char - '1';
                Relayout();
            }
            break;

        case '@':
        case '/':
        case '\\':
            if ((input.modifier & ~(Modifier::SHIFT)) == Modifier::None)
            {
                ShowFileList();
            }
            break;

        case '*':
            if ((input.modifier & (Modifier::ALT|Modifier::CTRL)) == Modifier::None)
            {
                RefreshDirectoryListing(e);
            }
            break;
        case 'f':
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
                    ReportError(e);
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
        case '\x15':    // CTRL-U
            if (m_tagged.AnyMarked())
            {
                m_tagged.Clear();
                m_dirty.MarkAll();
            }
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

std::vector<StrW> Chooser::GetTaggedFiles() const
{
    StrW s;
    std::vector<StrW> files;
    for (size_t i = 0; i < m_files.size(); ++i)
    {
        if (m_tagged.IsMarked(i))
        {
            const auto& file = m_files[i];
            if (!file.IsDirectory())
            {
                m_files[i].GetPathName(s);
                files.emplace_back(std::move(s));
            }
        }
    }
    return files;
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
        const intptr_t num_per_page = (m_visible_rows * m_num_per_row);
        if (top <= m_num_rows - m_visible_rows)
        {
            m_top = top;
            m_dirty.MarkAll();
        }
        if (m_top > m_num_rows - num_per_page && m_num_rows > num_per_page)
        {
            m_top = m_num_rows - num_per_page;
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
    OutputConsole(m_hout, s.Text(), s.Length());

    bool confirmed = false;
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

    OutputConsole(m_hout, s.Text(), s.Length());

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
        OutputConsole(m_hout, s.Text(), s.Length());
    }
    else
    {
        OutputConsole(m_hout, L"\r\n");
    }
}

void Chooser::NewFileMask(Error& e)
{
    StrW s;
    s.Printf(L"\x1b[%uH", m_terminal_height);
    s.AppendColor(GetColor(ColorElement::Command));
    s.Printf(L"\r\x1b[KEnter new file mask or path%s ", c_prompt_char);
    OutputConsole(m_hout, s.Text(), s.Length());

    ReadInput(s);

    OutputConsole(m_hout, c_norm);
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

void Chooser::ChangeAttributes(Error& e)
{
    if (size_t(m_index) >= m_files.size())
        return;
    if (m_files[m_index].IsPseudoDirectory())
        return;

    // TODO:  Support for marked files and directories.

    StrW path = GetSelectedFile();
    if (path.Empty())
        return;

    StrW s;
    s.Printf(L"\x1b[%uH", m_terminal_height);
    s.AppendColor(GetColor(ColorElement::Command));
    s.Printf(L"\r\x1b[KChange attributes ('ashr' to set or '-a-s-h-r' to clear)%s ", c_prompt_char);
    OutputConsole(m_hout, s.Text(), s.Length());

    ReadInput(s);

    OutputConsole(m_hout, c_norm);
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

    m_files[m_index].UpdateAttributes(update);
}

void Chooser::NewDirectory(Error& e)
{
    StrW s;
    s.Printf(L"\x1b[%uH", m_terminal_height);
    s.AppendColor(GetColor(ColorElement::Command));
    s.Printf(L"\rEnter new directory name%s ", c_prompt_char);
    OutputConsole(m_hout, s.Text(), s.Length());

    ReadInput(s);

    OutputConsole(m_hout, c_norm);
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
    OutputConsole(m_hout, s.Text(), s.Length());

    ReadInput(s);

    OutputConsole(m_hout, c_norm);
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

void Chooser::DeleteEntries(Error& e)
{
    std::vector<StrW> files;
    StrW name;
    bool is_dir = false;

    if (m_tagged.AnyMarked())
    {
        files = GetTaggedFiles();
    }
    else if (size_t(m_index) < m_files.size() && !m_files[m_index].IsPseudoDirectory())
    {
        name = GetSelectedFile();
        if (name.Empty())
            return;
        is_dir = m_files[m_index].IsDirectory();
    }

    if (files.empty() && name.Empty())
        return;

    StrW msg;
    if (files.size() <= 1)
    {
        const WCHAR* file = files.empty() ? name.Text() : files[0].Text();
        const WCHAR* name_part = FindName(file);
        if (name_part && *name_part)
            msg.Printf(L"Confirm delete '%s'?", name_part);
    }
    if (msg.Empty())
    {
        const size_t n = files.size() + !name.Empty();
        msg.Printf(L"Confirm delete %zu item%s?", n, (n == 1) ? L"" : L"s");
    }
    if (!AskForConfirmation(msg.Text()))
        return;

    UpdateDisplay();

    bool any = false;
    if (!files.empty())
    {
        for (const auto& file : files)
        {
            BOOL ok = false;
#ifdef DISALLOW_DESTRUCTIVE_OPERATIONS
            SetLastError(ERROR_ACCESS_DENIED);
            e.Set(L"(Destructive operations are disallowed.)");
#else
            ok = DeleteFileW(file.Text());
#endif
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
    }
    else if (!name.Empty())
    {
        BOOL ok = false;
#ifdef DISALLOW_DESTRUCTIVE_OPERATIONS
        SetLastError(ERROR_ACCESS_DENIED);
        e.Set(L"(Destructive operations are disallowed.)");
#else
        if (is_dir)
            ok = RemoveDirectoryW(name.Text());
        else
            ok = DeleteFileW(name.Text());
#endif
        if (!ok)
        {
            e.Sys();
            const WCHAR* name_part = FindName(name.Text());
            if (name_part && *name_part)
                e.Set(L"Unable to delete '%1'.") << name_part;
        }
        else
        {
            any = true;
        }
    }

    if (any)
    {
        Error dummy;
        Error* err = e.Test() ? &dummy : &e; // Don't overwrite e!
        RefreshDirectoryListing(*err);
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
        e.Set(L"(Destructive operations are disallowed.)");
#else
        s.AppendMaybeQuoted(file.Text());
#endif
    }

    if (s.Empty())
        return

    // Clear the current (alternate) screen in case programs switch to it.
    OutputConsole(m_hout, L"\x1b[J");

    // Swap back to original screen and console modes.
    std::unique_ptr<Interactive> inverted = m_interactive->MakeReverseInteractive();

    StrW msg;
    msg.Printf(L"\r\n%s '%s'...\r\n", edit ? L"Editing" : L"Running", file.Text());
    OutputConsole(m_hout, msg.Text(), msg.Length());

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
    OutputConsole(m_hout, s.Text(), s.Length());
    ReadInput(program);
    OutputConsole(m_hout, c_norm);
    ForceUpdateAll();

    program.TrimRight();
    if (!program.Length())
        return;

    UpdateDisplay();

    s.Clear();
    s.Printf(L"\x1b[%uH", m_terminal_height);
    s.AppendColor(GetColor(ColorElement::Command));
    s.Printf(L"\rArguments before file name%s ", c_prompt_char);
    OutputConsole(m_hout, s.Text(), s.Length());
    ok = ReadInput(args_before);
    OutputConsole(m_hout, c_norm);
    ForceUpdateAll();
    if (!ok)
        return;

    UpdateDisplay();

    s.Clear();
    s.Printf(L"\x1b[%uH", m_terminal_height);
    s.AppendColor(GetColor(ColorElement::Command));
    s.Printf(L"\rArguments after file name%s ", c_prompt_char);
    OutputConsole(m_hout, s.Text(), s.Length());
    ok = ReadInput(args_after);
    OutputConsole(m_hout, c_norm);
    ForceUpdateAll();
    if (!ok)
        return;

    // Clear the current (alternate) screen in case programs switch to it.
    OutputConsole(m_hout, L"\x1b[J");

    // Swap back to original screen and console modes.
    std::unique_ptr<Interactive> inverted = m_interactive->MakeReverseInteractive();

    // Report that it will run commands.
    const StrW sweepdivider = MakeColor(ColorElement::SweepDivider);
    const StrW sweepfile = MakeColor(ColorElement::SweepFile);
    const WCHAR* const c_div = sweepdivider.Text();
    s.Clear();
    s.Printf(L"\r\n%s---- Sweep %zu File(s) ----%s\r\n", c_div, files.size(), c_norm);
    OutputConsole(m_hout, s.Text(), s.Length());

    bool completed = true;
    size_t errors = 0;
    for (const auto& file : files)
    {
        // Report each file.
        s.Clear();
        s.Printf(L"%s%s%s\r\n", sweepfile.Text(), file.Text(), c_norm);
        OutputConsole(m_hout, s.Text(), s.Length());

        bool ok = false;
#ifdef DISALLOW_DESTRUCTIVE_OPERATIONS
        SetLastError(ERROR_ACCESS_DENIED);
        e.Set(L"(Destructive operations are disallowed.)");
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
            OutputConsole(m_hout, L"\r\n");
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
    OutputConsole(m_hout, s.Text(), s.Length());

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
