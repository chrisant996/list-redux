// Copyright (c) 2025 Christopher Antos
// Portions Copyright (c) 2016 Martin Ridgers
// License: http://opensource.org/licenses/MIT

#include "pch.h"
#include "input.h"
#include "output.h"
#include "wcwidth.h"
#include "wcwidth_iter.h"

static HANDLE s_interrupt = NULL;
static bool s_sending_terminal_request = false;

static const int32 CTRL_PRESSED = LEFT_CTRL_PRESSED|RIGHT_CTRL_PRESSED;
static const int32 ALT_PRESSED = LEFT_ALT_PRESSED|RIGHT_ALT_PRESSED;

const WCHAR c_prompt_char[] = L":";

InputRecord::InputRecord()
{
#ifdef DEBUG
    const BYTE* p = reinterpret_cast<BYTE*>(this);
    for (size_t i = 0; i < sizeof(*this); ++i)
        assert(0 == (*p++));
#endif
}

InputRecord::InputRecord(InputType _type)
: InputRecord()
{
    type = _type;
}

bool InputRecord::operator!=(const InputRecord& other) const
{
    if (type != other.type)
        return true;
    switch (type)
    {
    case InputType::Key:    return key != other.key || modifier != other.modifier;
    case InputType::Char:   return key_char != other.key_char || modifier != other.modifier;
    }
    return false;
}

void InputRecord::Clear()
{
    ZeroMemory(this, sizeof(*this));
}

static Modifier ModifierFromKeyFlags(int32 key_flags)
{
    Modifier mod = Modifier::None;
    if (key_flags & SHIFT_PRESSED) mod |= Modifier::SHIFT;
    if (key_flags & CTRL_PRESSED) mod |= Modifier::CTRL;
    if (key_flags & ALT_PRESSED) mod |= Modifier::ALT;
    return mod;
}

