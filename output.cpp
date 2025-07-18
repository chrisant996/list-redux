// Copyright (c) 2024 by Christopher Antos
// License: http://opensource.org/licenses/MIT

// vim: set et ts=4 sw=4 cino={0s:

#include "pch.h"
#include "output.h"
#include "colors.h"
#include "ecma48.h"
#include "wcwidth.h"
#include "wcwidth_iter.h"

#include <VersionHelpers.h>

const WORD c_cxTab = 8;

enum class EscapeCodesMode
{
    not_initialized,
    prohibit,
    allow,
    automatic,  // When not redirected.
};

static EscapeCodesMode s_escape_codes = EscapeCodesMode::not_initialized;
static bool s_utf8 = false;
static bool s_redirected_stdout = false;

const WCHAR c_hide_cursor[] = L"\x1b[?25l";
const WCHAR c_show_cursor[] = L"\x1b[?25h";

bool IsConsole(HANDLE h)
{
    DWORD dummy;
    return !!GetConsoleMode(h, &dummy);
}

void SetUtf8Output(bool utf8)
{
    s_utf8 = utf8;
}

void SetRedirectedStdOut(bool redirected)
{
    if (redirected)
        SetUseEscapeCodes(L"never");
}

bool SetUseEscapeCodes(const WCHAR* s)
{
    if (!s)
        return false;
    else if (!_wcsicmp(s, L"") || !_wcsicmp(s, L"always"))
        s_escape_codes = EscapeCodesMode::allow;
    else if (!_wcsicmp(s, L"never"))
        s_escape_codes = EscapeCodesMode::prohibit;
    else if (!_wcsicmp(s, L"auto"))
        s_escape_codes = EscapeCodesMode::automatic;
    else
        return false;
    return true;
}

bool CanUseEscapeCodes()
{
    switch (s_escape_codes)
    {
    case EscapeCodesMode::prohibit:
        return false;
    case EscapeCodesMode::allow:
        return true;
    case EscapeCodesMode::automatic:
        break;
    case EscapeCodesMode::not_initialized:
        s_escape_codes = EscapeCodesMode::automatic;
        break;
    default:
        assert(false);
        return false;
    }

    assert(s_escape_codes == EscapeCodesMode::automatic);

    // See https://no-color.org/.
    const WCHAR* env = _wgetenv(L"NO_COLOR");
    if (env && *env)
    {
        s_escape_codes = EscapeCodesMode::prohibit;
        return false;
    }

    if (!IsWindows10OrGreater())
    {
        s_escape_codes = EscapeCodesMode::prohibit;
        return false;
    }

    s_escape_codes = EscapeCodesMode::allow;
    return true;
}

