// Copyright (c) 2025 by Christopher Antos
// License: http://opensource.org/licenses/MIT

// vim: set et ts=4 sw=4 cino={0s:

#pragma once

#ifdef DEBUG
#define USE_SMALL_DATA_BUFFER
#endif

#ifdef USE_SMALL_DATA_BUFFER
const DWORD c_data_buffer_slop = 256;
const DWORD c_data_buffer_main = 256;
const DWORD c_default_max_line_length = 256;
#else
const DWORD c_data_buffer_slop = 4096 * 16;
const DWORD c_data_buffer_main = 4096 * 24;
const DWORD c_default_max_line_length = 2048;
#endif
static_assert(c_data_buffer_slop >= c_default_max_line_length);

enum class CtrlMode
{
    OEM437,
    EXPAND,
#ifdef INCLUDE_CTRLMODE_PERIOD
    PERIOD,
#endif
#ifdef INCLUDE_CTRLMODE_SPACE
    SPACE,
#endif
    __MAX
};

enum class TabMode
{
    EXPAND,
    HIGHLIGHT,
    RAW,
    __MAX
};

struct ViewerOptions
{
    unsigned max_line_length = c_default_max_line_length;
    CtrlMode ctrl_mode = CtrlMode::OEM437;
    TabMode tab_mode = TabMode::EXPAND;
    bool show_line_numbers = false;
    bool show_file_offsets = false;
    bool show_endoffile_line = true;
    bool show_ruler = false;
    bool show_scrollbar = true;
    uint8 hex_grouping = 0;             // Power of 2.

#ifdef DEBUG
    bool show_debug_info = true;
#else
    bool show_debug_info = false;
#endif
};

