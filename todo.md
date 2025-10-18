# FEATURES

### File Chooser (list files in directory)

- [x] point & shoot to select a file for viewing
- [x] Tag one, several, or all files for viewing
- [x] support UNC and \\?\ syntax
- [x] `*` or `F5` to refresh the directory listing
- [ ] Launch the program associated with a file
- [x] `W` for file sWeep command: execute a specified program using each tagged file as a parameter
- [ ] `S` configure sort order
- [x] `A` attrib for current file (enter combination of `ashr`/`ASHR` to clear/set attributes)
- [x] `R` rename current file
- [x] `N` create new directory
- [x] `DEL` to delete file(s) or (empty) directory
- [x] `.` change to parent directory
- [ ] `E` edit file (`%EDITOR%` or `notepad.exe` or maybe `edit.exe`)
- [ ] `F` new file mask (wildcard)
- [ ] `P` change drive/path
- [ ] some way to incrementally search the list of file names
- [ ] option to sort horizontally instead of vertically
- [ ] show used and free space in footer

### File Viewer (list file content)

- [x] optimize memory footprint for tracking line info (8 bytes per line, for file offset)
- [x] file size limit?  (LIST Plus did up to 16MB, LIST Enhanced did up to 500MB) _[Limited only by memory for line array.  Expensive for short lines.]_
- [x] piping
- [x] view files as text
- [x] view files as hex
- [x] wrapping
- [x] text files wrap with awareness of word breaks (can split after a run of spaces, or on a transition from punctuation to word characters, or if a single word exceeds the max line length)
  - [ ] word wrapping mutates line numbers; consider having an index of "user friendly" line numbers to show in the left margin for Go To Line.
- [x] search in text mode (/ for case sensitive, \ for caseless)
- [x] search in hex mode
- [x] show ruler (both in text and hex modes)
- [x] show line numbers
- [x] show file offset
- [x] expand ctrl chars
- [x] expand tabs
- [x] go to line
- [x] jump to marked line
- [x] mark center line
- [x] clear marked line (unmark)
- [ ] `F` open a new file
- [ ] toggle multi-file search (next/prev cross file boundaries)
- [ ] `/@` to supply a file with a list of names to view
- [x] `@` to display a list of names of files being viewed; use arrows to choose one, press Enter to make it the current file for viewing
- hex mode
  - [ ] go to offset
  - [ ] go to line
  - [ ] highlight newline characters in hex mode
  - [ ] option to show line numbers next to offsets in hex mode
  - [ ] option to end a row at a newline (requires computing and caching hex mode pagination/delineation offsets)
- Encodings:
  - [x] Control characters use symbols from OEM CP 437.
  - [x] Binary files **_and hex mode_** use OEM CP.
  - [x] Text files default to OEM CP.
  - [x] Detect codepages via MLang.
  - [ ] Detect UTF8 text files and render the text nicely.
  - [ ] When spliting lines, do not sever multi-byte characters!
  - [ ] Toggle between Binary and Text (and UTF8).
  - [ ] Allow selecting between CP 437 ("Text") and UTF8 ("UTF8").  ASCII files can be displayed as either, so default to CP 437 ("Text") for them.
  - [ ] **IMPORTANT:**  Are any OEM codepages multi-byte?  Should it just use 437?
- detect certain file types and render with formatting/color
  - [ ] detect git patches and render some lines with color
  - [ ] detect markdown and render some simple markdown formatting

### Future

- [ ] mouse input
  - [ ] toggle mouse input on/off (and configurable) since it interferes with the terminal host
- [ ] split screen display (view two files in two sizable windows in the terminal)
  - view any selected file in either window
- Store configuration how?  In an .ini file?
  - Colors
  - Options

### Maybe

- Option to suppress "Are you sure" confirmations on destructive operations (such as delete/moving files)?
- Cut and paste to new or existing file [did it really "cut" or just "copy"?]
- allow copying, moving tagged files?
- `/Nnn` lock the first `nn` lines of the file at the top of the display
- `/Cnn` lock the first `nn` columns of each line on the left side of the display
- search for hex bytes in hex mode
- modify files in hex mode
- search for text in multiple viewed files
- search for text in files in file chooser
- view file at cursor (`Alt-I` "insert file" in list.com)
- `\` present a directory tree of the selected drive; select a subdirectory to list by moving the cursor and pressing Enter
  - list.com shows 8 levels, but selecting any directory just goes to its top level parent directory
- archive files
  - view archive file contents and select files for viewing or extracting
  - add to archive files
- what would it take to support other 8-bit encodings?
- handle UTF16 and UTF16-BE encodings?



# KNOWN ISSUES

- Very large files consisting mostly of very short lines may take an excessive amount of memory to open.  This could lead to crashing the program.
- File encodings are not handled yet.

### Not Planned

- Telephone dialer
- Printing



# INFO

Buerg Software:  https://web.archive.org/web/20080704121832/http://www.buerg.com/list.htm

