# Quillmote

A simple, comfortable text editor for Linux, built with GTK 4 and GtkSourceView 5. The title bar, menus, tabs, editor, search controls, status bar, and dialogs follow the system's light/dark appearance at startup and while the app is open. Font settings are preserved.

## Build and run

```sh
make
./quillmote
./quillmote notes.txt
```

Requires a C compiler, `pkg-config`, GTK 4.12 or newer, the `gtksourceview-5` development package, and `glib-compile-resources` (GLib development tools). Spell checking uses the installed Enchant 2 runtime and dictionaries. Format → Spelling → Language lists the dictionaries available on your system; the initial choice follows the locale when a matching dictionary is installed, otherwise it uses `en_US`. Each launch opens an independent window with tabs. Pass several paths to open them in separate tabs. A command-line path that does not exist opens a new, empty document with that filename; the file is created when you Save. The parent folder must already exist.

## Install

For your user account:

```sh
make install PREFIX="$HOME/.local"
```

Make sure `~/.local/bin` is in your `PATH`. The desktop launcher, icon, manual page, and license are installed alongside the executable. No default file associations are changed. System-wide installs default to `/usr/local`; packagers can set `PREFIX` and stage files with `DESTDIR`:

```sh
make install PREFIX=/usr DESTDIR=/tmp/quillmote-stage
```

`make uninstall` removes the installed application files using the same paths. Your documents, preferences, and recovery files are retained.

The compiled program uses your system libraries. A binary built on a newer distribution may not run on an older one; build from source on the target system. `CPPFLAGS`, `CFLAGS`, `LDFLAGS`, `LDLIBS`, `CC`, and `PKG_CONFIG` are honored for packaging.

## Help and version

Press **F1** for an offline guide organized by topic. **Help → About Quillmote** shows the application version, MIT license, and runtime library versions. `quillmote --help` and `quillmote --version` also work without a graphical display. Use `quillmote -- -filename.txt` for filenames beginning with a dash.

## Features and shortcuts

| Menu | Commands |
| --- | --- |
| File | New Tab (`Ctrl+T` or `Ctrl+N`), Close Tab (`Ctrl+W`), Open (`Ctrl+O`), Save (`Ctrl+S`), Save As (`Ctrl+Shift+S`), Refresh from Disk (`Ctrl+Shift+R`), Page Setup, Print (`Ctrl+P`), Quit (`Ctrl+Q`) |
| Edit | Undo, Redo, Cut, Copy, Paste, Delete, Find (`Ctrl+F`), Find Next (`F3`), Replace (`Ctrl+H`), Go To Line (`Ctrl+G`), Select All, Insert Date and Time (`F5`) |
| Format | Word Wrap, Font family/style/size, Spelling on/off and Language |
| View | Word Count, Show Invisible Characters, Status Bar, Right-to-Left Reading Order, Zoom In/Out/Reset, Next/Previous Tab (`Ctrl+Tab` / `Ctrl+Shift+Tab`) |
| Help | Local help (`F1`), About Quillmote |

