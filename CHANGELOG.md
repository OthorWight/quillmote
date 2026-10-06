# Changelog

## 0.1.0 — initial public build preparation

- Restore saved files, tab order, active tab, cursor positions, and zoom after quitting.
- Detect external file changes with OS notifications and focus checks; offer Refresh with confirmation for unsaved edits and protect detected changes when saving.
- Remember Open and Save As folders and the default zoom between launches.
- Plain-text editing with tabs, independent undo histories, and drag-and-drop.
- Compact title row with menus, tabs, and a new-tab button directly after the tabs.
- Enter continues the current line's spaces and tabs; Shift+Enter inserts an unindented line break.
- Find and replace with highlighted matches, counts, and collapsible replacement controls.
- Word and character counts, invisible-character display, and zoom.
- Optional spelling with installed languages and contextual corrections.
- Background file loading, multiple encodings, and reversible raw-byte display.
- Clear closed documents and undo history even when GTK retains a selection buffer.
- Return unused heap pages to the OS after closing tabs on glibc systems, including after pending recovery writes finish.
- Crash recovery, printing, and saved appearance preferences.
- Grouped menus with native shortcut hints and tab navigation commands.
- Offline help, version reporting, desktop launcher, icon, and installation targets.
