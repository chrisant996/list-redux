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

extern const WCHAR c_prompt_char[];

class AutoMouseConsoleMode
{
public:
                    AutoMouseConsoleMode(bool enable=true);
                    ~AutoMouseConsoleMode();
    void            DisableMouseInput();
    void            DisableMouseInputIfShift();
private:
    void            UpdateMode(DWORD new_mode);
private:
    static HANDLE   s_hin;
    static DWORD    s_prev_mode;    // To differentiate between press vs release.
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
    };

public:
                    ClickableRow() = default;
                    ~ClickableRow() = default;

    void            Init(uint16 row, uint16 terminal_width);
    void            Add(const WCHAR* text, int16 id, int16 priority, bool right_align, ellipsify_mode fit_mode=ellipsify_mode::INVALID, uint16 min_fit_width=20);
    void            AddKeyName(const WCHAR* key, ColorElement color_after, const WCHAR* desc, int16 id, int16 priority, bool right_align);
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
    int16           m_threshold = 0x7fff;
    uint16          m_left_width = 0;
    uint16          m_right_width = 0;
    std::vector<Element> m_left_elements;
    std::vector<Element> m_right_elements;
    bool            m_need_layout = false;
};

InputRecord SelectInput(DWORD timeout=INFINITE, AutoMouseConsoleMode* mouse=nullptr);
bool ReadInput(StrW& out, History history=History::MAX, DWORD max_length=30, DWORD max_width=32, std::optional<std::function<int32(const InputRecord&, void*)>> input_callback=std::nullopt);

bool ParseULongLong(const WCHAR* s, ULONGLONG& out, int radix=10);
