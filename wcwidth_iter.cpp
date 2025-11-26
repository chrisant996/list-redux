// Copyright (c) 2023-2024 Christopher Antos
// License: http://opensource.org/licenses/MIT

#include "pch.h"
#include "wcwidth.h"
#include "wcwidth_iter.h"

//------------------------------------------------------------------------------
uint32 __wcswidth(const WCHAR* s, uint32 len)
{
    uint32 count = 0;

    wcwidth_iter iter(s, len);
    while (iter.next())
        count += iter.character_wcwidth_onectrl();

    return count;
}



//------------------------------------------------------------------------------
character_sequence_state::character_sequence_state(int32 ctrl_width)
: c_ctrl_width(ctrl_width)
, c_color_emoji(get_color_emoji())
{
}

//------------------------------------------------------------------------------
void character_sequence_state::reset()
{
    m_state = START;
    m_prev_width = 0;
    m_curr_width = 0;
    m_curr_last_width = 0;
#ifdef DEBUG
    m_emoji = false;
#endif
}

//------------------------------------------------------------------------------
// FUTURE:  Refactor wcwidth_iter::next() to use character_sequence_state, so
// there's only one implementation of the logic?
bool character_sequence_state::next(const char32_t c)
{
    if (!c)
    {
        finish_sequence();
        m_curr_last_width = 0;
        m_curr_width = c_ctrl_width;
        return true;                    // Started new sequence.
    }

    // In the Windows console subsystem, a combining mark by itself has a
    // column width of 1.
    combining_mark_width_scope cmwidth_one(1);

    while (START != m_state)
    {
        const uint32 curr_last_width = m_curr_width;

        // Test for country flag.
        if (MAYBE_FLAG == m_state)
        {
            m_state = START;
            if (c >= 0x1f1e6 && c <= 0x1f1ff)
            {
#ifdef DEBUG
                m_emoji = true;
#endif
                m_curr_last_width = curr_last_width;
                m_curr_width = 2;
                return false;           // Continued current sequence.
            }
            break;
        }

        if (BEGIN_EMOJI == m_state)
        {
#ifdef DEBUG
            m_emoji = true;
#endif
            m_state = CONSUME_EMOJI_SEQ;
            if (m_unqualified && is_variant_selector(c))
            {
                // A variant selector after an unqualified form makes it
                // fully-qualified and full width (2 cells).
                assert(m_curr_width == 1 || m_curr_width == 2);
                m_curr_last_width = curr_last_width;
                m_curr_width = max<char32_t>(m_curr_width, 2);
                {
                    // PERF:  Technically, these shouldn't need to be reset,
                    // because they're only used while m_state == BEGIN_EMOJI,
                    // and they should always be initialized when entering the
                    // BEGIN_EMOJI state.
                    m_unqualified = false;
                    m_prev_c = 0;
                }
                return false;           // Continued current sequence.
            }
            else if (m_prev_c == 0x3030 || m_prev_c == 0x303d ||
                     m_prev_c == 0x3297 || m_prev_c == 0x3299)
            {
                // Special cases:  Windows Terminal renders some unqualified
                // emoji the same as their fully-qualified forms.
                assert(m_curr_width > 0);
                m_curr_last_width = curr_last_width;
                m_curr_width = max<char32_t>(m_curr_width, 2);
            }
            goto consume_emoji_seq;
        }

        if (CONSUME_EMOJI_SEQ == m_state)
        {
            // Consume the emoji sequence.
consume_emoji_seq:
#ifdef DEBUG
            assert(m_emoji);
#endif

            // Within emoji sequences, combining marks have zero width.
            combining_mark_width_scope cmwidth_zero(0);

            if (is_variant_selector(c))
            {
                // Variant selector implies full width emoji (2 cells).
                assert(m_curr_width >= 0 && m_curr_width <= 2);
                m_curr_last_width = curr_last_width;
                m_curr_width = max<char32_t>(m_curr_width, 2);
            }
            else if (c == 0x200d)
            {
                // ZWJ implies full width emoji (2 cells).
                assert(m_curr_width == 1 || m_curr_width == 2);
                m_curr_last_width = curr_last_width;
                m_curr_width = max<char32_t>(m_curr_width, 2);
                m_state = CONSUME_EMOJI_SEQ_ZWJ;
            }
            else
            {
                // Not part of an emoji sequence.
                m_state = START;
                break;
            }
            return false;               // Continued current sequence.
        }

        if (CONSUME_EMOJI_SEQ_ZWJ == m_state)
        {
            // Stop parsing if the character after the ZWJ is not an emoji.
            if (!is_emoji(c) &&
                !is_possible_unqualified_half_width(c) &&
                c != 0x2640 &&          // woman
                c != 0x2642)            // man
            {
                m_state = START;
                break;
            }

            // Accept the emoji after the ZWJ and continue parsing.
            m_state = CONSUME_EMOJI_SEQ;
            return false;               // Continued current sequence.
        }

        assert(CONTINUING == m_state);

        {
            // Collect a run until the next non-zero width character.
            const int32 w = wcwidth(c);
            if (w == 0)
                return false;           // Continued current sequence.

            // Variant selectors affect non-emoji as well, so treat them as
            // zero width for continuation purposes, but make the width 2.
            if (c_color_emoji && is_variant_selector(c))
            {
                assert(m_curr_width == 1 || m_curr_width == 2);
                m_curr_last_width = curr_last_width;
                m_curr_width = max<char32_t>(m_curr_width, 2);
                // A variant selector essentially makes the grapheme an emoji,
                // even if the base character isn't an emoji.
#ifdef DEBUG
                m_emoji = true;
#endif
                return false;           // Continued current sequence.
            }

            // The character starts a new grapheme.
            m_state = START;
        }
    }

    assert(START == m_state);

    finish_sequence();

    m_curr_width = wcwidth(c);
    if (m_curr_width < 0)
    {
        m_curr_width = c_ctrl_width;
        return true;
    }

    // Try to parse emoji sequences.
    if (c_color_emoji && m_curr_width)
    {
        // Check for a country flag sequence.
        if (c >= 0x1f1e6 && c <= 0x1f1ff)
        {
            m_state = MAYBE_FLAG;
            return true;
        }

        // If it's an emoji character, then try to parse an emoji sequence.
        const bool unq = is_possible_unqualified_half_width(c);
        if (unq || is_emoji(c))
        {
            m_state = BEGIN_EMOJI;
            m_prev_c = c;
            m_unqualified = unq;
            return true;
        }

        // A variant selector by itself effectively starts an emoji.
        if (is_variant_selector(c))
        {
            assert(m_curr_width == 1 || m_curr_width == 2);
            m_curr_width = max<char32_t>(m_curr_width, 2);
            m_state = CONSUME_EMOJI_SEQ;
#ifdef DEBUG
            m_emoji = true;
#endif
            return true;
        }
    }

    // Collect a run until the next non-zero width character.
    m_state = CONTINUING;

    return true;
}

