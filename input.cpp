// Copyright (c) 2025 Christopher Antos
// Portions Copyright (c) 2016 Martin Ridgers
// License: http://opensource.org/licenses/MIT

#include "pch.h"
#include "input.h"
#include "output.h"
#include "vieweroptions.h"
#include "colors.h"
#include "wcwidth.h"
#include "wcwidth_iter.h"
#include "ellipsify.h"

#include <set>

static HANDLE s_interrupt = NULL;
static DWORD s_prev_button_state = 0;
static bool s_sending_terminal_request = false;

static const int32 CTRL_PRESSED = LEFT_CTRL_PRESSED|RIGHT_CTRL_PRESSED;
static const int32 ALT_PRESSED = LEFT_ALT_PRESSED|RIGHT_ALT_PRESSED;

const WCHAR c_prompt_char[] = L":";

// Don't use dwButtonState directly, due to an OS bug.  If SetConsoleMode
// removes ENABLE_MOUSE_INPUT while a mouse button is down, then
// ReadConsoleInputW keeps reporting the button is down.  The state doesn't
// resync with reality until after ENABLE_MOUSE_INPUT is added again and the
// button is pressed and released again.
#pragma region dwButtonState workaround

static short GetWheelDirection(const MOUSE_EVENT_RECORD& record)
{
    return short(HIWORD(record.dwButtonState));
}

#define dwButtonState __OS_bug_makes_dwButtonState_unreliable__

static DWORD GetButtonState()
{
    DWORD dw = 0;
    if (GetKeyState(VK_LBUTTON) & 0x8000)
        dw |= FROM_LEFT_1ST_BUTTON_PRESSED;
    if (GetKeyState(VK_RBUTTON) & 0x8000)
        dw |= RIGHTMOST_BUTTON_PRESSED;
    return dw;
}

#pragma endregion //dwButtonState workaround

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

    // Remember the previous button state, to differentiate between press vs
    // release.
    const auto btn = GetButtonState();
    const auto prv = s_prev_button_state;
    s_prev_button_state = btn;

    // In a race condition, both left and right click may happen simultaneously.
    // Only respond to one; left has priority over right.
    const bool left_click = (!(prv & FROM_LEFT_1ST_BUTTON_PRESSED) && (btn & FROM_LEFT_1ST_BUTTON_PRESSED));
    const bool right_click = !left_click && (!(prv & RIGHTMOST_BUTTON_PRESSED) && (btn & RIGHTMOST_BUTTON_PRESSED));
    const bool double_click = left_click && (record.dwEventFlags & DOUBLE_CLICK);
    const bool wheel = !left_click && !right_click && (record.dwEventFlags & MOUSE_WHEELED);
    const bool hwheel = !left_click && !right_click && !wheel && (record.dwEventFlags & MOUSE_HWHEELED);
    const bool drag = (btn & FROM_LEFT_1ST_BUTTON_PRESSED) && !left_click && !right_click && !wheel && !hwheel && (record.dwEventFlags & MOUSE_MOVED);

    enum class mouse_input_type { none, left_click, right_click, double_click, wheel, hwheel, drag };
    const mouse_input_type type = (left_click ? mouse_input_type::left_click :
                                   right_click ? mouse_input_type::right_click :
                                   double_click ? mouse_input_type::double_click :
                                   wheel ? mouse_input_type::wheel :
                                   hwheel ? mouse_input_type::hwheel :
                                   drag ? mouse_input_type::drag :
                                   mouse_input_type::none);

    if (type == mouse_input_type::none)
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
        int32 direction = (0 - GetWheelDirection(record)) / 120;
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
        int32 direction = (GetWheelDirection(record)) / 32;
        UINT hwheel_distance = 1;

        input.type = InputType::Mouse;
        input.key = Key::MouseHWheel;
        input.mouse_wheel_amount = direction * int32(hwheel_distance);
        return input;
    }

    return input;
}

