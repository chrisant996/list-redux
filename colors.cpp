﻿// Copyright (c) 2024 by Christopher Antos
// License: http://opensource.org/licenses/MIT

// https://github.com/eza-community/eza
// Formula for adjusting luminance copied from eza.

// vim: set et ts=4 sw=4 cino={0s:

#include "pch.h"
#include "colors.h"
#include "fileinfo.h"
#include "filesys.h"
#include "sorting.h"
#include "output.h"

#include <math.h>
#include <unordered_map>
#include <cmath>

extern const WCHAR c_norm[] = L"\x1b[m";
extern const WCHAR c_error[] = L"1;91";

static double s_min_luminance = 0.4;
static double s_hidden_opacity = 0.0;
static bool s_light_theme = false;

struct ColorDefinition
{
    const WCHAR* back;
    const WCHAR* text;
};

static const ColorDefinition c_colors[] =
{   //  Back            Text
    {   L"",            L"97" },        // File
    {   L"48;5;23",     L"97" },        // Selected
    {   L"",            L"36" },        // Tagged
    {   L"48;5;23",     L"30" },        // SelectedTagged
    {   L"",            L"93" },        // Command
    {   L"",            L"90" },        // Divider
    {   L"",            L"" },          // Content
    {   L"",            L"33" },        // CtrlCode
    {   L"",            L"7" },         // EndOfFileLine
    {   L"",            L"7" },         // MarkedLine
    {   L"",            L"7;36" },      // SearchFound
    {   L"",            L"7;36" },      // DebugRow
};
static_assert(_countof(c_colors) == size_t(ColorElement::MAX));

const WCHAR* GetBackColorParams(ColorElement element)
{
    return c_colors[size_t(element)].back;
}

const WCHAR* GetTextColorParams(ColorElement element)
{
    return c_colors[size_t(element)].text;
}

const WCHAR* GetColor(ColorElement element)
{
    static WCHAR s_tmp[size_t(ColorElement::MAX)][32] = {};

    if (!s_tmp[size_t(element)][0])
    {
        const ColorDefinition& def = c_colors[size_t(element)];
        const WCHAR* back = def.back;
        const WCHAR* text = def.text;
        WCHAR* p = s_tmp[size_t(element)];
        if (*back)
        {
            for (const WCHAR* q = back; *q; ++q)
                *(p++) = *q;
        }
        if (*text)
        {
            if (*back)
                *(p++) = ';';
            for (const WCHAR* q = text; *q; ++q)
                *(p++) = *q;
        }
        *(p++) = '\0';
    }

    return s_tmp[size_t(element)];
}

/*
 * Color manipulations.
 */