static InputRecord ProcessInput(KEY_EVENT_RECORD const& record)
{
    InputRecord input;

    int32 key_char = record.uChar.UnicodeChar;
    int32 key_vk = record.wVirtualKeyCode;
    int32 key_sc = record.wVirtualScanCode;
    int32 key_flags = record.dwControlKeyState;

    // Only respond to key down events.
    if (!record.bKeyDown)
    {
        // Some times conhost can send through ALT codes, with the resulting
        // Unicode code point in the Alt key-up event.
        if (key_vk == VK_MENU && key_char)
            key_flags = 0;
        else
            return { InputType::None };
    }

    // We filter out Alt key presses unless they generated a character.
    if (key_vk == VK_MENU)
    {
        if (key_char)
        {
            input.type = InputType::Char;
            input.key_char = key_char;
        }
        return input;
    }

    // Early out of unaccompanied Ctrl/Shift/Windows key presses.
    if (key_vk == VK_CONTROL || key_vk == VK_SHIFT || key_vk == VK_LWIN || key_vk == VK_RWIN)
        return { InputType::None };

    // Special treatment for escape.
    if (key_char == 0x1b /*&& key_vk == VK_ESCAPE*/)
    {
        input.type = InputType::Key;
        input.key = Key::ESC;
        return input;
    }

    // If the input was formed using AltGr or LeftAlt-LeftCtrl then things get
    // tricky. But there's always a Ctrl bit set, even if the user didn't press
    // a ctrl key. We can use this and the knowledge that Ctrl-modified keys
    // aren't printable to clear appropriate AltGr flags.
    if ((key_char > 0x1f && key_char != 0x7f) && (key_flags & CTRL_PRESSED))
        key_flags &= ~(CTRL_PRESSED|ALT_PRESSED);

    // Special case for ctrl-shift-I (to behave like shift-tab aka. back-tab).
    if (key_char == '\t')
    {
        input.type = InputType::Key;
        input.key = Key::TAB;
        input.modifier = ModifierFromKeyFlags(key_flags);
        return input;
    }

    // Function keys (kf1-kf48 from xterm+pcf2)
    unsigned key_func = key_vk - VK_F1;
    if (key_func <= (VK_F12 - VK_F1))
    {
        input.type = InputType::Key;
        input.key = Key(unsigned(Key::F1) + key_func);
        input.modifier = ModifierFromKeyFlags(key_flags);
        return input;
    }

    // Character keys.
    if (key_char)
    {
        assert(key_vk != VK_TAB);

        // Map Ctrl-H/I/M combinations to BACK/TAB/ENTER.
        if (key_flags & CTRL_PRESSED)
        {
            assert(input.key == Key::Invalid);
            switch (key_vk)
            {
            case 'H':   input.key = Key::BACK; break;
            case 'I':   input.key = Key::TAB; break;
            case 'M':   input.key = Key::ENTER; break;
            }
            if (input.key != Key::Invalid)
            {
                input.type = InputType::Key;
                input.modifier = ModifierFromKeyFlags(key_flags) & ~Modifier::CTRL;
                return input;
            }
        }

        bool simple_char;
        if (key_char == 0x1b && key_vk != VK_ESCAPE)
            simple_char = !(key_flags & ALT_PRESSED);
        else if (key_vk == VK_RETURN || key_vk == VK_BACK)
            simple_char = !(key_flags & (CTRL_PRESSED|SHIFT_PRESSED));
        else
            simple_char = !(key_flags & CTRL_PRESSED) || !(key_flags & SHIFT_PRESSED);

        if (simple_char)
        {
            if (key_vk == VK_RETURN || key_vk == VK_BACK || key_vk == VK_TAB)
            {
                // Don't handle these as characters, handle them as special
                // keys further down.
            }
            else
            {
                input.type = InputType::Char;
                input.key_char = key_char;
                input.modifier = ModifierFromKeyFlags(key_flags);
                return input;
            }
        }
    }

    // Special keys.
    Key key = Key::Invalid;
    switch (key_vk)
    {
    case VK_BACK:   key = Key::BACK; break;
    case VK_TAB:    key = Key::TAB; break;
    case VK_RETURN: key = Key::ENTER; break;
    case VK_UP:     key = Key::UP; break;
    case VK_DOWN:   key = Key::DOWN; break;
    case VK_LEFT:   key = Key::LEFT; break;
    case VK_RIGHT:  key = Key::RIGHT; break;
    case VK_HOME:   key = Key::HOME; break;
    case VK_END:    key = Key::END; break;
    case VK_INSERT: key = Key::INS; break;
    case VK_DELETE: key = Key::DEL; break;
    case VK_PRIOR:  key = Key::PGUP; break;
    case VK_NEXT:   key = Key::PGDN; break;
    }
    if (Key::Invalid != key)
    {
        input.type = InputType::Key;
        input.key = key;
        input.modifier = ModifierFromKeyFlags(key_flags);
        return input;
    }

    // Ctrl-Character keys.
    if (key_flags & CTRL_PRESSED)
    {
        bool ctrl_code = false;

        if (!(key_flags & SHIFT_PRESSED))
        {
            switch (key_vk)
            {
            case 'A':   case 'B':   case 'C':   case 'D':
            case 'E':   case 'F':   case 'G':   case 'H':
            case 'I':   case 'J':   case 'K':   case 'L':
            case 'M':   case 'N':   case 'O':   case 'P':
            case 'Q':   case 'R':   case 'S':   case 'T':
            case 'U':   case 'V':   case 'W':   case 'X':
            case 'Y':   case 'Z':
                assert(key_vk != 'H' && key_vk != 'I' && key_vk != 'M');
                key_vk -= 'A' - 1;
                ctrl_code = true;
                break;
            default:
                // Can't use VK_OEM_4, VK_OEM_5, and VK_OEM_6 for detecting ^[,
                // ^\, and ^] because OEM key mapping differ by keyboard/locale.
                // However, the OS/OEM keyboard driver produces enough details
                // to make it possible to identify what's really going on, at
                // least for these specific keys (but not for VK_OEM_MINUS, 2,
                // or 6).  Ctrl makes the bracket and backslash keys produce the
                // needed control code in key_char, so we can simply use that.
                switch (key_char)
                {
                case 0x1b:
                case 0x1c:
                case 0x1d:
                    key_vk = key_char;
                    ctrl_code = true;
                    break;
                }
                break;
            }
        }

        if (ctrl_code)
        {
            input.type = InputType::Char;
            input.key_char = key_vk;
            input.modifier = ModifierFromKeyFlags(key_flags);
            return input;
        }
    }

    switch (key_vk)
    {
    case 'A':   case 'B':   case 'C':   case 'D':
    case 'E':   case 'F':   case 'G':   case 'H':
    case 'I':   case 'J':   case 'K':   case 'L':
    case 'M':   case 'N':   case 'O':   case 'P':
    case 'Q':   case 'R':   case 'S':   case 'T':
    case 'U':   case 'V':   case 'W':   case 'X':
    case 'Y':   case 'Z':
    case '0':   case '1':   case '2':   case '3':
    case '4':   case '5':   case '6':   case '7':
    case '8':   case '9':
        input.type = InputType::Char;
        input.key_char = key_vk;
        input.modifier = ModifierFromKeyFlags(key_flags);
        return input;
#if 0
    case VK_OEM_1:              // ';:' for US
    case VK_OEM_PLUS:           // '+' for any country
    case VK_OEM_COMMA:          // ',' for any country
    case VK_OEM_MINUS:          // '-' for any country
    case VK_OEM_PERIOD:         // '.' for any country
    case VK_OEM_2:              // '/?' for US
    case VK_OEM_3:              // '`~' for US
    case VK_OEM_4:              // '[{' for US
    case VK_OEM_5:              // '\|' for US
    case VK_OEM_6:              // ']}' for US
    case VK_OEM_7:              // ''"' for US
#endif
    }

    return input;
}

