// Copyright (c) 2025 Christopher Antos
// License: http://opensource.org/licenses/MIT

#pragma once

#include <windows.h>
#include <functional>
#include <optional>

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
                    AutoMouseConsoleMode(HANDLE hin=0, bool enable=true);
                    ~AutoMouseConsoleMode();
    void            DisableMouseInputIfShift();
private:
    HANDLE          m_hin = 0;
    DWORD           m_orig_mode = 0;
    DWORD           m_prev_mode = 0;
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
                    MouseHelper(bool allow=false) : m_allow_acceleration(allow) {}
    void            AllowAcceleration(bool allow) { m_allow_acceleration = allow; }
    int32           LinesFromRecord(const InputRecord& input);
private:
    AccelerationHelper m_vert_accel;
    AccelerationHelper m_horz_accel;
    bool            m_allow_acceleration = false;
};

InputRecord SelectInput(DWORD timeout=INFINITE, AutoMouseConsoleMode* mouse=nullptr);
bool ReadInput(StrW& out, History history=History::MAX, DWORD max_length=32, DWORD max_width=32, std::optional<std::function<int32(const InputRecord&)>> input_callback=std::nullopt);

bool ParseULongLong(const WCHAR* s, ULONGLONG& out, int radix=10);
