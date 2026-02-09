# OPEN ISSUES

- What if codepage 437 isn't installed?  It's our fallback, but if it's not installed then choose another?
- Sometimes detects `clink.log` file as Binary when `debug.log_terminal` is enabled, but it's really UTF8 (maybe there's a 0x08 backspace character?).
- [ ] **BUG:** cursor position can be wrong in Viewer depending on how ClickableRow collapses.
- [ ] Put the "Searching..." feedback on a whole different row.  Otherwise the "Ctrl-Break to cancel" and the cursor position are difficult and have a lot of jitter.

# FEATURES

### General

- [x] read color configuration from `.listredux` file
- [x] history in ReadInput() prompts
- [x] include terminal emulation for running on older than Windows 10 build 15063
- [x] a key to temporarily swap back to the original screen
- mouse input
  - Mouse wheel input is automatically converted to UP/DOWN keys even when ENABLE_MOUSE_INPUT is omitted, because of switching to the alternate screen.
  - [x] mouse wheel should scroll by _N_ lines
  - [x] `Shift` to let mouse input through to terminal
  - [x] wheel in chooser
  - [x] left click in hex edit mode in viewer
  - [x] left click in scrollbars
  - [x] left click hotspots in chooser
  - [x] left click hotspots in viewer
  - [x] mouse input in popup list
- improve input routine
  - [x] scrollable bounds
  - [x] arrow keys
  - [x] Ctrl-arrow keys
  - [x] Ctrl-BACK and Ctrl-DEL keys
  - [x] CUA keys
  - [x] copy/paste keys
  - [x] undo/redo
  - [x] mouse input
  - [x] show markers at left/right end when scrolled horizontally
  - [x] encapsulate in a class, and have ability to stop processing one instance and start processing a different instance (effectively making it possible to navigate between different input boxes)
- command line flags
  - [x] goto line (only if exactly one file matches the command line args)
  - [x] goto offset (same limitation as goto line)
  - [x] codepage (same limitation as goto line)
  - [x] wrapping
  - [x] hex view
  - [x] hex edit (same limitation as goto line)
  - [x] searching (chooser or viewer is inferred from file arg(s), multifile is inferred from number of files)
- menu row
  - [x] clickable
  - [x] enable/disable entries appropriately (always compute it, but only draw it if its content changed)
- [x] `Alt-Shift-C` to save current settings into `.listredux` file as defaults
- [x] some ways to exit:  clear screen, restore screen, or leave screen as-is (which actually means restore the screen and then overwrite it with the List-Redux screen again before finishing exiting)
  - [x] `RestoreScreenOnExit` in `.listredux`
  - [x] `X` and `Alt-X` force exit immediately
  - [x] `Shift-X` and `Alt-Shift-X` force exit and do the opposite of current `RestoreScreenOnExit` setting
- [x] optionally build with RE2 regex library
  - **FWIW:** ChatGPT and/or Claude were wrong; the MSVC implementation of ECMAScript regular expressions does allow numeric escapes like `\x40`, and it is not necessary to use the RE2 library to gain numeric escapes.  But RE2 has other benefits.
  - Manual steps for building re2 locally:
    - Clone re2:  `cd \repos` and `git clone https://github.com/google/re2.git`.
    - Download bazel from https://github.com/bazelbuild/bazel/releases.
    - Build re2:  `cd \repos\re2` and `bazel build :all --features=static_link_msvcrt` (add `-c dbg` for debug builds).
    - Now the re2 *.obj files are at `\repos\re2\bazel-out\x64_windows-fastbuild\bin\_objs\re2` (use `x64_windows-dbg` for debug builds).
  - [x] For now, require manually building the re2 *.lib files, and have premake reference the pre-built files for linking.
  - [x] Should official releases link with ECMAScript or RE2 regular expressions?
    - with ECMAScript `list.exe` ends up about 640KB.
    - with RE2 `list.exe` ends up about 1980KB.
    - it's hard to justify an extra 1300KB just for RE2, especially when ECMAScript in fact does support numeric escapes like `\x40`.
    - [x] **NO, for now official releases will not use the RE2 library.**
- [ ] documentation for the `.listredux` file
- [x] documentation for regular expressions (link to MSVC ECMAScript or RE2 syntax page)
- [x] remember Literal/Regex mode in a session

### File Chooser (list files in directory)