static InputRecord ProcessInput(MOUSE_EVENT_RECORD const& record)
{
    InputRecord input;

    const COORD mouse_pos = record.dwMousePosition;
    const DWORD key_state = record.dwControlKeyState;
    const DWORD event_flags = record.dwEventFlags;

    // Remember the button state, to differentiate press vs release.
    static DWORD s_prev_button_state;
    const auto prv = s_prev_button_state;
    s_prev_button_state = record.dwButtonState;

    // In a race condition, both left and right click may happen simultaneously.
    // Only respond to one; left has priority over right.
    const auto btn = record.dwButtonState;
    const bool left_click = (!(prv & FROM_LEFT_1ST_BUTTON_PRESSED) && (btn & FROM_LEFT_1ST_BUTTON_PRESSED));
    const bool right_click = !left_click && (!(prv & RIGHTMOST_BUTTON_PRESSED) && (btn & RIGHTMOST_BUTTON_PRESSED));
    const bool double_click = left_click && (record.dwEventFlags & DOUBLE_CLICK);
    const bool wheel = !left_click && !right_click && (record.dwEventFlags & MOUSE_WHEELED);
    const bool hwheel = !left_click && !right_click && !wheel && (record.dwEventFlags & MOUSE_HWHEELED);
    const bool drag = (btn & FROM_LEFT_1ST_BUTTON_PRESSED) && !left_click && !right_click && !wheel && !hwheel && (record.dwEventFlags & MOUSE_MOVED);

    enum class mouse_input_type { none, left_click, right_click, double_click, wheel, hwheel, drag };
    const mouse_input_type mask = (left_click ? mouse_input_type::left_click :
                                   right_click ? mouse_input_type::right_click :
                                   double_click ? mouse_input_type::double_click :
                                   wheel ? mouse_input_type::wheel :
                                   hwheel ? mouse_input_type::hwheel :
                                   drag ? mouse_input_type::drag :
                                   mouse_input_type::none);

    if (mask == mouse_input_type::none)
        return input;

    input.mouse_pos = mouse_pos;

    // Left or right click, or drag.
    if (left_click || right_click || drag)
    {
        input.type = InputType::Mouse;
        input.key = (drag ? Key::MouseDrag :
                     right_click ? Key::MouseRightClick :
                     double_click ? Key::MouseLeftDblClick :
                     Key::MouseLeftClick);
        return input;
    }

    // Mouse wheel.
    if (wheel)
    {
        int32 direction = (0 - short(HIWORD(record.dwButtonState))) / 120;
        UINT wheel_scroll_lines = 3;
        SystemParametersInfo(SPI_GETWHEELSCROLLLINES, 0, &wheel_scroll_lines, false);

        input.type = InputType::Mouse;
        input.key = Key::MouseWheel;
        input.mouse_wheel_amount = direction * int32(wheel_scroll_lines);
        return input;
    }

    // Mouse horizontal wheel.
    if (hwheel)
    {
        int32 direction = (short(HIWORD(record.dwButtonState))) / 32;
        UINT hwheel_distance = 1;

        input.type = InputType::Mouse;
        input.key = Key::MouseHWheel;
        input.mouse_wheel_amount = direction * int32(hwheel_distance);
        return input;
    }

    return input;
}

