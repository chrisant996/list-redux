// Copyright (c) 2021 Christopher Antos
// License: http://opensource.org/licenses/MIT

#pragma once

#include <windows.h>
#include "str.h"
#include <vector>

struct PopupResult
{
    PopupResult()
    {
        Clear();
    }

    void Clear()
    {
        canceled = true;
        selected = -1;
    }

    bool canceled;
    intptr_t selected;
};

enum class PopupListFlags
{
    None                = 0x00,
    DimPaths            = 0x01,
};
DEFINE_ENUM_FLAG_OPERATORS(PopupListFlags);

constexpr uint32 c_min_popuplist_content_width = 40;

PopupResult ShowPopupList(const std::vector<StrW>& items, const WCHAR* title=nullptr, intptr_t index=0, PopupListFlags flags=PopupListFlags::None);
