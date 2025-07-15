// Copyright (c) 2025 by Christopher Antos
// License: http://opensource.org/licenses/MIT

// vim: set et ts=4 sw=4 cino={0s:

#pragma once

#include <windows.h>
#include "str.h"
#include "error.h"
#include "fileinfo.h"
#include "viewer.h"

enum class Help { CHOOSER, VIEWER };

ViewerOutcome ViewHelp(Help help, Error& e);

