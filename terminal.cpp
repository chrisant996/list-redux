// Copyright (c) 2025 Christopher Antos
// Portions Copyright (c) 2016 Martin Ridgers
// License: http://opensource.org/licenses/MIT

#include "pch.h"

#include "terminal.h"
#include "output.h"

#ifndef INCLUDE_TERMINAL_EMULATOR

Terminal::Terminal(int /*emulate*/)
: m_hout(GetStdHandle(STD_OUTPUT_HANDLE))
{
}

Terminal::~Terminal()
{
}

void Terminal::WriteConsole(const WCHAR* chars, unsigned length)
{
    DWORD written;
    WriteConsoleW(m_hout, chars, length, &written, nullptr);
}

#else // INCLUDE_TERMINAL_EMULATOR

#include <VersionHelpers.h>

static const uint8 s_rgb_cube[] = { 0x00, 0x5f, 0x87, 0xaf, 0xd7, 0xff };

static bool IsEmulationNeeded(int emulate=-1)
{
    if (emulate > 0)
        return true;
    if (emulate == 0)
        return false;

    // 'emulate' < 0 means automatically detect whether emulation is needed.

    if (!IsWindows10OrGreater())
        return true;

#pragma warning(push)
#pragma warning(disable:4996)
    OSVERSIONINFO ver = { sizeof(ver) };
    if (!GetVersionEx(&ver))
        return true;
#pragma warning(pop)

    return (ver.dwMajorVersion == 10 && ver.dwBuildNumber < 15063);
}

class ScopedEnterCritSec
{
public:
    ScopedEnterCritSec(CRITICAL_SECTION& cs) : m_cs(cs) { EnterCriticalSection(&m_cs); }
    ~ScopedEnterCritSec() { LeaveCriticalSection(&m_cs); }
private:
    CRITICAL_SECTION& m_cs;
};

#pragma region Attributes

class attributes
{
public:
    struct color
    {
        union
        {
            struct
            {
                unsigned short  r : 5;
                unsigned short  g : 5;
                unsigned short  b : 5;
                unsigned short  is_rgb : 1;
            };
            unsigned short      value;
        };

        bool                    operator == (const color& rhs) const { return value == rhs.value; }
        void                    as_888(RGB_t& out) const;
    };

    template <typename T>
    struct attribute
    {
        explicit                operator bool () const  { return bool(set); }
        const T*                operator -> () const { return &value; }
        const T                 value;
        const uint8             set : 1;
        const uint8             is_default : 1;
    };

    enum default_e { defaults };

                                attributes();
                                attributes(default_e);
    bool                        operator == (const attributes rhs);
    bool                        operator != (const attributes rhs) { return !(*this == rhs); }
    static attributes           merge(const attributes first, const attributes second);
    static attributes           diff(const attributes from, const attributes to);
    void                        reset_fg();
    void                        reset_bg();
    void                        set_fg(uint8 value);
    void                        set_bg(uint8 value);
    void                        set_fg(uint8 r, uint8 g, uint8 b);
    void                        set_bg(uint8 r, uint8 g, uint8 b);
    void                        set_bold(bool state=true);
    void                        set_underline(bool state=true);
    void                        set_reverse(bool state=true);
    attribute<color>            get_fg() const;
    attribute<color>            get_bg() const;
    attribute<bool>             get_bold() const;
    attribute<bool>             get_underline() const;
    attribute<bool>             get_reverse() const;

private:
    union flags
    {
        struct
        {
            uint8               fg : 1;
            uint8               bg : 1;
            uint8               bold : 1;
            uint8               underline : 1;
            uint8               reverse : 1;
        };
        uint8                   all;
    };

    union
    {
        struct
        {
            color               m_fg;
            color               m_bg;
            unsigned short      m_bold : 1;
            unsigned short      m_underline : 1;
            unsigned short      m_reverse : 1;
            flags               m_flags;
            uint8               m_unused;
        };
        uint64                  m_state;
    };
};

enum
{
    default_code = 231, // because xterm256's 231 == old-school color 15 (white)
};

