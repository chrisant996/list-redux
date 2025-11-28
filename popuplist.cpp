// Copyright (c) 2025 by Christopher Antos
// License: http://opensource.org/licenses/MIT

// vim: set et ts=4 sw=4 cino={0s:

#include "pch.h"
#include <assert.h>
#include "popuplist.h"
#include "input.h"
#include "output.h"
#include "colors.h"
#include "ellipsify.h"
#include "fileinfo.h"
#include "scroll_car.h"
#include "wcwidth_iter.h"

constexpr scroll_bar_style c_sbstyle = scroll_bar_style::whole_line_chars;

class PopupList
{
public:
                    PopupList() = default;

    PopupResult     Go(const WCHAR* title, const std::vector<StrW>& items, intptr_t index, PopupListFlags flags);

private:
    // Internal methods.
    void            update_layout();
    void            update_top();
    void            update_display();
    bool            handle_input(const InputRecord& input);
    int32           get_scroll_offset() const;
    void            set_top(intptr_t top, bool ignore_scroll_offset=false);
    bool            is_selected(intptr_t index) const;

    // Filtering.
    intptr_t        get_original_index(intptr_t index) const;
    const WCHAR*    get_item_text(intptr_t index) const;
    void            clear_filter();
    bool            filter_items();

    // Layout.
    int32           m_terminal_width = 0;
    int32           m_terminal_height = 0;
#if 0
    int32           m_mouse_offset = 0;
    int32           m_mouse_left = 0;
    int32           m_mouse_width = 0;
#endif
    int32           m_visible_rows = 0;
    int32           m_longest_visible = 0;
    int32           m_vert_scroll_car = 0;
    int32           m_vert_scroll_column = 0;
    const WCHAR*    m_title;

    // Entries.
    intptr_t        m_count = 0;
    const std::vector<StrW>* m_items = nullptr;
    uint32          m_longest = 0;
    uint32          m_margin = 1;
    PopupListFlags  m_flags = PopupListFlags::None;

    // Filtering.
    const WCHAR*    m_orig_title;
    StrW            m_filter_title;
    StrW            m_filter_string;
    intptr_t        m_filter_saved_index = -1;
    intptr_t        m_filter_saved_top = -1;
    std::vector<size_t> m_filtered_items;   // Maps filtered index to original index.

    // Current entry.
    intptr_t        m_top = 0;
    intptr_t        m_index = 0;
    intptr_t        m_prev_displayed = -1;

    // Current input.
    PopupResult     m_result;
    StrW            m_needle;
    bool            m_input_clears_needle = false;
    bool            m_ignore_scroll_offset = false;

    // Mouse.
#if 0
    scroll_helper   m_scroll_helper;
    bool            m_scroll_bar_clicked = false;
#endif

    // Configuration.
    int32           m_pref_height = 0;      // Automatic.
    int32           m_pref_width = 0;       // Automatic.
    bool            m_filter = true;
};

const int32 min_screen_cols = 20;

static bool strstr_compare(const StrW& needle, const WCHAR* haystack);

PopupResult PopupList::Go(const WCHAR* title, const std::vector<StrW>& items, intptr_t index, PopupListFlags flags)
{
    m_result.Clear();
    m_flags = flags;

#if 0
    m_scroll_helper.clear();
#endif

    const DWORD colsrows = GetConsoleColsRows();
    m_terminal_width = LOWORD(colsrows);
    m_terminal_height = HIWORD(colsrows);

    if (items.empty())
    {
cancel:
        assert(m_result.canceled);
        return m_result;
    }

    m_orig_title = (title && *title) ? title : nullptr;
    m_title = m_orig_title;

    // Attach to list of items.
    m_items = &items;
    m_count = items.size();

    // Initialize the various modes.
    m_pref_height = 0;
    m_pref_width = 0;
    m_filter = true;

    // Measure longest item.
    m_longest = m_pref_width;
    if (!m_longest)
    {
        for (const auto& item : *m_items)
            m_longest = max(m_longest, cell_count(item.Text()));
    }
    m_longest = max<uint32>(m_longest, c_min_popuplist_content_width);
    if (m_title)
        m_longest = max(m_longest, cell_count(m_title) + 4);

    // Make sure there's room.
    update_layout();
    if (m_visible_rows <= 0)
        goto cancel;

    // Initialize the view.
    if (index < 0 || index >= m_count)
    {
        m_index = m_count - 1;
        m_top = int32(max<intptr_t>(0, m_count - m_visible_rows));
    }
    else
    {
        m_index = index;
        m_top = int32(max<intptr_t>(0, min<intptr_t>(m_index - (m_visible_rows / 3), m_count - m_visible_rows)));
    }

    while (true)
    {
        update_display();

        const InputRecord input = SelectInput();
        switch (input.type)
        {
        case InputType::None:
        case InputType::Error:
            continue;

        case InputType::Resize:
            goto cancel;

        case InputType::Key:
        case InputType::Char:
            if (handle_input(input))
                return m_result;
            break;
        }
    }
}

