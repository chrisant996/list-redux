// Copyright (c) 2025 by Christopher Antos
// License: http://opensource.org/licenses/MIT

// vim: set et ts=4 sw=4 cino={0s:

#include "pch.h"
#include "signaled.h"
#include "output.h"

static bool s_signaled = false;

class CRestoreConsole
{
public:
    CRestoreConsole();
    ~CRestoreConsole();

    void                SetGracefulExit() { m_graceful = true; }

private:
    void                Restore();
    static BOOL WINAPI  BreakHandler(DWORD CtrlType);

private:
    HANDLE              m_hout = 0;
    HANDLE              m_herr = 0;
    DWORD               m_mode_out;
    DWORD               m_mode_err;
    bool                m_graceful = false;
};

static CRestoreConsole s_restoreConsole;

void SetGracefulExit()
{
    s_restoreConsole.SetGracefulExit();
}

bool IsSignaled()
{
    return s_signaled;
}

void ClearSignaled()
{
    s_signaled = false;
}

CRestoreConsole::CRestoreConsole()
{
    HANDLE hout = GetStdHandle(STD_OUTPUT_HANDLE);
    HANDLE herr = GetStdHandle(STD_ERROR_HANDLE);

    if (hout && !GetConsoleMode(hout, &m_mode_out))
        hout = 0;
    if (herr && !GetConsoleMode(herr, &m_mode_err))
        herr = 0;

    if (!hout && !herr)
        return;

    m_hout = hout;
    m_herr = herr;

    SetConsoleCtrlHandler(BreakHandler, true);

    if (hout)
        SetConsoleMode(hout, m_mode_out|ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    if (herr)
        SetConsoleMode(herr, m_mode_err|ENABLE_VIRTUAL_TERMINAL_PROCESSING);
}

CRestoreConsole::~CRestoreConsole()
{
    Restore();
}

void CRestoreConsole::Restore()
{
    if (m_hout || m_herr)
    {
        if (!m_graceful)
        {
            DWORD dummy;
            if (m_hout && CanUseEscapeCodes() && GetConsoleMode(m_hout, &dummy))
                WriteConsoleW(m_hout, L"\x1b[m", 3, &dummy, nullptr);
            if (m_herr && CanUseEscapeCodes() && GetConsoleMode(m_herr, &dummy))
                WriteConsoleW(m_herr, L"\x1b[m", 3, &dummy, nullptr);
        }
    }

    if (m_hout)
        SetConsoleMode(m_hout, m_mode_out);
    if (m_herr)
        SetConsoleMode(m_herr, m_mode_err);

    m_hout = 0;
    m_herr = 0;
}

BOOL CRestoreConsole::BreakHandler(DWORD CtrlType)
{
    if (CtrlType == CTRL_C_EVENT || CtrlType == CTRL_BREAK_EVENT)
    {
        // Do not terminate on Ctrl-C or Ctrl-Break.
        s_signaled = true;
        return true;
    }
    return false;
}