void attributes::color::as_888(RGB_t& out) const
{
    out.r = (r << 3) | (r & 7);
    out.g = (g << 3) | (g & 7);
    out.b = (b << 3) | (b & 7);
}

attributes::attributes()
: m_state(0)
{
}

attributes::attributes(default_e)
: attributes()
{
    reset_fg();
    reset_bg();
    set_bold(false);
    set_underline(false);
    set_reverse(false);
}

bool attributes::operator == (const attributes rhs)
{
    int32 cmp = 1;
    #define CMP_IMPL(x) (m_flags.x & rhs.m_flags.x) ? (m_##x == rhs.m_##x) : 1;
    cmp &= CMP_IMPL(fg);
    cmp &= CMP_IMPL(bg);
    cmp &= CMP_IMPL(bold);
    cmp &= CMP_IMPL(underline);
    cmp &= CMP_IMPL(reverse);
    #undef CMP_IMPL
    return (cmp != 0);
}

attributes attributes::merge(const attributes first, const attributes second)
{
    attributes mask;
    mask.m_flags.all = ~0;
    mask.m_fg.value = second.m_flags.fg ? ~0 : 0;
    mask.m_bg.value = second.m_flags.bg ? ~0 : 0;
    mask.m_bold = second.m_flags.bold;
    mask.m_underline = second.m_flags.underline;
    mask.m_reverse = second.m_flags.reverse;

    attributes out;
    out.m_state = first.m_state & ~mask.m_state;
    out.m_state |= second.m_state & mask.m_state;
    out.m_flags.all |= first.m_flags.all;

    return out;
}

attributes attributes::diff(const attributes from, const attributes to)
{
    flags changed;
    changed.fg = !(to.m_fg == from.m_fg);
    changed.bg = !(to.m_bg == from.m_bg);
    changed.bold = (to.m_bold != from.m_bold);
    changed.underline = (to.m_underline != from.m_underline);
    changed.reverse = (to.m_reverse != from.m_reverse);

    attributes out = to;
    out.m_flags.all &= changed.all;
    return out;
}

void attributes::reset_fg()
{
    m_flags.fg = 1;
    m_fg.value = default_code;
}

void attributes::reset_bg()
{
    m_flags.bg = 1;
    m_bg.value = default_code;
}

void attributes::set_fg(uint8 value)
{
    if (value == default_code)
        value = 15;

    m_flags.fg = 1;
    m_fg.value = value;
}

void attributes::set_bg(uint8 value)
{
    if (value == default_code)
        value = 15;

    m_flags.bg = 1;
    m_bg.value = value;
}

void attributes::set_fg(uint8 r, uint8 g, uint8 b)
{
    m_flags.fg = 1;
    m_fg.r = r >> 3;
    m_fg.g = g >> 3;
    m_fg.b = b >> 3;
    m_fg.is_rgb = 1;
}

void attributes::set_bg(uint8 r, uint8 g, uint8 b)
{
    m_flags.bg = 1;
    m_bg.r = r >> 3;
    m_bg.g = g >> 3;
    m_bg.b = b >> 3;
    m_bg.is_rgb = 1;
}

void attributes::set_bold(bool state)
{
    m_flags.bold = 1;
    m_bold = !!state;
}

void attributes::set_underline(bool state)
{
    m_flags.underline = 1;
    m_underline = !!state;
}

void attributes::set_reverse(bool state)
{
    m_flags.reverse = 1;
    m_reverse = !!state;
}

attributes::attribute<attributes::color> attributes::get_fg() const
{
    return { m_fg, m_flags.fg, (m_fg.value == default_code) };
}

attributes::attribute<attributes::color> attributes::get_bg() const
{
    return { m_bg, m_flags.bg, (m_bg.value == default_code) };
}

attributes::attribute<bool> attributes::get_bold() const
{
    return { bool(m_bold), bool(m_flags.bold) };
}

attributes::attribute<bool> attributes::get_underline() const
{
    return { bool(m_underline), bool(m_flags.underline) };
}

attributes::attribute<bool> attributes::get_reverse() const
{
    return { bool(m_reverse), bool(m_flags.reverse) };
}