static void advance_index(intptr_t& i, intptr_t direction, intptr_t max_count)
{
    i += direction;
    if (direction < 0)
    {
        if (i < 0)
            i = max_count - 1;
    }
    else
    {
        if (i >= max_count)
            i = 0;
    }
}

bool PopupList::handle_input(const InputRecord& input)
{
    bool from_begin = false;
    bool need_display = false;

    // Cancel if no room.
    if (m_visible_rows <= 0)
    {
        m_result.canceled = true;
        return true;
    }

    m_ignore_scroll_offset = false;

    if (input.type == InputType::Key)
    {
        switch (input.key)
        {
        case Key::UP:
            m_index--;
navigated:
            if (m_index >= m_count)
                m_index = m_count - 1;
            if (m_index < 0)
                m_index = 0;
            update_display();
            break;
        case Key::DOWN:
            m_index++;
            goto navigated;

        case Key::HOME:
            m_index = 0;
            goto navigated;
        case Key::END:
            m_index = m_count - 1;
            goto navigated;

        case Key::PGDN:
        case Key::PGUP:
            {
                const intptr_t y = m_index;
                const int32 rows = int32(min<intptr_t>(m_count, m_visible_rows));
                const int32 scroll_ofs = get_scroll_offset();
                // Use rows as the page size (vs the more common rows-1) for
                // compatibility with Conhost's F7 popup list behavior.
                const int32 scroll_rows = (rows - scroll_ofs);
                if (input.key == Key::PGUP)
                {
                    if (y > 0)
                    {
                        int32 new_y = int32(max<intptr_t>(0, (y <= m_top + scroll_ofs) ? y - scroll_rows : m_top + scroll_ofs));
                        m_index += (new_y - y);
                        goto navigated;
                    }
                }
                else if (input.key == Key::PGDN)
                {
                    if (y < m_count - 1)
                    {
                        intptr_t bottom_y = m_top + scroll_rows - 1;
                        intptr_t new_y = int32(min<intptr_t>(m_count - 1, (y == bottom_y) ? y + scroll_rows : bottom_y));
                        m_index += (new_y - y);
                        if (m_index > m_count - 1)
                        {
                            set_top(int32(max<intptr_t>(0, m_count - m_visible_rows)));
                            m_index = m_count - 1;
                        }
                        goto navigated;
                    }
                }
            }
            break;

        case Key::ESC:
            m_result.canceled = true;
            return true;

        case Key::ENTER:
            if (m_index < 0 || m_index >= m_count)
                break;
            m_result.canceled = false;
            m_result.selected = get_original_index(m_index);
            return true;

#if 0
        case __mouse_wheel_up__:
            m_index -= __wheel_amount__;
            m_ignore_scroll_offset = false;
            goto navigated;
        case __mouse_wheel_down__:
            m_index += __wheel_amount__;
            m_ignore_scroll_offset = false;
            goto navigated;
#endif

        case Key::BACK:
            if (!m_needle.Empty())
            {
                unsigned prev_len = 0;
                wcwidth_iter iter(m_needle.Text(), m_needle.Length());
                while (iter.more())
                {
                    prev_len = unsigned(iter.get_pointer() - m_needle.Text());
                    iter.next();
                }
                m_needle.SetLength(prev_len);
                need_display = true;
                from_begin = true;
                goto update_needle;
            }
            break;
        }
    }
    else if (input.type == InputType::Char)
    {
        if (m_input_clears_needle)
        {
            m_input_clears_needle = false;
        }

        if (input.key_char < ' ')
        {
            // Ignore ctrl characters.
        }
        else
        {
            m_needle.Append(input.key_char);
            if (input.key_char2)
                m_needle.Append(input.key_char2);
            need_display = true;
        }

update_needle:
        m_title = m_orig_title;
        if (m_needle.Length())
        {
            m_filter_title.Clear();
            m_filter_title.Printf(L"filter: %-10s", m_needle.Text());
            m_title = m_filter_title.Text();
        }
        if (filter_items())
        {
            m_prev_displayed = -1;
            need_display = true;
        }
        if (need_display)
            update_display();
    }

    // Keep dispatching input.
    return false;
}