//------------------------------------------------------------------------------
void character_sequence_state::finish_sequence()
{
    m_state = START;
    m_prev_width = m_curr_width;
    m_curr_width = 0;
    m_curr_last_width = 0;
#ifdef DEBUG
    m_emoji = false;
#endif
}



//------------------------------------------------------------------------------
wcwidth_iter::wcwidth_iter(const WCHAR* s, int32 len)
: m_iter(s, len)
{
    m_chr_ptr = m_chr_end = m_iter.get_pointer();
    m_next = m_iter.next();
}

//------------------------------------------------------------------------------
wcwidth_iter::wcwidth_iter(const wcwidth_iter& i)
: m_iter(i.m_iter)
, m_next(i.m_next)
, m_chr_ptr(i.m_chr_ptr)
, m_chr_end(i.m_chr_end)
, m_chr_wcwidth(i.m_chr_wcwidth)
, m_emoji(i.m_emoji)
{
}

//------------------------------------------------------------------------------
// This collects a char run according to the following rules:
//
//  - NUL ends a run without being part of the run.
//  - A control character or DEL is a run by itself.
//  - An emoji codepoint starts a run that includes the codepoint and
//    following codepoints for certain variant selectors, or zero width joiner
//    followed by another emoji codepoint.
//  - Otherwise a run includes a Unicode codepoint and any following
//    codepoints whose wcwidth is 0.
//
// This returns the first codepoint in the run.
char32_t wcwidth_iter::next()
{
    m_chr_ptr = m_chr_end;
    m_emoji = false;

    const char32_t c = m_next;

    if (!c)
    {
        m_chr_wcwidth = 0;
        return c;
    }

    m_chr_end = m_iter.get_pointer();
    m_next = m_iter.next();

    // In the Windows console subsystem, combining marks actually have a
    // column width of 1, not 0 as the original wcwidth implementation
    // expected.
    combining_mark_width_scope cmwidth(1);

    m_chr_wcwidth = wcwidth(c);
    if (m_chr_wcwidth < 0)
        return c;

    // Try to parse emoji sequences.
    const bool c_color_emoji = get_color_emoji();
    if (c_color_emoji && m_chr_wcwidth)
    {
        // Check for a country flag sequence.
        if (c >= 0x1f1e6 && c <= 0x1f1ff && m_next >= 0x1f1e6 && m_next <= 0x1f1ff)
        {
            m_emoji = true;
            m_chr_wcwidth = 2;
            m_chr_end = m_iter.get_pointer();
            m_next = m_iter.next();
            return c;
        }

        // If it's an emoji character, then try to parse an emoji sequence.
        const bool unq = is_possible_unqualified_half_width(c);
        if (unq || is_emoji(c))
        {
            // A variant selector after an unqualified form makes it
            // fully-qualified and be full width (2 cells).
            if (unq && is_variant_selector(m_next))
            {
                m_chr_end = m_iter.get_pointer();
fully_qualified:
                assert(m_chr_wcwidth == 1 || m_chr_wcwidth == 2);
                m_chr_wcwidth = max<char32_t>(m_chr_wcwidth, 2);
                m_next = m_iter.next();
            }
            else if (c == 0x3030 || c == 0x303d || c == 0x3297 || c == 0x3299)
            {
                // Special cases:  Windows Terminal renders some unqualified
                // emoji the same as their fully-qualified forms.
                assert(m_chr_wcwidth > 0);
                goto fully_qualified;
            }

            // Consume the emoji sequence.
emoji_sequence:
            consume_emoji_sequence();
            m_emoji = true;
            return c;
        }
        else if (is_variant_selector(c))
        {
            assert(m_chr_wcwidth == 1 || m_chr_wcwidth == 2);
            m_chr_wcwidth = max<char32_t>(m_chr_wcwidth, 2);
            goto emoji_sequence;
        }
    }

    // Collect a run until the next non-zero width character.
    while (m_next)
    {
        const int32 w = wcwidth(m_next);
        if (w != 0)
        {
            // Variant selectors affect non-emoji as well, so treat them as
            // zero width for continuation purposes, but make the width 2.
            if (c_color_emoji && is_variant_selector(m_next))
            {
                assert(m_chr_wcwidth == 1 || m_chr_wcwidth == 2);
                m_chr_wcwidth = max<char32_t>(m_chr_wcwidth, 2);
                m_emoji = true; // These essentially make it an emoji, even if the base character isn't an emoji.
            }
            else
                break;
        }
        m_chr_end = m_iter.get_pointer();
        m_next = m_iter.next();
    }

    return c;
}

