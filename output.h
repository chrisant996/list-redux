// Copyright (c) 2024 by Christopher Antos
// License: http://opensource.org/licenses/MIT

// vim: set et ts=4 sw=4 cino={0s:

#pragma once

#include <memory>

enum class ColorElement;
class Error;

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

enum class ReportErrorFlags { NONE, CANABORT, INLINE };
DEFINE_ENUM_FLAG_OPERATORS(ReportErrorFlags);
StrW MakeMsgBoxText(const WCHAR* message, const WCHAR* directive, ColorElement color_elm);
bool ReportError(Error& e, ReportErrorFlags flags=ReportErrorFlags::NONE);

#ifdef DEBUG
void dbgprintf(const WCHAR* format, ...);
#endif

class Interactive
{
public:
    Interactive(bool begin=true);
    ~Interactive();
    void Begin();
    void End();
    bool Active() const { return m_active; }
    std::unique_ptr<Interactive> MakeReverseInteractive() const;
private:
    DWORD m_begin_mode_in;
    DWORD m_begin_mode_out;
    DWORD m_end_mode_in;
    DWORD m_end_mode_out;
    bool m_inverted = false;
    bool m_active = false;
};

extern const WCHAR c_hide_cursor[];
extern const WCHAR c_show_cursor[];

