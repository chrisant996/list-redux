// Copyright (c) 2023,2025 Christopher Antos
// License: http://opensource.org/licenses/MIT

#pragma once

// This is disabled so that the PopupList scroll car can have a separate color
// from the border.
//#define USE_HALF_CHARS

int32 calc_scroll_car_size(intptr_t rows, intptr_t total);
int32 calc_scroll_car_offset(intptr_t top, int32 rows, intptr_t total, int32 car_size);
const WCHAR* get_scroll_car_char(intptr_t visible_offset, intptr_t car_offset, int32 car_size, bool floating);
int32 hittest_scroll_car(intptr_t row, int32 rows, intptr_t total);