void PopupList::update_layout()
{
    const int32 slop_rows = 2;
    const int32 border_rows = 2;
    const int32 target_rows = m_pref_height ? m_pref_height : m_terminal_height*5/7;
    m_visible_rows = min<int32>(target_rows, m_terminal_height - border_rows - slop_rows);

    if (m_terminal_width <= min_screen_cols)
        m_visible_rows = 0;

    m_vert_scroll_car = calc_scroll_car_size(m_visible_rows, m_count, c_sbstyle);
#if 0
    m_vert_scroll_column = 0;
#endif

    m_ignore_scroll_offset = false;
}

void PopupList::update_top()
{
    const intptr_t y = m_index;
    if (m_top > y)
    {
        set_top(y);
    }
    else
    {
        const int32 rows = int32(min<intptr_t>(m_count, m_visible_rows));
        intptr_t top = max<intptr_t>(0, y - max<int32>(rows - 1, 0));
        if (m_top < top)
            set_top(top);
    }

    if (!m_ignore_scroll_offset)
    {
        const int32 scroll_ofs = get_scroll_offset();
        if (scroll_ofs > 0)
        {
            const int32 visible_rows = int32(min<intptr_t>(m_count, m_visible_rows));
            const int32 last_row = int32(max<intptr_t>(0, m_count - visible_rows));
            if (m_top > max<intptr_t>(0, m_index - scroll_ofs))
                set_top(max<intptr_t>(0, m_index - scroll_ofs));
            else if (m_top < min<intptr_t>(last_row, m_index + scroll_ofs - visible_rows + 1))
                set_top(min<intptr_t>(last_row, m_index + scroll_ofs - visible_rows + 1));
        }
    }

    assert(m_top >= 0);
    assert(m_top <= int32(max<intptr_t>(0, m_count - m_visible_rows)));
}

static void make_horz_border(const WCHAR* message, int32 col_width, bool bars, StrW& out,
                             const WCHAR* header_color=nullptr, const WCHAR* border_color=nullptr)
{
    out.Clear();

    if (!message || !*message)
    {
        while (col_width-- > 0)
            out.Append(L"\u2500");                                      // ─
        return;
    }

    if (!header_color || !border_color || wcscmp(header_color, border_color) == 0)
    {
        header_color = nullptr;
        border_color = nullptr;
    }

    int32 cells = 0;
    int32 len = 0;

    {
        const WCHAR* walk = message;
        int32 remaining = col_width - (2 + 2); // Bars, spaces.
        wcwidth_iter iter(message);
        while (iter.next())
        {
            const int32 width = iter.character_wcwidth_onectrl();
            if (width > remaining)
                break;
            cells += width;
            remaining -= width;
            len += iter.character_length();
        }
    }

    int32 x = (col_width - cells) / 2;
    x--;

    for (int32 i = x; i-- > 0;)
    {
        if (i == 0 && bars)
            out.Append(L"\u2524");                                      // ┤
        else
            out.Append(L"\u2500");                                      // ─
    }

    x += 1 + cells + 1;
    if (header_color && border_color)
        out.AppendColor(header_color);
    out.Append(L" ", 1);
    out.Append(message, len);
    out.Append(L" ", 1);
    if (header_color && border_color)
        out.AppendColor(border_color);

    bool cap = bars;
    for (int32 i = col_width - x; i-- > 0;)
    {
        if (cap)
        {
            cap = false;
            out.Append(L"\u251c");                                      // ├
        }
        else
        {
            out.Append(L"\u2500");                                      // ─
        }
    }
}