int ValidateColor(const WCHAR* p)
{
    // NOTE:  The caller is responsible for stripping leading/trailing spaces.

    if (!p || !*p)              return false;   // nullptr or "" is "has no color specified".
    else if (p[0] == '0')
    {
        if (!p[1])              return false;   // "0" is default.
        if (p[1] == '0')
        {
            if (!p[2])          return false;   // "00" is default.
        }
    }

    enum { ST_NORMAL, ST_BYTES1, ST_BYTES2, ST_BYTES3, ST_XCOLOR };
    int state = ST_NORMAL;

    // Validate recognized color/style escape codes.
    for (unsigned num = 0; true; ++p)
    {
        if (!p || *p == ';')
        {
            if (state == ST_NORMAL)
            {
                switch (num)
                {
                case 0:     // reset or normal
                case 1:     // bold
                case 2:     // faint or dim
                case 3:     // italic
                case 4:     // underline
                // case 5:     // slow blink
                // case 6:     // rapid blink
                case 7:     // reverse
                // case 8:     // conceal (hide)
                case 9:     // strikethrough
                case 21:    // double underline
                case 22:    // not bold and not faint
                case 23:    // not italic
                case 24:    // not underline
                case 25:    // not blink
                case 27:    // not reverse
                // case 28:    // reveal (not conceal/hide)
                case 29:    // not strikethrough
                case 30: case 31: case 32: case 33: case 34: case 35: case 36: case 37: case 39:
                case 40: case 41: case 42: case 43: case 44: case 45: case 46: case 47: case 49:
                // case 51:    // framed
                // case 52:    // encircled
                case 53:    // overline
                // case 54:    // not framed and not encircled
                case 55:    // not overline
                case 59:    // default underline color
                // case 73:    // superscript
                // case 74:    // subscript
                // case 75:    // not superscript and not subscript
                case 90: case 91: case 92: case 93: case 94: case 95: case 96: case 97:
                case 100: case 101: case 102: case 103: case 104: case 105: case 106: case 107:
                    break;
                case 38:    // set foreground color
                case 48:    // set background color
                // case 58:    // set underline color
                    state = ST_XCOLOR;
                    break;
                default:
                    return -1; // Unsupported SGR code.
                }
            }
            else if (state == ST_XCOLOR)
            {
                if (num == 2)
                    state = ST_BYTES3;
                else if (num == 5)
                    state = ST_BYTES1;
                else
                    return -1; // Unsupported extended color mode.
            }
            else if (state >= ST_BYTES1 && state <= ST_BYTES3)
            {
                if (num >= 0 && num <= 255)
                    --state;
                else
                    return -1; // Unsupported extended color.
            }
            else
            {
                assert(false);
                return -1; // Internal error.
            }

            num = 0;
        }

        if (!*p)
            return true; // The SGR code has been successfully validated.

        if (*p >= '0' && *p <= '9')
        {
            num *= 10;
            num += *p - '0';
        }
        else if (*p != ';')
            return -1; // Unsupported or invalid SGR code.
    }
}

/*
 * Restore console mode and attributes on exit or ^C or ^Break.
 */

static HANDLE s_hConsoleMutex = CreateMutex(0, false, 0);

static void AcquireConsoleMutex()
{
    if (s_hConsoleMutex)
        WaitForSingleObject(s_hConsoleMutex, INFINITE);
}

static void ReleaseConsoleMutex()
{
    if (s_hConsoleMutex)
        ReleaseMutex(s_hConsoleMutex);
}

class AutoConsoleMutex
{
public:
    AutoConsoleMutex() { AcquireConsoleMutex(); }
    ~AutoConsoleMutex() { ReleaseConsoleMutex(); }
};

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
        return true;
    }
    return false;
}

/*
 * OutputConsole.
 */

void SetGracefulExit()
{
    s_restoreConsole.SetGracefulExit();
}

DWORD GetConsoleColsRows(HANDLE hout)
{
    assert(hout);

    static BOOL s_initialized = false;
    static HANDLE s_hout = INVALID_HANDLE_VALUE;
    static HANDLE s_console = INVALID_HANDLE_VALUE;
    static BOOL s_is_console;
    static WORD s_num_cols = 80;
    static WORD s_num_rows = 25;

    if (hout != s_hout)
    {
        s_initialized = false;
        s_hout = hout;
        if (s_console != INVALID_HANDLE_VALUE)
            CloseHandle(s_console);
        s_console = INVALID_HANDLE_VALUE;
    }

    if (!s_initialized)
    {
        s_is_console = IsConsole(hout);

        if (s_is_console)
        {
            s_console = hout;
        }
        else
        {
            s_num_cols = 0;
            s_num_rows = 0;
            s_console = CreateFileW(L"CONOUT$", GENERIC_READ, FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, 0);
        }

        if (s_is_console || s_console != INVALID_HANDLE_VALUE)
        {
            CONSOLE_SCREEN_BUFFER_INFO csbi;

            if (GetConsoleScreenBufferInfo(s_console, &csbi))
            {
                s_num_cols = (csbi.srWindow.Right - csbi.srWindow.Left) + 1;
                s_num_rows = (csbi.srWindow.Bottom - csbi.srWindow.Top) + 1;
            }
        }

        s_initialized = true;
    }

    return (s_num_rows << 16) | s_num_cols;
}

