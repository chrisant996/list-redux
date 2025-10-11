// Copyright (c) 2024 by Christopher Antos
// License: http://opensource.org/licenses/MIT

// vim: set et ts=4 sw=4 cino={0s:

#pragma once

bool IsConsole(HANDLE h);

void SetUtf8Output(bool utf8);
void SetRedirectedStdOut(bool redirected);

bool SetUseEscapeCodes(const WCHAR* s);
bool CanUseEscapeCodes();
void SetGracefulExit();

DWORD GetConsoleColsRows(HANDLE hout);

void ExpandTabs(const WCHAR* s, StrW& out, unsigned max_width=0);
void WrapText(const WCHAR* s, StrW& out, unsigned max_width=0);
void OutputConsole(HANDLE h, const WCHAR* p, unsigned len=unsigned(-1), const WCHAR* color=nullptr);

void PrintfV(const WCHAR* format, va_list args);
void Printf(const WCHAR* format, ...);

#ifdef DEBUG
void dbgprintf(const WCHAR* format, ...);
#endif

class Interactive
{
public:
    Interactive(bool hide_cursor=false);
    ~Interactive();
    void Begin();
    void End();
    bool Active() const { return m_active; }
private:
    DWORD m_orig_mode_in;
    DWORD m_orig_mode_out;
    bool m_active = false;
    const bool m_hide_cursor;
};

extern const WCHAR c_hide_cursor[];
extern const WCHAR c_show_cursor[];

