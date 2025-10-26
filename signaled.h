// Copyright (c) 2025 by Christopher Antos
// License: http://opensource.org/licenses/MIT

// vim: set et ts=4 sw=4 cino={0s:

#pragma once

/*
 * Restore console mode and attributes on exit or ^C or ^Break.
 */

void SetGracefulExit();

bool IsSignaled();
void ClearSignaled();
