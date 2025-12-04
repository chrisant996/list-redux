// Copyright (c) 2025 Christopher Antos
// Portions Copyright (c) 2016 Martin Ridgers
// License: http://opensource.org/licenses/MIT

#include "pch.h"
#include "input.h"
#include "output.h"
#include "colors.h"
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

struct SelectionState
{
                    SelectionState() : m_anchor(0), m_caret(0), m_dirty(false) { ResetWordAnchor(); }
                    SelectionState(unsigned short caret) : m_anchor(caret), m_caret(caret), m_dirty(false) { ResetWordAnchor(); }
                    SelectionState(unsigned short anchor, unsigned short caret) : m_anchor(anchor), m_caret(caret), m_dirty(false) { ResetWordAnchor(); }

    void            SetCaret(unsigned short caret) { SetSelection(caret, caret); }
    void            SetSelection(unsigned short anchor, unsigned short caret);
#if 0
    void            ResetWordAnchor() { m_word_anchor_begin = m_anchor; m_word_anchor_end = m_caret; }
    void            ResetWordAnchor(unsigned short caret) { m_word_anchor_begin = m_anchor; m_word_anchor_end = caret; }
#else
    void            ResetWordAnchor() {}
#endif

    unsigned short  GetAnchor() const { return m_anchor; }
    unsigned short  GetCaret() const { return m_caret; }
    unsigned short  GetSelBegin() const { return min(m_anchor, m_caret); }
    unsigned short  GetSelEnd() const { return max(m_anchor, m_caret); }
#if 0
    int             GetWordAnchorBegin() const { return m_word_anchor_begin; }
    int             GetWordAnchorEnd() const { return m_word_anchor_end; }
#endif
    bool            HasSelection() const { return m_anchor != m_caret; }

    bool            IsDirty() const { return m_dirty; }
    void            ClearDirty() { m_dirty = false; }

    unsigned short& GetAnchorOut() { return m_anchor; }
    unsigned short& GetCaretOut() { return m_caret; }

private:
    unsigned short  m_anchor;
    unsigned short  m_caret;
#if 0
    short           m_word_anchor_begin;
    short           m_word_anchor_end;
#endif
    bool            m_dirty;
};

void SelectionState::SetSelection(unsigned short anchor, unsigned short caret)
{
    assert(anchor != static_cast<unsigned short>(-1));
    assert(caret != static_cast<unsigned short>(-1));
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
public:
                    ReadInputState();
                    ~ReadInputState();

    void            SetMaxWidth(DWORD m) { m_max_width = static_cast<unsigned short>(min<DWORD>(m, 0x7fff)); }
    void            SetMaxLength(DWORD m) { m_max_length = static_cast<unsigned short>(min<DWORD>(m, 0x7fff)); }

    void            EnsureLeft();
    void            PrintVisible(unsigned short x);

    void            Home(Modifier modifier);
    void            End(Modifier modifier);
    void            Left(Modifier modifier);
    void            Right(Modifier modifier);
    void            Backspace(bool word=false);
    void            Delete(bool word=false);

    void            CopyToClipboard();
    void            PasteFromClipboard();

    unsigned short  GetCaret() const { return m_sel.GetCaret(); }
    void            ReplaceFromHistory(const StrW& s, bool keep_undo);
    void            InsertChar(WCHAR c, WCHAR c2=0);
    void            InsertText(const WCHAR* s, size_t len);
    void            RemoveText(unsigned short begin, unsigned short end);
    bool            ElideSelectedText();

    void            ClearUndo() { InitUndo(); }
    void            BeginUndoGroup();
    void            EndUndoGroup();
    void            Undo();
    void            Redo();

    void            TransferText(StrW& out) { out = std::move(m_s); }

#ifdef DEBUG
    void            DumpUndoStack();
#endif

private:
    void            InitUndo();
    void            ClearUndoInternal();
    void            UnlinkUndoEntry(UndoEntry* p);

private:
    StrW            m_s;
    unsigned short  m_max_width = 32;
    unsigned short  m_max_length = 32;
    unsigned short  m_left = 0;
    SelectionState  m_sel;

    // Undo/Redo queue.
    UndoEntry*      m_undo_head = nullptr;
    UndoEntry*      m_undo_tail = nullptr;
    UndoEntry*      m_undo_current = nullptr;
    short           m_grouping = 0;  // >0 means an undo group is in progress.
    bool            m_defer_init_undo = false;
};