static COLORREF RgbFromColorTable(BYTE value)
{
    static CONSOLE_SCREEN_BUFFER_INFOEX s_infoex;
    static bool s_have = false;

    static const BYTE c_ansi_to_vga[] =
    {
        0,  4,  2,  6,  1,  5,  3,  7,
        8, 12, 10, 14,  9, 13, 11, 15,
    };

    if (!s_have)
    {
        COLORREF table[] =
        {
            // Windows Terminal doesn't implement GetConsoleScreenBufferInfoEx
            // yet, and returns a default table instead.  But it can return a
            // version of the default table with the R and B values swapped.
            RGB(0x0c, 0x0c, 0x0c),
            RGB(0xda, 0x37, 0x00),
            RGB(0x0e, 0xa1, 0x13),
            RGB(0xdd, 0x96, 0x3a),
            RGB(0x1f, 0x0f, 0xc5),
            RGB(0x98, 0x17, 0x88),
            RGB(0x00, 0x9c, 0xc1),
            RGB(0xcc, 0xcc, 0xcc),
            RGB(0x76, 0x76, 0x76),
            RGB(0xff, 0x78, 0x3b),
            RGB(0x0c, 0xc6, 0x16),
            RGB(0xd6, 0xd6, 0x61),
            RGB(0x56, 0x48, 0xe7),
            RGB(0x9e, 0x00, 0xb4),
            RGB(0xa5, 0xf1, 0xf9),
            RGB(0xf2, 0xf2, 0xf2),
        };

        s_infoex.cbSize = sizeof(s_infoex);
        if (!GetConsoleScreenBufferInfoEx(GetStdHandle(STD_OUTPUT_HANDLE), &s_infoex))
        {
            static_assert(sizeof(s_infoex.ColorTable) == sizeof(table), "table sizes do not match!");
            memcpy(s_infoex.ColorTable, table, sizeof(s_infoex.ColorTable));
            for (auto& rgb : s_infoex.ColorTable)
                rgb = RGB(GetBValue(rgb), GetGValue(rgb), GetRValue(rgb));
            s_infoex.wAttributes = 0x07;
        }
        else if (memcmp(s_infoex.ColorTable, table, sizeof(s_infoex.ColorTable)) == 0)
        {
            for (auto& rgb : s_infoex.ColorTable)
                rgb = RGB(GetBValue(rgb), GetGValue(rgb), GetRValue(rgb));
        }

        s_have = true;
    }

    if (value == 49)
        value = (s_infoex.wAttributes & 0xf0) >> 4;
    else if (value < 0 || value >= 16)
        value = s_infoex.wAttributes & 0x0f;
    else
        value = c_ansi_to_vga[value];
    return s_infoex.ColorTable[value];
}

static bool ParseNum(const WCHAR*& p, DWORD& num)
{
    num = 0;
    while (*p && *p != ';')
    {
        if (!iswdigit(*p))
            return false;
        num *= 10;
        num += *p - '0';
        if (num >= 256)
            return false;
        ++p;
    }
    return true;
}

