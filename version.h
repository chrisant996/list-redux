// Copyright (c) 2025 by Christopher Antos
// License: http://opensource.org/licenses/MIT

#pragma once

#define VERSION_MAJOR           0
#define VERSION_MINOR           1

#define IND_VER2( a, b ) #a ## "." ## #b
#define IND_VER4( a, b, c, d ) L#a ## L"." ## L#b ## L"." ## L#c ## L"." ## L#d
#define DOT_VER2( a, b ) IND_VER2( a, b )
#define DOT_VER4( a, b, c, d ) IND_VER4( a, b, c, d )

#define RC_VERSION                VERSION_MAJOR, VERSION_MINOR, 0, 0
#define RC_VERSION_STR  DOT_VER4( VERSION_MAJOR, VERSION_MINOR, 0, 0 )

#define    VERSION_STR  DOT_VER2( VERSION_MAJOR, VERSION_MINOR )

#define STR_COPYRIGHTASCII "Copyright (c) 2025 Christopher Antos.  All rights reserved."
#define STR_COPYRIGHT_RC "Copyright \xA9 2025 Christopher Antos.  All rights reserved.\0"