InputRecord SelectInput(const DWORD timeout)
{
    const HANDLE hin = GetStdHandle(STD_INPUT_HANDLE);
    const HANDLE hout = GetStdHandle(STD_OUTPUT_HANDLE);

    static INPUT_RECORD s_cached_record;
    static bool s_has_cached_record = false;

    InputRecord input;
    InputRecord lead_surrogate;
    bool has_lead_surrogate = false;
    while (input.type == InputType::None || has_lead_surrogate)
    {
        // Synthesize resize events by checking whether the terminal
        // dimensions have changed.  But not while trying to read both high
        // and low surrogates in a surrogate pair).

        static DWORD s_dimensions = GetConsoleColsRows(hout);
        const DWORD dimensions = GetConsoleColsRows(hout);
        if (dimensions != s_dimensions && !has_lead_surrogate)
        {
            initialize_wcwidth();
            s_dimensions = dimensions;
            return { InputType::Resize };
        }

        // Wait for input.

        if (!s_has_cached_record)
        {
            uint32 count = 1;
            HANDLE handles[3] = { hin };

            const DWORD waited = WaitForMultipleObjects(count, handles, false, timeout);
            if (waited == WAIT_TIMEOUT)
                return { InputType::None };
            if (waited != WAIT_OBJECT_0)
                return { InputType::Error };
        }

        // Read the available input.

        INPUT_RECORD record;

        if (!s_has_cached_record)
        {
            DWORD count;
            if (!ReadConsoleInputW(hin, &record, 1, &count))
                return { InputType::Error };
        }
        else
        {
            assert(!has_lead_surrogate);
            record = s_cached_record;
            s_has_cached_record = false;
        }

        // Process the input.

        if (has_lead_surrogate)
        {
            assert(!lead_surrogate.key_char2);
            if (record.EventType == KEY_EVENT)
            {
                input = ProcessInput(record.Event.KeyEvent);
                if (input.type == InputType::None)
                    continue;
                if (input.type == InputType::Char && IS_LOW_SURROGATE(input.key_char))
                    lead_surrogate.key_char2 = input.key_char;
                else
                    goto severed;
            }
            else
            {
severed:
                s_cached_record = record;
                s_has_cached_record = true;
                lead_surrogate.key_char = 0xfffd;
            }
            return lead_surrogate;
        }

        switch (record.EventType)
        {
        case KEY_EVENT:
            input = ProcessInput(record.Event.KeyEvent);
            // When timeout is INFINITE, try to return both surrogate pairs at
            // the same time.
            if (timeout == INFINITE && input.type == InputType::Char && IS_HIGH_SURROGATE(input.key_char))
            {
                assert(!has_lead_surrogate);
                lead_surrogate = input;
                has_lead_surrogate = true;
                continue;
            }
            break;
        case MOUSE_EVENT:
            input = ProcessInput(record.Event.MouseEvent);
            break;
        default:
            continue;
        }
    }

    return input;
}