ReadInputState::ReadInputState()
{
    InitUndo();
}

ReadInputState::~ReadInputState()
{
    ClearUndoInternal();
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
        unsigned short backup_left = m_sel.GetCaret();
        BackUpByAmount(backup_left, m_s.Text(), m_sel.GetCaret(), 4);
        if (m_left > backup_left)
            m_left = backup_left;
    }
}

void ReadInputState::PrintVisible(unsigned short x)
{
    StrW tmp;
    tmp.Printf(L"%s\x1b[%uG", c_hide_cursor, x + 1);
    OutputConsole(tmp.Text(), tmp.Length());

    unsigned width = 0;
    const unsigned len = FitsInWcwidth(m_s.Text() + m_left, m_s.Length() - m_left, m_max_width, &width);

    tmp.Clear();
    tmp.AppendColor(GetColor(ColorElement::Input));

    if (m_sel.GetAnchor() <= m_s.Length())
    {
        const unsigned short begin = clamp<unsigned short>(m_sel.GetSelBegin(), m_left, m_left + len);
        const unsigned short end = clamp<unsigned short>(m_sel.GetSelEnd(), m_left, m_left + len);
        tmp.Append(m_s.Text() + m_left, begin - m_left);
        if (begin < end)
            tmp.AppendColor(GetColor(ColorElement::InputSelection));
        tmp.Append(m_s.Text() + begin, end - begin);
        if (begin < end)
            // REVIEW:  Should this append a space here if the selection isn't fully drawn due to character width clipping?
            tmp.AppendColor(GetColor(ColorElement::Input));
        tmp.Append(m_s.Text() + end, len - end);
    }
    else
    {
        tmp.Append(m_s.Text() + m_left, len);
    }

    tmp.AppendSpaces(m_max_width - width);
    tmp.Printf(L"\x1b[%uG%s", x + 1 + __wcswidth(m_s.Text() + m_left, m_sel.GetCaret() - m_left), c_show_cursor);
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
        unsigned short caret = m_sel.GetCaret();
        unsigned short anchor = m_sel.GetAnchor();
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
        unsigned short caret = m_sel.GetCaret();
        unsigned short anchor = m_sel.GetAnchor();
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
        const unsigned short old_pos = m_sel.GetCaret();
#endif
        const unsigned short moved = PosMover(m_s.Text(), m_s.Length(), m_sel.GetCaretOut(), false/*forward*/, word);
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
        unsigned short del_pos = m_sel.GetCaret();
        const unsigned short moved = PosMover(m_s.Text(), m_s.Length(), del_pos, true/*forward*/, word);
        m_sel.SetCaret(del_pos - moved);
        RemoveText(m_sel.GetCaret(), m_sel.GetCaret() + moved);
    }

    EndUndoGroup();
}