- Find highlights all matches and shows the current match and total (for example, **3 of 12**), or **No matches**. It supports case matching, upward/downward search, and optional wraparound. The arrow beside the Find field expands or collapses replacement controls while keeping your search and options. Ctrl+H also opens Replace. Replace supports individual matches and Replace All, including undo of the whole operation. Escape closes the search panel.
- View → Word Count shows words and Unicode character counts in the status bar, counting selected text when a selection exists. Counts update in short batches; punctuation separates words, while internal apostrophes and combining accents stay within words.
- The status bar groups cursor position, counts, file format, and editing controls. Click the line/column position to go to a line, toggle Wrap, or open Spelling to change checking and language. Use the zoom minus/plus buttons, or click the percentage to enter a zoom level and reset to 100%. Counts shorten first when the window is narrow; their full text is available on hover.
- View → Show Invisible Characters reveals spaces, tabs, non-breaking spaces, and line endings without changing the file.
- Menus show shortcut hints beside commands and group related actions with separators. Standard GTK text-editing shortcuts work, including `Ctrl+Z`, `Ctrl+Shift+Z`, `Ctrl+X/C/V`, `Ctrl+A`, and keyboard navigation. Go To uses document line numbers and remains available with word wrap enabled.
- Enter copies the current line's leading spaces and tabs onto the new line, preserving mixed indentation. Shift+Enter inserts a line break without indentation.
- New and Open create tabs without interrupting edits in existing documents. Closing a modified tab offers Save / Don't Save / Cancel; Quit checks each tab in turn. Cancelling Save As retains the original filename. Failed saves preserve the document and stop closing. Closing the last tab leaves a blank document. Tabs share the top row with the menus, and the **+** button follows the last tab. Each tab keeps its own text, undo history, cursor, search, encoding, and view settings. `Ctrl+Tab` / `Ctrl+Shift+Tab` switch tabs; drag tab headers to reorder them.
- Starting without filenames reopens the files from the most recently closed window, preserving tab order, the active tab, cursor positions, and each tab's zoom. Files that were deleted or are unavailable are skipped. Opening filenames explicitly starts with those files. Closing a tab removes it from the next saved session; unsaved documents still use Save / Don't Save / Cancel when quitting.
- Open starts in the last folder you chose. Save As uses the current document's location, or the remembered Save As folder for new documents (falling back to the Open folder). Cancelling a file picker leaves these choices unchanged.
- Select several files in Open, drop files onto the window, or pass several command-line paths to open tabs. Opening an already-open path selects its existing tab and keeps unsaved edits.
- Changes made by another program show a banner with **Refresh**, and a ↻ marker on the affected tab. Files are also checked when the window regains focus or a tab becomes active. Refresh loads the latest disk contents and asks before discarding unsaved edits; cancellation or failure keeps your text. File → Refresh from Disk (`Ctrl+Shift+R`) is also available. Save asks before overwriting a detected external change. If the file is deleted or unavailable, your text stays open so you can Save or Save As.
- File reading and decoding run in a worker thread, and the new text buffer is filled in chunks. A loading bar offers Cancel; cancellation or failure keeps the previous document. Spelling also runs in short batches to keep the UI responsive.
- Open goes straight to the file picker with **All files** selected and detects the encoding automatically: BOMs and valid UTF-8 are recognized, with Windows-1252 as the fallback. Save As offers UTF-8, UTF-8 with BOM, Unicode (UTF-16 little endian), Unicode big endian, and ANSI (Windows-1252). ANSI characters that cannot be represented produce an error instead of being discarded.
- Files containing NUL bytes or data that cannot be decoded as text open automatically in a raw-byte view. Printable ASCII remains readable; other bytes are represented with Latin-1 characters and visible control symbols. Saving in Raw bytes mode reverses that mapping exactly, including mixed line endings. ASCII edits work normally; use a Unicode encoding in Save As to export characters outside the byte range. This displays a file’s contents as text, rather than rendering images, PDFs, or other document formats. File size is limited by available memory and GTK’s text-buffer limit.
- Windows CRLF, Unix LF, and classic Mac CR line endings are recognized and preserved; Save As can change the convention. Mixed line endings are normalized to the first convention found. New documents default to UTF-8 and LF.
- A file with `.LOG` on its first line gets the current time/date appended when opened. F5 inserts a timestamp at the cursor, replacing any selection.
- Escape dismisses open menus and returns focus to the editor. On GTK 4.14 and newer, submenus navigate within one popup to avoid nested popup grabs.
- Right-click an underlined word for spelling suggestions beside the normal editing commands. Shift+F10 or the Menu key opens suggestions at the cursor. Corrections support Undo. The same context menu offers Add to Dictionary and Ignore Word; ignored words last for the current dictionary session. Format → Spelling lets you turn checking off or choose an installed language. Additional languages require installing a dictionary supported by Enchant through your system’s package manager.
- `Ctrl++` (or `Ctrl+=`) zooms in, `Ctrl+-` zooms out, and `Ctrl+0` resets to 100%. Zoom ranges from 50–300% and changes only the on-screen text, leaving the chosen font and printing size unchanged. The zoom level is remembered between launches.
- Font, zoom, last Open/Save As folders, word wrap, word counts, invisible characters, status-bar visibility, spelling language/on-off, window size/maximized state, and page setup are saved in `$XDG_CONFIG_HOME/quillmote/settings.ini` (normally `~/.config/quillmote/settings.ini`).

## Crash recovery

Modified documents are checked for recovery snapshots every two seconds. Snapshots are captured in chunks and written atomically in the background to `~/.config/quillmote/recovery/` (or under `$XDG_CONFIG_HOME`). They include unsaved text, the filename, encoding, line endings, and cursor position, and are private to your user account. Each tab holds its own lock so recovery cannot claim another live tab’s work.

Starting Quillmote without a file automatically restores available snapshots in separate modified tabs marked **Recovered**. When you open a specific file, a small Recover banner offers previous work without interrupting the requested open. The banner opens snapshots in additional tabs, keeping the requested file open. Saving or explicitly discarding a document clears its snapshot. Recovery never writes over the original file automatically.

The most recent edits may be lost if a crash happens before a snapshot finishes. Capturing a large document restarts if it changes during capture; recovery is not a substitute for saving.

Saved session paths and positions are stored in `$XDG_CONFIG_HOME/quillmote/session.ini`; document contents are kept only in the separate crash-recovery snapshots. A successful quit replaces the saved session; cancelling quit keeps the previous one.

## Printing

Page Setup selects paper size, orientation, margins, header, and footer. Print uses the system print dialog for printer selection, copies, page ranges, and Print to File/PDF where available. Printed text is black on white, without spelling underlines, and uses the selected font.

Headers and footers accept `&f` (filename), `&p` (page number), `&d` (date), `&t` (time), `&l` / `&c` / `&r` (alignment), and `&&` (literal ampersand). Clear a field to omit it. Defaults are `&f` and `Page &p`.

## Verification

```sh
make test
make test-cli check-desktop
```

Run the full suite in a graphical session. Tests cover encoding round trips and conversion failures, system appearance and font preservation, contextual spelling, native menu activation and Escape focus recovery, find/replace and undo, navigation, save/discard/cancel workflows, preference persistence, multipage PDF generation, multi-file drag-and-drop and command-line tabs, tab close/save/cancel, Unicode counts, invisible characters, search counts and highlighting, nonexistent command-line paths, zoom, dictionary controls, responsive/cancelled loading, multi-tab recovery after a forced process termination, session restoration with missing files, cancelled quit, and recovery precedence, and external file changes with safe refresh and save-conflict handling. They briefly open test windows and dialogs; documents, preferences, screenshots, and PDFs go into temporary directories. No document is sent to a physical printer.

For the file-format tests without a graphical session:

```sh
make tests/document
./tests/document
```

## Source releases

`make dist` creates `dist/quillmote-0.1.0.tar.gz` and a SHA-256 checksum. The archive includes sources, tests, desktop assets, and documentation, with no dependency downloads during the build. See [CONTRIBUTING.md](CONTRIBUTING.md) for development and release verification.

## License

Quillmote source and artwork are available under the [MIT license](LICENSE). GTK, GtkSourceView, Enchant, and installed dictionaries retain their own licenses; they are system dependencies, not bundled in the source archive.
