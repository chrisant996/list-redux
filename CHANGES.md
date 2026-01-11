## v0.29 -- TBD

- Added `ShowRuler` and `HexMode` config settings in `.listredux` file.
- Added `RestoreScreenOnExit` config setting in `.listredux` file.
- Added `X` and `Alt-X` to exit the program immediately.
- Added `Shift-X` and `Alt-Shift-X` to exit the program and do the opposite of the current `RestoreScreenOnExit` config setting.
- Added `Alt-V` in the viewer to force going to the chooser (even when `Esc` would exit the program).
- Added `Q` in the viewer to quit from the viewer if on the last file, otherwise go to the next file.
- Added `1` in the viewer to go to the first file.
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