InputRecord SelectInput(const DWORD timeout, AutoMouseConsoleMode* mouse)
{
    const HANDLE hin = GetStdHandle(STD_INPUT_HANDLE);

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

        static DWORD s_dimensions = GetConsoleColsRows();
        const DWORD dimensions = GetConsoleColsRows();
        if (dimensions != s_dimensions && !has_lead_surrogate)
        {
            initialize_wcwidth();
            s_dimensions = dimensions;
            return { InputType::Resize };
        }

        // Wait for input.

        if (mouse)
            mouse->DisableMouseInputIfShift();

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

bool IsMouseLeftButtonDown()
{
    return !!(GetButtonState() & FROM_LEFT_1ST_BUTTON_PRESSED);
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

static void BackUpByAmount(textpos_t& pos, const WCHAR* s, unsigned len, unsigned backup)
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

static textpos_t PosMover(const WCHAR* s, const unsigned len, textpos_t& pos, const bool forward, const bool word)
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

    textpos_t moved = 0;
    const size_t begin = min(index_pos, orig_index_pos);
    const size_t end = max(index_pos, orig_index_pos);
    for (size_t i = begin; i < end; ++i)
        moved += characters[i].length;
    return moved;
}

struct SelectionState
{
                    SelectionState() : m_anchor(0), m_caret(0), m_dirty(false) { ResetWordAnchor(); }
                    SelectionState(textpos_t caret) : m_anchor(caret), m_caret(caret), m_dirty(false) { ResetWordAnchor(); }
                    SelectionState(textpos_t anchor, textpos_t caret) : m_anchor(anchor), m_caret(caret), m_dirty(false) { ResetWordAnchor(); }

    void            SetCaret(textpos_t caret) { SetSelection(caret, caret); }
    void            SetSelection(textpos_t anchor, textpos_t caret);
#if 0
    void            ResetWordAnchor() { m_word_anchor_begin = m_anchor; m_word_anchor_end = m_caret; }
    void            ResetWordAnchor(textpos_t caret) { m_word_anchor_begin = m_anchor; m_word_anchor_end = caret; }
#else
    void            ResetWordAnchor() {}
#endif

    textpos_t       GetAnchor() const { return m_anchor; }
    textpos_t       GetCaret() const { return m_caret; }
    textpos_t       GetSelBegin() const { return min(m_anchor, m_caret); }
    textpos_t       GetSelEnd() const { return max(m_anchor, m_caret); }
#if 0
    int             GetWordAnchorBegin() const { return m_word_anchor_begin; }
    int             GetWordAnchorEnd() const { return m_word_anchor_end; }
#endif
    bool            HasSelection() const { return m_anchor != m_caret; }

    bool            IsDirty() const { return m_dirty; }
    void            ClearDirty() { m_dirty = false; }

    textpos_t&      GetAnchorOut() { return m_anchor; }
    textpos_t&      GetCaretOut() { return m_caret; }

private:
    textpos_t       m_anchor;
    textpos_t       m_caret;
#if 0
    short           m_word_anchor_begin;
    short           m_word_anchor_end;
#endif
    bool            m_dirty;
};

void SelectionState::SetSelection(textpos_t anchor, textpos_t caret)
{
    assert(anchor != static_cast<textpos_t>(-1));
    assert(caret != static_cast<textpos_t>(-1));
    if (anchor != m_anchor || caret != m_caret)
        m_dirty = true;
    m_anchor = anchor;
    m_caret = caret;
}

struct UndoEntry
{
                    UndoEntry() = default;
                    ~UndoEntry();
    void            LinkAtTail(UndoEntry*& head, UndoEntry*& tail);
    void            Unlink(UndoEntry*& head, UndoEntry*& tail);

    StrW            m_s;
    SelectionState  m_sel_before;
    SelectionState  m_sel_after;

    UndoEntry*      m_prev = nullptr;
    UndoEntry*      m_next = nullptr;
};

UndoEntry::~UndoEntry()
{
    assert(!m_prev);
    assert(!m_next);
}

void UndoEntry::LinkAtTail(UndoEntry*& head, UndoEntry*& tail)
{
    assert(!m_prev);
    assert(!m_next);
    m_prev = tail;
    if (tail)
        tail->m_next = this;
    if (!head)
        head = this;
    tail = this;
}

void UndoEntry::Unlink(UndoEntry*& head, UndoEntry*& tail)
{
    if (m_prev)
        m_prev->m_next = m_next;
    else
        head = m_next;
    if (m_next)
        m_next->m_prev = m_prev;
    else
        tail = m_prev;
    m_prev = nullptr;
    m_next = nullptr;
}

class ReadInputState
{
    enum class Outcome { Cancelled, Done, DontResetHistoryIndex, ResetHistoryIndex };

public:
                    ReadInputState();
                    ~ReadInputState();

    void            SetMaxWidth(DWORD m) { m_max_width = static_cast<textpos_t>(min<DWORD>(m, INT16_MAX)); }
    void            SetMaxLength(DWORD m) { m_max_length = static_cast<textpos_t>(min<DWORD>(m, INT16_MAX)); }
    void            SetCallback(std::optional<std::function<int32(const InputRecord&, void*)>> input_callback);
    void            SetHistory(std::vector<StrW>* history);
    void            SetHorizScrollMarkers(bool show) { m_horiz_scroll_markers = show; }
    void            SetOrigin(COORD coord) { m_origin = coord; }

    void            InitializeText(const WCHAR* s, int32 len=-1);

    int32           Go(void* cookie=nullptr);

    void            Home(Modifier modifier);
    void            End(Modifier modifier);
    void            Left(Modifier modifier);
    void            Right(Modifier modifier);
    void            Backspace(bool word=false);
    void            Delete(bool word=false);

    void            SetSelection(textpos_t anchor, textpos_t caret);
    void            SelectWord();

    void            CopyToClipboard();
    void            CutToClipboard();
    void            PasteFromClipboard();

    textpos_t       GetCaret() const { return m_sel.GetCaret(); }
    void            ReplaceFromHistory(const StrW& s, bool keep_undo);
    void            InsertChar(WCHAR c, WCHAR c2=0);
    void            InsertText(const WCHAR* s, size_t len);
    void            RemoveText(textpos_t begin, textpos_t end);
    bool            ElideSelectedText();

    void            ClearUndo() { InitUndo(); }
    void            BeginUndoGroup();
    void            EndUndoGroup();
    void            Undo();
    void            Redo();

    void            TransferText(StrW& out);

#ifdef DEBUG
    void            DumpUndoStack();
#endif

private:
    void            EnsureLeft();
    void            PrintVisible();
    Outcome         HandleInput(const InputRecord& input);
    void            InitUndo();
    void            ClearUndoInternal();
    void            UnlinkUndoEntry(UndoEntry* p);

private:
    // Configuration.
    uint16          m_max_width = 32;
    uint16          m_max_length = 32;
    COORD           m_origin = { -1, -1 };
    bool            m_horiz_scroll_markers = true;

    // Content and state.
    StrW            m_s;
    uint32          m_change_counter = 0;
    uint16          m_terminal_row = 0;
    textpos_t       m_left = 0;
    SelectionState  m_sel;
    MouseHelper     m_mouse_helper;
    bool            m_can_drag = false;

    // Undo/Redo queue.
    UndoEntry*      m_undo_head = nullptr;
    UndoEntry*      m_undo_tail = nullptr;
    UndoEntry*      m_undo_current = nullptr;
    short           m_grouping = 0;  // >0 means an undo group is in progress.
    bool            m_defer_init_undo = false;

    // History.
    std::vector<StrW>* m_history = nullptr;
    size_t          m_history_index = 0;
    StrW            m_curr_input_history;

    // Callback.
    std::optional<std::function<int32(const InputRecord&, void*)>> m_callback;
};

ReadInputState::ReadInputState()
{
    InitUndo();
}

ReadInputState::~ReadInputState()
{
    ClearUndoInternal();
}

void ReadInputState::SetCallback(std::optional<std::function<int32(const InputRecord&, void*)>> input_callback)
{
    m_callback = input_callback;
}

void ReadInputState::SetHistory(std::vector<StrW>* history)
{
    m_history = history;
    m_history_index = m_history ? m_history->size() : 0;
}

void ReadInputState::InitializeText(const WCHAR* s, int32 len)
{
    if (!s)
    {
        s = L"";
        len = 0;
    }
    else if (len < 0)
    {
        len = int32(min<size_t>(INT16_MAX, wcslen(s)));
    }

    ClearUndoInternal();
    m_sel.SetCaret(0);
    InsertText(s, len);
    m_sel.ClearDirty();
    m_left = m_s.Length();
    InitUndo();

    assert(!m_sel.IsDirty());
    assert(!m_sel.GetCaret());
    assert(!m_sel.GetAnchor());
    assert(m_s.Empty());
    assert(!m_defer_init_undo);

    m_history_index = m_history ? m_history->size() : 0;
}

int32 ReadInputState::Go(void* cookie)
{
    const HANDLE hin = GetStdHandle(STD_INPUT_HANDLE);
    const HANDLE hout = GetStdHandle(STD_OUTPUT_HANDLE);

    CONSOLE_SCREEN_BUFFER_INFO csbi;
    GetConsoleScreenBufferInfo(hout, &csbi);

    if (unsigned(csbi.dwCursorPosition.X) + 8 >= unsigned(csbi.dwSize.X))
        return false;
    if (unsigned(csbi.dwCursorPosition.X) + m_max_width >= unsigned(csbi.dwSize.X))
        m_max_width = csbi.dwSize.X - csbi.dwCursorPosition.X;

    if (m_origin.X < 0 || m_origin.Y < 0)
        SetOrigin(csbi.dwCursorPosition);

    AutoMouseConsoleMode mouse(g_options.allow_mouse);
    m_mouse_helper.ClearClicks();
    m_can_drag = false;

#ifdef DEBUG
    StrW prev_text(m_s);
    uint32 prev_counter = m_change_counter;
#endif

    while (true)
    {
        EnsureLeft();
        PrintVisible();

#ifdef DEBUG
        // Verify any time m_s changes then m_change_counter also increases.
        if (!prev_text.Equal(m_s))
        {
            assert(int32(m_change_counter) - int32(prev_counter) > 0);
            prev_text.Set(m_s);
            prev_counter = m_change_counter;
        }
#endif

        const InputRecord input = SelectInput(INFINITE, &mouse);
        switch (input.type)
        {
        case InputType::None:
        case InputType::Error:
            continue;
        case InputType::Resize:
            return false;

        case InputType::Key:
        case InputType::Char:
        case InputType::Mouse:
            if (m_callback)
            {
                const int32 result = (*m_callback)(input, cookie);
                // Negative means break out of the loop.
                if (result < 0)
                    return -1;
                // Positive means do not process (already handled).
                if (result > 0)
                    continue;
                // Zero means allow normal processing.
            }
            switch (HandleInput(input))
            {
            case Outcome::Cancelled:
                return false;
            case Outcome::Done:
                return true;
            case Outcome::DontResetHistoryIndex:
                break;
            case Outcome::ResetHistoryIndex:
                if (m_history && m_history_index < m_history->size())
                    m_history_index = m_history->size();
                break;
            default:
                assert(false);
                break;
            }
            break;

        default:
            assert(false);
            break;
        }
    }
}

ReadInputState::Outcome ReadInputState::HandleInput(const InputRecord& input)
{
    AutoCleanup cleanup;
    const uint32 prev_counter = m_change_counter;

    if (input.type == InputType::Key)
    {
        cleanup.Set([&]() {
            m_can_drag = false;
        });

        switch (input.key)
        {
        case Key::ESC:
            return Outcome::Cancelled;
        case Key::BACK:
            if ((input.modifier & ~Modifier::CTRL) == Modifier::None)
                Backspace((input.modifier & Modifier::CTRL) == Modifier::CTRL);
            break;
        case Key::INS:
            if (input.modifier == Modifier::CTRL)
                CopyToClipboard();
            else if (input.modifier == Modifier::SHIFT)
                PasteFromClipboard();
            break;
        case Key::DEL:
            if ((input.modifier & ~Modifier::CTRL) == Modifier::None)
                Delete((input.modifier & Modifier::CTRL) == Modifier::CTRL);
            else if (input.modifier == Modifier::SHIFT)
                CutToClipboard();
            break;
        case Key::ENTER:
            if (m_history && m_s.Length())
                m_history->emplace_back(m_s);
            return Outcome::Done;
        case Key::HOME:
            Home(input.modifier);
            break;
        case Key::END:
            End(input.modifier);
            break;
        case Key::UP:
            if (m_history && m_history_index)
            {
                if (m_history_index == m_history->size())
                    TransferText(m_curr_input_history);
                --m_history_index;
                ReplaceFromHistory((*m_history)[m_history_index], false/*keep_undo*/);
            }
            break;
        case Key::DOWN:
            if (m_history && m_history_index < m_history->size())
            {
                ++m_history_index;
                if (m_history_index == m_history->size())
                    ReplaceFromHistory(m_curr_input_history, true/*keep_undo*/);
                else
                    ReplaceFromHistory((*m_history)[m_history_index], false/*keep_undo*/);
            }
            break;
        case Key::LEFT:
            Left(input.modifier);
            break;
        case Key::RIGHT:
            Right(input.modifier);
            break;
        default:
            break;
        }
    }
    else if (input.type == InputType::Char)
    {
        cleanup.Set([&]() {
            m_can_drag = false;
        });

        if (input.key_char >= ' ')
        {
            InsertChar(input.key_char, input.key_char2);
        }
        else
        {
            switch (input.key_char)
            {
            case 'A'-'@':
                Home(Modifier::None);
                End(Modifier::SHIFT);
                break;
            case 'C'-'@':
                CopyToClipboard();
                break;
            case 'V'-'@':
                PasteFromClipboard();
                break;
            case 'X'-'@':
                CutToClipboard();
                break;
            case 'Y'-'@':
                Redo();
                break;
            case 'Z'-'@':
                Undo();
                break;
            }
        }
    }
    else if (input.type == InputType::Mouse)
    {
        switch (input.key)
        {
        case Key::MouseWheel:
            break;
        case Key::MouseLeftClick:
        case Key::MouseLeftDblClick:
            m_can_drag = true;
            __fallthrough;
        case Key::MouseDrag:
            if (m_can_drag)
            {
                const bool drag = (input.key == Key::MouseDrag);
                if (drag || (input.mouse_pos.Y == m_origin.Y &&
                             input.mouse_pos.X >= m_origin.X && input.mouse_pos.X < m_origin.X + m_max_width))
                {
                    const uint8 clicks = (drag ?
                        m_mouse_helper.GetClicks() :
                        m_mouse_helper.OnClick(input.mouse_pos, (input.key == Key::MouseLeftDblClick)));
                    if (clicks == 3)
                    {
                        m_sel.SetSelection(0, m_s.Length());
                    }
                    else if (clicks)
                    {
                        // Translate input.mouse_pos to textpos_t.
                        textpos_t pos = 0;
                        int x = input.mouse_pos.X - m_origin.X;
                        wcwidth_iter iter(m_s.Text(), m_s.Length());
                        while (iter.next())
                        {
                            if (pos < m_left)
                            {
                                pos += iter.character_length();
                                continue;
                            }
                            if (x <= 0)
                                break;
                            pos += iter.character_length();
                            x -= iter.character_wcwidth_onectrl();
                        }

                        if (drag)
                        {
                            textpos_t anchor;
                            if (m_mouse_helper.GetAnchor(pos, anchor, pos) && clicks == 2)
                            {
                                SelectionState old = m_sel;
                                if (pos < anchor)
                                {
                                    Right(Modifier::CTRL);
                                    Left(Modifier::CTRL);
                                    if (m_sel.GetCaret() > pos)
                                    {
                                        m_sel.SetCaret(pos);
                                        Left(Modifier::CTRL);
                                    }
                                }
                                else
                                {
                                    Left(Modifier::CTRL);
                                    Right(Modifier::CTRL);
                                    if (m_sel.GetCaret() <= pos)
                                    {
                                        m_sel.SetCaret(pos);
                                        Right(Modifier::CTRL);
                                    }
                                }
                                pos = m_sel.GetCaret();
                                m_sel = old;
                            }
                            m_sel.SetSelection(anchor, pos);
                        }
                        else
                        {
                            const bool moved = (pos != m_sel.GetCaret());
                            m_sel.SetCaret(pos);
                            m_mouse_helper.SetAnchors(pos, pos);
                            if (clicks == 2)
                            {
                                SelectWord();
                                m_mouse_helper.SetAnchors(m_sel.GetAnchor(), m_sel.GetCaret());
                            }
                        }
                    }
                }
                else
                {
                    m_mouse_helper.ClearClicks();
                }
            }
            break;
        default:
            m_can_drag = false;
            break;
        }
    }
    else
    {
        assert(false);
    }

    const bool changed = (prev_counter != m_change_counter);
    return changed ? Outcome::ResetHistoryIndex : Outcome::DontResetHistoryIndex;
}

void ReadInputState::EnsureLeft()
{
    m_left = min(m_left, m_sel.GetCaret());

    // Auto-scroll horizontally forward.
    while (__wcswidth(m_s.Text() + m_left, m_sel.GetCaret() - m_left) >= m_max_width)
    {
        wcwidth_iter iter(m_s.Text() + m_left, m_s.Length() - m_left);
        if (!iter.next())
            break;
        m_left += iter.character_length();
    }

    // Auto-scroll horizontally backward.
    assert(m_sel.GetCaret() >= m_left);
    {
        textpos_t backup_left = m_sel.GetCaret();
        BackUpByAmount(backup_left, m_s.Text(), m_sel.GetCaret(), 4);
        if (m_left > backup_left)
            m_left = backup_left;
    }
}

void ReadInputState::PrintVisible()
{
    StrW tmp;
    tmp.Printf(L"%s\x1b[%u;%uH", c_hide_cursor, m_origin.Y + 1, m_origin.X + 1);
    OutputConsole(tmp.Text(), tmp.Length());

    uint16 max_width = m_max_width;
    bool left_marker = m_horiz_scroll_markers && (m_left > 0);
    bool right_marker = false;
    unsigned lo_limit = m_left;
    unsigned hi_limit = 0;

    if (left_marker)
    {
        wcwidth_iter wi(m_s.Text() + m_left);
        if (wi.next())
        {
            lo_limit += wi.character_length();
            max_width -= 1; // Width of left marker, not the iter character.
        }
    }

    unsigned width = 0;
    const unsigned len = FitsInWcwidth(m_s.Text() + lo_limit, m_s.Length() - lo_limit, max_width - m_horiz_scroll_markers, &width);
    hi_limit = lo_limit + len;

    if (m_horiz_scroll_markers && width > 0)
    {
        wcwidth_iter wi(m_s.Text() + lo_limit + len);
        if (wi.next())
        {
            if (hi_limit + wi.character_length() == m_s.Length() &&
                width + wi.character_wcwidth_onectrl() <= max_width)
            {
                hi_limit = m_s.Length();
                width += wi.character_wcwidth_onectrl();
            }
            else
            {
                right_marker = true;
                --max_width;
            }
        }
    }

    tmp.Clear();
    if (left_marker)
    {
        tmp.AppendColor(GetColor(ColorElement::InputHorizScroll));
        tmp.Append(L"<", 1);
    }
    tmp.AppendColor(GetColor(ColorElement::Input));

    if (m_sel.GetAnchor() <= m_s.Length())
    {
        const textpos_t begin = clamp<textpos_t>(m_sel.GetSelBegin(), lo_limit, hi_limit);
        const textpos_t end = clamp<textpos_t>(m_sel.GetSelEnd(), lo_limit, hi_limit);
        tmp.Append(m_s.Text() + lo_limit, begin - lo_limit);
        if (begin < end)
        {
            tmp.AppendColor(GetColor(ColorElement::InputSelection));
            tmp.Append(m_s.Text() + begin, end - begin);
            // REVIEW:  Should this append a space here if the selection isn't fully drawn due to character width clipping?
            tmp.AppendColor(GetColor(ColorElement::Input));
        }
        if (hi_limit > end)
            tmp.Append(m_s.Text() + end, hi_limit - end);
    }
    else
    {
        tmp.Append(m_s.Text() + lo_limit, len);
    }

    tmp.AppendSpaces(max_width - width);
    if (right_marker)
    {
        tmp.AppendColor(GetColor(ColorElement::InputHorizScroll));
        tmp.Append(L">", 1);
    }
    tmp.Printf(L"\x1b[%u;%uH%s", m_origin.Y + 1, m_origin.X + 1 + left_marker + __wcswidth(m_s.Text() + lo_limit, m_sel.GetCaret() - lo_limit), c_show_cursor);
    OutputConsole(tmp.Text(), tmp.Length());
}

void ReadInputState::Home(Modifier modifier)
{
    const bool shift = ((modifier & Modifier::SHIFT) == Modifier::SHIFT);
    if (!shift)
        m_sel.SetCaret(0);
    else if (!m_sel.HasSelection())
        m_sel.SetSelection(m_sel.GetCaret(), 0);
    else
        m_sel.SetSelection(m_sel.GetAnchor(), 0);
    m_left = 0;
    if (!shift)
        m_sel.ResetWordAnchor();
}

void ReadInputState::End(Modifier modifier)
{
    const bool shift = ((modifier & Modifier::SHIFT) == Modifier::SHIFT);
    if (!shift)
        m_sel.SetCaret(m_s.Length());
    else if (!m_sel.HasSelection())
        m_sel.SetSelection(m_sel.GetCaret(), m_s.Length());
    else
        m_sel.SetSelection(m_sel.GetAnchor(), m_s.Length());
    m_left = m_sel.GetCaret();

    BackUpByAmount(m_left, m_s.Text(), m_sel.GetCaret(), m_max_width - 1);

    if (!shift)
        m_sel.ResetWordAnchor();
}

void ReadInputState::Left(Modifier modifier)
{
    const bool shift = ((modifier & Modifier::SHIFT) == Modifier::SHIFT);
    if (!shift && m_sel.HasSelection())
    {
        m_sel.SetCaret(m_sel.GetSelBegin());
    }
    else if (m_sel.GetCaret() > 0)
    {
        textpos_t caret = m_sel.GetCaret();
        textpos_t anchor = m_sel.GetAnchor();
        const bool word = ((modifier & Modifier::CTRL) == Modifier::CTRL);
        PosMover(m_s.Text(), m_s.Length(), caret, false/*forward*/, word);
        m_sel.SetSelection(shift ? anchor : caret, caret);
    }
    if (!shift)
        m_sel.ResetWordAnchor();
}

void ReadInputState::Right(Modifier modifier)
{
    const bool shift = ((modifier & Modifier::SHIFT) == Modifier::SHIFT);
    if (!shift && m_sel.HasSelection())
    {
        m_sel.SetCaret(m_sel.GetSelEnd());
    }
    else if (m_sel.GetCaret() < m_s.Length())
    {
        textpos_t caret = m_sel.GetCaret();
        textpos_t anchor = m_sel.GetAnchor();
        const bool word = ((modifier & Modifier::CTRL) == Modifier::CTRL);
        PosMover(m_s.Text(), m_s.Length(), caret, true/*forward*/, word);
        m_sel.SetSelection(shift ? anchor : caret, caret);
    }
    if (!shift)
        m_sel.ResetWordAnchor();
}

void ReadInputState::Backspace(bool word)
{
    m_sel.ResetWordAnchor();
    if (m_sel.GetCaret() <= 0)
        return;

    BeginUndoGroup();

    if (!ElideSelectedText())
    {
#ifdef DEBUG
        const textpos_t old_pos = m_sel.GetCaret();
#endif
        const textpos_t moved = PosMover(m_s.Text(), m_s.Length(), m_sel.GetCaretOut(), false/*forward*/, word);
#ifdef DEBUG
        assert(old_pos == m_sel.GetCaret() + moved);
#endif
        RemoveText(m_sel.GetCaret(), m_sel.GetCaret() + moved);
    }

    EndUndoGroup();
}

void ReadInputState::Delete(bool word)
{
    m_sel.ResetWordAnchor();
    if (m_sel.GetCaret() >= m_s.Length())
        return;

    BeginUndoGroup();

    if (!ElideSelectedText())
    {
        textpos_t del_pos = m_sel.GetCaret();
        const textpos_t moved = PosMover(m_s.Text(), m_s.Length(), del_pos, true/*forward*/, word);
        m_sel.SetCaret(del_pos - moved);
        RemoveText(m_sel.GetCaret(), m_sel.GetCaret() + moved);
    }

    EndUndoGroup();
}

void ReadInputState::SetSelection(textpos_t anchor, textpos_t caret)
{
    m_sel.SetSelection(anchor, caret);
}

void ReadInputState::SelectWord()
{
    const textpos_t orig_pos = m_sel.GetCaret();

    // Look forward for a word.
    Right(Modifier::CTRL);
    textpos_t end = m_sel.GetCaret();
    Left(Modifier::CTRL);
    const textpos_t high_mid = m_sel.GetCaret();

    m_sel.SetCaret(orig_pos);

    // Look backward for a word.
    Left(Modifier::CTRL);
    textpos_t begin = m_sel.GetCaret();
    Right(Modifier::CTRL);
    const textpos_t low_mid = m_sel.GetCaret();

    if (high_mid <= orig_pos)
    {
        begin = high_mid;
    }
    else if (low_mid > orig_pos)
    {
        end = low_mid;
    }
    else
    {
        // The position is between two words; select the text between.
        begin = low_mid;
        end = high_mid;
    }

    m_sel.SetSelection(begin, end);
}

void ReadInputState::CopyToClipboard()
{
    if (!m_sel.HasSelection())
        return;

    const textpos_t begin = m_sel.GetSelBegin();
    const textpos_t end = m_sel.GetSelEnd();
    const textpos_t len = (end - begin);

    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE|GMEM_ZEROINIT, (len + 1) * sizeof(WCHAR));
    if (mem == nullptr)
        return;

    WCHAR* data = (WCHAR*)GlobalLock(mem);
    memcpy(data, m_s.Text() + begin, len * sizeof(WCHAR));
    data[len] = 0;
    GlobalUnlock(mem);

    if (!OpenClipboard(0))
    {
        GlobalFree(mem);
        return;
    }

    EmptyClipboard();
    SetClipboardData(CF_UNICODETEXT, mem);
    CloseClipboard();
}

