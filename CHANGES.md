## v0.26 -- ?

- Added `F12` to show original screen until a key is pressed.
- Added `-f`, `--find`, `-i`, `--ignore-case`, `-i-`, `--exact-case`, `-r`, and `--regex` command line options for searching.
- Added custom color for file content.
- Added horizontal scroll markers when scrolling input text horizontally.
- Added mouse input for input text.
- Added file size in the file details display in the chooser (bottom right).
- Changed search in the chooser so the search string carries over into the viewer.
- Fixed column width in chooser for short filenames.
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
