// Copyright (c) 2025 by Christopher Antos
// Portions Copyright (C) 2016-2018 by Martin Ridgers
// License: http://opensource.org/licenses/MIT

// vim: set et ts=4 sw=4 cino={0s:

#pragma once

#include "ecma48.h"
#include "palette.h"

#include <vector>

#undef WriteConsole

class attributes;

enum class clear { below, above, all };
enum class clear_line { right, left, all };

class Terminal
{
public:
                        Terminal(int emulate=-1);
                        ~Terminal();

    void                WriteConsole(const WCHAR* chars, unsigned length);

private:
    const HANDLE        m_hout;

#ifdef INCLUDE_TERMINAL_EMULATOR
public:
    void                SetEmulation(int emulate=-1);

private:
#pragma region Emulation Methods
    void                write_c1(const ecma48_code& code);
    void                write_c0(int32 c0);
    void                write_icf(const ecma48_code& code);
    void                set_attributes(const ecma48_code::csi_base& csi);
    void                erase_in_display(const ecma48_code::csi_base& csi);
    void                erase_in_line(const ecma48_code::csi_base& csi);
    void                set_horiz_cursor(const ecma48_code::csi_base& csi);
    void                set_cursor(const ecma48_code::csi_base& csi);
    void                save_cursor();
    void                restore_cursor();
    void                insert_chars(const ecma48_code::csi_base& csi);
    void                delete_chars(const ecma48_code::csi_base& csi);
    void                set_private_mode(const ecma48_code::csi_base& csi);
    void                reset_private_mode(const ecma48_code::csi_base& csi);
#pragma endregion Emulation Methods
#pragma region Screen Methods
    void                do_write(const WCHAR* chars, unsigned length);
    bool                do_cursor_style(int /*style*/, int visible);
    bool                do_alternate_screen(bool alternate);
    void                do_set_cursor(int column, int row);
    void                do_set_horiz_cursor(int column);
    void                do_move_cursor(int dx, int dy);
    void                do_clear(clear mode);
    void                do_clear_line(clear_line mode);
    void                do_insert_chars(int count);
    void                do_delete_chars(int count);
    void                do_set_attributes(attributes attr);
#pragma endregion Screen Methods

private:
    CRITICAL_SECTION    m_cs;
    bool                m_emulate;

#pragma region Emulation State
    enum : WORD
    {
        attr_mask_fg        = 0x000f,
        attr_mask_bg        = 0x00f0,
        attr_mask_bold      = 0x0008,
        attr_mask_underline = 0x8000,
        attr_mask_all       = attr_mask_fg|attr_mask_bg|attr_mask_underline,
    };

    ecma48_state        m_state;
    WORD                m_default_attr = 0x07;
    WORD                m_ready = 0;
    bool                m_bold = false;
    bool                m_reverse = false;
    bool                m_alternate_screen = false;
    COORD               m_saved_cursor = { -1, -1 };

    std::vector<CHAR_INFO> m_screen_buffer;
    COORD               m_screen_dimensions = {};
    COORD               m_screen_cursor = {};
#pragma endregion Emulation State
#endif
};