void ReadInputState::CutToClipboard()
{
    BeginUndoGroup();
    CopyToClipboard();
    ElideSelectedText();
    EndUndoGroup();
}

void ReadInputState::PasteFromClipboard()
{
    if (!OpenClipboard(0))
        return;

    HANDLE mem = GetClipboardData(CF_UNICODETEXT);
    if (mem)
    {
        size_t len = size_t(GlobalSize(mem) / sizeof(WCHAR));
        LPCWSTR data = LPCWSTR(GlobalLock(mem));

        while (len && !data[len - 1])
            --len;
        InsertText(data, len);

        GlobalUnlock(mem);
    }

    CloseClipboard();
}

void ReadInputState::ReplaceFromHistory(const StrW& s, bool keep_undo)
{
    ++m_change_counter;

    m_s.Set(s);
    m_sel.SetCaret(m_s.Length());
    m_defer_init_undo = !keep_undo;

    m_left = GetCaret();
    BackUpByAmount(m_left, m_s.Text(), m_left, m_max_width - 1);
}

void ReadInputState::InsertChar(WCHAR c, WCHAR c2)
{
    if (!c)
        return;

    const size_t len = (c2 ? 2 : 1);
    WCHAR chars[2] = { c, c2 };
    InsertText(chars, len);
}