void PopupList::update_display()
{
    const bool is_filter_active = (m_items->size() && !m_filter_string.Empty());
    if (m_visible_rows > 0 || is_filter_active)
    {
        // Display list.
        const intptr_t count = m_count;
        StrW line;
        StrW left;
        StrW horzline;
        StrW tmp;
        StrW tmp2;

        update_top();

        OutputConsole(c_hide_cursor);

        const bool draw_border = (m_prev_displayed < 0) || (m_title != m_orig_title);
        const uint32 extra = 2 * (1 + m_margin);
        const int32 popup_height = m_visible_rows + 2; // +2 for borders.
        const int32 popup_width = min<int32>(m_longest + extra, m_terminal_width);
        const int32 content_height = popup_height - 2;
        const int32 content_width = popup_width - extra;

        const int32 y = (m_terminal_height - (popup_height + 1)) / 2;
        const int32 x = (m_terminal_width - (popup_width + 1)) / 2;
        if (x > 0)
            left.Printf(L"\x1b[%uG", x + 1);
#if 0
        m_mouse_left = x + 1;
        m_mouse_width = content_width;
#endif

        line.Clear();
        line.Printf(L"\x1b[%uH", y + 1);
        OutputConsole(line.Text(), line.Length());

        // Display border.
        if (draw_border)
        {
            make_horz_border(m_title,
                             content_width + (2 * m_margin),
                             (m_title != m_orig_title),
                             horzline,
                             GetColor(ColorElement::PopupHeader),
                             GetColor(ColorElement::PopupBorder));
            line.Clear();
            line.Append(left);
            line.AppendColor(GetColor(ColorElement::PopupBorder));
            line.Append(L"\u250c");                                 // ┌
            line.Append(horzline);                                  // ─
            line.Append(L"\u2510");                                 // ┐
            line.AppendNormalIf(true);
            OutputConsole(line.Text(), line.Length());
        }

        const int32 car_top = calc_scroll_car_offset(m_top, content_height, count, m_vert_scroll_car, c_sbstyle);
#if 0
        m_vert_scroll_column = m_mouse_left + m_mouse_width;
#endif

        // Display items.
        const bool dim_paths = ((m_flags & PopupListFlags::DimPaths) == PopupListFlags::DimPaths);
        for (int32 row = 0; row < content_height; row++)
        {
            const intptr_t i = m_top + row;

            OutputConsole(L"\r\n", 2);

            if (m_prev_displayed < 0 ||
                is_selected(i) ||
                i == m_prev_displayed)
            {
                line.Clear();
                line.Append(left);
                line.AppendColor(GetColor(ColorElement::PopupBorder));
                line.Append(L"\u2502");                             // │

                const WCHAR* const maincolor = GetColor(is_selected(i) ? ColorElement::PopupSelect : ColorElement::PopupContent);
                line.AppendColor(maincolor);

                const WCHAR* item = get_item_text(i);
                const WCHAR* const dimcolor = GetColor(is_selected(i) ? ColorElement::PopupSelect : ColorElement::PopupContentDim);
                if (dim_paths)
                {
                    const WCHAR* name = FindName(item);
                    tmp2.Clear();
                    tmp2.Append(item, size_t(name - item));
                    tmp2.AppendColor(maincolor);
                    tmp2.Append(name);
                    item = tmp2.Text();
                }

                const int32 cell_len = ellipsify_ex(item, content_width, ellipsify_mode::PATH, tmp);
                line.AppendSpaces(m_margin);
                if (dim_paths)
                    line.AppendColor(dimcolor);
                line.Append(tmp);                                   // main text
                line.AppendSpaces(content_width + m_margin - cell_len);

                const WCHAR* car = get_scroll_car_char(row, car_top, m_vert_scroll_car, false/*floating*/, scroll_bar_style::whole_line_chars);
                line.AppendNormalIf(true);
                if (car)
                {
                    line.AppendColor(GetColor(ColorElement::PopupScrollCar));
                    line.Append(car);                               // ┃ or etc
                }
                else
                {
                    line.AppendColor(GetColor(ColorElement::PopupBorder));
                    line.Append(L"\u2502");                         // │
                }
                line.AppendNormalIf(true);
                OutputConsole(line.Text(), line.Length());
            }
        }

        // Display border.
        if (draw_border)
        {
            OutputConsole(L"\r\n", 2);
            make_horz_border(L"ENTER=View, ESC=Cancel", content_width + (2 * m_margin), true/*bars*/, horzline,
                             GetColor(ColorElement::PopupFooter), GetColor(ColorElement::PopupBorder));
            line.Clear();
            line.Append(left);
            line.AppendColor(GetColor(ColorElement::PopupBorder));
            line.Append(L"\u2514");                                 // └
            line.Append(horzline);                                  // ─
            line.Append(L"\u2518");                                 // ┘
            line.AppendNormalIf(true);
            OutputConsole(line.Text(), line.Length());
        }

        m_prev_displayed = m_index;

        // Move cursor.
        line.Clear();
        line.Printf(L"\x1b[%u;%uH", 1+y+1+(m_index-m_top), 1+x+1);
        OutputConsole(line.Text(), line.Length());
#if 0
        m_mouse_offset = 1 + y + 1;
#endif

        OutputConsole(c_show_cursor);
    }
}