void ReadInputState::CopyToClipboard()
{
    if (!m_sel.HasSelection())
        return;

    const unsigned short begin = m_sel.GetSelBegin();
    const unsigned short end = m_sel.GetSelEnd();
    const unsigned short len = (end - begin);

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

    unsigned short len = 0;
    wcwidth_iter iter(s, static_cast<unsigned short>(available));
    while (iter.next())
    {
        if (m_s.Length() + iter.character_length() > m_max_length)
            break;
        len += iter.character_length();
    }

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

void ReadInputState::RemoveText(unsigned short begin, unsigned short end)
{
    BeginUndoGroup();

    m_sel.ResetWordAnchor();

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

    const unsigned short begin = m_sel.GetSelBegin();
    const unsigned short end = m_sel.GetSelEnd();
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
DumpUndoStack();
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

    m_s.Set(r->m_s);
    m_sel = r->m_sel_after;

    m_undo_current = r;
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

    ReadInputState state;
    state.SetMaxWidth(max_width);
    state.SetMaxLength(max_length);

    while (true)
    {
        state.EnsureLeft();
        state.PrintVisible(csbi.dwCursorPosition.X);

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
                    state.Backspace((input.modifier & Modifier::CTRL) == Modifier::CTRL);
                break;
            case Key::INS:
                if (input.modifier == Modifier::CTRL)
                    state.CopyToClipboard();
                else if (input.modifier == Modifier::SHIFT)
                    state.PasteFromClipboard();
            case Key::DEL:
                if ((input.modifier & ~Modifier::CTRL) == Modifier::None)
                    state.Delete((input.modifier & Modifier::CTRL) == Modifier::CTRL);
                if (input.modifier == Modifier::SHIFT)
                    goto cut_to_clip;
                break;
            case Key::ENTER:
                state.TransferText(out);
                if (history && out.Length())
                    history->emplace_back(out);
                return true;
            case Key::HOME:
                state.Home(input.modifier);
                continue;
            case Key::END:
                state.End(input.modifier);
                continue;
            case Key::UP:
                if (history && history_index)
                {
                    if (history_index == history->size())
                        state.TransferText(curr_input_history);
                    --history_index;
                    state.ReplaceFromHistory((*history)[history_index], false/*keep_undo*/);
                }
                continue; // Avoid resetting history_index.
            case Key::DOWN:
                if (history && history_index < history->size())
                {
                    ++history_index;
                    if (history_index == history->size())
                        state.ReplaceFromHistory(curr_input_history, true/*keep_undo*/);
                    else
                        state.ReplaceFromHistory((*history)[history_index], false/*keep_undo*/);
                }
                continue; // Avoid resetting history_index.
            case Key::LEFT:
                state.Left(input.modifier);
                break;
            case Key::RIGHT:
                state.Right(input.modifier);
                break;
            }
        }
        else if (input.type == InputType::Char)
        {
            if (input.key_char >= ' ')
            {
                state.InsertChar(input.key_char, input.key_char2);
            }
            else
            {
                switch (input.key_char)
                {
                case 'A'-'@':
                    state.Home(Modifier::None);
                    state.End(Modifier::SHIFT);
                    break;
                case 'C'-'@':
                    state.CopyToClipboard();
                    break;
                case 'V'-'@':
                    state.PasteFromClipboard();
                    break;
                case 'X'-'@':
cut_to_clip:
                    state.BeginUndoGroup();
                    state.CopyToClipboard();
                    state.ElideSelectedText();
                    state.EndUndoGroup();
                    break;
                case 'Y'-'@':
                    state.Redo();
                    break;
                case 'Z'-'@':
                    state.Undo();
                    break;
                }
            }
        }

        if (history && history_index < history->size())
            history_index = history->size();
    }
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

AutoMouseConsoleMode::AutoMouseConsoleMode(HANDLE hin, bool enable)
{
    if (enable)
    {
        m_hin = hin ? hin : GetStdHandle(STD_INPUT_HANDLE);
        if (m_hin && !GetConsoleMode(m_hin, &m_orig_mode))
            m_hin = 0;
        DisableMouseInputIfShift();
    }
}

AutoMouseConsoleMode::~AutoMouseConsoleMode()
{
    if (m_hin)
        SetConsoleMode(m_hin, m_orig_mode);
}

void AutoMouseConsoleMode::DisableMouseInputIfShift()
{
    if (m_hin)
    {
        DWORD new_mode = (m_orig_mode & ~(ENABLE_MOUSE_INPUT|ENABLE_QUICK_EDIT_MODE));
        if (GetKeyState(VK_SHIFT) & 0x8000)
            new_mode |= ENABLE_QUICK_EDIT_MODE;
        else
            new_mode |= ENABLE_MOUSE_INPUT;
        if (new_mode != m_prev_mode && SetConsoleMode(m_hin, new_mode))
            m_prev_mode = new_mode;
    }
}