static bool WriteConsoleInternal(HANDLE h, const WCHAR* p, unsigned len, const WCHAR* color=nullptr)
{
    static HANDLE s_h = 0;
    static bool s_console = false;
    DWORD written;

    if (s_h != h)
    {
        s_console = IsConsole(h);
        s_h = h;
    }

    AutoConsoleMutex acm;

    if (color)
    {
        if (!CanUseEscapeCodes() || ValidateColor(color) <= 0)
            color = nullptr;
    }

    if (color)
    {
        StrW tmp;
        tmp.Printf(L"\x1b[0;%sm", color);
        if (s_console)
        {
            if (!WriteConsoleW(h, tmp.Text(), tmp.Length(), &written, nullptr))
                return false;
        }
        else
        {
            StrA tmp2;
            tmp2.SetW(tmp);
            if (!WriteFile(h, tmp2.Text(), tmp2.Length(), &written, nullptr))
                return false;
        }
    }

    StrA tmp;
    while (len)
    {
        if (p[0] == '\n')
        {
            if (s_console)
            {
                if (!WriteConsoleW(h, L"\r\n", 2, &written, nullptr))
                    return false;
            }
            else
            {
                if (!WriteFile(h, "\r\n", 2, &written, nullptr))
                    return false;
            }
            --len;
            ++p;
        }

        unsigned run = 0;
        while (run < len && p[run] != '\n')
            ++run;

        if (run)
        {
            if (s_console)
            {
                if (!WriteConsoleW(h, p, run, &written, nullptr))
                    return false;
            }
            else
            {
                const UINT cp = s_utf8 ? CP_UTF8 : GetConsoleOutputCP();
                const size_t needed = WideCharToMultiByte(cp, 0, p, int(run), 0, 0, 0, 0);
                char* out = tmp.Reserve(needed + 1);
                int used = WideCharToMultiByte(cp, 0, p, int(run), out, int(needed), 0, 0);

                assert(unsigned(used) < tmp.Capacity());
                out[used] = '\0';
                tmp.ResyncLength();

                if (!WriteFile(h, tmp.Text(), tmp.Length(), &written, nullptr))
                    return false;
            }
        }

        len -= run;
        p += run;
    }

    if (color)
    {
        if (s_console)
        {
            if (!WriteConsoleW(h, L"\x1b[m", 3, &written, nullptr))
                return false;
        }
        else
        {
            if (!WriteFile(h, "\x1b[m", 3, &written, nullptr))
                return false;
        }
    }

    assert(!len);
    return true;
}

void OutputConsole(HANDLE h, const WCHAR* p, unsigned len, const WCHAR* color)
{
    if (len == unsigned(-1))
        len = unsigned(wcslen(p));
    if (!len)
        return;

    if (!WriteConsoleInternal(h, p, len, color))
    {
        // TODO: error handling...
        exit(1);
        return;
    }
}