#pragma endregion Attributes
#pragma region Palette

static bool find_best_palette_match(HANDLE hout, const RGB_t& rgb, uint8& attr)
{
    static HMODULE hmod = GetModuleHandle(L"kernel32.dll");
    static FARPROC proc = GetProcAddress(hmod, "GetConsoleScreenBufferInfoEx");
    typedef BOOL (WINAPI* GCSBIEx)(HANDLE, PCONSOLE_SCREEN_BUFFER_INFOEX);
    assert(proc);
    if (!proc)
        return false;

    CONSOLE_SCREEN_BUFFER_INFOEX infoex = { sizeof(infoex) };
    if (!GCSBIEx(proc)(hout, &infoex))
        return false;

    const RGB_t (*palette)[16] = reinterpret_cast<const RGB_t (*)[16]>(&infoex.ColorTable);
    const int32 best_idx = FindBestPaletteMatch(rgb, *palette);
    if (best_idx < 0)
        return false;

    static const int32 dos_to_ansi_order[] = { 0, 4, 2, 6, 1, 5, 3, 7 };
    attr = (best_idx & 0x08) + dos_to_ansi_order[best_idx & 0x07];
    return true;
}

static bool find_best_palette_match(HANDLE hout, attributes& attr)
{
    const attributes::color fg = attr.get_fg().value;
    const attributes::color bg = attr.get_bg().value;
    if (fg.is_rgb)
    {
        uint8 val;
        RGB_t rgb;
        fg.as_888(rgb);
        if (!find_best_palette_match(hout, rgb, val))
            return false;
        attr.set_fg(val);
    }
    if (bg.is_rgb)
    {
        uint8 val;
        RGB_t rgb;
        bg.as_888(rgb);
        if (!find_best_palette_match(hout, rgb, val))
            return false;
        attr.set_bg(val);
    }
    return true;
}

#pragma endregion Palette
#pragma region Terminal

#define SCOPED_LOCK ScopedEnterCritSec scoped_lock(m_cs)

Terminal::Terminal(int emulate)
: m_hout(GetStdHandle(STD_OUTPUT_HANDLE))
, m_emulate(IsEmulationNeeded(emulate))
{
    InitializeCriticalSection(&m_cs);
}

Terminal::~Terminal()
{
    DeleteCriticalSection(&m_cs);
}

void Terminal::SetEmulation(int emulate)
{
    m_emulate = IsEmulationNeeded(emulate);
    m_screen_buffer = std::move(std::vector<CHAR_INFO>());
}

void Terminal::WriteConsole(const WCHAR* chars, unsigned length)
{
    SCOPED_LOCK;

    if (m_emulate)
    {
        int32 need_next = (length == 1 || (chars[0] && !chars[1]));
        ecma48_iter iter(chars, m_state, length);
        while (const ecma48_code& code = iter.next())
        {
            switch (code.get_type())
            {
            case ecma48_code::type_chars:
                do_write(code.get_pointer(), code.get_length());
                break;

            case ecma48_code::type_c0:
                write_c0(code.get_code());
                break;

            case ecma48_code::type_c1:
                write_c1(code);
                break;

            case ecma48_code::type_icf:
                write_icf(code);
                break;
            }
        }
    }
    else
    {
        do_write(chars, length);
    }
}

#pragma region Emulation Methods

