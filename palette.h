// Copyright (c) 2025 by Christopher Antos
// License: http://opensource.org/licenses/MIT

// vim: set et ts=4 sw=4 cino={0s:

#pragma once

struct RGB_t
{
    unsigned char r;
    unsigned char g;
    unsigned char b;
    unsigned char unused;  // Pad RGB to allow reinterpret_cast from COLORREF.
};

// Find the best palette match using weighted ΔE2000 + hue-based finalist selection.
int FindBestPaletteMatch(const RGB_t& input, const RGB_t (&palette)[16]);
