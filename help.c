#include "help.h"
#include <gtksourceview/gtksource.h>
#include "app-info.h"

static const struct { const char *id, *title, *text; } topics[] = {
    {"start", "Getting started",
     "Quillmote keeps everyday writing and editing simple. Open a file, write your text, and save when you are ready.\n\n"
     "Use File → Open (Ctrl+O), drop files onto the window, or pass filenames on the command line. You can select several files at once. Each file opens in its own tab; opening the same path again selects its existing tab.\n\n"
     "Ctrl+S saves the active document. Ctrl+Shift+S opens Save As, where you can change the filename, encoding, and line endings. A star beside the filename means you have unsaved changes.\n\n"
     "Launching quillmote new-file.txt opens an empty document if that file does not exist. Save creates it; its parent folder must already exist."},
    {"tabs", "Tabs and editing",
     "Ctrl+T or Ctrl+N creates a tab. Ctrl+W closes the current tab. Ctrl+Tab and Ctrl+Shift+Tab move between tabs; these commands are also in the View menu. You can drag tab headers to reorder them, and hover a filename to see its full path.\n\n"
     "Each tab keeps its own undo history, cursor position, search, and view settings. Closing a modified tab asks whether to Save, Don't Save, or Cancel. Closing the last tab leaves a blank document. Ctrl+Q closes the window, checking each tab for unsaved changes.\n\n"
     "Ctrl+Z undoes an edit; Ctrl+Shift+Z redoes it. Ctrl+X, Ctrl+C, and Ctrl+V cut, copy, and paste. Ctrl+A selects all text.\n\n"
     "Enter carries the current line's leading spaces and tabs onto the new line. Shift+Enter inserts a plain line break without copying indentation.\n\n"
     "Ctrl+G goes to a document line, including when word wrap is enabled. F5 inserts the current date and time. Files beginning with .LOG on their own first line receive a timestamp when opened."},
    {"search", "Find and replace",
     "Ctrl+F opens Find. Type your search to highlight matches and see their count. Enter or F3 selects the next match; the counter shows your position, such as 3 of 12.\n\n"
     "Match case makes letter case significant. Search up reverses the search direction. Wrap around continues at the other end of the document.\n\n"
     "Click the arrow beside Find, or press Ctrl+H, to reveal the replacement field. Your search and options stay in place. Replace changes the selected match and moves to the next; Replace All changes all occurrences. One Undo reverses an entire Replace All operation. An empty replacement deletes matches.\n\n"
     "Escape closes the search panel and returns focus to the editor."},
    {"view", "Appearance and counts",
     "The editor follows your system's light or dark appearance. Format → Font chooses the text family, style, and size. Word Wrap keeps long lines inside the window without inserting line breaks.\n\n"
     "Ctrl++ (or Ctrl+=) zooms in, Ctrl+- zooms out, and Ctrl+0 resets to 100%. Zoom affects the screen, not the saved text or printed font size.\n\n"
     "View → Word Count shows words and characters in the status bar. When text is selected, it counts the selection. Counts include Unicode text and update in small batches for large files.\n\n"
     "View → Show Invisible Characters reveals spaces, tabs, non-breaking spaces, and line endings. It does not change or add characters to the file.\n\n"
     "View also controls the status bar and right-to-left reading order. Font, spelling, and view preferences are remembered for new tabs and future launches."},
    {"spelling", "Spelling and languages",
     "Right-click an underlined word to choose a correction, Add to Dictionary, or Ignore Word. Shift+F10 or the Menu key opens suggestions at the cursor. Corrections can be undone.\n\n"
     "Format → Spelling turns checking on or off and selects a language. The language list contains dictionaries installed on your system.\n\n"
     "To add a language, install an Enchant-compatible dictionary with your distribution's package manager, then restart Quillmote. On Debian-based systems, for example, hunspell-es provides Spanish dictionaries.\n\n"
     "If spelling is unavailable, install the Enchant 2 runtime and a dictionary. Text editing works without them. Raw-byte documents are not spell checked. Ignore Word lasts for the current tab's dictionary session; Add to Dictionary is persistent."},
    {"files", "File formats",
     "Open detects text encoding automatically. UTF-8 and Unicode byte-order marks are recognized; Windows-1252 is a fallback for older text files. Save As can choose UTF-8, UTF-8 with BOM, UTF-16 in either byte order, or Windows-1252 (ANSI).\n\n"
     "Line endings are detected and preserved. Save As can choose LF, CRLF, or CR. Mixed endings in text documents are normalized to the first convention found.\n\n"
     "Non-text data opens in a reversible raw-byte view. Printable text stays readable, while control bytes appear as symbols. Saving in Raw bytes mode preserves the byte mapping, including mixed line endings. To use characters outside that range, choose Unicode in Save As.\n\n"
     "Quillmote displays file contents as text; it does not render images, PDFs, or formatted documents. Large files load in the background and offer a Cancel button. The maximum size depends on available memory."},
    {"recovery", "Saving and recovery",
     "Save regularly with Ctrl+S. Crash recovery is a backup for interrupted sessions, not an automatic save to your original file.\n\n"
     "Quillmote snapshots unsaved changes about every two seconds. After a crash, starting without filenames restores available snapshots in separate tabs marked Recovered. When opening a specific file, use the Recover banner to bring back previous work alongside it.\n\n"
     "Saving or explicitly discarding a document clears its snapshot. Recovery never overwrites the original file automatically. The most recent edits can be lost if a crash happens before a snapshot finishes.\n\n"
     "Preferences are stored in ~/.config/quillmote/settings.ini and recovery files in ~/.config/quillmote/recovery/. If XDG_CONFIG_HOME is set, that directory is used instead. Other running tabs' recovery files are left alone."},
    {"printing", "Printing",
     "File → Page Setup chooses paper, orientation, margins, and headers and footers. Ctrl+P opens your desktop's print dialog, including Print to File or PDF when available.\n\n"
     "Printing uses the selected font and black text on white paper. On-screen zoom, spelling marks, search highlights, and invisible-character markers are not printed.\n\n"
     "Header and footer commands:\n"
     "&f  Filename\n&p  Page number\n&d  Date\n&t  Time\n&l  Align left\n&c  Align center\n&r  Align right\n&&  A literal ampersand\n\n"
     "Clear a header or footer field to omit it."}
};