- [x] point & shoot to select a file for viewing
- [x] Tag one, several, or all files for viewing
- [x] support UNC and \\?\ syntax
- [x] `*` or `F5` to refresh the directory listing
- [x] Launch the program associated with a file
- [x] `W` for file sWeep command: execute a specified program using each tagged file as a parameter
- [x] `A` attrib for current or tagged files (enter combination of `ashr`/`-a-s-h-r` to set/clear attributes)
- [x] `R` rename current file
- [x] `N` create new directory
- [x] `DEL` to recycle file(s) or directory
- [x] `Shift-DEL` to delete file(s) or an empty directory
- [x] `.` change to parent directory
- [x] `E` edit file (`%EDITOR%` or `notepad.exe` or maybe `edit.exe`)
- [x] `P` new file mask (wildcard) or path
- [x] `F2` incrementally search the list of file names
- [x] `F` `S` `/` `\` search for text in files in file chooser (tag matching files)
- [x] `Ctrl-N` invert tagged files
- [x] show count of tagged files at the bottom
- [ ] some way to configure per-file colors similar to `dirx`

### File Viewer (list file content)

- [x] optimize memory footprint for tracking line info (8 bytes per line, for file offset)
- [x] file size limit?  (LIST Plus did up to 16MB, LIST Enhanced did up to 500MB) _[Limited only by memory for line array.  Expensive for short lines.]_
- [x] piping
- [x] view files as text
- [x] `H` view files as hex
- [x] `W` wrapping
  - [x] text files wrap with awareness of word breaks (can split after a run of spaces, or on a transition from punctuation to word characters, or if a single word exceeds the max line length)
  - [x] word wrapping mutates line numbers; consider having an index of "user friendly" line numbers to show in the left margin for Go To Line.
  - [x] toggle hanging indent (up to half the terminal width) (a third? a quarter? configurable maximum?)
  - [x] source code files should add +8 to hanging indent, other files should add +0 to hanging indent
- [x] `R` show ruler (both in text and hex modes)
- [x] `N` show line numbers
- [x] `O` show file offset
- [x] `E` toggle show line endings
- [x] `Space` toggle show whitespace
- [x] `C` expand ctrl chars
- [x] `T` expand tabs
- [x] `G` go to line
- [x] `J` jump to marked line
- [x] `M` mark center line
- [x] `U` clear marked line (unmark)
- [x] `Alt-O` open a new file
- [x] `Alt-C` close current file
- [x] `/@` to supply a file with a list of names to view
- [x] `'` or `@` to display a list of names of files being viewed; use arrows to choose one, press Enter to make it the current file for viewing
- [x] `F5` to reprocess/reload the file (but not documented, since the file is opened with deny shared writing)
- [x] remember scroll position in each file
- [x] `Ctrl-T` override tab stop width
- [x] `Alt-V` forces going to the chooser, even if the program was started with a filename and initially went directly to the viewer (it was `Alt-V` in the original LIST)
- [x] `Q` mimics the behavior from the original LIST; quit from the file viewer if on the last file, otherwise go to the next file
- [x] `1` go to the first file
- searching
  - [x] search in text mode (/ for case sensitive, \ for caseless)
  - [x] search in hex mode
  - [x] `F4` toggle multi-file search (next/prev cross file boundaries)
  - [x] make search interruptible with `Ctrl-Break`
  - [x] search for regex (search line by line)
  - [x] viewer should inherit most recent search string from chooser (unless viewer has a more recent search string?)
  - [ ] a way to search for raw hex bytes in hex mode -- because using regex numeric escapes like `\xAB` searches converted content (UTF16 for ECMAScript or UTF8 for RE2) rather than the raw unconverted content (unless the content is already natively UTF16 or UTF8, depending on ECMAScript or RE2)
- bookmarks
  - [x] 'Ctrl-Y` to add a bookmark
  - [x] 'Alt-Y` to cycle through bookmarks in reverse order
  - [x] 'Alt-Shift-Y` to cycle through bookmarks in forward order
  - [x] 'Ctrl-Alt-Shift-Y` to clear bookmarks (also cleared when closing the file or exiting the viewer)
  - [x] indicator for the middle line, when line number or file offsets are visible
- hex mode
  - [x] always show hex ruler on a second header row
  - [x] go to offset
  - [x] highlight newline characters in hex mode
  - [x] go to line
  - [x] `Alt-A` toggle ASCII filter in characters column
  - [x] remember hex mode across viewers
  - [x] option to show line numbers next to offsets in hex mode (show first _new_ line number on a row)
  - [x] `Alt-E` toggle hex edit mode
    - [x] navigate through bytes
    - [x] navigate through characters (`TAB` to toggle between bytes and characters)
    - [x] change bytes
    - [x] change characters
    - [x] in hex exit mode, interpret typeable keys as edits instead of commands (and add Alt-key versions)
    - [x] `F7`/`F8` prev/next change
    - [x] `Esc` prompt to save or discard changes
    - [x] `Ctrl-S` to save changes
    - [x] `Ctrl-U` discard edited byte at cursor
    - [x] `Ctrl-Z` discard unsaved edited bytes
    - [x] `Ctrl-Z` undo saved bytes (restore to original bytes)
    - [x] Viewer should open file for read with read/write sharing
    - [x] Saving in hex edit mode should open a new file handle for writing with read sharing
    - [x] Handle write errors better (say the offset)
