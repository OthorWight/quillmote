# Quillmote

A native C plain-text editor inspired by Windows XP Notepad, built with GTK 4 and GtkSourceView 5. The title bar, menus, and editor follow the system's light/dark appearance, including changes while the app is open. Font settings are preserved.

## Build and run

```sh
make
./quillmote
./quillmote notes.txt
```

Requires a C compiler, `pkg-config`, GTK 4.10 or newer, and the `gtksourceview-5` development package. Spell checking uses the installed Enchant 2 runtime and dictionaries. Format → Spelling → Language lists the dictionaries available on your system; the initial choice follows the locale when a matching dictionary is installed, otherwise it uses `en_US`. Each launch opens an independent document window. A command-line path that does not exist opens a new, empty document with that filename; the file is created when you Save. The parent folder must already exist. Existing preferences from the former `notepad` configuration folder are picked up automatically.

## Features and shortcuts

| Menu | Commands |
| --- | --- |
| File | New (`Ctrl+N`), Open (`Ctrl+O`), Save (`Ctrl+S`), Save As (`Ctrl+Shift+S`), Page Setup, Print (`Ctrl+P`), Exit (`Ctrl+Q`) |
| Edit | Undo, Redo, Cut, Copy, Paste, Delete, Find (`Ctrl+F`), Find Next (`F3`), Replace (`Ctrl+H`), Go To (`Ctrl+G`), Select All, Time/Date (`F5`) |
| Format | Word Wrap, Font family/style/size, Spelling on/off and Language |
| View | Status Bar, Right-to-Left Reading Order, Zoom In/Out/Reset |
| Help | Local help (`F1`), About Quillmote |

- Find supports case matching, upward/downward search, and optional wraparound. The Replace toggle in the Find bar reveals replacement controls while keeping your search and options. Ctrl+H also opens Replace. Replace supports individual matches and Replace All, including undo of the whole operation. Escape closes the search panel.
- Standard GTK text-editing shortcuts work, including `Ctrl+Z`, `Ctrl+Shift+Z`, `Ctrl+X/C/V`, `Ctrl+A`, and keyboard navigation. Go To uses document line numbers and remains available with word wrap enabled.
- New, Open, Exit, and closing the window offer Save / Don't Save / Cancel when the document is modified. Cancelling Save As retains the original filename. Failed saves preserve the document and do not continue a pending close or open.
- Drop a single file anywhere on the window to open it. Unsaved changes get the same Save / Don’t Save / Cancel choices as Open.
- File reading and decoding run in a worker thread, and the new text buffer is filled in chunks. A loading bar offers Cancel; cancellation or failure keeps the previous document. Spelling also runs in short batches to keep the UI responsive.
- Open goes straight to the file picker with **All files** selected and detects the encoding automatically: BOMs and valid UTF-8 are recognized, with Windows-1252 as the fallback. Save As offers UTF-8, UTF-8 with BOM, Unicode (UTF-16 little endian), Unicode big endian, and ANSI (Windows-1252). ANSI characters that cannot be represented produce an error instead of being discarded.
- Files containing NUL bytes or data that cannot be decoded as text open automatically in a raw-byte view. Printable ASCII remains readable; other bytes are represented with Latin-1 characters and visible control symbols. Saving in Raw bytes mode reverses that mapping exactly, including mixed line endings. ASCII edits work normally; use a Unicode encoding in Save As to export characters outside the byte range. This displays a file’s contents as text, rather than rendering images, PDFs, or other document formats. File size is limited by available memory and GTK’s text-buffer limit.
- Windows CRLF, Unix LF, and classic Mac CR line endings are recognized and preserved; Save As can change the convention. Mixed line endings are normalized to the first convention found. New documents default to UTF-8 and LF.
- A file with `.LOG` on its first line gets the current time/date appended when opened. F5 inserts a timestamp at the cursor, replacing any selection.
- Escape dismisses open menus and returns focus to the editor. On GTK 4.14 and newer, submenus navigate within one popup to avoid nested popup grabs.
- Right-click an underlined word for spelling suggestions beside the normal editing commands. Shift+F10 or the Menu key opens suggestions at the cursor. Corrections support Undo. The same context menu offers Add to Dictionary and Ignore Word; ignored words last for the current dictionary session. Format → Spelling lets you turn checking off or choose an installed language. Additional languages require installing a dictionary supported by Enchant through your system’s package manager.
- `Ctrl++` (or `Ctrl+=`) zooms in, `Ctrl+-` zooms out, and `Ctrl+0` resets to 100%. Zoom ranges from 50–300% and changes only the on-screen text, leaving the chosen font and printing size unchanged. It resets to 100% on launch.
- Font, word wrap, status-bar visibility, spelling language/on-off, window size/maximized state, and page setup are saved in `$XDG_CONFIG_HOME/quillmote/settings.ini` (normally `~/.config/quillmote/settings.ini`).

## Crash recovery

Modified documents are checked for recovery snapshots every two seconds. Snapshots are captured in chunks and written atomically in the background to `~/.config/quillmote/recovery/` (or under `$XDG_CONFIG_HOME`). They include unsaved text, the filename, encoding, line endings, and cursor position, and are private to your user account. Each running window holds its own lock so it cannot recover another live window’s work.

Starting Quillmote without a file automatically restores an available snapshot as a modified document marked **Recovered**. When you open a specific file, a small Recover banner offers previous work without interrupting the requested open. Remaining snapshots can be recovered one at a time using that banner. Saving or explicitly discarding a document clears its snapshot. Recovery never writes over the original file automatically.

The most recent edits may be lost if a crash happens before a snapshot finishes. Capturing a large document restarts if it changes during capture; recovery is not a substitute for saving.

## Printing

Page Setup selects paper size, orientation, margins, header, and footer. Print uses the system print dialog for printer selection, copies, page ranges, and Print to File/PDF where available. Printed text is black on white, without spelling underlines, and uses the selected font.

Headers and footers accept `&f` (filename), `&p` (page number), `&d` (date), `&t` (time), `&l` / `&c` / `&r` (alignment), and `&&` (literal ampersand). Clear a field to omit it. Defaults are `&f` and `Page &p`.

The header/footer commands and `.LOG` behavior follow [Microsoft's Notepad documentation](https://support.microsoft.com/en-us/windows/apps/help-in-notepad). This is a Linux desktop adaptation: it keeps newer conveniences such as redo, contextual spelling, and working status/Go To controls with word wrap.

## Verification

```sh
make test
```

Run the full suite in a graphical session. Tests cover encoding round trips and conversion failures, system appearance and font preservation, contextual spelling, native menu activation and Escape focus recovery, find/replace and undo, navigation, save/discard/cancel workflows, preference persistence, multipage PDF generation, drag-and-drop save prompts, nonexistent command-line paths, zoom, dictionary controls, responsive/cancelled loading, and recovery after a forced process termination. They briefly open test windows and dialogs; documents, preferences, screenshots, and PDFs go into temporary directories. No document is sent to a physical printer.

For the file-format tests without a graphical session:

```sh
make tests/document
./tests/document
```