void ReadInputState::InsertText(const WCHAR* s, size_t available)
{
    if (!available)
        return;

    BeginUndoGroup();

    m_sel.ResetWordAnchor();

    ElideSelectedText();

    if (available > size_t(m_max_length))
        available = m_max_length;

    textpos_t len = 0;
    wcwidth_iter iter(s, static_cast<textpos_t>(available));
    while (iter.next())
    {
        if (m_s.Length() + iter.character_length() > m_max_length)
            break;
        len += iter.character_length();
    }

    ++m_change_counter;

    if (m_sel.GetCaret() == m_s.Length())
    {
        m_s.Append(s, len);
        m_sel.SetCaret(m_s.Length());
    }
    else
    {
        StrW tmp;
        const int32 insert_pos = m_sel.GetCaret();
        tmp.Set(m_s.Text(), insert_pos);
        tmp.Append(s, len);
        m_sel.SetCaret(tmp.Length());
        tmp.Append(m_s.Text() + insert_pos, m_s.Length() - insert_pos);
        m_s = std::move(tmp);
    }

    EndUndoGroup();
}

void ReadInputState::RemoveText(textpos_t begin, textpos_t end)
{
    BeginUndoGroup();

    m_sel.ResetWordAnchor();

    ++m_change_counter;

    if (end == m_s.Length())
    {
        m_s.SetLength(begin);
    }
    else
    {
        StrW tmp;
        tmp.Append(m_s.Text(), begin);
        tmp.Append(m_s.Text() + end, m_s.Length() - end);
        m_s = std::move(tmp);
    }

    m_sel.SetCaret(begin);

    EndUndoGroup();
}

