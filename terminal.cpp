// Copyright (c) 2025 Christopher Antos
// Portions Copyright (c) 2016 Martin Ridgers
// License: http://opensource.org/licenses/MIT

#include "pch.h"
#include "terminal.h"

class ScopedEnterCritSec
{
public:
    ScopedEnterCritSec(CRITICAL_SECTION& cs) : m_cs(cs) { EnterCriticalSection(&m_cs); }
    ~ScopedEnterCritSec() { LeaveCriticalSection(&m_cs); }
private:
    CRITICAL_SECTION& m_cs;
};

#define SCOPED_LOCK ScopedEnterCritSec scoped_lock(m_cs)

Terminal::Terminal()
: m_hout(GetStdHandle(STD_OUTPUT_HANDLE))
{
    InitializeCriticalSection(&m_cs);
}

Terminal::~Terminal()
{
    DeleteCriticalSection(&m_cs);
}

bool Terminal::WriteConsole(const WCHAR* s, unsigned len)
{
    SCOPED_LOCK;

    DWORD written;
    return !!WriteConsoleW(m_hout, s, len, &written, nullptr);
}
