# FEATURES

### File Chooser (list files in directory)

- [x] point & shoot to select a file for viewing
- [x] Tag one, several, or all files for viewing
- [x] support UNC and \\?\ syntax
- [x] `*` or `F5` to refresh the directory listing
- [ ] Launch the program associated with a file
- [ ] configure sort order
- [ ] `A` attrib for current file (enter combination of `ashr`/`ASHR` to clear/set attributes)
- [ ] `R` rename current file
- [ ] `N` create new directory
- [ ] some way to incrementally search the list of file names

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
- [ ] split screen display (view two files in two sizable windows in the terminal)
  - view any selected file in either window
- [ ] `/@` to supply a file with a list of names to view
- [ ] `@` to display a list of names of files being viewed; use arrows to choose one, press Enter to make it the current file for viewing
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

### Future

- [ ] mouse input (configurable on/off since it interferes with the terminal host)
- [ ] detect certain file types (e.g. git patches) and render some lines with color
- [ ] `W` for file sWeep command: execute a specified program using each tagged file as a parameter
- Store configuration how?  In an .ini file?
  - Colors
  - Options

### Maybe

- Option to suppress "Are you sure" confirmations on destructive operations (such as delete/moving files)?
- Cut and paste to new or existing file [did it really "cut" or just "copy"?]
- allow copying, deleting, moving tagged files?
- `/Nnn` lock the first `nn` lines of the file at the top of the display
- `/Cnn` lock the first `nn` columns of each line on the left side of the display
- search for hex bytes in hex mode
- modify files in hex mode
- search for text in multiple viewed files
- search for text in files in file chooser
- detect markdown and apply some simple markdown formatting
- `\` present a directory tree of the selected drive; select a subdirectory to list by moving the cursor and pressing Enter
- View archive file directories and select files for viewing
    - **Enhanced:** or extracting from or adding to archive files
    - or printing files?  (**Enhanced:** partial or entire files)
- what would it take to support other 8-bit encodings?
- handle UTF16 and UTF16-BE encodings?



# KNOWN ISSUES

- Very large files consisting mostly of very short lines may take an excessive amount of memory to open.  This could lead to crashing the program.
- File encodings are not handled yet.

### Not Planned

- Telephone dialer



# INFO

Buerg Software:  https://web.archive.org/web/20080704121832/http://www.buerg.com/list.htm