bool ReadInputState::ElideSelectedText()
{
    if (!m_sel.HasSelection())
        return false;

    const textpos_t begin = m_sel.GetSelBegin();
    const textpos_t end = m_sel.GetSelEnd();
    RemoveText(begin, end);
    return true;
}

void ReadInputState::ClearUndoInternal()
{
    while (m_undo_head)
        UnlinkUndoEntry(m_undo_head);
    assert(!m_undo_head);
    assert(!m_undo_tail);
    m_undo_current = nullptr;
}

void ReadInputState::InitUndo()
{
    ClearUndoInternal();
    m_undo_head = m_undo_tail = new UndoEntry;
    m_undo_tail->m_s.Set(m_s);
    m_undo_tail->m_sel_before = m_sel;
    m_undo_tail->m_sel_after = m_sel;
    m_defer_init_undo = false;
}

void ReadInputState::UnlinkUndoEntry(UndoEntry* p)
{
    p->Unlink(m_undo_head, m_undo_tail);
}

void ReadInputState::BeginUndoGroup()
{
    if (!m_undo_head)
        return;

    assert(m_grouping >= 0);
    if (!m_grouping)
    {
        if (m_defer_init_undo)
            InitUndo();

        if (m_undo_current)
        {
            // Keep current, discard everything after current.
            m_undo_current = m_undo_current->m_next;
            while (m_undo_current)
            {
                UndoEntry* del = m_undo_current;
                m_undo_current = m_undo_current->m_next;
                UnlinkUndoEntry(del);
                delete del;
            }
            assert(!m_undo_current);
        }

        UndoEntry* p = new UndoEntry;
        p->m_sel_before = m_sel;
        p->LinkAtTail(m_undo_head, m_undo_tail);
        assert(p == m_undo_tail);
    }
    ++m_grouping;
}