void Terminal::write_c1(const ecma48_code& code)
{
    if (code.get_code() == ecma48_code::c1_csi)
    {
        ecma48_code::csi<32> csi;
        code.decode_csi(csi);

        if (csi.private_use)
        {
            switch (csi.final)
            {
            case 'h': set_private_mode(csi);    break;
            case 'l': reset_private_mode(csi);  break;
            }
        }
        else
        {
            switch (csi.final)
            {
            case '@': insert_chars(csi);        break;
            case 'G': set_horiz_cursor(csi);    break;
            case 'H': set_cursor(csi);          break;
            case 'J': erase_in_display(csi);    break;
            case 'K': erase_in_line(csi);       break;
            case 'P': delete_chars(csi);        break;
            case 'm': set_attributes(csi);      break;
            case 's': save_cursor();            break;
            case 'u': restore_cursor();         break;

            case 'A': do_move_cursor(0, -csi.get_param(0, 1)); break;
            case 'B': do_move_cursor(0,  csi.get_param(0, 1)); break;
            case 'C': do_move_cursor( csi.get_param(0, 1), 0); break;
            case 'D': do_move_cursor(-csi.get_param(0, 1), 0); break;
            }
        }
    }
#if 0
    else if (code.get_code() == ecma48_code::c1_osc)
    {
        ecma48_code::osc osc;
        if (code.decode_osc(osc))
        {
            switch (osc.command)
            {
            case '9':
                if (osc.output.length())
                    write(osc.output.c_str(), osc.output.length());
                break;
            }
        }
    }
#endif
}

void Terminal::write_c0(int32 c0)
{
    switch (c0)
    {
    case ecma48_code::c0_bel:
        MessageBeep(0xffffffff);
        break;

    case ecma48_code::c0_bs:
        do_move_cursor(-1, 0);
        break;

    case ecma48_code::c0_cr:
        do_move_cursor(INT_MIN, 0);
        break;

    case ecma48_code::c0_ht:
    case ecma48_code::c0_lf:
        {
            WCHAR c = char(c0);
            do_write(&c, 1);
            break;
        }
    }
}

void Terminal::write_icf(const ecma48_code& code)
{
    if (code.get_code() == ecma48_code::icf_vb)
    {
#if 0
        do_visible_bell();
#endif
    }
}

void Terminal::set_attributes(const ecma48_code::csi_base& csi)
{
    // Empty parameters to 'CSI SGR' implies 0 (reset).
    if (csi.param_count == 0)
        return do_set_attributes(attributes::defaults);

    // Process each code that is supported.
    attributes attr;
    for (int32 i = 0, n = csi.param_count; i < csi.param_count; ++i, --n)
    {
        uint32 param = csi.params[i];

        switch (param)
        {
        // Resets.
        case 0:     attr = attributes::defaults; break;
        case 49:    attr.reset_bg(); break;
        case 39:    attr.reset_fg(); break;

        // Bold.
        case 1:
        case 2:
        case 22:
            attr.set_bold(param == 1);
            break;

        // Underline.
        case 4:
        case 24:
            attr.set_underline(param == 4);
            break;

        // Foreground colors.
        case 30:    case 90:
        case 31:    case 91:
        case 32:    case 92:
        case 33:    case 93:
        case 34:    case 94:
        case 35:    case 95:
        case 36:    case 96:
        case 37:    case 97:
            param += (param >= 90) ? 14 : 2;
            attr.set_fg(param & 0x0f);
            break;

        // Background colors.
        case 40:    case 100:
        case 41:    case 101:
        case 42:    case 102:
        case 43:    case 103:
        case 44:    case 104:
        case 45:    case 105:
        case 46:    case 106:
        case 47:    case 107:
            param += (param >= 100) ? 4 : 8;
            attr.set_bg(param & 0x0f);
            break;

        // Reverse.
        case 7:
        case 27:
            attr.set_reverse(param == 7);
            break;

        // Xterm extended color support.
        case 38:
        case 48:
            if (n > 1)
            {
                i++;
                n--;
                bool is_fg = (param == 38);
                uint32 type = csi.params[i];
                if (type == 2)
                {
                    // RGB 24-bit color
                    if (n > 3)
                    {
                        if (is_fg)
                            attr.set_fg(csi.params[i + 1], csi.params[i + 2], csi.params[i + 3]);
                        else
                            attr.set_bg(csi.params[i + 1], csi.params[i + 2], csi.params[i + 3]);
                    }
                    i += 3;
                    n -= 3;
                }
                else if (type == 5)
                {
                    // XTerm256 color
                    if (n > 1)
                    {
                        uint8 idx = csi.params[i + 1];
                        if (idx < 16)
                        {
                            if (is_fg)
                                attr.set_fg(idx);
                            else
                                attr.set_bg(idx);
                        }
                        else if (idx >= 232)
                        {
                            uint8 gray = 0x08 + (int32(idx) - 232) * 10;
                            if (is_fg)
                                attr.set_fg(gray, gray, gray);
                            else
                                attr.set_bg(gray, gray, gray);
                        }
                        else
                        {
                            idx -= 16;
                            uint8 b = idx % 6;
                            idx /= 6;
                            uint8 g = idx % 6;
                            idx /= 6;
                            uint8 r = idx;
                            if (is_fg)
                                attr.set_fg(s_rgb_cube[r], s_rgb_cube[g], s_rgb_cube[b]);
                            else
                                attr.set_bg(s_rgb_cube[r], s_rgb_cube[g], s_rgb_cube[b]);
                        }
                    }
                    i++;
                    n--;
                }
            }
            break;
        }
    }

    do_set_attributes(attr);
}

