// Copyright (c) 2025 by Christopher Antos
// License: http://opensource.org/licenses/MIT

// vim: set et ts=4 sw=4 cino={0s:

#include "pch.h"
#include "chooser.h"
#include "scan.h"
#include "list_format.h"
#include "input.h"
#include "output.h"
#include "colors.h"
#include "ellipsify.h"
#include "ecma48.h"
#include "sorting.h"
#include "help.h"

#include <algorithm>

static const WCHAR c_clreol[] = L"\x1b[K";

static const WCHAR c_no_files_tagged[] = L"*** No Files Tagged ***";

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

Chooser::Chooser()
: m_hout(GetStdHandle(STD_OUTPUT_HANDLE))
{
}

void Chooser::Navigate(const WCHAR* dir, std::vector<FileInfo>&& files)
{
    Reset();
    m_dir.Set(dir);
    m_files = std::move(files);
    m_count = intptr_t(m_files.size());
    m_ever_navigated = true;
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
// TODO:  Print an error box.
#if 0
                e.Report();
#endif
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

    m_top = 0;
    m_index = 0;
    m_tagged.Clear();
    m_prev_input.type = InputType::None;
    m_prev_latched = false;

    m_feedback.Clear();

    ForceUpdateAll();
}

void Chooser::ForceUpdateAll()
{
    m_dirty_header = true;
    m_dirty.MarkAll();
    m_dirty_footer = true;
    m_prev_visible_rows = unsigned(-1);
}

void Chooser::UpdateDisplay()
{
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

    // Header.
    if (m_dirty_header)
    {
        StrW left;
        StrW right;
        StrW dir;

        s.Append(L"\x1b[1H");
        s.AppendColor(GetColor(ColorElement::Command));

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
        const unsigned y = 1 + unsigned((m_index % m_num_rows) - m_top) + 1;
        unsigned x = 1;
        for (size_t ii = m_index / m_num_rows; ii--;)
            x += m_col_widths[ii] + m_padding;

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
                m_num_per_row = std::max<intptr_t>(1, m_col_widths.size());
                m_num_rows = std::min<intptr_t>(m_terminal_height - 2, m_files.size());
                m_visible_rows = (terminal_height > 2) ? m_num_rows : 0;
            }
        }

        // If the files didn't all fit, then fit as many columns as possible
        // into the terminal width.
        if (m_col_widths.empty())
        {
            m_col_widths = CalculateColumns([this](size_t index){
                return WidthForFileInfo(&m_files[index], m_details, m_max_size_width);
            }, m_files.size(), true, m_padding, terminal_width, terminal_width / 4);

            m_num_per_row = std::max<intptr_t>(1, m_col_widths.size());
            m_num_rows = (m_count + m_num_per_row - 1) / m_num_per_row;
            m_visible_rows = std::min<intptr_t>(m_num_rows, (terminal_height > 2) ? terminal_height - 2 : 0);
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

        case Key::ESC:
            return ChooserOutcome::EXITAPP;

        case Key::ENTER:
            if (m_index >= 0 && m_index < m_count)
            {
                const auto& info = m_files[m_index];
                if (info.IsDirectory())
                {
                    StrW dir;
                    info.GetPathName(dir);
                    if (info.IsPseudoDirectory())
                    {
                        dir.SetEnd(FindName(dir.Text()));   // Strip "..".
                        StripTrailingSlashes(dir);
                        dir.SetEnd(FindName(dir.Text()));   // Go up to parent.
                    }

                    Navigate(dir.Text(), e);
// TODO:  Print an error box.
#if 0
                    if (e.Test())
                        e.Report();
#endif
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
            if (m_index)
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
            if (!m_prev_latched)
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

        case '*':
            if ((input.modifier & (Modifier::ALT|Modifier::CTRL)) == Modifier::None)
            {
                RefreshDirectoryListing(e);
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
                // TODO:  Change file attributes.
            }
            break;
        case 'n':
            if (input.modifier == Modifier::None)
            {
                // TODO:  Create new directory.
            }
            break;
        case 'r':
            if (input.modifier == Modifier::None)
            {
                // TODO:  Rename file or directory.
            }
            break;

        case ' ':
            if (input.modifier == Modifier::None)
            {
                if (m_index < m_count && !m_files[m_index].IsDirectory())
                {
                    m_tagged.Mark(m_index, 0);      // Toggle.
                    m_dirty.Mark(m_index % m_num_rows, 1);
                    goto LNext;
                }
            }
            break;
        case 't':
            if (input.modifier == Modifier::None)
            {
                if (m_index < m_count && !m_files[m_index].IsDirectory())
                {
                    m_tagged.Mark(m_index, 1);      // Mark.
                    m_dirty.Mark(m_index % m_num_rows, 1);
                    goto LNext;
                }
            }
            break;
        case 'u':
            if (input.modifier == Modifier::None)
            {
                if (m_index < m_count && !m_files[m_index].IsDirectory())
                {
                    m_tagged.Mark(m_index, -1);     // Unmark.
                    m_dirty.Mark(m_index % m_num_rows, 1);
                    goto LNext;
                }
            }
            break;
        case '\x01':
        case '\x14':
            if (!m_tagged.AllMarked())
            {
                m_tagged.MarkAll();
                m_dirty.MarkAll();
            }
            break;
        case '\x15':
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
            m_files[i].GetPathName(s);
            files.emplace_back(std::move(s));
        }
    }
    return files;
}

void Chooser::SetIndex(intptr_t index)
{
    assert(index >= 0);
    assert(index < m_count);
    if (index >= m_count)
        index = m_count - 1;
    if (index < 0)
        index = 0;
    m_dirty.Mark(m_index % m_num_rows, 1);
    m_index = index;
    m_dirty.Mark(m_index % m_num_rows, 1);
    m_dirty_footer = true;
}

void Chooser::SetTop(intptr_t top)
{
    assert(top >= 0);
    assert(!top || top < m_num_rows);
    assert(m_num_rows >= m_visible_rows);
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
    StrW dir;
    dir.Set(m_dir);
    assert(*FindName(dir.Text()) == '*');
    dir.SetEnd(FindName(dir.Text()));   // Strip "*".
    Navigate(dir.Text(), e);
}