void ReadInputState::EndUndoGroup()
{
    if (!m_undo_head)
        return;

    assert(m_grouping > 0);
    --m_grouping;
    if (!m_grouping)
    {
        m_undo_tail->m_s.Set(m_s);
        m_undo_tail->m_sel_after = m_sel;
    }
}

void ReadInputState::Undo()
{
    assert(!m_grouping);
    if (m_grouping)
        return;
    if (!m_undo_head)
        return;

    if (!m_undo_current)
        m_undo_current = m_undo_tail;
    UndoEntry* p = m_undo_current->m_prev;
    if (!p)
        return;

    ++m_change_counter;
    m_s.Set(p->m_s);
    m_sel = m_undo_current->m_sel_before;
    m_undo_current = p;
}

void ReadInputState::Redo()
{
    assert(!m_grouping);
    if (m_grouping)
        return;
    if (!m_undo_tail)
        return;

    if (!m_undo_current || m_undo_current == m_undo_tail)
        return;

    UndoEntry* r = m_undo_current->m_next;
    assert(r);

    ++m_change_counter;
    m_s.Set(r->m_s);
    m_sel = r->m_sel_after;

    m_undo_current = r;
}

void ReadInputState::TransferText(StrW& out)
{
    out = std::move(m_s);
    InitializeText(nullptr, 0);
}

#ifdef DEBUG
void ReadInputState::DumpUndoStack()
{
    puts("");
    for (UndoEntry* p = m_undo_head; p; p = p->m_next)
    {
        StrA tag;
        if (p == m_undo_head) tag.Append("H");
        if (p == m_undo_tail) tag.Append("T");
        if (p == m_undo_current) tag.Append("C");
        printf("%s\tcaret %u/%u, anchor %u/%u, text '%ls'\n", tag.Text(), p->m_sel_before.GetCaret(), p->m_sel_after.GetCaret(), p->m_sel_before.GetAnchor(), p->m_sel_after.GetAnchor(), p->m_s.Text());
    }
    printf("----\n");
}
#endif

bool ReadInput(StrW& out, History hindex, DWORD max_length, DWORD max_width, std::optional<std::function<int32(const InputRecord&, void*)>> input_callback)
{
    static std::vector<StrW> s_histories[size_t(History::MAX)];

    max_length = max<DWORD>(max_length, 1);
    max_length = min<DWORD>(max_length, 1024);
    out.Clear();

    ReadInputState state;
    state.SetMaxWidth(max_width);
    state.SetMaxLength(max_length);
    state.SetCallback(input_callback);
    state.SetHistory((size_t(hindex) < _countof(s_histories)) ? &s_histories[size_t(hindex)] : nullptr);

    const int32 result = state.Go();
    if (result > 0)
    {
        state.TransferText(out);
        return true;
    }

    return false;
}

static bool wcstonum(const WCHAR* text, unsigned radix, unsigned __int64& out)
{
    assert(radix == 10 || radix == 16);

    if (!*text)
        return false;

    unsigned __int64 num = 0;
    while (*text)
    {
        if (*text >= '0' && *text <= '9')
            num = (num * radix) + (*text - '0');
        else if (radix != 16)
            return false;
        else if (*text >= 'A' && *text <= 'F')
            num = (num * radix) + (10 + *text - 'A');
        else if (*text >= 'a' && *text <= 'f')
            num = (num * radix) + (10 + *text - 'a');
        ++text;
    }

    out = num;
    return true;
}

bool ParseULongLong(const WCHAR* s, ULONGLONG& out, int radix)
{
    // Parse radix selector.
    if (s[0] == '$')
    {
        radix = 16;
        ++s;
    }
    else if (s[0] == '#')
    {
        radix = 10;
        ++s;
    }
    else if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
    {
        radix = 16;
        s += 2;
    }

    return wcstonum(s, radix, out);
}