static void weak_window_free(gpointer data) {
    GWeakRef *ref = data;
    g_weak_ref_clear(ref); g_free(ref);
}

static gboolean help_escape(GtkEventControllerKey *controller, guint keyval, guint keycode,
                            GdkModifierType modifiers, gpointer data) {
    (void)controller; (void)keycode; (void)modifiers;
    if (keyval != GDK_KEY_Escape) return FALSE;
    gtk_window_destroy(GTK_WINDOW(data)); return TRUE;
}

void show_help(GtkWindow *parent) {
    GWeakRef *ref = g_object_get_data(G_OBJECT(parent), "quillmote-help");
    GtkWindow *existing = ref ? g_weak_ref_get(ref) : NULL;
    if (existing) { gtk_window_present(existing); g_object_unref(existing); return; }
    GtkWidget *window = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(window), "Quillmote Help");
    gtk_window_set_default_size(GTK_WINDOW(window), 780, 560);
    gtk_window_set_transient_for(GTK_WINDOW(window), parent);
    gtk_window_set_destroy_with_parent(GTK_WINDOW(window), TRUE);
    gtk_window_set_titlebar(GTK_WINDOW(window), gtk_header_bar_new());
    if (!ref) {
        ref = g_new0(GWeakRef, 1); g_weak_ref_init(ref, NULL);
        g_object_set_data_full(G_OBJECT(parent), "quillmote-help", ref, weak_window_free);
    }
    g_weak_ref_set(ref, window);
    GtkWidget *layout = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    GtkWidget *stack = gtk_stack_new(), *sidebar = gtk_stack_sidebar_new();
    gtk_stack_sidebar_set_stack(GTK_STACK_SIDEBAR(sidebar), GTK_STACK(stack));
    gtk_box_append(GTK_BOX(layout), sidebar);
    gtk_box_append(GTK_BOX(layout), gtk_separator_new(GTK_ORIENTATION_VERTICAL));
    gtk_widget_set_hexpand(stack, TRUE); gtk_widget_set_vexpand(stack, TRUE);
    gtk_box_append(GTK_BOX(layout), stack); gtk_window_set_child(GTK_WINDOW(window), layout);
    for (guint i = 0; i < G_N_ELEMENTS(topics); i++) {
        GtkWidget *scroll = gtk_scrolled_window_new();
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
        GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
        gtk_widget_set_margin_start(box, 24); gtk_widget_set_margin_end(box, 24);
        gtk_widget_set_margin_top(box, 24); gtk_widget_set_margin_bottom(box, 24);
        GtkWidget *title = gtk_label_new(topics[i].title);
        gtk_widget_add_css_class(title, "title-2"); gtk_label_set_xalign(GTK_LABEL(title), 0);
        gtk_box_append(GTK_BOX(box), title);
        GtkWidget *text = gtk_label_new(topics[i].text);
        gtk_label_set_wrap(GTK_LABEL(text), TRUE); gtk_label_set_wrap_mode(GTK_LABEL(text), PANGO_WRAP_WORD_CHAR);
        gtk_label_set_xalign(GTK_LABEL(text), 0); gtk_label_set_selectable(GTK_LABEL(text), TRUE);
        gtk_widget_set_valign(text, GTK_ALIGN_START); gtk_box_append(GTK_BOX(box), text);
        gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), box);
        gtk_stack_add_titled(GTK_STACK(stack), scroll, topics[i].id, topics[i].title);
    }
    GtkEventController *keys = gtk_event_controller_key_new();
    g_signal_connect(keys, "key-pressed", G_CALLBACK(help_escape), window);
    gtk_widget_add_controller(window, keys);
    gtk_window_present(GTK_WINDOW(window));
}

void show_about(GtkWindow *parent) {
    gchar *libraries = g_strdup_printf("GTK %u.%u.%u · GtkSourceView %u.%u.%u",
        gtk_get_major_version(), gtk_get_minor_version(), gtk_get_micro_version(),
        gtk_source_get_major_version(), gtk_source_get_minor_version(), gtk_source_get_micro_version());
    gtk_show_about_dialog(parent,
        "program-name", QUILLMOTE_NAME, "version", QUILLMOTE_VERSION,
        "comments", QUILLMOTE_SUMMARY "\nWrite, find, and edit across tabs, with spelling and crash recovery.",
        "logo-icon-name", QUILLMOTE_APP_ID,
        "license-type", GTK_LICENSE_MIT_X11,
        "copyright", "Copyright © 2026 Quillmote contributors",
        "system-information", libraries, NULL);
    g_free(libraries);
}