void Terminal::erase_in_display(const ecma48_code::csi_base& csi)
{
    /* CSI ? Ps J : Erase in Display (DECSED).
            Ps = 0  -> Selective Erase Below (default).
            Ps = 1  -> Selective Erase Above.
            Ps = 2  -> Selective Erase All.
            Ps = 3  -> Selective Erase Saved Lines (xterm). */
    switch (csi.get_param(0))
    {
    case 0: return do_clear(clear::below);
    case 1: return do_clear(clear::above);
    case 2: return do_clear(clear::all);
    }
}

void Terminal::erase_in_line(const ecma48_code::csi_base& csi)
{
    /* CSI Ps K : Erase in Line (EL).
            Ps = 0  -> Erase to Right (default).
            Ps = 1  -> Erase to Left.
            Ps = 2  -> Erase All. */
    switch (csi.get_param(0))
    {
    case 0: return do_clear_line(clear_line::right);
    case 1: return do_clear_line(clear_line::left);
    case 2: return do_clear_line(clear_line::all);
    }
}

void Terminal::set_horiz_cursor(const ecma48_code::csi_base& csi)
{
    /* CSI Ps G : Cursor Horizontal Absolute [column] (default = 1) (CHA). */
    int32 column = csi.get_param(0, 1);
    do_set_horiz_cursor(column - 1);
}

void Terminal::set_cursor(const ecma48_code::csi_base& csi)
{
    /* CSI Ps ; Ps H : Cursor Position [row;column] (default = [1,1]) (CUP). */
    int32 row = csi.get_param(0, 1);
    int32 column = csi.get_param(1, 1);
    do_set_cursor(column - 1, row - 1);
}

void Terminal::save_cursor()
{
    /* CSI s : Save Current Cursor Position (SCP, SCOSC). */
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (GetConsoleScreenBufferInfo(m_hout, &csbi))
    {
        const SMALL_RECT& window = csbi.srWindow;
        int32 width = (window.Right - window.Left) + 1;
        int32 height = (window.Bottom - window.Top) + 1;

        m_saved_cursor.X = clamp(csbi.dwCursorPosition.X - window.Left, 0, width);
        m_saved_cursor.Y = clamp(csbi.dwCursorPosition.Y - window.Top, 0, height);
    }
}

void Terminal::restore_cursor()
{
    /* CSI u : Restore Saved Cursor Position (RCP, SCORC). */
    if (m_saved_cursor.X >= 0 && m_saved_cursor.Y >= 0)
        do_set_cursor(m_saved_cursor.X, m_saved_cursor.Y);
}

void Terminal::insert_chars(const ecma48_code::csi_base& csi)
{
    /* CSI Ps @  Insert Ps (Blank) Character(s) (default = 1) (ICH). */
    int32 count = csi.get_param(0, 1);
    do_insert_chars(count);
}

void Terminal::delete_chars(const ecma48_code::csi_base& csi)
{
    /* CSI Ps P : Delete Ps Character(s) (default = 1) (DCH). */
    int32 count = csi.get_param(0, 1);
    do_delete_chars(count);
}

