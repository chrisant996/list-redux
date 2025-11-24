# BUGS

- Some exotic characters are calculated as the wrong width in Windows Terminal (e.g. from dirx\icons.cpp when choosing inaccurate encodings).
- What if codepage 437 isn't installed?  It's our fallback, but if it's not installed then choose another?

# FEATURES

### General

- [x] read color configuration from `.listredux` file
- [x] history in ReadInput() prompts
- [ ] some way to configure colors inside the app
- [ ] optionally save configured colors to `.listredux` file
- [ ] some way to revert to default colors inside the app
- [ ] documentation for the `.listredux` file
- [ ] documentation for regular expressions (link to MSVC ECMAScript syntax page)

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
- [ ] show used and free space in footer

### File Viewer (list file content)

- [x] optimize memory footprint for tracking line info (8 bytes per line, for file offset)
- [x] file size limit?  (LIST Plus did up to 16MB, LIST Enhanced did up to 500MB) _[Limited only by memory for line array.  Expensive for short lines.]_
- [x] piping
- [x] view files as text
- [x] `H` view files as hex
- [x] `W` wrapping
  - [x] text files wrap with awareness of word breaks (can split after a run of spaces, or on a transition from punctuation to word characters, or if a single word exceeds the max line length)
  - [x] word wrapping mutates line numbers; consider having an index of "user friendly" line numbers to show in the left margin for Go To Line.
  - [ ] toggle hanging indent (up to half the terminal width) (a third? a quarter? configurable maximum?)
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
- [x] `@` to display a list of names of files being viewed; use arrows to choose one, press Enter to make it the current file for viewing
- [ ] `F5` to reprocess/reload the file
- searching
  - [x] search in text mode (/ for case sensitive, \ for caseless)
  - [x] search in hex mode
  - [x] `F4` toggle multi-file search (next/prev cross file boundaries)
  - [x] make search interruptible with `Ctrl-Break`
  - [x] search for regex (search line by line)
  - [ ] search for hex bytes in hex mode
- hex mode
  - [x] always show hex ruler on a second header row
  - [x] go to offset
  - [x] highlight newline characters in hex mode
  - [ ] option to show line numbers next to offsets in hex mode (show first _new_ line number on a row)
  - [x] go to line
  - [x] `Alt-E` toggle hex edit mode
    - [x] navigate through bytes
    - [ ] navigate through characters (`Tab` to toggle between bytes and characters)
    - [x] change bytes
    - [ ] change characters
    - [x] in hex exit mode, interpret typeable keys as edits instead of commands (and add Alt-key versions)
    - [x] `F7`/`F8` prev/next change
    - [x] `Esc` prompt to save or discard changes
    - [ ] `Ctrl-S` to save changes
    - [ ] discard edited byte at cursor
    - [ ] ability to undo saving edited bytes (restore to original bytes)
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

### Future

- [ ] mouse input
  - Mouse wheel input is automatically converted to UP/DOWN keys even when ENABLE_MOUSE_INPUT is omitted, because of switching to the alternate screen.
  - [ ] toggle mouse input on/off (and configurable) since it interferes with the terminal host
  - [ ] mouse wheel should scroll by _N_ lines
- improve input routine
  - [ ] arrow keys
  - [ ] CUA keys
  - [ ] scrollable bounds
- [ ] `S` configure sort order
- detect certain file types and render with formatting/color
  - [ ] detect git patches and render some lines with color
  - [ ] detect markdown and render some simple markdown formatting
  - [ ] detect C++, Lua, etc and render syntax coloring
- [ ] option to sort horizontally instead of vertically

### Maybe

- use RE2 regex library
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
  - a key to switch between split screens
  - a key (or modifier) to scroll both split screens
- option to suppress "Are you sure" confirmations on destructive operations (such as delete/moving files)?
- `\` present a directory tree of the selected drive; select a subdirectory to list by moving the cursor and pressing Enter
  - list.com shows 8 levels, but selecting any directory just goes to its top level parent directory



# KNOWN ISSUES

- Very large files consisting mostly of very short lines may take an excessive amount of memory to open.  This could lead to crashing the program.
- File encodings are not handled yet.

### Not Planned

- Telephone dialer
- Printing
- hex mode: option to end a row at a newline (requires computing and caching hex mode pagination/delineation offsets) _[Too many drawbacks; including that it can't just seek without parsing anymore.]_
- a key to temporarily swap back to the original screen? _[Not generally very useful, and I don't want to perturb the original screen by printing "Press any key to return".]_
- use ICU for encodings when available _[ICU is independent from codepages, so it uses strings to identify encodings, and it has its own analogs to MLang and MultiByteToWideChar, and it's only available in Win10+ circa 2019 onward, so adjusting to use ICU when available will be an invasive change.]_



# INFO

Buerg Software:  https://web.archive.org/web/20080704121832/http://www.buerg.com/list.htm

