// Copyright (c) 2025 by Christopher Antos
// License: http://opensource.org/licenses/MIT

// vim: set et ts=4 sw=4 cino={0s:

#pragma once

#undef WriteConsole

class Terminal
{
public:
                    Terminal();
                    ~Terminal();

    bool            WriteConsole(const WCHAR* s, unsigned len);

private:
    CRITICAL_SECTION m_cs;

    const HANDLE    m_hout;
};
