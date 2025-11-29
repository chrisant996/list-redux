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
    DWORD               m_mode_out;
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
    if (hout && !GetConsoleMode(hout, &m_mode_out))
        hout = 0;
    if (!hout)
        return;

    m_hout = hout;

    SetConsoleCtrlHandler(BreakHandler, true);

    if (hout)
        SetConsoleMode(hout, m_mode_out|ENABLE_VIRTUAL_TERMINAL_PROCESSING);
}

CRestoreConsole::~CRestoreConsole()
{
    Restore();
}

void CRestoreConsole::Restore()
{
    if (m_hout)
    {
        if (!m_graceful)
        {
            DWORD dummy;
            if (m_hout && GetConsoleMode(m_hout, &dummy))
                OutputConsole(L"\x1b[m", 3);
        }
        SetConsoleMode(m_hout, m_mode_out);
    }
    m_hout = 0;
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
