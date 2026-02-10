// Copyright (c) 2025 Christopher Antos
// License: http://opensource.org/licenses/MIT

#pragma once

#include <windows.h>
#include <functional>
#include <optional>

#include "ellipsify.h"
#include "colors.h"

typedef uint16 textpos_t;

enum class InputType
{
    None,
    Key,
    Char,
    Mouse,
    Resize,
    Error,
};

enum class Key
{
    Invalid,
    ESC,
    BACK,
    TAB,
    ENTER,
    UP,
    DOWN,
    LEFT,
    RIGHT,
    INS,
    DEL,
    HOME,
    END,
    PGUP,
    PGDN,
    F1,
    F2,
    F3,
    F4,
    F5,
    F6,
    F7,
    F8,
    F9,
    F10,
    F11,
    F12,
    MouseLeftClick,
    MouseLeftDblClick,
    MouseRightClick,
    MouseDrag,
    MouseWheel,
    MouseHWheel,
};

enum class History
{
    Search,
    FileMask,
    ChangeAttr,
    NewDirectory,
    RenameEntry,
    SweepProgram,
    SweepArgsBefore,
    SweepArgsAfter,
    Goto,
    OpenFile,
    MAX,
};

enum class Modifier
{
    None,
    SHIFT   = 0x1,
    CTRL    = 0x2,
    ALT     = 0x4,
};
DEFINE_ENUM_FLAG_OPERATORS(Modifier);

bool HasModifier(const Modifier modifier, const Modifier mask);
bool MatchModifier(const Modifier modifier, const Modifier mask);
bool MatchModifier(const Modifier modifier, const Modifier mask, const Modifier match);

struct InputRecord
{
                    InputRecord();
                    InputRecord(InputType type);
    bool            operator!=(const InputRecord& other) const;
    void            Clear();

    InputType       type = InputType::None;
    Key             key = Key::Invalid;
    WCHAR           key_char = 0;
    WCHAR           key_char2 = 0;  // If key_char2 is a high surrogate, then key_char2 is the low surrogate (or 0 if invalid input).
    Modifier        modifier = Modifier::None;
    COORD           mouse_pos = {0,0};
    int32           mouse_wheel_amount = 0;
};

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

class ReadInputBuffer
{
public:
                    ReadInputBuffer() = default;
                    ~ReadInputBuffer() = default;

    textpos_t       GetCaret() const { return m_sel.GetCaret(); }
    const SelectionState& GetSelectionState() const { return m_sel; }

    const StrW&     GetText() const { return m_s; }

protected:
    // Content and state.
    StrW            m_s;
    SelectionState  m_sel;
};

extern const WCHAR c_prompt_char[];

class AutoMouseConsoleMode
{
public:
                    AutoMouseConsoleMode(bool enable=true);
                    ~AutoMouseConsoleMode();
    void            DisableMouseInput();
    void            DisableMouseInputIfShift();
    static void     SetStdInputHandle(HANDLE hin);
private:
    void            UpdateMode(DWORD new_mode, bool force=false);
private:
    static HANDLE   s_hin;
    static DWORD    s_prev_mode;
    DWORD           m_restore_mode = 0;
    bool            m_can_restore = false;
};

class MouseHelper
{
    struct AccelerationHelper
    {
                    AccelerationHelper() = default;
        int32       MaybeAccelerate(int32 lines);
        int32       m_acceleration = 0;
        DWORD       m_last_tick = 0;
    };
public:
                    MouseHelper(bool allow=false) : m_allow_acceleration(allow) { ClearClicks(); }
    void            AllowAcceleration(bool allow) { m_allow_acceleration = allow; }
    int32           LinesFromRecord(const InputRecord& input);
    uint8           OnClick(COORD coord, bool dblclk);
    uint8           GetClicks() const { return m_clicks; }
    void            ClearClicks();
    void            SetAnchors(textpos_t a1, textpos_t a2);
    bool            GetAnchor(textpos_t pos, textpos_t& anchor, textpos_t& caret) const;
private:
    AccelerationHelper m_vert_accel;
    AccelerationHelper m_horz_accel;
    bool            m_allow_acceleration = false;
    uint8           m_clicks;
    COORD           m_coord;
    DWORD           m_tick;
    textpos_t       m_anchor1;
    textpos_t       m_anchor2;
};

class ClickableRow
{
    struct Element
    {
        StrW        m_text;
        StrW        m_fitted;
        uint16      m_width;
        uint16      m_effective_width;
        int16       m_id;
        int16       m_priority;
        int16       m_left;
        ellipsify_mode m_fit_mode;
        uint16      m_min_fit_width;
        bool        m_enabled;
    };

public:
                    ClickableRow() = default;
                    ~ClickableRow() = default;

    void            Init(uint16 row, uint16 terminal_width, uint16 reserve_left=0);
    void            Add(const WCHAR* text, int16 id, int16 priority, bool right_align, ellipsify_mode fit_mode=ellipsify_mode::INVALID, uint16 min_fit_width=20, bool enabled=true);
    void            AddKeyName(const WCHAR* key, ColorElement color_after, const WCHAR* desc, int16 id, int16 priority, bool right_align, bool enabled=true);
    uint16          GetLeftWidth();
    uint16          GetRightWidth();
    void            BuildOutput(StrW& out, const WCHAR* color=nullptr);
    int16           InterpretInput(const InputRecord& input) const;

private:
    void            EnsureLayout();
    uint16          AppendOutput(StrW& out, const Element& elm, const WCHAR* color);

private:
    uint16          m_row = 0;
    uint16          m_terminal_width = 0;
    uint16          m_reserve_left = 0;
    int16           m_threshold = 0x7fff;
    uint16          m_left_width = 0;
    uint16          m_right_width = 0;
    std::vector<Element> m_left_elements;
    std::vector<Element> m_right_elements;
    bool            m_need_layout = false;
};

InputRecord SelectInput(DWORD timeout=INFINITE, AutoMouseConsoleMode* mouse=nullptr);
bool ReadInput(StrW& out, History history=History::MAX, DWORD max_length=30, DWORD max_width=32, std::optional<std::function<int32(const InputRecord&, const ReadInputBuffer&, void*)>> input_callback=std::nullopt);
bool IsMouseLeftButtonDown();

bool ParseULongLong(const WCHAR* s, ULONGLONG& out, int radix=10);
