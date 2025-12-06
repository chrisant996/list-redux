// Copyright (c) 2022 Christopher Antos
// License: http://opensource.org/licenses/MIT

#pragma once

class StrW;

typedef void (*vstrlen_func_t)(const WCHAR* s, int32 len);
enum ellipsify_mode : int16 { INVALID=-1, RIGHT, LEFT, PATH };

int32 ellipsify(const WCHAR* in, int32 limit, StrW& out, bool expand_ctrl);
int32 ellipsify_to_callback(const WCHAR* in, int32 limit, int32 expand_ctrl, vstrlen_func_t callback);
int32 ellipsify_ex(const WCHAR* in, int32 limit, ellipsify_mode mode, StrW& out, const WCHAR* ellipsis=nullptr, bool expand_ctrl=false, bool* truncated=nullptr);