void ClickableRow::Init(uint16 row, uint16 terminal_width, uint16 reserve_left)
{
    m_row = row;
    m_terminal_width = terminal_width;
    m_reserve_left = reserve_left;
    m_threshold = INT16_MAX;
    m_left_width = 0;
    m_right_width = 0;
    m_left_elements.clear();
    m_right_elements.clear();
    m_need_layout = true;
}

void ClickableRow::Add(const WCHAR* text, int16 id, int16 priority, bool right_align, ellipsify_mode fit_mode, uint16 min_fit_width, bool enabled)
{
    Element elm;
    std::vector<Element>& vec = right_align ? m_right_elements : m_left_elements;

    if (text)
    {
        elm.m_text.Set(text);
        elm.m_width = cell_count(text);
        elm.m_id = id;
    }
    else
    {
        elm.m_width = id;
        elm.m_id = -1;
    }
    elm.m_priority = priority;
    elm.m_left = 0;
    elm.m_fit_mode = fit_mode;
    elm.m_min_fit_width = min_fit_width;
    elm.m_enabled = enabled;

    vec.emplace_back(std::move(elm));

    m_need_layout = true;
}

void ClickableRow::AddKeyName(const WCHAR* key, ColorElement color_after, const WCHAR* desc, int16 id, int16 priority, bool right_align, bool enabled)
{
    StrW tmp;
    AppendKeyName(tmp, key, color_after, desc, enabled);
    Add(tmp.Text(), id, priority, right_align, INVALID, 20, enabled);
}

uint16 ClickableRow::GetLeftWidth()
{
    EnsureLayout();
    return m_left_width;
}

uint16 ClickableRow::GetRightWidth()
{
    EnsureLayout();
    return m_right_width;
}

void ClickableRow::BuildOutput(StrW& out, const WCHAR* color)
{
    EnsureLayout();

    if (color)
        out.AppendColor(color);

    uint16 width = 0;
    uint16 orig_length = out.Length();
    const uint16 right_width = GetRightWidth();

    for (const auto& elm : m_left_elements)
        width += AppendOutput(out, elm, color);

    if (width > m_terminal_width)
    {
        StrW tmp;
        width = ellipsify_ex(out.Text() + orig_length, m_terminal_width, ellipsify_mode::RIGHT, tmp);
        if (width < m_terminal_width)
            tmp.Append(c_clreol);
        out.SetLength(orig_length);
        out.Append(tmp);
    }
    else
    {
        if (right_width)
            out.AppendSpaces(m_terminal_width - width - right_width);
        else if (width < m_terminal_width)
            out.Append(c_clreol);
    }

    if (right_width)
    {
        for (const auto& elm : m_right_elements)
            AppendOutput(out, elm, color);
    }
}

void ClickableRow::EnsureLayout()
{
    if (!m_need_layout)
        return;

    // Calculate total needed width.
    uint16 total_width = 0;
    uint16 min_fit_width = 0;
    uint16 num_fit_elements = 0;
    for (auto& elm : m_left_elements)
    {
        total_width += elm.m_width;
        elm.m_fitted.Clear();
        elm.m_effective_width = 0;
        if (elm.m_fit_mode != ellipsify_mode::INVALID && elm.m_width > elm.m_min_fit_width)
        {
            total_width -= elm.m_width;
            total_width += elm.m_min_fit_width;
            ++num_fit_elements;
        }
    }
    total_width = max(total_width, m_reserve_left);
    for (auto& elm : m_right_elements)
    {
        total_width += elm.m_width;
        elm.m_fitted.Clear();
        elm.m_effective_width = 0;
        if (elm.m_fit_mode != ellipsify_mode::INVALID && elm.m_width > elm.m_min_fit_width)
        {
            total_width -= elm.m_width;
            total_width += elm.m_min_fit_width;
            ++num_fit_elements;
        }
    }

    // Drop elements in priority order until something fits.
    m_threshold = INT16_MIN;
    uint16 priority_width = 0;
    if (total_width > m_terminal_width)
    {
        // Collect priority groups.
        std::set<int16> priorities;
        for (const auto& elm : m_left_elements)
            priorities.emplace(elm.m_priority);
        for (const auto& elm : m_right_elements)
            priorities.emplace(elm.m_priority);

        // Iterate over the priority groups.
        for (auto iter = priorities.begin(); iter != priorities.end(); ++iter)
        {
            // Keep the highest priority group (it will be truncated).
            auto next = iter;
            ++next;
            if (next == priorities.end())
                break;

            // Calculate width of the priority group.
            priority_width = 0;
            for (const auto& elm : m_left_elements)
            {
                if (elm.m_priority == (*iter))
                {
                    if (elm.m_fit_mode != ellipsify_mode::INVALID && elm.m_width > elm.m_min_fit_width)
                        priority_width += elm.m_min_fit_width;
                    else
                        priority_width += elm.m_width;
                }
            }
            for (const auto& elm : m_right_elements)
            {
                if (elm.m_priority == (*iter))
                {
                    if (elm.m_fit_mode != ellipsify_mode::INVALID && elm.m_width > elm.m_min_fit_width)
                        priority_width += elm.m_min_fit_width;
                    else
                        priority_width += elm.m_width;
                }
            }

            // Drop the priority group.
            total_width -= priority_width;
            m_threshold = (*iter) + 1;
            if (total_width <= m_terminal_width)
                break;
        }
    }

    // Calculate effective widths.
    const uint16 each_extra = (!num_fit_elements || m_terminal_width < total_width) ? 0 : (m_terminal_width - total_width) / num_fit_elements;
    m_left_width = 0;
    for (auto& elm : m_left_elements)
    {
        if (elm.m_priority >= m_threshold)
        {
            if (elm.m_fit_mode != ellipsify_mode::INVALID && elm.m_width > elm.m_min_fit_width)
                elm.m_effective_width = ellipsify_ex(elm.m_text.Text(), elm.m_min_fit_width + each_extra, elm.m_fit_mode, elm.m_fitted);
            else
                elm.m_effective_width = elm.m_width;
        }
        m_left_width += elm.m_effective_width;
    }
    m_right_width = 0;
    for (auto& elm : m_right_elements)
    {
        if (elm.m_priority >= m_threshold)
        {
            if (elm.m_fit_mode != ellipsify_mode::INVALID && elm.m_width > elm.m_min_fit_width)
               elm.m_effective_width = ellipsify_ex(elm.m_text.Text(), elm.m_min_fit_width + each_extra, elm.m_fit_mode, elm.m_fitted);
            else
                elm.m_effective_width = elm.m_width;
        }
        m_right_width += elm.m_effective_width;
    }

    // Special case when there's only one priority group and it's still too
    // large to fit.
    if (max(m_left_width, m_reserve_left) + m_right_width > m_terminal_width)
    {
        // TODO:  This should redo ellipsify_ex for left elements.
        m_right_width = 0;
    }

    // Layout.
    uint16 x = 0;
    for (auto& elm : m_left_elements)
    {
        elm.m_left = x;
        if (elm.m_priority >= m_threshold)
            x += elm.m_effective_width;
    }
    if (m_right_width)
    {
        x = m_terminal_width - m_right_width;
        for (auto& elm : m_right_elements)
        {
            elm.m_left = x;
            if (elm.m_priority >= m_threshold)
                x += elm.m_effective_width;
        }
    }

    m_need_layout = false;
}

