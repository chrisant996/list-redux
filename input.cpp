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

InputRecord SelectInput(DWORD timeout)
{
    const HANDLE hin = GetStdHandle(STD_INPUT_HANDLE);
    const HANDLE hout = GetStdHandle(STD_OUTPUT_HANDLE);

    InputRecord input;
    while (input.type == InputType::None)
    {
        // Synthesize resize events by checking whether the terminal
        // dimensions have changed.

        static DWORD s_dimensions = GetConsoleColsRows(hout);
        const DWORD dimensions = GetConsoleColsRows(hout);
        if (dimensions != s_dimensions)
        {
            initialize_wcwidth();
            s_dimensions = dimensions;
            return { InputType::Resize };
        }

        // Wait for input.

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

        {
            DWORD count;
            if (!ReadConsoleInputW(hin, &record, 1, &count))
                return { InputType::Error };
        }

        // Process the input.

        switch (record.EventType)
        {
        case KEY_EVENT:
            input = ProcessInput(record.Event.KeyEvent);
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

bool ReadInput(StrW& out, std::vector<StrW>* history, DWORD max_width, std::optional<std::function<int32(const InputRecord&)>> input_callback)
{
    out.Clear();

    const HANDLE hin = GetStdHandle(STD_INPUT_HANDLE);
    const HANDLE hout = GetStdHandle(STD_OUTPUT_HANDLE);

    CONSOLE_SCREEN_BUFFER_INFO csbi;
    GetConsoleScreenBufferInfo(hout, &csbi);

    StrW curr_input_history;
    size_t history_index = history ? history->size() : 0;

    StrW tmp;
    while (true)
    {
        tmp.Clear();
        tmp.Printf(L"%s\x1b[%uG", c_hide_cursor, csbi.dwCursorPosition.X + 1);
        OutputConsole(hout, tmp.Text(), tmp.Length());

        const unsigned width = TruncateWcwidth(out, max_width, 0);
        tmp.Set(out);
        tmp.AppendSpaces(max_width - width);
        tmp.Printf(L"\x1b[%uG%s", csbi.dwCursorPosition.X + 1 + width, c_show_cursor);
        OutputConsole(hout, tmp.Text(), tmp.Length());

        const InputRecord input = SelectInput();
        switch (input.type)
        {
        case InputType::None:
        case InputType::Error:
        case InputType::Resize:
            continue;
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
                if (out.Length())
                    TruncateWcwidth(out, __wcswidth(out.Text()) - 1, 0);
                break;
            case Key::ENTER:
                if (history && out.Length())
                    history->emplace_back(out);
                return true;
            case Key::UP:
                if (history && history_index)
                {
                    if (history_index == history->size())
                        curr_input_history.Set(out);
                    --history_index;
                    out.Set((*history)[history_index]);
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
                }
                continue; // Avoid resetting history_index.
            }
        }
        else if (input.type == InputType::Char)
        {
            if (input.key_char >= ' ')
                out.Append(&input.key_char, 1);
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