int32 PopupList::get_scroll_offset() const
{
    const int32 ofs = 3;
    if (ofs <= 0)
        return 0;
    return min(ofs, max(0, (m_visible_rows - 1) / 2));
}

void PopupList::set_top(intptr_t top, bool ignore_scroll_offset)
{
    assert(top >= 0);
    assert(top <= int32(max<intptr_t>(0, m_count - m_visible_rows)));
    if (top != m_top)
    {
        m_top = top;
        m_prev_displayed = -1;
        m_ignore_scroll_offset = ignore_scroll_offset;
    }
}

bool PopupList::is_selected(intptr_t index) const
{
    if (index < 0 || index >= m_count)
        return false;
    return m_index == index;
}

intptr_t PopupList::get_original_index(intptr_t index) const
{
    if (index < 0 || index >= m_count)
        return -1;
    if (!m_filter_string.Empty())
        index = m_filtered_items[index];
    return index;
}

const WCHAR* PopupList::get_item_text(intptr_t index) const
{
    if (index < 0 || index >= m_count)
        return L"";
    if (!m_filter_string.Empty())
        index = m_filtered_items[index];
    return (*m_items)[index].Text();
}

void PopupList::clear_filter()
{
    if (!m_filter_string.Empty())
    {
        m_count = m_items->size();
        m_filter_string.Clear();
        m_filtered_items.clear();
        m_index = m_filter_saved_index;
        m_ignore_scroll_offset = false;
        set_top(m_filter_saved_top);
        m_vert_scroll_car = calc_scroll_car_size(m_visible_rows, m_count, c_sbstyle);
    }
}

bool PopupList::filter_items()
{
    if (m_filter_string.Equal(m_needle.Text()))
        return false;

    if (m_needle.Empty())
    {
        clear_filter();
        return true;
    }

    int32 defer_test = 0;
    int32 tested = 0;
    auto test_input = [&](){
        ++tested;
        defer_test = 128;
        const InputRecord input = SelectInput(0);
        if (input.type == InputType::None)
            return false;
        if (input.type == InputType::Char)
        {
            assert(input.key_char != 0x08);
            if (input.key_char >= ' ')
                return true;
        }
        else if (input.type == InputType::Key)
        {
            switch (input.key)
            {
            case Key::ESC:
            case Key::BACK:
                return true;
            }
        }
        //
        defer_test = -1;
        return false;
    };

    // Build new filtered list.
    std::vector<size_t> filtered_items;
    if (!m_filter_string.Empty() && StrCmpN(m_needle.Text(), m_filter_string.Text(), m_filter_string.Length()) == 0)
    {
        // Further filter the filtered list.
        for (size_t i = 0; i < m_filtered_items.size(); ++i)
        {
            // Interrupt if more input is available.
            if (!defer_test-- && test_input())
                return false;

            const size_t original_index = m_filtered_items[i];
            const bool match = m_needle.Empty() || strstr_compare(m_needle, (*m_items)[original_index].Text());

            if (match)
                filtered_items.push_back(original_index);
        }
    }
    else
    {
        for (size_t i = 0; i < m_items->size(); ++i)
        {
            // Interrupt if more input is available.
            if (!defer_test-- && test_input())
                return false;

            const bool match = m_needle.Empty() || strstr_compare(m_needle, (*m_items)[i].Text());

            if (match)
                filtered_items.push_back(i);
        }
    }

    // Swap new filtered list into place.
    m_filtered_items = std::move(filtered_items);
    m_count = m_filtered_items.size();

    // Save selected item if no filtered applied yet.
    if (m_filter_string.Empty())
    {
        m_filter_saved_index = m_index;
        m_filter_saved_top = m_top;
    }

    // Remember the filter string.
    m_filter_string = m_needle.Text();

    // Reset the selected item.
    m_index = 0;
    set_top(0);
    assert(!m_ignore_scroll_offset);
    update_top();

    // Update the size of the scroll bar, since m_count may have changed.
    m_vert_scroll_car = calc_scroll_car_size(m_visible_rows, m_count, c_sbstyle);
    return true;
}

PopupResult ShowPopupList(const std::vector<StrW>& items, const WCHAR* title, intptr_t index, PopupListFlags flags)
{
    PopupList popup;
    return popup.Go(title, items, index, flags);
}

//------------------------------------------------------------------------------
#include <shlwapi.h>
static bool strstr_compare(const StrW& needle, const WCHAR* haystack)
{
    if (haystack && *haystack)
        return !!StrStrIW(haystack, needle.Text());
    return false;
}
