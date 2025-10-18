// Copyright (c) 2023,2025 Christopher Antos
// License: http://opensource.org/licenses/MIT

#include "pch.h"
#include "scroll_car.h"

//------------------------------------------------------------------------------
#ifdef USE_HALF_CHARS
constexpr int32 c_min_car_size = 2;
constexpr int32 c_scale_positions = 2;
template<typename T> inline T round(T pos) { return (pos & ~1); }
#else
constexpr int32 c_min_car_size = 1;
constexpr int32 c_scale_positions = 1;
#endif

//------------------------------------------------------------------------------
int32 calc_scroll_car_size(intptr_t rows, intptr_t total)
{
    if (rows <= 0 || rows >= total)
        return 0;

    const int32 car_size = int32(max<intptr_t>(c_min_car_size, min(c_scale_positions * rows, (c_scale_positions * rows * rows + (total / 2)) / total)));
    return car_size;
}

//------------------------------------------------------------------------------
int32 calc_scroll_car_offset(intptr_t top, int32 rows, intptr_t total, int32 car_size)
{
    if (car_size <= 0)
        return 0;

    const intptr_t car_positions = ((rows * c_scale_positions) + 1 - car_size);
    if (car_positions <= 0)
        return 0;

    const double per_car_position = double(total - rows) / double(car_positions);
    if (per_car_position <= 0)
        return 0;

    const int32 car_offset = min<int32>((rows * c_scale_positions) - car_size, int32(double(top) / per_car_position));
    return car_offset;
}

//------------------------------------------------------------------------------
const WCHAR* get_scroll_car_char(intptr_t row, intptr_t car_offset, int32 car_size, bool floating)
{
    if (car_size <= 0)
        return nullptr;

    row *= c_scale_positions;

#ifdef USE_HALF_CHARS
    int32 full_car_size = car_size;
    if (car_offset != round(car_offset))
        ++full_car_size;
    if (car_offset + car_size != round(car_offset + car_size))
        ++full_car_size;
    full_car_size = round(full_car_size + 1);

    if (row >= round(car_offset) && row < round(car_offset) + full_car_size)
    {
        static const WCHAR* const c_car_chars[] = {
            L"\u257d",                                                   // ╽
            L"\u2503",                                                   // ┃
            L"\u257f",                                                   // ╿
            L"\u2577",                                                   // ╷
            L"\u2502",                                                   // │
            L"\u2575",                                                   // ╵
        };

        int32 index;
        if (row == round(car_offset) && // This is the first cell of the scroll car...
            row != car_offset)          // ...and the first half is not part of the car.
        {
            index = 0;
        }
        else if (row == round(car_offset + car_size) && // This is the last cell of the scroll car...
                 row != car_offset + car_size)          // ...and the second half is not part of the car.
        {
            index = 2;
        }
        else
        {
            index = 1;
        }

        if (floating)
            index += 3;

        return c_car_chars[index];
    }
#else
    if (row >= car_offset && row < car_offset + car_size)
    {
        static const WCHAR* const c_car_chars[] = {
            L"\u2503",                                                   // ┃
            L"\u2502",                                                   // │
        };
        return c_car_chars[floating];
    }
#endif

    return nullptr;
}

//------------------------------------------------------------------------------
int32 hittest_scroll_car(intptr_t row, intptr_t rows, intptr_t total)
{
    if (rows <= 1 || total <= 1)
        return 0;

    return int32(row * (total - 1) / (rows - 1));
}
