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

// Define this to prevent executing destructive operations (such as deleting
// a file or directory).  This only affects operations that have cooperatively
// opted in to be controlled by this.
//#define DISALLOW_DESTRUCTIVE_OPERATIONS

#define implies(x, y)           (!(x) || (y))

template <typename T> T clamp(T value, T min, T max)
{
    value = value < min ? min : value;
    return value > max ? max : value;
}

typedef __int8 int8;
typedef unsigned __int8 uint8;
typedef __int32 int32;
typedef unsigned __int32 uint32;

#undef min
#undef max
template<class T> T min(T a, T b) { return (a <= b) ? a : b; }
template<class T> T max(T a, T b) { return (a >= b) ? a : b; }

#include "str.h"
#include "path.h"
#include "error.h"
#include "handle.h"