struct GraphemeInfo
{
    unsigned short index;
    unsigned short length;
    unsigned short width;
};

static std::vector<GraphemeInfo> ParseGraphemes(const WCHAR* s, const unsigned len, const unsigned short pos, size_t& index_pos)
{
    std::vector<GraphemeInfo> characters;

    wcwidth_iter iter(s, len);
    unsigned short char_index = 0;
    size_t i_p = 0;
    while (iter.next())
    {
        if (char_index <= pos)
            i_p = characters.size();
        const unsigned short char_length = iter.character_length();
        characters.push_back(GraphemeInfo { char_index, char_length, (unsigned short)iter.character_wcwidth_onectrl() });
        char_index += char_length;
    }
    assert(char_index == len);

    index_pos = i_p;
    return characters;
}

static void BackUpByAmount(unsigned short& pos, const WCHAR* s, unsigned len, unsigned backup)
{
    if (pos)
    {
        size_t index_pos = 0;
        std::vector<GraphemeInfo> characters = ParseGraphemes(s, len, pos, index_pos);
        if (!characters.size())
            return;

        if (!index_pos)
        {
            pos = 0;
            return;
        }

        if (index_pos >= characters.size() || characters[index_pos].index == pos)
            --index_pos;

        bool at_least_one = true;
        while (at_least_one || characters[index_pos].width <= backup)
        {
            at_least_one = false;
            pos = characters[index_pos].index;
            backup -= characters[index_pos].width;
            if (!index_pos)
                break;
            --index_pos;
        }
    }
}

static unsigned short PosMover(const WCHAR* s, const unsigned len, unsigned short& pos, const bool forward, const bool word)
{
    size_t index_pos = 0;
    std::vector<GraphemeInfo> characters = ParseGraphemes(s, len, pos, index_pos);

    if (pos && index_pos < characters.size() && pos != characters[index_pos].index)
    {
        if (forward)
            --index_pos;
        else
            ++index_pos;
    }

    const size_t orig_index_pos = index_pos;

    if (forward)
    {
        if (pos < len)
        {
            if (!word)
            {
                if (index_pos < characters.size())
                    ++index_pos;
            }
            else
            {
                while (index_pos < characters.size())
                {
                    const auto& g = characters[index_pos];
                    if (!(g.length == 1 && iswspace(s[g.index])))
                        break;
                    ++index_pos;
                }
                while (index_pos < characters.size())
                {
                    const auto& g = characters[index_pos];
                    if (g.length == 1 && iswspace(s[g.index]))
                        break;
                    ++index_pos;
                }
            }

            if (index_pos < characters.size())
                pos = characters[index_pos].index;
            else
                pos = len;
        }
    }
    else
    {
        if (pos > 0)
        {
            if (!word)
            {
                if (index_pos)
                    --index_pos;
            }
            else
            {
                assert(index_pos);
                while (index_pos)
                {
                    const size_t test_index = index_pos - 1;
                    const auto& g = characters[test_index];
                    if (!(g.length == 1 && iswspace(s[g.index])))
                        break;
                    index_pos = test_index;
                }
                while (index_pos)
                {
                    const size_t test_index = index_pos - 1;
                    const auto& g = characters[test_index];
                    if (g.length == 1 && iswspace(s[g.index]))
                        break;
                    index_pos = test_index;
                }
            }

            if (index_pos < characters.size())
                pos = characters[index_pos].index;
            else
                pos = 0;
        }
    }

    unsigned short moved = 0;
    const size_t begin = min(index_pos, orig_index_pos);
    const size_t end = max(index_pos, orig_index_pos);
    for (size_t i = begin; i < end; ++i)
        moved += characters[i].length;
    return moved;
}

