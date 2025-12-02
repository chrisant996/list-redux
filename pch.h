// Copyright (c) 2024 by Christopher Antos
// License: http://opensource.org/licenses/MIT

#pragma warning(disable: 4290)

#include <windows.h>
#include <stdlib.h>
#include <stdio.h>
#include <new>
#include <tchar.h>
#include <uchar.h>
#include <assert.h>
#include <optional>
#include <functional>

// Define this to prevent executing destructive operations (such as deleting
// a file or directory).  This only affects operations that have cooperatively
// opted in to be controlled by this.
//#define DISALLOW_DESTRUCTIVE_OPERATIONS

// Define this to include an ECMA-48 compliant terminal emulator, to allow
// running on Windows versions before 10 build 15063.
#define INCLUDE_TERMINAL_EMULATOR

//#define INCLUDE_MENU_ROW

//#define INCLUDE_CTRLMODE_PERIOD
//#define INCLUDE_CTRLMODE_SPACE

#define implies(x, y)           (!(x) || (y))

template <typename T> T clamp(T value, T min, T max)
{
    value = value > max ? max : value;
    return value < min ? min : value; // min last, for cases like (0, 0, -1).
}

typedef __int8 int8;
typedef unsigned __int8 uint8;
typedef __int32 int32;
typedef unsigned __int32 uint32;
typedef __int64 int64;
typedef unsigned __int64 uint64;

#undef min
#undef max
#undef abs
#undef sgn
template<class T> T min(T a, T b) { return (a <= b) ? a : b; }
template<class T> T max(T a, T b) { return (a >= b) ? a : b; }
template<class T> T abs(T a) { return (a < 0) ? -a : a; }
template<class T> T sgn(T a) { return (a > 0) ? 1 : (a < 0) ? -1 : 0; }

class AutoCleanup
{
public:
                    AutoCleanup() = default;
                    AutoCleanup(std::function<void()> fn) : m_fn(fn) {}
                    ~AutoCleanup() { Cleanup(); }
    void            Set(std::function<void()> fn) { assert(!m_fn.has_value()); m_fn = fn; }
    void            Cleanup() { if (m_fn.has_value()) m_fn.value()(); }
    void            Discard() { m_fn.reset(); }
private:
    std::optional<std::function<void()>> m_fn;
};

#include "str.h"
#include "path.h"
#include "error.h"
#include "handle.h"
