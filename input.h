// Copyright (c) 2025 Christopher Antos
// License: http://opensource.org/licenses/MIT

#pragma once

#include <windows.h>

enum class InputType
{
    None,
    Key,
    Char,
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
};

InputRecord SelectInput(DWORD timeout=INFINITE);
void ReadInput(StrW& out, DWORD max_width=32);

