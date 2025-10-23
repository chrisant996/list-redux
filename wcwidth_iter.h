// Copyright (c) 2023-2024 Christopher Antos
// License: http://opensource.org/licenses/MIT

#pragma once

#include "ecma48.h"

//------------------------------------------------------------------------------
uint32 __wcswidth(const WCHAR* s, uint32 len=-1);

//------------------------------------------------------------------------------
class wcwidth_iter
{
public:
    explicit        wcwidth_iter(const WCHAR* s, int32 len=-1);
                    wcwidth_iter(const wcwidth_iter& i);
    char32_t        next();
    void            unnext();
    const WCHAR*    character_pointer() const { return m_chr_ptr; }
    uint32          character_length() const { return uint32(m_chr_end - m_chr_ptr); }
    int32           character_wcwidth_signed() const { return m_chr_wcwidth; }
    uint32          character_wcwidth_zeroctrl() const { return (m_chr_wcwidth < 0) ? 0 : m_chr_wcwidth; }
    uint32          character_wcwidth_onectrl() const { return (m_chr_wcwidth < 0) ? 1 : m_chr_wcwidth; }
    uint32          character_wcwidth_twoctrl() const { return (m_chr_wcwidth < 0) ? 2 : m_chr_wcwidth; }
    bool            character_is_emoji() const { return m_emoji; }
    const WCHAR*    get_pointer() const;
    void            reset_pointer(const WCHAR* s);
    bool            more() const;
    uint32          length() const;

private:
    void            consume_emoji_sequence();

private:
    str_iter        m_iter;
    char32_t        m_next;
    const WCHAR*    m_chr_ptr;
    const WCHAR*    m_chr_end;
    int32           m_chr_wcwidth = 0;
    bool            m_emoji = false;
};

//------------------------------------------------------------------------------
class character_sequence_state
{
    enum parsing_state : uint8 { START, MAYBE_FLAG, BEGIN_EMOJI, CONSUME_EMOJI_SEQ, CONSUME_EMOJI_SEQ_ZWJ, CONTINUING };

public:
                    character_sequence_state(int32 ctrl_width=1);
    void            reset();
    bool            next(char32_t c);   // true=began sequence, false=continued sequence.
    uint32          width() const { return m_curr_width; }
    uint32          width_delta() const { return m_curr_width - m_curr_last_width; }
    uint32          prev_width() const { return m_prev_width; }

private:
    void            finish_sequence();

private:
    const int32     c_ctrl_width;
    const bool      c_color_emoji;

    parsing_state   m_state = START;
    int32           m_prev_width = 0;
    int32           m_curr_width = 0;
    int32           m_curr_last_width = 0;

    char32_t        m_prev_c = 0;           // Only used in BEGIN_EMOJI state.
    bool            m_unqualified = false;  // Only used in BEGIN_EMOJI state.

#ifdef DEBUG
    bool            m_emoji = false;
#endif
};
