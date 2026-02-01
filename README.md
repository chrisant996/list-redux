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

### How to build it

List Redux uses [Premake](http://premake.github.io) to generate Visual Studio solutions or makefiles for MinGW. Note that Premake >= 5.0.0-beta8 is required.

1. Cd to your git clone of the [list-redux](https://github.com/chrisant996/list-redux) repo.
2. Run `premake5.exe _toolchain_` (where `_toolchain_` is one of Premake's actions such as `vs2022` -- see `premake5.exe --help`).
   - If building with the RE2 library (see below), then add the `--re2` flag to the end of the command (e.g. `premake5 vs2022 --re2`).
3. Build scripts will be generated in `.build\_toolchain_`. For example `.build\vs2022\list-redux.sln`.
4. Call your toolchain of choice (VS, mingw32-make.exe, msbuild.exe, etc). GNU makefiles (Premake's _gmake_ target) have a **help** target for more info.

#### Including the RE2 library

The list-redux repo defaults to using ECMAScript regular expressions, but you can optionally build using the RE2 regular expression engine. Here are the additional steps for that.

1. Make sure [bazel](https://bazel.build) is installed.
2. Cd to your git clone of the [RE2](https://github.com/google/re2) repo. Note that the re2 repo directory needs to be a sibling of the list-redux repo directory.
3. Run `bazel build :all --features=static_link_msvcrt` and/or `bazel build :all -c dbg --features=static_link_msvcrt` (for debug build).
4. Follow the normal steps for building List-Redux, but add the `--re2` flag where appropriate.

> [!NOTE]
> Using the RE2 library more than triples the size of the `list.exe` executable file.
