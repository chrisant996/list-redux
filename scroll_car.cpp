// Copyright (c) 2023,2025 Christopher Antos
// License: http://opensource.org/licenses/MIT

#include "pch.h"
#include "scroll_car.h"
#include "input.h"

//------------------------------------------------------------------------------
static int32 get_scale_positions(scroll_bar_style style)
{
    switch (style)
    {
    default:
    case scroll_bar_style::whole_line_chars:    return 1;
    case scroll_bar_style::whole_block_chars:   return 1;
    case scroll_bar_style::half_line_chars:     return 2;
    case scroll_bar_style::eighths_block_chars: return 8;
    }
}

//------------------------------------------------------------------------------
template<typename T> inline T round_two(T pos) { return (pos & ~1); }
template<typename T> inline T round_eight(T pos) { return (pos & ~7); }

//------------------------------------------------------------------------------
int32 calc_scroll_car_size(intptr_t rows, intptr_t total, scroll_bar_style style)
{
    if (rows <= 0 || rows >= total)
        return 0;

    const int32 scale_positions = get_scale_positions(style);
    const int32 car_size = int32(max<intptr_t>(scale_positions, min(scale_positions * rows, (intptr_t(scale_positions) * rows * rows + (total / 2)) / total)));
    return car_size;
}

//------------------------------------------------------------------------------
int32 calc_scroll_car_offset(intptr_t top, int32 rows, intptr_t total, int32 car_size, scroll_bar_style style)
{
    if (car_size <= 0)
        return 0;

    const int32 scale_positions = get_scale_positions(style);
    const intptr_t car_positions = ((rows * scale_positions) + 1 - car_size);
    if (car_positions <= 0)
        return 0;

    const double per_car_position = double(total - rows) / double(car_positions);
    if (per_car_position <= 0)
        return 0;

    const int32 car_offset = min<int32>((rows * scale_positions) - car_size, int32(double(top) / per_car_position));
    return car_offset;
}

//------------------------------------------------------------------------------
const WCHAR* get_scroll_car_char(intptr_t row, intptr_t car_offset, int32 car_size, bool floating, scroll_bar_style style)
{
    if (car_size <= 0)
        return nullptr;

    const int32 scale_positions = get_scale_positions(style);
    row *= scale_positions;

    switch (style)
    {
    default:
    case scroll_bar_style::whole_line_chars:
        {
            static const WCHAR* const c_car_chars[] = {
                L"\u2503",                                               // ┃
                L"\u2502",                                               // │
            };
            if (row >= car_offset && row < car_offset + car_size)
                return c_car_chars[floating];
        }
        break;
    case scroll_bar_style::whole_block_chars:
        {
            static const WCHAR* const c_car_chars[] = {
                L"\u2588",                                               // █
                //L"\u2592",                                               // ▒
            };
            if (row >= car_offset && row < car_offset + car_size)
                return c_car_chars[0];
        }
        break;
    case scroll_bar_style::half_line_chars:
        {
            static const WCHAR* const c_car_chars[] = {
                L"\u257d",                                               // ╽
                L"\u2503",                                               // ┃
                L"\u257f",                                               // ╿
                L"\u2577",                                               // ╷
                L"\u2502",                                               // │
                L"\u2575",                                               // ╵
            };

            if (row == round_two(car_offset) && // This is the first cell of the scroll car...
                row != car_offset)          // ...and the first half is not part of the car.
            {
                const int32 index = 0 + (floating ? 3 : 0);
                return c_car_chars[index];
            }
            else if (row == round_two(car_offset + car_size) && // This is the last cell of the scroll car...
                     row != car_offset + car_size)        // ...and the second half is not part of the car.
            {
                const int32 index = 2 + (floating ? 3 : 0);
                return c_car_chars[index];
            }
            else if (row >= round_two(car_offset) && row < round_two(car_offset + car_size))
            {
                const int32 index = 1 + (floating ? 3 : 0);
                return c_car_chars[index];
            }
        }
        break;
    case scroll_bar_style::eighths_block_chars:
        {
            static const WCHAR* const c_car_chars[] = {
                L"\u2588",                                               // █
                L"\u2587",                                               // ▇
                L"\u2586",                                               // ▆
                L"\u2585",                                               // ▅
                L"\u2584",                                               // ▄
                L"\u2583",                                               // ▃
                L"\u2582",                                               // ▂
                L"\u2581",                                               // ▁
            };

            if (row == round_eight(car_offset) && // This is the first cell of the scroll car...
                row != car_offset)          // ...and the first eighth is not part of the car.
            {
                const int32 index = (car_offset & 7);
                return c_car_chars[index];
            }
            else if (row == round_eight(car_offset + car_size) && // This is the last cell of the scroll car...
                     row != car_offset + car_size)          // ...and the last eighth is not part of the car.
            {
                static StrW s_bottom_char;
                const int32 index = ((car_offset + car_size) & 7);
                s_bottom_char.Clear();
                s_bottom_char.Printf(L"\x1b[7m%s\x1b[27m", c_car_chars[index]);
                return s_bottom_char.Text();
            }
            else if (row >= round_eight(car_offset) && row < round_eight(car_offset + car_size))
            {
                const int32 index = 0;
                return c_car_chars[index];
            }
        }
        break;
    }

    return nullptr;
}

//------------------------------------------------------------------------------
intptr_t hittest_scrollbar(int32 row, int32 rows, intptr_t total)
{
    if (row < 0 || row >= rows || rows > total)
        return -1;

    if (rows <= 1 || total <= 1)
        return 0;

    return (row * (total - 1) / (rows - 1));
}

//------------------------------------------------------------------------------
void scroll_car::set_style(scroll_bar_style style)
{
    m_style = style;
}

//------------------------------------------------------------------------------
void scroll_car::set_extents(int32 rows, intptr_t total)
{
    if (rows < 0 || total < 0 || rows >= total)
    {
        rows = 0;
        total = 0;
    }
    m_rows = rows;
    m_total = total;
    m_car_size = (m_rows > 0) ? calc_scroll_car_size(m_rows, m_total, m_style) : 0;
    m_car_top = -1;
}

//------------------------------------------------------------------------------
void scroll_car::set_position(intptr_t top)
{
    if (m_rows > 0)
        m_car_top = calc_scroll_car_offset(top, m_rows, m_total, m_car_size, m_style);
    else
        assert(m_car_top <= 0);
}

//------------------------------------------------------------------------------
int32 scroll_car::get_car_top() const
{
    return m_car_top;
}

//------------------------------------------------------------------------------
int32 scroll_car::get_car_size() const
{
    return m_car_size;
}

//------------------------------------------------------------------------------
const WCHAR* scroll_car::get_char(int32 row, bool floating) const
{
    if (m_car_top < 0)
        return nullptr;
    assert(m_rows > 0 && m_total > 0);
    return get_scroll_car_char(row, m_car_top, m_car_size, floating, m_style);
}

//------------------------------------------------------------------------------
intptr_t scroll_car::hittest_scrollbar(const InputRecord& input, int32 top)
{
    if (input.type != InputType::Mouse)
        return -1;
    if (input.key != Key::MouseLeftClick && input.key != Key::MouseDrag)
        return -1;
    if (input.mouse_pos.Y < top || input.mouse_pos.Y >= top + m_rows)
        return -1;

    const intptr_t index = ::hittest_scrollbar(input.mouse_pos.Y - top, m_rows, m_total);
    return index;
}
