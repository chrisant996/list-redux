# LIST REDUX

Copyright 2025 by Christopher Antos.  All rights reserved.
Distributed under the MIT License.

In memory of Vernon D. Buerg, 1948-2009, author of the original LIST.COM program for DOS.

### What is it?

This is a terminal based file viewer.  It includes parity with most functionality from the old popular LIST.COM program, and it also adds many other features (including UTF8 support).

Some highlights:
- Run `list` to show an interactive list of files from the current directory (the "chooser").
- Run `list some_dir` to show an interactive list of files from "some_dir".
- Run `list file1 [file2 [...]]` to load "file1" (and "file2" (and etc)) into a file viewer (the "viewer").
- <kbd>F1</kbd> for help in the chooser or viewer.
- Tag/untag files in the chooser.
- Run a program on all tagged files in the chooser.
- Search and tag matching files in the chooser (with regex support).
- Load selected file or all tagged files into the viewer.
- Search the current file in the viewer (or turn on multifile searching to search all loaded files in the viewer).
- Optional word wrap, with hanging indent.
- Optional hex view mode, and also hex edit mode.
- Various other mode toggles in the viewer:  expand control characters, show line endings, show whitespace, show line numbers, show file offsets, show a column ruler, etc.
- Auto-detect file encoding, or override file encoding.
- Mouse input.
- Configurable colors and options via `.listredux` file (the color names and option names aren't documented yet, but in the meantime color names can be found [here](https://github.com/chrisant996/list-redux/blob/ca1aeb391a25296030ca242a03c5b5e00fc4ab51/colors.cpp#L803) and option names can be found [here](https://github.com/chrisant996/list-redux/blob/ca1aeb391a25296030ca242a03c5b5e00fc4ab51/config.cpp#L19)).
- Built-in terminal emulator for use when running on versions of Windows that don't include native VT support.

The "todo" list can be found [here](https://github.com/chrisant996/list-redux/blob/main/todo.md).