bool ReadInput(StrW& out, History hindex, DWORD max_length, DWORD max_width, std::optional<std::function<int32(const InputRecord&)>> input_callback)
{
    static std::vector<StrW> s_histories[size_t(History::MAX)];
    std::vector<StrW>* const history = (size_t(hindex) < _countof(s_histories)) ? &s_histories[size_t(hindex)] : nullptr;
    max_length = max<DWORD>(max_length, 1);
    max_length = min<DWORD>(max_length, 1024);

    out.Clear();

    const HANDLE hin = GetStdHandle(STD_INPUT_HANDLE);
    const HANDLE hout = GetStdHandle(STD_OUTPUT_HANDLE);

    CONSOLE_SCREEN_BUFFER_INFO csbi;
    GetConsoleScreenBufferInfo(hout, &csbi);

    if (unsigned(csbi.dwCursorPosition.X) + 8 >= unsigned(csbi.dwSize.X))
        return false;
    if (unsigned(csbi.dwCursorPosition.X) + max_width >= unsigned(csbi.dwSize.X))
        max_width = csbi.dwSize.X - csbi.dwCursorPosition.X;

    StrW curr_input_history;
    size_t history_index = history ? history->size() : 0;

    StrW tmp;
    unsigned short left = 0;
    unsigned short pos = 0;
    while (true)
    {
        tmp.Clear();
        tmp.Printf(L"%s\x1b[%uG", c_hide_cursor, csbi.dwCursorPosition.X + 1);
        OutputConsole(hout, tmp.Text(), tmp.Length());

        left = min(left, pos);

        // Auto-scroll horizontally forward.
        while (__wcswidth(out.Text() + left, pos - left) >= max_width)
        {
            wcwidth_iter iter(out.Text() + left, out.Length() - left);
            if (!iter.next())
                break;
            left += iter.character_length();
        }

        // Auto-scroll horizontally backward.
        assert(pos >= left);
        {
            unsigned short backup_left = pos;
            BackUpByAmount(backup_left, out.Text(), pos, 4);
            if (left > backup_left)
                left = backup_left;
        }

        // Print the visible part of the input string.
        tmp.Set(out.Text() + left, out.Length() - left);
        const unsigned width = TruncateWcwidth(tmp, max_width, 0);
        tmp.AppendSpaces(max_width - width);
        tmp.Printf(L"\x1b[%uG%s", csbi.dwCursorPosition.X + 1 + __wcswidth(out.Text() + left, pos - left), c_show_cursor);
        OutputConsole(hout, tmp.Text(), tmp.Length());

        const InputRecord input = SelectInput();
        switch (input.type)
        {
        case InputType::None:
        case InputType::Error:
            continue;
        case InputType::Resize:
            return false;
        }

        if (input_callback)
        {
            const int32 result = (*input_callback)(input);
            // Negative means cancel (like ESC).
            if (result < 0)
                return false;
            // Positive means do not process (already handled).
            if (result > 0)
                continue;
            // Zero means allow normal processing.
        }

        if (input.type == InputType::Key)
        {
            switch (input.key)
            {
            case Key::ESC:
                out.Clear();
                return false;
            case Key::BACK:
                if ((input.modifier & ~Modifier::CTRL) == Modifier::None)
                {
                    if (pos > 0)
                    {
                        const bool word = ((input.modifier & Modifier::CTRL) == Modifier::CTRL);
                        const unsigned short old_pos = pos;
                        const unsigned short moved = PosMover(out.Text(), out.Length(), pos, false/*forward*/, word);
                        if (old_pos == out.Length())
                        {
                            out.SetLength(pos);
                        }
                        else
                        {
                            const unsigned short pos_keep = (pos + moved);
                            tmp.Set(out.Text(), pos);
                            tmp.Append(out.Text() + pos_keep, out.Length() - pos_keep);
                            out = std::move(tmp);
                        }
                    }
                }
                break;
            case Key::DEL:
                if ((input.modifier & ~Modifier::CTRL) == Modifier::None)
                {
                    if (unsigned(pos) < out.Length())
                    {
                        const bool word = ((input.modifier & Modifier::CTRL) == Modifier::CTRL);
                        unsigned short del_pos = pos;
                        const unsigned short moved = PosMover(out.Text(), out.Length(), del_pos, true/*forward*/, word);
                        pos = del_pos - moved;
                        if (pos + moved == out.Length())
                        {
                            out.SetLength(pos);
                        }
                        else
                        {
                            const unsigned short pos_keep = (pos + moved);
                            tmp.Set(out.Text(), pos);
                            tmp.Append(out.Text() + pos_keep, out.Length() - pos_keep);
                            out = std::move(tmp);
                        }
                    }
                }
                break;
            case Key::ENTER:
                if (history && out.Length())
                    history->emplace_back(out);
                return true;
            case Key::HOME:
                pos = 0;
                left = 0;
                continue;
            case Key::END:
                pos = out.Length();
                left = pos;
                BackUpByAmount(left, out.Text(), pos, max_width - 1);
                continue;
            case Key::UP:
                if (history && history_index)
                {
                    if (history_index == history->size())
                        curr_input_history.Set(out);
                    --history_index;
                    out.Set((*history)[history_index]);
                    pos = out.Length();
                    left = pos;
                    BackUpByAmount(left, out.Text(), pos, max_width - 1);
                }
                continue; // Avoid resetting history_index.
            case Key::DOWN:
                if (history && history_index < history->size())
                {
                    ++history_index;
                    if (history_index == history->size())
                        out.Set(curr_input_history);
                    else
                        out.Set((*history)[history_index]);
                    pos = out.Length();
                    left = pos;
                    BackUpByAmount(left, out.Text(), pos, max_width - 1);
                }
                continue; // Avoid resetting history_index.
            case Key::LEFT:
// TODO:  CUA (Shift-LEFT, Ctrl-Shift-LEFT)
                if (pos > 0)
                {
                    const bool word = ((input.modifier & Modifier::CTRL) == Modifier::CTRL);
                    PosMover(out.Text(), out.Length(), pos, false/*forward*/, word);
                }
                break;
            case Key::RIGHT:
// TODO:  CUA (Shift-RIGHT, Ctrl-Shift-RIGHT)
                if (unsigned(pos) < out.Length())
                {
                    const bool word = ((input.modifier & Modifier::CTRL) == Modifier::CTRL);
                    PosMover(out.Text(), out.Length(), pos, true/*forward*/, word);
                }
                break;
            }
        }
        else if (input.type == InputType::Char)
        {
            if (input.key_char >= ' ')
            {
                // Limit the length.
                if (out.Length() >= max_length)
                    continue;

                // Append or insert the character.
                if (pos == out.Length())
                {
                    out.Append(&input.key_char, input.key_char2 ? 2 : 1);
                    pos = out.Length();
                }
                else
                {
                    const int32 i = pos;
                    tmp.Set(out.Text(), i);
                    tmp.Append(&input.key_char, input.key_char2 ? 2 : 1);
                    pos = tmp.Length();
                    tmp.Append(out.Text() + i, out.Length() - i);
                    out = std::move(tmp);
                }
            }
        }

        if (history && history_index < history->size())
            history_index = history->size();
    }
}

AutoMouseConsoleMode::AutoMouseConsoleMode(HANDLE hin)
{
    m_hin = hin ? hin : GetStdHandle(STD_INPUT_HANDLE);
    if (m_hin && !GetConsoleMode(m_hin, &m_orig_mode))
        m_hin = 0;
}

AutoMouseConsoleMode::~AutoMouseConsoleMode()
{
    if (m_hin)
        SetConsoleMode(m_hin, m_orig_mode);
}

void AutoMouseConsoleMode::EnableMouseInput(bool enable)
{
    if (m_hin)
    {
        const DWORD new_mode = ((m_orig_mode & ~ENABLE_MOUSE_INPUT) |
                                (enable ? ENABLE_MOUSE_INPUT : 0));
        if (new_mode != m_prev_mode && SetConsoleMode(m_hin, new_mode))
            m_prev_mode = new_mode;
    }
}