int HasBackgroundColor(const WCHAR* p)
{
    // NOTE:  The caller is responsible for stripping leading/trailing spaces.

    if (!p || !*p)      return false;   // nullptr or "" is "has no color specified".

    if (p[0] == '0')
    {
        if (!p[1])      return false;   // "0" is default.
        if (p[1] == '0')
        {
            if (!p[2])  return false;   // "00" is default.
        }
    }

    enum { ST_NORMAL, ST_BYTES1, ST_BYTES2, ST_BYTES3, ST_XCOLOR };
    int state = ST_NORMAL;
    bool bk = false;

    // Validate recognized color/style escape codes.
    for (unsigned num = 0; true; ++p)
    {
        if (*p == ';' || !*p)
        {
            if (state == ST_NORMAL)
            {
                switch (num)
                {
                case 0:     // reset or normal
                    bk = false;
                    break;
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
                /*case 40: case 41: case 42: case 43: case 44: case 45: case 46: case 47:*/ case 49:
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
                // case 100: case 101: case 102: case 103: case 104: case 105: case 106: case 107:
                    break;
                case 38:    // set foreground color
                // case 48:    // set background color
                // case 58:    // set underline color
                    state = ST_XCOLOR;
                    break;
                case 40: case 41: case 42: case 43: case 44: case 45: case 46: case 47:
                case 100: case 101: case 102: case 103: case 104: case 105: case 106: case 107:
                case 48:
                    bk = true;
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
            return bk; // Return whether background color was found.

        if (*p >= '0' && *p <= '9')
        {
            num *= 10;
            num += *p - '0';
        }
        else if (*p != ';')
            return -1; // Unsupported or invalid SGR code.
    }
}

enum class RgbFromColorMode { Foreground, PreferBackground, Background, BackgroundNotDefault };

static COLORREF RgbFromColor(const WCHAR* color, RgbFromColorMode mode=RgbFromColorMode::Foreground)
{
    static const BYTE c_cube_series[] = { 0x00, 0x5f, 0x87, 0xaf, 0xd7, 0xff };

    unsigned format = 0;    // 5=8-bit, 2=24-bit, 0=30..37,39
    DWORD value = 39;
    bool bold = false;
    bool bg = false;

    bool start = true;
    int num = 0;
    for (const WCHAR* p = color; true; ++p)
    {
        if (!*p || *p == ';')
        {
            switch (num)
            {
            case 0:
                format = 0;
                value = 39;
                bold = false;
                break;
            case 1:
                bold = true;
                break;
            case 22:
                bold = false;
                break;
            case 90: case 91: case 92: case 93: case 94: case 95: case 96: case 97:
            case 30: case 31: case 32: case 33: case 34: case 35: case 36: case 37:
            case 39:
                if (mode < RgbFromColorMode::Background && !bg)
                {
                    format = 0;
                    value = num;
                }
                break;
            case 100: case 101: case 102: case 103: case 104: case 105: case 106: case 107:
            case 40: case 41: case 42: case 43: case 44: case 45: case 46: case 47:
            case 49:
                if (mode != RgbFromColorMode::Foreground)
                {
                    format = 0;
                    value = num;
                    bg = true;
                }
                break;
            }

            if (!*p)
                break;

            start = true;
            num = 0;
            continue;
        }

        if (start && ((wcsncmp(p, L"38;2;", 5) == 0) ||
                      (mode != RgbFromColorMode::Foreground && wcsncmp(p, L"48;2;", 5) == 0)))
        {
            bg = (*p == '4');
            p += 5;
            DWORD r, g, b;
            if (!ParseNum(p, r) || !*(p++))
                return 0xffffffff;
            if (!ParseNum(p, g) || !*(p++))
                return 0xffffffff;
            if (!ParseNum(p, b))
                return 0xffffffff;
            format = 2;
            value = RGB(BYTE(r), BYTE(g), BYTE(b));
            num = -1;
            --p; // Counteract the loop.
        }
        else if (start && ((wcsncmp(p, L"38;5;", 5) == 0) ||
                           (mode != RgbFromColorMode::Foreground && wcsncmp(p, L"48;2;", 5) == 0)))
        {
            bg = (*p == '4');
            p += 5;
            if (!ParseNum(p, value))
                return 0xffffffff;
            format = 5;
            num = -1;
            --p; // Counteract the loop.
        }
        else if (iswdigit(*p))
        {
            num *= 10;
            num += *p - '0';
        }
        else
            return 0xffffffff;

        start = false;
    }

    switch (format)
    {
    case 5:
        // 8-bit color.
        assert(value >= 0 && value <= 255);
        if (value <= 15)
            return RgbFromColorTable(BYTE(value));
        else if (value >= 232 && value <= 255)
        {
            const BYTE gray = 8 + ((BYTE(value) - 232) * 10);
            return RGB(gray, gray, gray);
        }
        else
        {
            value -= 16;
            const BYTE r = BYTE(value) / 36;
            value -= r * 36;
            const BYTE g = BYTE(value) / 6;
            value -= g * 6;
            const BYTE b = BYTE(value);
            return RGB(c_cube_series[r],
                       c_cube_series[g],
                       c_cube_series[b]);
        }
        break;

    case 2:
        // 24-bit color.
        return value;

    default:
        // 4-bit color.
        bg = (mode >= RgbFromColorMode::Background);
        if (value >= 30 && value <= 37)
            return bg ? 0xffffffff : RgbFromColorTable(BYTE(value) - 30 + (bold && !bg ? 8 : 0));
        else if (value >= 90 && value <= 97)
            return bg ? 0xffffffff : RgbFromColorTable(BYTE(value) - 90 + 8);
        else if (value == 39 || value == 49)
            return (bg && value != 49) ? 0xffffffff : RgbFromColorTable(BYTE(value));
        else if (value >= 40 && value <= 47)
            return RgbFromColorTable(BYTE(value) - 40 + (bold && !bg ? 8 : 0));
        else if (value >= 100 && value <= 107)
            return RgbFromColorTable(BYTE(value) - 100 + 8);
        assert(false);
        return 0xffffffff;
    }
}

namespace colorspace
{

// The Oklab code here is based on https://bottosson.github.io/posts/oklab, in
// the public domain (and also available under the MIT License).

struct Oklab
{
    Oklab() = default;
    Oklab(COLORREF cr) { from_rgb(cr); }

    void from_rgb(COLORREF cr);
    COLORREF to_rgb() const;

    float L = 0;
    float a = 0;
    float b = 0;

    inline static float rgb_to_linear(BYTE val)
    {
        float x = float(val) / 255.0f;
        return (x > 0.04045f) ? std::pow((x + 0.055f) / 1.055f, 2.4f) : (x / 12.92f);
    }

    inline static BYTE linear_to_rgb(float val)
    {
        float x = (val >= 0.0031308f) ? (1.055f * std::pow(val, 1.0f / 2.4f) - 0.055f) : (12.92f * val);
        return BYTE(clamp(int(x * 255), 0, 255));
    }
};

void Oklab::from_rgb(COLORREF cr)
{
    float _r = rgb_to_linear(GetRValue(cr));
    float _g = rgb_to_linear(GetGValue(cr));
    float _b = rgb_to_linear(GetBValue(cr));

    float l = 0.4122214708f * _r + 0.5363325363f * _g + 0.0514459929f * _b;
    float m = 0.2119034982f * _r + 0.6806995451f * _g + 0.1073969566f * _b;
    float s = 0.0883024619f * _r + 0.2817188376f * _g + 0.6299787005f * _b;

    l = std::cbrt(l);
    m = std::cbrt(m);
    s = std::cbrt(s);

    L = 0.2104542553f * l + 0.7936177850f * m - 0.0040720468f * s;
    a = 1.9779984951f * l - 2.4285922050f * m + 0.4505937099f * s;
    b = 0.0259040371f * l + 0.7827717662f * m - 0.8086757660f * s;
}

COLORREF Oklab::to_rgb() const
{
    float l = L + 0.3963377774f * a + 0.2158037573f * b;
    float m = L - 0.1055613458f * a - 0.0638541728f * b;
    float s = L - 0.0894841775f * a - 1.2914855480f * b;

    l = l * l * l;
    m = m * m * m;
    s = s * s * s;

    float _r = +4.0767416621f * l - 3.3077115913f * m + 0.2309699292f * s;
    float _g = -1.2684380046f * l + 2.6097574011f * m - 0.3413193965f * s;
    float _b = -0.0041960863f * l - 0.7034186147f * m + 1.7076147010f * s;

    return RGB(linear_to_rgb(_r), linear_to_rgb(_g), linear_to_rgb(_b));
}

}; // namespace colorspace

const WCHAR* ApplyGradient(const WCHAR* color, ULONGLONG value, ULONGLONG min, ULONGLONG max)
{
    assert(color);

    COLORREF rgb = RgbFromColor(color);
    if (rgb == 0xffffffff || min > max)
        return color;

    // This formula for applying a gradient effect is borrowed from eza.
    // https://github.com/eza-community/eza/blob/626eb34df26376fc36758894424676ffa4363785/src/output/color_scale.rs#L201-L213
    {
        colorspace::Oklab oklab(rgb);

        double ratio = double(value - min) / double(max - min);
        if (std::isnan(ratio))
            ratio = 1.0;
        oklab.L = float(clamp(s_min_luminance + (1.0 - s_min_luminance) * exp(-4.0 * (1.0 - ratio)), 0.0, 1.0));

        rgb = oklab.to_rgb();
    }

    static StrW s_color;
    s_color.Set(color);
    if (*color)
        s_color.Append(';');
    s_color.Printf(L"38;2;%u;%u;%u", GetRValue(rgb), GetGValue(rgb), GetBValue(rgb));
    return s_color.Text();
}

const WCHAR* StripLineStyles(const WCHAR* color)
{
    if (!color)
        return color;

    static StrW s_tmp;
    s_tmp.Clear();

    const WCHAR* start = color;
    unsigned num = 0;
    int skip = 0;
    bool any_stripped = false;
    for (const WCHAR* p = color; true; ++p)
    {
        if (!*p || *p == ';')
        {
            bool strip = false;

            if (skip < 0)
            {
                if (num == 2)
                    skip = 3;
                else if (num == 5)
                    skip = 1;
                else
                    skip = 0;
            }
            else if (skip > 0)
            {
                --skip;
            }
            else
            {
                switch (num)
                {
                case 4:     // Underline.
                case 9:     // Strikethrough.
                case 21:    // Double underline.
                case 53:    // Overline.
                    strip = true;
                    break;
                case 38:
                case 48:
                case 58:
                    skip = -1;
                    break;
                }
            }

            if (strip)
                any_stripped = true;
            else
                s_tmp.Append(start, unsigned(p - start));

            if (!*p)
                break;

            start = p;
            num = 0;
            continue;
        }

        if (iswdigit(*p))
        {
            num *= 10;
            num += *p - '0';
        }
        else
            return L"";
    }

    return any_stripped ? s_tmp.Text() : color;
}

void ReportColorlessError(Error& e)
{
    if (e.Test())
    {
        StrW tmp;
        e.Format(tmp);
        tmp.Append(L"\n");
        // Use normal text color instead of error color.
        OutputConsole(GetStdHandle(STD_ERROR_HANDLE), tmp.Text(), tmp.Length());
        e.Clear();
    }
}

void InitColors()
{
    Error e;

    COLORREF rgbBack = RgbFromColor(L"49", RgbFromColorMode::Background);
    s_light_theme = (rgbBack != 0xffffffff && colorspace::Oklab(rgbBack).L > 0.6);

    const WCHAR* env = _wgetenv(L"LIST_MIN_LUMINANCE");
    if (env)
    {
        const int x = clamp(_wtoi(env), -100, 100);
        s_min_luminance = double(x) / 100;
    }
}

inline BYTE BlendValue(BYTE a, BYTE b, BYTE alpha)
{
    return ((WORD(a) * alpha) + (WORD(b) * (255 - alpha))) / 255;
}

static const WCHAR* MaybeDim(const WCHAR* color)
{
    assert(s_hidden_opacity > 0);
    if (color && s_hidden_opacity > 0)
    {
        COLORREF rgb = RgbFromColor(color);
        COLORREF rgbBack = RgbFromColor(color, RgbFromColorMode::BackgroundNotDefault);
        if (rgb != 0xffffffff)
        {
            colorspace::Oklab oklab(rgb);
            if (rgbBack != 0xffffffff)
            {
                const BYTE alpha = BYTE(clamp<int>(int(s_hidden_opacity * 255), 0, 255));
                rgb = RGB(BlendValue(GetRValue(rgb), GetRValue(rgbBack), alpha),
                          BlendValue(GetGValue(rgb), GetGValue(rgbBack), alpha),
                          BlendValue(GetBValue(rgb), GetBValue(rgbBack), alpha));
            }
            else
            {
                if (s_light_theme)
                    oklab.L = float(clamp(oklab.L + ((1.0 - oklab.L) * (1.0 - s_hidden_opacity)), 0.0, 1.0));
                else
                    oklab.L = float(clamp(oklab.L * s_hidden_opacity, 0.0, 1.0));
                rgb = oklab.to_rgb();
            }

            static StrW s_color;
            s_color.Set(color);
            if (*color)
                s_color.Append(';');
            s_color.Printf(L"38;2;%u;%u;%u", GetRValue(rgb), GetGValue(rgb), GetBValue(rgb));
            return s_color.Text();
        }
    }
    return color;
}