void Terminal::set_private_mode(const ecma48_code::csi_base& csi)
{
    /* CSI ? Pm h : DEC Private Mode Set (DECSET).
            Ps = 5  -> Reverse Video (DECSCNM).
            Ps = 12 -> Start Blinking Cursor (att610).
            Ps = 25 -> Show Cursor (DECTCEM). */
    for (int32 i = 0; i < csi.param_count; ++i)
    {
        switch (csi.params[i])
        {
        case 25:
            do_cursor_style(-1, 1);
            break;
        case 1049:
            do_alternate_screen(true);
            break;
        }
    }
}

void Terminal::reset_private_mode(const ecma48_code::csi_base& csi)
{
    /* CSI ? Pm l : DEC Private Mode Reset (DECRST).
            Ps = 5  -> Normal Video (DECSCNM).
            Ps = 12 -> Stop Blinking Cursor (att610).
            Ps = 25 -> Hide Cursor (DECTCEM). */
    for (int32 i = 0; i < csi.param_count; ++i)
    {
        switch (csi.params[i])
        {
        case 25:
            do_cursor_style(-1, 0);
            break;
        case 1049:
            do_alternate_screen(false);
            break;
        }
    }
}

#pragma endregion Emulation Methods
#pragma region Screen Methods

void Terminal::do_write(const WCHAR* chars, unsigned length)
{
    DWORD written;
    WriteConsoleW(m_hout, chars, length, &written, nullptr);
}

bool Terminal::do_cursor_style(int /*style*/, int visible)
{
    CONSOLE_CURSOR_INFO ci;
    GetConsoleCursorInfo(m_hout, &ci);
    const bool was_visible = !!ci.bVisible;

    if (visible < 0)
        return was_visible;

    if (visible >= 0)
        ci.bVisible = !!visible;

    SetConsoleCursorInfo(m_hout, &ci);

    return was_visible;
}

bool Terminal::do_alternate_screen(bool alternate)
{
    const bool was_alternate = m_alternate_screen;

    if (was_alternate == alternate)
        return was_alternate;

    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (!GetConsoleScreenBufferInfo(m_hout, &csbi))
        return was_alternate;

    const COORD origin = {};

    // Get current screen info.
    const DWORD capacity = csbi.dwSize.X * csbi.dwSize.Y;
    std::vector<CHAR_INFO> buffer;
    try
    {
        buffer.reserve(capacity);
    } catch (...)
    {
        SMALL_RECT region = {};
        region.Right = csbi.dwSize.X - 1;
        region.Bottom = csbi.dwSize.Y - 1;
        ReadConsoleOutputW(m_hout, &*buffer.begin(), csbi.dwSize, origin, &region);
    }

    // Apply screen info for the screen being activated.
    if (m_screen_buffer.empty())
    {
        do_clear(clear::all);
    }
    else
    {
        SMALL_RECT region = {};
        region.Right = csbi.dwSize.X - 1;
        region.Bottom = csbi.dwSize.Y - 1;
        WriteConsoleOutput(m_hout, &*m_screen_buffer.begin(), m_screen_dimensions, origin, &region);
        m_screen_buffer.clear();
    }
    SetConsoleCursorPosition(m_hout, m_screen_cursor);

    // Remember the screen info.
    m_screen_buffer = std::move(buffer);
    m_screen_dimensions = csbi.dwSize;
    m_screen_cursor = csbi.dwCursorPosition;

    m_alternate_screen = alternate;

    return was_alternate;
}

void Terminal::do_set_cursor(int column, int row)
{
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (!GetConsoleScreenBufferInfo(m_hout, &csbi))
        return;

    const SMALL_RECT& window = csbi.srWindow;
    int width = (window.Right - window.Left) + 1;
    int height = (window.Bottom - window.Top) + 1;

    column = clamp(column, 0, width - 1);
    row = clamp(row, 0, height - 1);

    COORD xy = { short(window.Left + column), short(window.Top + row) };
    SetConsoleCursorPosition(m_hout, xy);
}