void ExpandTabs(const WCHAR* s, StrW& out, unsigned max_width)
{
    StrW tmp;

    if (!max_width)
    {
        const DWORD dwColsRows = GetConsoleColsRows(GetStdHandle(STD_OUTPUT_HANDLE));
        max_width = LOWORD(dwColsRows);
        // max_width is always non-zero, but the code below in this function
        // recognizes 0 as meaning unlimited width.
        if (!max_width)
            --max_width;
    }

    unsigned cx = 0;

    ecma48_state state;
    ecma48_iter iter(s, state);
    while (const ecma48_code& code = iter.next())
    {
        if (code.get_type() != ecma48_code::type_chars &&
            code.get_type() != ecma48_code::type_c0)
        {
            tmp.Append(code.get_pointer(), code.get_length());
            continue;
        }

        wcwidth_iter inner_iter(code.get_pointer(), code.get_length());
        while (true)
        {
            const WCHAR* const s = inner_iter.get_pointer();
            const char32_t c = inner_iter.next();
            if (!c)
                break;

            switch (c)
            {
            case '\b':
                if (cx)
                    --cx;
                tmp.Append(s, 1);
                break;
            case '\r':
            case '\n':
                cx = 0;
                tmp.Append(s, 1);
                break;
            case '\t':
                {
                    const unsigned new_cx = cx + c_cxTab - (cx % c_cxTab);
                    if (new_cx >= max_width)
                    {
                        tmp.AppendSpaces(max_width - cx);
                        cx = 0;
                    }
                    else
                    {
                        tmp.AppendSpaces(new_cx - cx);
                        cx = new_cx;
                    }
                }
                break;
            default:
                const int32 w = inner_iter.character_wcwidth_zeroctrl();
                cx += w;
                if (cx >= max_width)
                    cx = (cx > max_width) ? w : 0;
                tmp.Append(s, inner_iter.character_length());
                break;
            }
        }
    }

    out.Swap(tmp);
}

class WrapBuilder
{
public:
    WrapBuilder(StrW& out, unsigned max_width)
    : m_out(out)
    {
        if (!max_width)
        {
            const DWORD dwColsRows = GetConsoleColsRows(GetStdHandle(STD_OUTPUT_HANDLE));
            max_width = LOWORD(dwColsRows);
        }
        // IMPORTANT:  The minimum wrapping width is 80 because some sections
        // in the usage text do not support less than 80 columns.
        m_max_width = max<unsigned>(80, max_width);
    }

    void Append(const WCHAR* s, unsigned len)
    {
        if (m_auto_hanging)
        {
            for (const WCHAR* p = s; *p; ++p)
            {
                if (*p != ' ')
                {
                    SetHangingIndent();
                    m_hanging_indent += unsigned(p - s);
                    break;
                }
            }
        }
        m_word.Append(s, len);
    }

    void SetHangingIndent()
    {
        m_hanging_indent = m_columns + cell_count(m_word.Text());
        m_auto_hanging = false;
    }

    bool EnableWrapping(bool wrapping)
    {
        const bool was = m_wrapping;
        m_wrapping = wrapping;
        return was;
    }

    void FlushWord()
    {
        unsigned cols = cell_count(m_word.Text());
        if (cols)
        {
            const WCHAR* word = m_word.Text();
            if (m_wrapping && m_columns > 0 && m_columns + cols >= m_max_width)
            {
                FlushLine();
                while (*word == ' ')
                    ++word;
                cols = cell_count(word);
            }
            m_columns += cols;
            m_out.Append(word);
            m_word.Clear();
        }
    }

    void NewLine()
    {
        ResetLine();
        FlushLine(true/*force*/);
    }

    void End()
    {
        FlushWord();
        ResetLine();
        FlushLine(false/*force*/);
    }

private:
    void ResetLine()
    {
        m_hanging_indent = 0;
        m_auto_hanging = true;
    }

    void FlushLine(bool force=false)
    {
        if (m_columns || force)
        {
            m_out.Append(L"\n");
            m_columns = 0;
            if (m_hanging_indent)
            {
                const unsigned cxMaxHanging = min<unsigned>(m_max_width / 2, 40);
                const unsigned hanging = min<unsigned>(cxMaxHanging, m_hanging_indent);
                m_out.AppendSpaces(hanging);
                m_columns += hanging;
            }
        }
    }

private:
    unsigned m_max_width;
    unsigned m_columns = 0;
    unsigned m_hanging_indent = 0;
    bool m_wrapping = true;
    bool m_auto_hanging = true;
    StrW m_word;
    StrW& m_out;
};