//------------------------------------------------------------------------------
void wcwidth_iter::consume_emoji_sequence()
{
    // Within emoji sequences, combining marks have zero width.
    combining_mark_width_scope cmwidth(0);

    while (m_next)
    {
        if (is_variant_selector(m_next))
        {
            m_chr_end = m_iter.get_pointer();
            m_next = m_iter.next();
            // Variant selector implies full width emoji (2 cells).
            assert(m_chr_wcwidth >= 0 && m_chr_wcwidth <= 2);
            m_chr_wcwidth = max<char32_t>(m_chr_wcwidth, 2);
        }
        else if (m_next == 0x200d)
        {
            m_chr_end = m_iter.get_pointer();
            m_next = m_iter.next();
            // ZWJ implies full width emoji (2 cells).
            assert(m_chr_wcwidth == 1 || m_chr_wcwidth == 2);
            m_chr_wcwidth = max<char32_t>(m_chr_wcwidth, 2);
            // Stop parsing if the next character is not an emoji.
            if (!is_emoji(m_next) &&
                !is_possible_unqualified_half_width(m_next) &&
                m_next != 0x2640 &&                     // woman
                m_next != 0x2642)                       // man
                break;
            // Accept the next emoji, and advance to continue with the next
            // character, to handle joiners and variants.
            m_chr_end = m_iter.get_pointer();
            m_next = m_iter.next();
        }
        else
            break;
    }
}

//------------------------------------------------------------------------------
void wcwidth_iter::unnext()
{
    assert(m_iter.get_pointer() > m_chr_ptr || !m_iter.more());
    reset_pointer(m_chr_ptr);
}

//------------------------------------------------------------------------------
const WCHAR* wcwidth_iter::get_pointer() const
{
    return m_chr_end;
}

//------------------------------------------------------------------------------
void wcwidth_iter::reset_pointer(const WCHAR* s)
{
    m_iter.reset_pointer(s);
    m_chr_end = m_chr_ptr = s;
    m_chr_wcwidth = 0;
    m_emoji = false;
    m_next = m_iter.next();
}

//------------------------------------------------------------------------------
bool wcwidth_iter::more() const
{
    return (m_chr_end < m_iter.get_pointer()) || m_iter.more();
}

//------------------------------------------------------------------------------
uint32 wcwidth_iter::length() const
{
    return m_iter.length() + uint32(m_iter.get_pointer() - m_chr_end);
}