void Terminal::do_set_horiz_cursor(int column)
{
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (!GetConsoleScreenBufferInfo(m_hout, &csbi))
        return;

    const SMALL_RECT& window = csbi.srWindow;
    int32 width = (window.Right - window.Left) + 1;
    int32 height = (window.Bottom - window.Top) + 1;

    column = clamp(column, 0, width - 1);

    COORD xy = { short(window.Left + column), csbi.dwCursorPosition.Y };
    SetConsoleCursorPosition(m_hout, xy);
}

void Terminal::do_move_cursor(int dx, int dy)
{
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (!GetConsoleScreenBufferInfo(m_hout, &csbi))
        return;

    COORD xy = {
        clamp<SHORT>(csbi.dwCursorPosition.X + dx, 0, csbi.dwSize.X - 1),
        clamp<SHORT>(csbi.dwCursorPosition.Y + dy, 0, csbi.dwSize.Y - 1),
    };
    SetConsoleCursorPosition(m_hout, xy);
}

void Terminal::do_clear(clear mode)
{
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (GetConsoleScreenBufferInfo(m_hout, &csbi))
    {
        int width, height, count = 0;
        COORD xy;

        switch (mode)
        {
        case clear::below:
            width = csbi.dwSize.X;
            height = csbi.srWindow.Bottom - csbi.dwCursorPosition.Y;
            xy = { csbi.dwCursorPosition.X, csbi.dwCursorPosition.Y };
            count = width - csbi.dwCursorPosition.X;
            break;
        case clear::above:
            width = csbi.dwSize.X;
            height = csbi.dwCursorPosition.Y - csbi.srWindow.Top;
            xy = { 0, csbi.srWindow.Top };
            count = csbi.dwCursorPosition.X + 1;
            break;
        case clear::all:
            width = csbi.dwSize.X;
            height = (csbi.srWindow.Bottom - csbi.srWindow.Top) + 1;
            xy = { 0, csbi.srWindow.Top };
            break;
        }

        count += width * height;

        DWORD written;
        FillConsoleOutputCharacterW(m_hout, ' ', count, xy, &written);
        FillConsoleOutputAttribute(m_hout, csbi.wAttributes, count, xy, &written);
    }
}

void Terminal::do_clear_line(clear_line mode)
{
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (GetConsoleScreenBufferInfo(m_hout, &csbi))
    {
        int width;
        COORD xy;
        switch (mode)
        {
        case clear_line::right:
            width = csbi.dwSize.X - csbi.dwCursorPosition.X;
            xy = { csbi.dwCursorPosition.X, csbi.dwCursorPosition.Y };
            break;
        case clear_line::left:
            width = csbi.dwCursorPosition.X + 1;
            xy = { 0, csbi.dwCursorPosition.Y };
            break;
        case clear_line::all:
            width = csbi.dwSize.X;
            xy = { 0, csbi.dwCursorPosition.Y };
            break;
        }

        DWORD written;
        FillConsoleOutputCharacterW(m_hout, ' ', width, xy, &written);
        FillConsoleOutputAttribute(m_hout, csbi.wAttributes, width, xy, &written);
    }
}

void Terminal::do_insert_chars(int count)
{
    if (count <= 0)
        return;

    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (!GetConsoleScreenBufferInfo(m_hout, &csbi))
        return;

    SMALL_RECT rect;
    rect.Left = csbi.dwCursorPosition.X;
    rect.Right = csbi.dwSize.X;
    rect.Top = rect.Bottom = csbi.dwCursorPosition.Y;

    CHAR_INFO fill;
    fill.Char.AsciiChar = ' ';
    fill.Attributes = csbi.wAttributes;

    csbi.dwCursorPosition.X += count;

    ScrollConsoleScreenBuffer(m_hout, &rect, NULL, csbi.dwCursorPosition, &fill);
}