- Encodings:
  - [x] Control characters use symbols from OEM CP 437.
  - [x] Binary files **_and hex mode_** use OEM CP.
  - [x] Text files default to OEM CP.
  - [x] Detect codepages via MLang.
  - [x] Decode individual codepoints from single-byte and multibyte encodings.
  - [x] Calculate grapheme widths of sequences of Unicode codepoints.
  - [x] Decode UTF16 encoding.
  - [x] Handle UTF16-BE encoding (1201).
  - [x] **IMPORTANT:**  Are any OEM codepages multi-byte?  Should it just use 437?
  - [x] allow toggling between Binary and detected Text codepage
  - [x] allow manual override for Text encoding
  - [x] detect UTF8 more aggressively; MLang chooses Western European instead of UTF8 in a file that's ASCII except for one emoji.
- [ ] option to end a row in hex mode at a newline
  - [ ] requires computing and caching hex mode pagination/delineation offsets
  - [ ] apply line-based parsing to hex mode for seek (i.e. Go To); must already be at least partially in place because of Show Line Numbers in hex mode
  - [ ] end at 0x0A, but if the next byte is 0x00 then keep it on the same line (favors UTF16-LE)

### Future

- [ ] improve message box routine (make it not full terminal width, have clickable buttons, etc)
  - [ ] mouse input in message box routine
- [ ] some way to configure colors inside the app (could benefit from ability to navigate between different input boxes)
- [ ] some way to revert to default colors inside the app
- detect certain file types and render with formatting/color
  - [ ] detect git patches and render some lines with color
  - [ ] detect markdown and render some simple markdown formatting
  - [ ] detect C++, Lua, etc and render syntax coloring
  - [ ] allow showing CSI SGR codes inline?  (but wrapping and max line length are problematic)
- [ ] let user override hanging indent per file
- [ ] `S` configure sort order (NOTE: `Chooser::NumTaggedFiles` expects directories sorted first)
- [ ] option to sort horizontally instead of vertically

### Maybe

- use ICU for encodings when available _[ICU is independent from codepages, so it uses strings to identify encodings, and it has its own analogs to MLang and MultiByteToWideChar, and it's only available in Win10+ circa 2019 onward, so adjusting to use ICU when available will be an invasive change.]_
- show used and free space in chooser?
- persist history lists for input prompts?
- cut and paste to new or existing file [did it really "cut" or just "copy"?]
- allow copying, moving tagged files?
- `/Nnn` lock the first `nn` lines of the file at the top of the display
- `/Cnn` lock the first `nn` columns of each line on the left side of the display
- view file at cursor (`Alt-I` "insert file" in list.com)
- archive files
  - view archive file contents and select files for viewing or extracting
  - add to archive files
- `Alt-W` for split screen display (again to unsplit)
  - a separate pair of chooser and viewer operate in each split screen
  - a key to switch between split screens (was `^V` in the original LIST)
  - a key (or modifier) to scroll both split screens
- option to suppress "Are you sure" confirmations on destructive operations (such as delete/moving files)?
- `\` present a directory tree of the selected drive; select a subdirectory to list by moving the cursor and pressing Enter
  - list.com shows 8 levels, but selecting any directory just goes to its top level parent directory
- the original LIST had a "pre-loading" feature which would load the entire file into memory before displaying anything (and add a config for it)?
- the original LIST had a "toggle screen saving" command (and add a config for it)?
- the original LIST had a way of marking lines (`Alt-M` for top, `Alt-B` for bottom) and writing them to a file (`Alt-D`) or appending them to the previously written file (`Alt-O`); using them repeatedly only grows the marked section; shrinking is only possible via `Alt-U` to clear the marked section and start over
- the original LIST considers "found text" as already marked, for the purposes of `Alt-D` and `Alt-O` (and maybe other things?)
- the original LIST had a "junk filter" feature which treated CR (0x0D) as though a LF (0x0A) follows it (even if it doesn't) and made BS (0x08) actually back up one character (and add a config for it)?



# KNOWN ISSUES

- This might be defined VT behavior, but when Windows Terminal applies `CSI K` (clear to end of line) while `SGR 7` (reverse video) is active, it applies the `CSI K` as though `SGR 7` is **_not_** active, resulting in backwards coloring.  So, in order for the display to look right, avoid configuring the `MarkedColor` definition with `7`.
- Mouse input is always enabled.  Hold the `SHIFT` key to let mouse clicks through to the terminal (e.g. for Quick Edit selection).
- Some exotic characters are calculated as the wrong width in Windows Terminal (e.g. from dirx\icons.cpp when choosing inaccurate encodings).
  - [x] Mitigate width miscalculations by making the scroll bar characters always use explicit positioning escape codes.
- Very large files consisting mostly of very short lines may take an excessive amount of memory to open.  This could lead to crashing the program.  For example, a 2GB file containing only 0x0A bytes would be interpreted as 2 billion lines, and take many times that much space to maintain tracking data for all 2 billion lines.

### Not Planned

- Telephone dialer
- Printing



# INFO

Buerg Software:  https://web.archive.org/web/20080704121832/http://www.buerg.com/list.htm