void WrapText(const WCHAR* s, StrW& out, unsigned max_width)
{
    StrW tmp;
    WrapBuilder build(tmp, max_width);

    ecma48_state state;
    ecma48_iter iter(s, state);
    while (const ecma48_code& code = iter.next())
    {
        if (code.get_type() != ecma48_code::type_chars &&
            code.get_type() != ecma48_code::type_c0)
        {
            build.Append(code.get_pointer(), code.get_length());
            continue;
        }

        bool non_spaces = false;
        wcwidth_iter inner_iter(code.get_pointer(), code.get_length());
        while (true)
        {
            const WCHAR* const s = inner_iter.get_pointer();
            const char32_t c = inner_iter.next();
            if (!c)
                break;

            assert(c != '\b' && c != '\t');
            const unsigned len = unsigned(inner_iter.get_pointer() - s);

            switch (c)
            {
            case '\r':
            case '\n':
                build.FlushWord();
                build.NewLine();
                non_spaces = false;
                break;
            case ' ':
                if (non_spaces)
                {
                    build.FlushWord();
                    non_spaces = false;
                }
                build.Append(s, len);
                break;
            case '\001':    // Disable wrapping.
            case '\002':    // Enable wrapping.
                build.EnableWrapping(c == '\x02');
                break;
            case '\030':    // Non-breaking space.
                build.Append(L" ", 1);
                break;
            case '\032':    // Set hanging indent.
                build.FlushWord();
                build.SetHangingIndent();
                break;
            default:
                build.Append(s, len);
                non_spaces = true;
                break;
            }
        }
    }

    build.End();

    out.Swap(tmp);
}

void PrintfV(const WCHAR* format, va_list args)
{
    StrW s;
    s.PrintfV(format, args);
    OutputConsole(GetStdHandle(STD_OUTPUT_HANDLE), s.Text(), s.Length());
}

void Printf(const WCHAR* format, ...)
{
    va_list args;
    va_start(args, format);
    PrintfV(format, args);
    va_end(args);
}

#ifdef DEBUG
void dbgprintf(const WCHAR* format, ...)
{
    StrW s;
    va_list args;
    va_start(args, format);
    s.Append(L"LIST: ");
    s.PrintfV(format, args);
    s.Append(L"\r\n");
    OutputDebugStringW(s.Text());
    va_end(args);
}
#endif

/*
 * Interactive.
 */

Interactive::Interactive(bool hide_cursor)
: m_hide_cursor(hide_cursor)
{
    const HANDLE hin = GetStdHandle(STD_INPUT_HANDLE);
    const HANDLE hout = GetStdHandle(STD_OUTPUT_HANDLE);

    GetConsoleMode(hin, &m_orig_mode_in);
    GetConsoleMode(hout, &m_orig_mode_out);

    SetConsoleMode(hin, ENABLE_WINDOW_INPUT | (m_orig_mode_in & ~(ENABLE_PROCESSED_INPUT|ENABLE_LINE_INPUT|ENABLE_ECHO_INPUT|ENABLE_VIRTUAL_TERMINAL_INPUT)));
    SetConsoleMode(hout, ENABLE_PROCESSED_OUTPUT|ENABLE_VIRTUAL_TERMINAL_PROCESSING | m_orig_mode_out);

    if (m_hide_cursor)
        OutputConsole(hout, c_hide_cursor);
    OutputConsole(hout, L"\x1b[?1049h\x1b[H\x1b[J");
}

Interactive::~Interactive()
{
    const HANDLE hin = GetStdHandle(STD_INPUT_HANDLE);
    const HANDLE hout = GetStdHandle(STD_OUTPUT_HANDLE);

    OutputConsole(hout, L"\x1b[?1049l");
    if (m_hide_cursor)
        OutputConsole(hout, c_show_cursor);

    SetConsoleMode(hout, m_orig_mode_out);
    SetConsoleMode(hin, m_orig_mode_in);
}

