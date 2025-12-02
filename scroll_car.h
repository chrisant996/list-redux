// Copyright (c) 2023,2025 Christopher Antos
// License: http://opensource.org/licenses/MIT

#pragma once

struct InputRecord;
enum class scroll_bar_style { whole_line_chars, half_line_chars, whole_block_chars, eighths_block_chars };

int32 calc_scroll_car_size(intptr_t rows, intptr_t total, scroll_bar_style style);
int32 calc_scroll_car_offset(intptr_t top, int32 rows, intptr_t total, int32 car_size, scroll_bar_style style);
const WCHAR* get_scroll_car_char(intptr_t visible_offset, intptr_t car_offset, int32 car_size, bool floating, scroll_bar_style style);
intptr_t hittest_scrollbar(int32 row, int32 rows, intptr_t total, scroll_bar_style style);

class scroll_car
{
public:
                        scroll_car() = default;
                        ~scroll_car() = default;

    void                set_style(scroll_bar_style style);
    void                set_extents(int32 rows, intptr_t total);
    void                set_position(intptr_t top);
    bool                has_car() const { return m_car_size > 0; }

    int32               get_car_top() const;
    int32               get_car_size() const;
    const WCHAR*        get_char(int32 row, bool floating) const;
    intptr_t            hittest_scrollbar(const InputRecord& input, int32 top);

private:
    scroll_bar_style    m_style = scroll_bar_style::whole_line_chars;
    int32               m_rows = 0;
    intptr_t            m_total = 0;
    int32               m_car_size = 0;
    int32               m_car_top = -1;
};