uint16 ClickableRow::AppendOutput(StrW& out, const Element& elm, const WCHAR* color)
{
    if (elm.m_priority < m_threshold)
        return 0;

    if (elm.m_text.Empty())
    {
        out.AppendSpaces(elm.m_width);
    }
    else
    {
        if (elm.m_fitted.Empty())
            out.Append(elm.m_text);
        else
            out.Append(elm.m_fitted);
        if (color && wcschr(elm.m_text.Text(), '\x1b'))
            out.AppendColor(color);
    }
    return elm.m_effective_width;
}

int16 ClickableRow::InterpretInput(const InputRecord& input) const
{
    if (input.type != InputType::Mouse)
        return -1;
// FUTURE:  Showing visual click feedback requires a new Key::MouseLeftRelease.
    if (input.key != Key::MouseLeftClick && input.key != Key::MouseLeftDblClick)
        return -1;
    if (input.mouse_pos.Y != m_row)
        return -1;

    const uint16 max_pass = (m_right_width ? 2 : 1);
    for (uint16 pass = 0; pass < max_pass; ++pass)
    {
        const std::vector<Element>& vec = !pass ? m_left_elements : m_right_elements;
        for (const auto& elm : vec)
        {
            if (elm.m_id >= 0 &&
                input.mouse_pos.X >= elm.m_left &&
                input.mouse_pos.X < elm.m_left + elm.m_width)
            {
                return elm.m_enabled ? elm.m_id : -1;
            }
        }
    }

    return -1;
}

int32 MouseHelper::LinesFromRecord(const InputRecord& input)
{
    assert(input.type == InputType::Mouse);
    assert(input.key == Key::MouseWheel || input.key == Key::MouseHWheel);
    if (!m_allow_acceleration)
        return input.mouse_wheel_amount;
    else if (input.key == Key::MouseHWheel)
        return m_horz_accel.MaybeAccelerate(input.mouse_wheel_amount);
    else
        return m_vert_accel.MaybeAccelerate(input.mouse_wheel_amount);
}

int32 MouseHelper::AccelerationHelper::MaybeAccelerate(int32 lines)
{
    if (sgn(m_acceleration) != sgn(lines) || GetTickCount() - m_last_tick > 50)
    {
        // Reset if direction changes or time expires.
        m_acceleration = 0;
    }

    m_acceleration = clamp(m_acceleration + sgn(lines), -4, 4);
    m_last_tick = GetTickCount();

    if (abs(m_acceleration) >= 4)
        return lines * (1 + (abs(m_acceleration) / 4)) * 2;

    return lines;
}

HANDLE AutoMouseConsoleMode::s_hin = GetStdHandle(STD_INPUT_HANDLE);
DWORD AutoMouseConsoleMode::s_prev_mode = ([]() {
    DWORD dw = 0;
    GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &dw);
    return dw;
})();

AutoMouseConsoleMode::AutoMouseConsoleMode(bool enable)
{
    m_can_restore = (s_hin && GetConsoleMode(s_hin, &m_restore_mode));
    if (enable)
        DisableMouseInputIfShift();
    else
        DisableMouseInput();
}

AutoMouseConsoleMode::~AutoMouseConsoleMode()
{
    if (m_can_restore)
        UpdateMode(m_restore_mode);
}

void AutoMouseConsoleMode::UpdateMode(DWORD new_mode, bool force)
{
    if ((force || new_mode != s_prev_mode) && SetConsoleMode(s_hin, new_mode))
    {
        s_prev_mode = new_mode;
        s_prev_button_state = GetButtonState();
    }
}

void AutoMouseConsoleMode::DisableMouseInput()
{
    if (m_can_restore)
    {
        const DWORD new_mode = (m_restore_mode & ~ENABLE_MOUSE_INPUT)|ENABLE_QUICK_EDIT_MODE;
        UpdateMode(new_mode);
    }
}

void AutoMouseConsoleMode::DisableMouseInputIfShift()
{
    if (m_can_restore)
    {
        DWORD new_mode = (m_restore_mode & ~(ENABLE_MOUSE_INPUT|ENABLE_QUICK_EDIT_MODE));
        if (GetKeyState(VK_SHIFT) & 0x8000)
            new_mode |= ENABLE_QUICK_EDIT_MODE;
        else
            new_mode |= ENABLE_MOUSE_INPUT;
        UpdateMode(new_mode);
    }
}

void AutoMouseConsoleMode::SetStdInputHandle(HANDLE hin)
{
    s_hin = hin;
}

void MouseHelper::ClearClicks()
{
    m_tick = GetTickCount() - 0xffff;
    m_clicks = 0;
}

uint8 MouseHelper::OnClick(COORD coord, bool dblclk)
{
    const DWORD now = GetTickCount();

    if (dblclk)
        m_clicks = 2;
    else if (m_clicks == 2 && coord.X == m_coord.X && coord.Y == m_coord.Y && now - m_tick <= GetDoubleClickTime())
        m_clicks = 3;
    else
        m_clicks = 1;

    m_coord = coord;
    m_tick = now;

    return m_clicks;
}

void MouseHelper::SetAnchors(textpos_t a1, textpos_t a2)
{
    m_anchor1 = min(a1, a2);
    m_anchor2 = max(a1, a2);
}

bool MouseHelper::GetAnchor(textpos_t pos, textpos_t& anchor, textpos_t& caret) const
{
    if (pos < m_anchor1)
    {
        anchor = m_anchor2;
        caret = pos;
        return true;
    }
    if (pos >= m_anchor2)
    {
        anchor = m_anchor1;
        caret = pos;
        return true;
    }
    anchor = m_anchor1;
    caret = m_anchor2;
    return false;
}
