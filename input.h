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
    bool            operator!=(const InputRecord& other) const;

    InputType       type = InputType::None;
    Key             key = Key::Invalid;
    WCHAR           key_char = 0;
    Modifier        modifier = Modifier::None;
    COORD           mouse_pos = {0,0};
    int32           mouse_wheel_amount = 0;
};

extern const WCHAR c_prompt_char[];

InputRecord SelectInput(DWORD timeout=INFINITE);
bool ReadInput(StrW& out, DWORD max_width=32, std::optional<std::function<int32(const InputRecord&)>> input_callback=std::nullopt);

class AutoMouseConsoleMode
{
public:
                    AutoMouseConsoleMode(HANDLE hin=0);
                    ~AutoMouseConsoleMode();
    void            EnableMouseInput(bool enable);
private:
    HANDLE          m_hin = 0;
    DWORD           m_orig_mode = 0;
    DWORD           m_prev_mode = 0;
};