void Terminal::do_delete_chars(int count)
{
    if (count <= 0)
        return;

    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (!GetConsoleScreenBufferInfo(m_hout, &csbi))
        return;

    SMALL_RECT rect;
    rect.Left = csbi.dwCursorPosition.X + count;
    rect.Right = csbi.dwSize.X - 1;
    rect.Top = rect.Bottom = csbi.dwCursorPosition.Y;

    CHAR_INFO fill;
    fill.Char.AsciiChar = ' ';
    fill.Attributes = csbi.wAttributes;

    ScrollConsoleScreenBuffer(m_hout, &rect, NULL, csbi.dwCursorPosition, &fill);

    int32 chars_moved = rect.Right - rect.Left + 1;
    if (chars_moved < count)
    {
        COORD xy = csbi.dwCursorPosition;
        xy.X += chars_moved;

        count -= chars_moved;

        DWORD written;
        FillConsoleOutputCharacterW(m_hout, ' ', count, xy, &written);
        FillConsoleOutputAttribute(m_hout, csbi.wAttributes, count, xy, &written);
    }
}

void Terminal::do_set_attributes(attributes attr)
{
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (!GetConsoleScreenBufferInfo(m_hout, &csbi))
        return;

    int32 out_attr = csbi.wAttributes & attr_mask_all;

    // Un-reverse so processing can operate on normalized attributes.
    if (m_reverse)
    {
        int32 fg = (out_attr & ~attr_mask_bg);
        int32 bg = (out_attr & attr_mask_bg);
        out_attr = (fg << 4) | (bg >> 4);
    }

    // Note to self; lookup table is probably much faster.
    auto swizzle = [] (int32 rgbi) {
        int32 b_r_ = ((rgbi & 0x01) << 2) | !!(rgbi & 0x04);
        return (rgbi & 0x0a) | b_r_;
    };

    // Map RGB/XTerm256 colors
    if (!find_best_palette_match(m_hout, attr))
        return;

    // Bold
    bool apply_bold = false;
    if (auto bold_attr = attr.get_bold())
    {
        m_bold = !!(bold_attr.value);
        apply_bold = true;
    }

    // Underline
    if (auto underline = attr.get_underline())
    {
        if (underline.value)
            out_attr |= attr_mask_underline;
        else
            out_attr &= ~attr_mask_underline;
    }

    // Foreground color
    bool bold = m_bold;
    if (auto fg = attr.get_fg())
    {
        int32 value = fg.is_default ? m_default_attr : swizzle(fg->value);
        value &= attr_mask_fg;
        out_attr = (out_attr & ~attr_mask_fg) | value;
        bold |= (value > 7);
    }
    else
        bold |= (out_attr & attr_mask_bold) != 0;

    // Adjust intensity per bold.  Bold can add intensity.  Nobold can remove
    // intensity added by bold, but cannot remove intensity built into the
    // color number.
    //
    // In other words:
    //  - If the color is 36 (cyan) then bold can make it bright cyan.
    //  - If the color is 36 (cyan) then nobold has no visible effect.
    //  - If the color is 1;36 (bold cyan) then nobold can make it cyan.
    //  - If the color is 96 (bright cyan) then bold has no visible effect (but
    //    some terminal implementations apply a bold font with bright cyan as
    //    the color).
    //  - If the color is 96 (bright cyan) then nobold has no visible effect.
    //  - If the color is 1;96 (bold bright cyan) then nobold has no visible
    //    effect (but some terminal implementations apply a non-bold font with
    //    bright cyan as the color).
    if (apply_bold)
    {
        if (bold)
            out_attr |= attr_mask_bold;
        else
            out_attr &= ~attr_mask_bold;
    }

    // Background color
    if (auto bg = attr.get_bg())
    {
        int32 value = bg.is_default ? m_default_attr : (swizzle(bg->value) << 4);
        out_attr = (out_attr & ~attr_mask_bg) | (value & attr_mask_bg);
    }

    // Reverse video
    if (auto rev = attr.get_reverse())
        m_reverse = rev.value;

    // Apply reverse video
    if (m_reverse)
    {
        int32 fg = (out_attr & ~attr_mask_bg);
        int32 bg = (out_attr & attr_mask_bg);
        out_attr = (fg << 4) | (bg >> 4);
    }

    out_attr |= csbi.wAttributes & ~attr_mask_all;
    SetConsoleTextAttribute(m_hout, short(out_attr));
}

#pragma endregion Screen Methods

#pragma endregion Terminal

#endif