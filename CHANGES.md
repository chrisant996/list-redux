## v0.31 -- 2026/02/10

- Changed word wrap to limit the hanging indent to not exceed half of the terminal width.
- Fixed infinite loop if word wrap encounters a line with leading whitespace that exceeds the wrap width.
- Fixed searches not matching the first line of a file.
- Fixed subsequent searches canceling immediately after `Ctrl-Break` used during a search.
- Fixed showing search progress in the chooser.
- Fixed failure when search in the chooser encounters an empty file.
- Fixed entering `0x` in the GoTo prompt.
- Fixed entering a `00` byte in hex edit mode.
- Fixed rendering an empty file in hex view mode.
- Fixed max path length when entering a new file path (was 32, now 2048).
- Fixed `ESC` in the file viewer accidentally exiting if used after `Alt-V` after launching the program directly to the viewer.
- Fixed the `Alt-E:EditMode` indicator to be disabled when viewing piped input.
- Fixed updating the `>` mark row indicator (e.g. for `M` and `U`) as the cursor moves in hex edit mode.

## v0.30 -- 2026/01/18

- Added bookmarks in the file viewer (`Ctrl-Y` sets a bookmark, `Alt-Y` cycles through bookmarks, etc).
- Added `Alt-Shift-R` which is the same as `Alt-R` except that non-GUI programs start a new console window.
- Added a visual indicator next to the "middle" row in the file viewer, when line numbers or file offsets are visible.  That's the row that used when marking a line or setting a bookmark.
- Changed `Alt-R` to avoid prompting to continue after launching a GUI program (unless the GUI program forcibly writes to its parent's console...which, surprisingly, some programs actually do).
- Fixed inaccurate centering when toggling hex mode.

## v0.29 -- 2026/01/11

- Added `ShowRuler` and `HexMode` config settings in `.listredux` file.
- Added `RestoreScreenOnExit` config setting in `.listredux` file.
- Added `X` and `Alt-X` to exit the program immediately.
- Added `Shift-X` and `Alt-Shift-X` to exit the program and do the opposite of the current `RestoreScreenOnExit` config setting.
- Added `Alt-V` in the viewer to force going to the chooser (even when `Esc` would exit the program).
- Added `Q` in the viewer to quit from the viewer if on the last file, otherwise go to the next file.
- Added `1` in the viewer to go to the first file.
- Added `F1` in the search prompt to view the regular expression syntax documention.
- Changed to remember the regex search mode during the current session.

## v0.28 -- 2026/01/04

- Changed the default value of the MarkedLine color definition to avoid using reverse video.
- Fixed highlight colors in the hex view characters pane.

## v0.27 -- 2025/12/26

- Added `F10` to toggle showing a clickable "menu row" showing some commonly used key bindings.
- Added `Alt-Shift-C` to save the current configuration state to the config file (`%USERPROFILE%\.listredux`).
- Show count of tagged files in the chooser.
- Changed what shows up in the command row in hex mode.
- Changed the `Command` color config name to `Footer` for symmetry with the `Header` color config name.
- Fixed whitespace and control code coloring in the viewer.
- Fixed left mouse button sometimes not responding.
- Fixed piped input.
- Fixed "Access denied" error when using `Ctrl-Z` to undo saved hex edits.
- Fixed `Ctrl-Z` so it also works when in hex view mode (versus edit mode).
- Fixed `F7` and `F8` to also work in hex view mode (versus edit mode).
- Fixed Edit, Run, and Sweep to not try to operate on directories.
- Fixed some display errors when the terminal width is small.
- Other minor cosmetic changes.

## v0.26 -- 2025/12/21

- Added `F12` to show original screen until a key is pressed.
- Added `-f`, `--find`, `-i`, `--ignore-case`, `-i-`, `--exact-case`, `-r`, and `--regex` command line options for searching.
- Added `Details` config setting in `.listredux` file to set the initial details level in the chooser (accepted values are `1` through `4`).
- Added custom color for file content.
- Added horizontal scroll markers when scrolling input text horizontally.
- Added mouse input for input text.
- Added file size in the file details display in the chooser (bottom right).
- Changed search in the chooser so the search string carries over into the viewer.
- Changed navigating to parent dir in the chooser so it reselects the child dir.
- Fixed column width in chooser for short filenames.
- Fixed Change Attributes in the chooser to say whether it will affect the current entry or all tagged entries.
- Fixed `Esc` in search input after `Ctrl-I` or `Ctrl-X`.
- Fixed edge cases for `Ins` and `Del` in input routine.
- Fixed applying colors to blank areas.
- Fixed hex mode width calculation.
- Fixed terminal emulation for restoring original screen.

## v0.25 -- 2025/12/09

- Fixed `Ctrl-I` and `Ctrl-X` accidentally clearing the input text in search prompts.
- Fixed display update glitches when scrolling input text horizontally.
- Fixed which row the search prompt is printed on in hex edit mode.
- Fixed inconsistency about what "Y" or "N" confirms in certain prompts in the viewer.

## v0.24 -- Initial release, 2025/12/08
