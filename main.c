#include <gtk/gtk.h>
#include <gtksourceview/gtksource.h>
#include <dlfcn.h>
#include <pango/pango.h>
#include <string.h>
#include "document.h"
#include "printing.h"
#include "recovery.h"
#include <glib/gstdio.h>
#include <errno.h>

typedef enum { PENDING_NONE, PENDING_NEW, PENDING_OPEN, PENDING_QUIT, PENDING_PATH, PENDING_CREATE, PENDING_RECOVER } PendingAction;

typedef struct LoadJob LoadJob;

typedef struct {
    GtkApplication *app;
    GtkWidget *window;
    GtkSourceView *view;
    GtkSourceBuffer *buffer;
    GtkWidget *status;
    GtkWidget *find_bar;
    GtkWidget *find_entry;
    GtkWidget *replace_toggle, *replace_row, *replace_entry, *match_case, *search_up, *search_wrap, *search_message;
    GtkWidget *scroller;
    GtkNativeDialog *file_dialog;
    TextEncoding encoding;
    LineEnding ending;
    PendingAction pending;
    gchar *pending_path;
    gboolean busy, preferences_ready;
    gboolean word_wrap, show_status;
    PrintOptions printing;
    gchar *filename;
    guint spell_idle;
    guint64 document_revision;
    GMenu *spelling_menu;
    GtkCssProvider *css;
    PangoFontDescription *font;
    gboolean dark_mode;
    int window_width, window_height, zoom;
    gboolean maximized, spell_enabled;
    gchar *spell_language;
    GPtrArray *spell_languages;
    int spell_offset;
    LoadJob *loading;
    GCancellable *load_cancel;
    GtkWidget *loading_bar, *loading_label, *recovery_bar;
    gboolean quit_after_load;
    RecoverySession *recovery;
    gchar *recovery_directory;
    guint recovery_timer, recovery_idle, recovery_start_idle;
    gboolean recovered;
    guint64 recovery_epoch, recovery_revision, snapshot_revision;
    gboolean recovery_writing, recovery_failed;
    GString *snapshot;
    int snapshot_offset;
    void (*broker_list_dicts)(void *, void (*)(const char *, const char *, const char *, const char *, void *), void *);
    void (*dict_add)(void *, const char *, ssize_t);
    void (*dict_add_to_session)(void *, const char *, ssize_t);
    void *enchant_library;
    void *broker;
    void *dictionary;
    void *(*broker_init)(void);
    void *(*broker_request_dict)(void *, const char *);
    int (*dict_check)(void *, const char *, size_t);
    char **(*dict_suggest)(void *, const char *, size_t, size_t *);
    void (*dict_free_string_list)(void *, char **);
    void (*broker_free)(void *);
    void (*broker_free_dict)(void *, void *);
} AppState;

static AppState *action_state(GSimpleAction *action) {
    return g_object_get_data(G_OBJECT(action), "quillmote-state");
}

static void apply_editor_style(AppState *state) {
    const PangoFontDescription *font = state->font;
    const char *family = pango_font_description_get_family(font);
    GString *escaped_family = g_string_new(NULL);
    for (const unsigned char *p = (const unsigned char *)(family ? family : "Monospace"); *p; p++) {
        if (*p == '\'' || *p == '\\' || *p < 0x20 || *p == 0x7f)
            g_string_append_printf(escaped_family, "\\%x ", *p);
        else
            g_string_append_c(escaped_family, *p);
    }
    char size[G_ASCII_DTOSTR_BUF_SIZE];
    g_ascii_dtostr(size, sizeof size, (double)pango_font_description_get_size(font) / PANGO_SCALE * (state->zoom ? state->zoom : 100) / 100.0);
    PangoStyle style = pango_font_description_get_style(font);
    char *css = g_strdup_printf(
        ".quillmote-editor { font-family: '%s'; font-size: %s%s; font-weight: %d; font-style: %s; } "
        ".quillmote-status { padding: 4px 8px; background: %s; color: %s; } "
        ".find-bar { padding: 5px; background: %s; }",
        escaped_family->str, size, pango_font_description_get_size_is_absolute(font) ? "px" : "pt",
        pango_font_description_get_weight(font),
        style == PANGO_STYLE_ITALIC ? "italic" : style == PANGO_STYLE_OBLIQUE ? "oblique" : "normal",
        state->dark_mode ? "#252525" : "#d9d9d9", state->dark_mode ? "#eeeeee" : "#222222",
        state->dark_mode ? "#303030" : "#e5e5e5");
    gtk_css_provider_load_from_string(state->css, css);
    g_free(css);
    g_string_free(escaped_family, TRUE);
    GtkSourceStyleScheme *scheme = gtk_source_style_scheme_manager_get_scheme(
        gtk_source_style_scheme_manager_get_default(), state->dark_mode ? "classic-dark" : "classic");
    gtk_source_buffer_set_style_scheme(state->buffer, scheme);
}

static void update_title(AppState *state) {
    gboolean modified = gtk_text_buffer_get_modified(GTK_TEXT_BUFFER(state->buffer));
    gchar *name = state->filename ? g_path_get_basename(state->filename) : g_strdup("Untitled");
    gchar *title = g_strdup_printf("%s%s%s - Quillmote", modified ? "*" : "", name, state->recovered ? " (Recovered)" : "");
    gtk_window_set_title(GTK_WINDOW(state->window), title);
    g_free(name);
    g_free(title);
}

static void update_status(AppState *state) {
    GtkTextIter iter;
    GtkTextMark *mark = gtk_text_buffer_get_insert(GTK_TEXT_BUFFER(state->buffer));
    gtk_text_buffer_get_iter_at_mark(GTK_TEXT_BUFFER(state->buffer), &iter, mark);
    gchar *message = g_strdup_printf("Ln %d, Col %d     %s     %s     Zoom %d%%     Spell check: %s%s",
        gtk_text_iter_get_line(&iter) + 1, gtk_text_iter_get_line_offset(&iter) + 1,
        encoding_label(state->encoding), state->encoding == ENCODING_BYTES ? "Byte-preserving view" : state->ending == ENDING_CRLF ? "Windows (CRLF)" : state->ending == ENDING_CR ? "Mac (CR)" : "Unix (LF)",
        state->zoom, state->encoding == ENCODING_BYTES ? "off for raw bytes" : !state->spell_enabled ? "off" :
        state->dictionary ? state->spell_language : "unavailable",
        state->recovery_failed ? "     Recovery unavailable" : "");
    gtk_label_set_text(GTK_LABEL(state->status), message);
    g_free(message);
}

static void show_error(AppState *state, const char *title, GError *error) {
    GtkAlertDialog *dialog = gtk_alert_dialog_new("%s", title);
    gtk_alert_dialog_set_detail(dialog, error->message);
    gtk_alert_dialog_show(dialog, GTK_WINDOW(state->window));
    g_object_unref(dialog);
}

static void perform_pending(AppState *state);
static void choose_file(AppState *state, gboolean save);
static void request_document_change(AppState *state, PendingAction pending, const char *path);
static void schedule_spell_scan(GtkTextBuffer *buffer, gpointer data);
static void cursor_moved(GtkTextBuffer *buffer, GtkTextIter *location, GtkTextMark *mark, gpointer data);
static void modified_changed(GtkTextBuffer *buffer, gpointer data);
static void recovery_clear(AppState *state);
static void recovery_refresh(AppState *state);
static void save_preferences(AppState *state);

static gchar *buffer_text(AppState *state) {
    GtkTextIter start, end;
    gtk_text_buffer_get_bounds(GTK_TEXT_BUFFER(state->buffer), &start, &end);
    return gtk_text_buffer_get_text(GTK_TEXT_BUFFER(state->buffer), &start, &end, FALSE);
}

static gchar *time_date(void) {
    GDateTime *now = g_date_time_new_now_local();
    gchar *stamp = g_date_time_format(now, "%X %x");
    g_date_time_unref(now);
    return stamp;
}

static gboolean save_contents(AppState *state, const char *filename, TextEncoding encoding, LineEnding ending) {
    gchar *text = buffer_text(state);
    GError *error = NULL;
    GBytes *bytes = encode_document(text, encoding, ending, &error);
    g_free(text);
    gboolean saved = FALSE;
    if (bytes) {
        gsize size;
        const gchar *data = g_bytes_get_data(bytes, &size);
        saved = g_file_set_contents(filename, data, size, &error);
        g_bytes_unref(bytes);
    }
    if (saved) {
        recovery_clear(state);
        gchar *name = g_strdup(filename);
        g_free(state->filename); state->filename = name;
        state->encoding = encoding; state->ending = ending;
        gtk_text_buffer_set_modified(GTK_TEXT_BUFFER(state->buffer), FALSE);
        update_title(state); update_status(state);
    } else {
        show_error(state, "Could not save file", error);
        g_clear_error(&error);
    }
    return saved;
}

struct LoadJob {
    AppState *state;
    gchar *path;
    int requested_encoding;
    gboolean create, recover;
    RecoverySession *claimed;
    RecoveryDocument *document;
    GtkSourceBuffer *staging;
    gsize offset, length;
};

static void bind_buffer(AppState *state, GtkSourceBuffer *buffer) {
    if (state->spell_idle) { g_source_remove(state->spell_idle); state->spell_idle = 0; }
    if (state->buffer) g_signal_handlers_disconnect_by_data(state->buffer, state);
    g_set_object(&state->buffer, buffer);
    gtk_text_view_set_buffer(GTK_TEXT_VIEW(state->view), GTK_TEXT_BUFFER(buffer));
    GdkRGBA color; gdk_rgba_parse(&color, "#e33b3b");
    gtk_text_buffer_create_tag(GTK_TEXT_BUFFER(buffer), "misspelled", "underline", PANGO_UNDERLINE_ERROR,
                               "underline-rgba", &color, NULL);
    g_signal_connect(buffer, "changed", G_CALLBACK(schedule_spell_scan), state);
    g_signal_connect(buffer, "modified-changed", G_CALLBACK(modified_changed), state);
    g_signal_connect(buffer, "mark-set", G_CALLBACK(cursor_moved), state);
    apply_editor_style(state);
}

static void loading_controls(AppState *state, gboolean loading) {
    state->busy = loading;
    gtk_widget_set_visible(state->loading_bar, loading);
    gtk_widget_set_sensitive(GTK_WIDGET(state->view), !loading);
    gtk_widget_set_sensitive(state->find_bar, !loading);
    gchar **names = g_action_group_list_actions(G_ACTION_GROUP(state->app));
    for (int i = 0; names[i]; i++) {
        if (g_str_equal(names[i], "quit")) continue;
        GAction *action = g_action_map_lookup_action(G_ACTION_MAP(state->app), names[i]);
        g_simple_action_set_enabled(G_SIMPLE_ACTION(action), !loading);
    }
    g_strfreev(names);
}

static void load_finished(LoadJob *job, GError *error) {
    AppState *state = job->state;
    gboolean quit = state->quit_after_load;
    state->quit_after_load = FALSE;
    state->loading = NULL;
    g_clear_object(&state->load_cancel);
    loading_controls(state, FALSE);
    if (error && !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        show_error(state, job->recover ? "Could not recover document" : "Could not open file", error);
    g_clear_error(&error);
    recovery_session_free(job->claimed);
    recovery_document_free(job->document);
    g_clear_object(&job->staging); g_free(job->path); g_free(job);
    if (quit) request_document_change(state, PENDING_QUIT, NULL);
    g_application_release(G_APPLICATION(state->app));
}

static gboolean insert_loaded_chunk(gpointer data) {
    LoadJob *job = data;
    AppState *state = job->state;
    if (g_cancellable_is_cancelled(state->load_cancel)) {
        load_finished(job, NULL); return G_SOURCE_REMOVE;
    }
    /* A detached buffer avoids relayout and leaves the old document intact
     * until the entire new document is ready. Yield between UTF-8 chunks. */
    gsize end = MIN(job->offset + 32768, job->length);
    while (end < job->length && (job->document->text[end] & 0xc0) == 0x80) end--;
    GtkTextIter iter;
    gtk_text_buffer_get_end_iter(GTK_TEXT_BUFFER(job->staging), &iter);
    gtk_text_buffer_insert(GTK_TEXT_BUFFER(job->staging), &iter,
                          job->document->text + job->offset, end - job->offset);
    job->offset = end;
    if (end < job->length) {
        gchar *label = g_strdup_printf("Loading… %d%%", (int)(100.0 * end / job->length));
        gtk_label_set_text(GTK_LABEL(state->loading_label), label); g_free(label);
        return G_SOURCE_CONTINUE;
    }
    recovery_clear(state);
    if (job->claimed) {
        recovery_session_free(state->recovery);
        state->recovery = job->claimed; job->claimed = NULL;
    }
    g_free(state->filename); state->filename = g_strdup(job->document->filename);
    state->encoding = job->document->encoding; state->ending = job->document->ending;
    gtk_text_buffer_set_enable_undo(GTK_TEXT_BUFFER(job->staging), TRUE);
    bind_buffer(state, job->staging);
    GtkTextBuffer *buffer = GTK_TEXT_BUFFER(state->buffer);
    gtk_text_buffer_set_modified(buffer, job->recover);
    const char *text = job->document->text;
    if (!job->recover && state->encoding != ENCODING_BYTES &&
        (g_str_equal(text, ".LOG") || g_str_has_prefix(text, ".LOG\n"))) {
        gtk_text_buffer_get_end_iter(buffer, &iter);
        gchar *stamp = time_date();
        gchar *entry = g_strdup_printf("%s%s\n", g_str_has_suffix(text, "\n") ? "" : "\n", stamp);
        gtk_text_buffer_insert(buffer, &iter, entry, -1);
        g_free(stamp); g_free(entry);
    } else gtk_text_buffer_get_iter_at_offset(buffer, &iter, job->recover ?
        CLAMP(job->document->cursor, 0, gtk_text_buffer_get_char_count(buffer)) : 0);
    gtk_text_buffer_place_cursor(buffer, &iter);
    schedule_spell_scan(buffer, state);
    gtk_text_view_scroll_mark_onscreen(GTK_TEXT_VIEW(state->view), gtk_text_buffer_get_insert(buffer));
    state->recovered = job->recover; update_title(state);
    recovery_refresh(state);
    load_finished(job, NULL);
    return G_SOURCE_REMOVE;
}

static void read_document_thread(GTask *task, gpointer source, gpointer task_data, GCancellable *cancel) {
    (void)source;
    LoadJob *job = task_data;
    GError *error = NULL;
    if (job->recover) {
        job->document = recovery_read(job->path, &error);
    } else {
        GFile *file = g_file_new_for_path(job->path);
        GFileInfo *info = g_file_query_info(file, G_FILE_ATTRIBUTE_STANDARD_TYPE,
                                          G_FILE_QUERY_INFO_NONE, cancel, &error);
        if (!info && job->create && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND)) {
            g_clear_error(&error);
            GFile *parent = g_file_get_parent(file);
            GFileInfo *parent_info = parent ? g_file_query_info(parent, G_FILE_ATTRIBUTE_STANDARD_TYPE,
                G_FILE_QUERY_INFO_NONE, cancel, &error) : NULL;
            if (parent_info && g_file_info_get_file_type(parent_info) == G_FILE_TYPE_DIRECTORY) {
                job->document = g_new0(RecoveryDocument, 1);
                job->document->text = g_strdup("");
            } else if (!error) g_set_error_literal(&error, G_IO_ERROR, G_IO_ERROR_NOT_DIRECTORY,
                                                    "The parent folder does not exist.");
            g_clear_object(&parent_info); g_clear_object(&parent);
        } else if (info && g_file_info_get_file_type(info) != G_FILE_TYPE_REGULAR) {
            g_set_error_literal(&error, G_IO_ERROR, G_IO_ERROR_NOT_REGULAR_FILE, "Choose a regular file.");
        } else if (info) {
            gchar *contents = NULL; gsize length;
            if (g_file_load_contents(file, cancel, &contents, &length, NULL, &error)) {
                job->document = g_new0(RecoveryDocument, 1);
                job->document->text = decode_document(contents, length, job->requested_encoding,
                    &job->document->encoding, &job->document->ending, &error);
            }
            g_free(contents);
        }
        if (job->document) job->document->filename = g_strdup(job->path);
        g_clear_object(&info); g_object_unref(file);
    }
    if (error) g_task_return_error(task, error);
    else g_task_return_boolean(task, TRUE);
}

static void document_read(GObject *source, GAsyncResult *result, gpointer data) {
    (void)source;
    LoadJob *job = data;
    GError *error = NULL;
    if (!g_task_propagate_boolean(G_TASK(result), &error)) { load_finished(job, error); return; }
    job->length = strlen(job->document->text);
    job->staging = gtk_source_buffer_new(NULL);
    gtk_text_buffer_set_enable_undo(GTK_TEXT_BUFFER(job->staging), FALSE);
    g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, insert_loaded_chunk, job, NULL);
}

static gboolean load_file_full(AppState *state, const char *path, int encoding, gboolean create, gboolean recover) {
    if (state->loading) return FALSE;
    LoadJob *job = g_new0(LoadJob, 1);
    job->state = state; job->path = g_strdup(path); job->requested_encoding = encoding;
    job->create = create; job->recover = recover;
    if (recover) {
        GError *error = NULL;
        job->claimed = recovery_session_claim(path, &error);
        if (!job->claimed) {
            show_error(state, "Could not recover document", error); g_clear_error(&error);
            g_free(job->path); g_free(job); return FALSE;
        }
    }
    state->loading = job; state->load_cancel = g_cancellable_new();
    loading_controls(state, TRUE);
    gtk_label_set_text(GTK_LABEL(state->loading_label), recover ? "Recovering unsaved work…" : "Loading…");
    g_application_hold(G_APPLICATION(state->app));
    GTask *task = g_task_new(NULL, state->load_cancel, document_read, job);
    g_task_set_task_data(task, job, NULL);
    g_task_run_in_thread(task, read_document_thread); g_object_unref(task);
    return TRUE;
}

static gboolean load_file(AppState *state, const char *path, int encoding) {
    return load_file_full(state, path, encoding, FALSE, FALSE);
}

static void cancel_load(GtkButton *button, gpointer data) {
    (void)button; AppState *state = data;
    if (state->load_cancel) g_cancellable_cancel(state->load_cancel);
}

static void cancel_pending(AppState *state) {
    state->busy = FALSE; state->pending = PENDING_NONE;
    g_clear_pointer(&state->pending_path, g_free);
}

/* GtkFileChooserNative supports encoding choices in the native file dialog. */
G_GNUC_BEGIN_IGNORE_DEPRECATIONS
static void file_chosen(GtkNativeDialog *dialog, int response, gpointer data) {
    AppState *state = data;
    GtkFileChooser *chooser = GTK_FILE_CHOOSER(dialog);
    gboolean save = gtk_file_chooser_get_action(chooser) == GTK_FILE_CHOOSER_ACTION_SAVE;
    gboolean success = FALSE;
    if (response == GTK_RESPONSE_ACCEPT) {
        GFile *file = gtk_file_chooser_get_file(chooser);
        gchar *path = file ? g_file_get_path(file) : NULL;
        if (path) {
            if (save) {
                const char *choice = gtk_file_chooser_get_choice(chooser, "encoding");
                TextEncoding encoding = choice ? g_ascii_strtoll(choice, NULL, 10) : state->encoding;
                const char *line_choice = gtk_file_chooser_get_choice(chooser, "line-ending");
                LineEnding ending = line_choice ? g_ascii_strtoll(line_choice, NULL, 10) : state->ending;
                success = save_contents(state, path, encoding, ending);
            } else success = load_file(state, path, -1);
        } else {
            GError *error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "Choose a file on a local or mounted drive.");
            show_error(state, "Could not access file", error); g_error_free(error);
        }
        g_free(path); g_clear_object(&file);
    }
    state->file_dialog = NULL;
    gtk_native_dialog_destroy(dialog); g_object_unref(dialog);
    if (!save && success) return;
    state->busy = FALSE;
    if (save && success) perform_pending(state);
    else cancel_pending(state);
}

static void choose_file(AppState *state, gboolean save) {
    state->busy = TRUE;
    GtkFileChooserNative *dialog = gtk_file_chooser_native_new(save ? "Save As" : "Open", GTK_WINDOW(state->window),
        save ? GTK_FILE_CHOOSER_ACTION_SAVE : GTK_FILE_CHOOSER_ACTION_OPEN, save ? "Save" : "Open", "Cancel");
    state->file_dialog = GTK_NATIVE_DIALOG(dialog);
    GtkFileChooser *chooser = GTK_FILE_CHOOSER(dialog);
    GtkFileFilter *text = g_object_ref_sink(gtk_file_filter_new()); gtk_file_filter_set_name(text, "Text documents (*.txt)");
    gtk_file_filter_add_pattern(text, "*.txt"); gtk_file_chooser_add_filter(chooser, text); g_object_unref(text);
    GtkFileFilter *all = g_object_ref_sink(gtk_file_filter_new()); gtk_file_filter_set_name(all, "All files");
    gtk_file_filter_add_pattern(all, "*"); gtk_file_chooser_add_filter(chooser, all);
    if (!save) gtk_file_chooser_set_filter(chooser, all);
    g_object_unref(all);
    if (state->filename) {
        GFile *file = g_file_new_for_path(state->filename);
        gtk_file_chooser_set_file(chooser, file, NULL); g_object_unref(file);
    } else if (save) gtk_file_chooser_set_current_name(chooser, "Untitled.txt");
    /* Portal backends may show custom choices as a separate preliminary screen.
     * Open needs no choices: detect the encoding after the file is selected. */
    if (save) {
        const char *ids[] = {"0", "1", "2", "3", "4", "5", NULL};
        const char *labels[] = {"UTF-8", "UTF-8 with BOM", "Unicode (UTF-16 LE)",
                                "Unicode big endian (UTF-16 BE)", "ANSI (Windows-1252)", "Raw bytes (Latin-1)", NULL};
        gtk_file_chooser_add_choice(chooser, "encoding", "Encoding", ids, labels);
        gtk_file_chooser_set_choice(chooser, "encoding", ids[state->encoding]);
        const char *line_ids[] = {"0", "1", "2", NULL};
        const char *line_labels[] = {"Unix (LF)", "Windows (CRLF)", "Mac (CR)", NULL};
        gtk_file_chooser_add_choice(chooser, "line-ending", "Line endings", line_ids, line_labels);
        gtk_file_chooser_set_choice(chooser, "line-ending", line_ids[state->ending]);
    }
    gtk_native_dialog_set_modal(GTK_NATIVE_DIALOG(dialog), TRUE);
    g_signal_connect(dialog, "response", G_CALLBACK(file_chosen), state);
    gtk_native_dialog_show(GTK_NATIVE_DIALOG(dialog));
}
G_GNUC_END_IGNORE_DEPRECATIONS

static void save_file(AppState *state) {
    if (state->filename) {
        if (save_contents(state, state->filename, state->encoding, state->ending)) perform_pending(state);
        else cancel_pending(state);
    } else choose_file(state, TRUE);
}

static void perform_pending(AppState *state) {
    PendingAction pending = state->pending;
    state->pending = PENDING_NONE; state->busy = FALSE;
    if (pending == PENDING_NEW) {
        recovery_clear(state);
        GtkTextBuffer *buffer = GTK_TEXT_BUFFER(state->buffer);
        gtk_text_buffer_set_enable_undo(buffer, FALSE);
        gtk_text_buffer_set_text(buffer, "", 0);
        gtk_text_buffer_set_enable_undo(buffer, TRUE);
        g_clear_pointer(&state->filename, g_free);
        state->encoding = ENCODING_UTF8; state->ending = ENDING_LF;
        gtk_text_buffer_set_modified(buffer, FALSE);
        update_title(state); update_status(state);
    } else if (pending == PENDING_OPEN) choose_file(state, FALSE);
    else if (pending == PENDING_PATH || pending == PENDING_CREATE || pending == PENDING_RECOVER) {
        load_file_full(state, state->pending_path, -1, pending == PENDING_CREATE, pending == PENDING_RECOVER);
        g_clear_pointer(&state->pending_path, g_free);
    } else if (pending == PENDING_QUIT) {
        recovery_clear(state); save_preferences(state);
        gtk_window_destroy(GTK_WINDOW(state->window));
    }
}

static void unsaved_answer(GObject *source, GAsyncResult *result, gpointer data) {
    AppState *state = data;
    GError *error = NULL;
    int answer = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(source), result, &error);
    g_clear_error(&error);
    if (answer == 2) save_file(state);
    else if (answer == 1) perform_pending(state);
    else cancel_pending(state);
}

static void request_document_change(AppState *state, PendingAction pending, const char *path) {
    if (state->loading && pending == PENDING_QUIT) {
        state->quit_after_load = TRUE; g_cancellable_cancel(state->load_cancel); return;
    }
    if (state->busy) return;
    state->pending = pending;
    g_free(state->pending_path); state->pending_path = g_strdup(path);
    if (!gtk_text_buffer_get_modified(GTK_TEXT_BUFFER(state->buffer))) { perform_pending(state); return; }
    state->busy = TRUE;
    gchar *name = state->filename ? g_path_get_basename(state->filename) : g_strdup("Untitled");
    GtkAlertDialog *dialog = gtk_alert_dialog_new("Save changes to %s?", name); g_free(name);
    gtk_alert_dialog_set_detail(dialog, "Your changes will be lost if you don't save them.");
    const char *buttons[] = {"Cancel", "Don't Save", "Save", NULL};
    gtk_alert_dialog_set_buttons(dialog, buttons);
    gtk_alert_dialog_set_cancel_button(dialog, 0); gtk_alert_dialog_set_default_button(dialog, 2);
    gtk_alert_dialog_set_modal(dialog, TRUE);
    gtk_alert_dialog_choose(dialog, GTK_WINDOW(state->window), NULL, unsaved_answer, state);
    g_object_unref(dialog);
}

static gboolean close_requested(GtkWindow *window, gpointer data) {
    (void)window; request_document_change(data, PENDING_QUIT, NULL); return TRUE;
}

static void action_new(GSimpleAction *action, GVariant *parameter) { (void)parameter; request_document_change(action_state(action), PENDING_NEW, NULL); }
static void action_open(GSimpleAction *action, GVariant *parameter) { (void)parameter; request_document_change(action_state(action), PENDING_OPEN, NULL); }
static void action_save(GSimpleAction *action, GVariant *parameter) { (void)parameter; AppState *state = action_state(action); if (!state->busy) save_file(state); }
static void action_save_as(GSimpleAction *action, GVariant *parameter) { (void)parameter; AppState *state = action_state(action); if (!state->busy) choose_file(state, TRUE); }
static void action_quit(GSimpleAction *action, GVariant *parameter) { (void)parameter; request_document_change(action_state(action), PENDING_QUIT, NULL); }
static void action_cut(GSimpleAction *action, GVariant *parameter) { (void)parameter; gtk_text_buffer_cut_clipboard(GTK_TEXT_BUFFER(action_state(action)->buffer), gtk_widget_get_clipboard(GTK_WIDGET(action_state(action)->view)), TRUE); }
static void action_copy(GSimpleAction *action, GVariant *parameter) { (void)parameter; gtk_text_buffer_copy_clipboard(GTK_TEXT_BUFFER(action_state(action)->buffer), gtk_widget_get_clipboard(GTK_WIDGET(action_state(action)->view))); }
static void action_paste(GSimpleAction *action, GVariant *parameter) { (void)parameter; gtk_text_buffer_paste_clipboard(GTK_TEXT_BUFFER(action_state(action)->buffer), gtk_widget_get_clipboard(GTK_WIDGET(action_state(action)->view)), NULL, TRUE); }

static void action_select_all(GSimpleAction *action, GVariant *parameter) {
    (void)parameter;
    AppState *state = action_state(action);
    GtkTextIter start, end;
    gtk_text_buffer_get_bounds(GTK_TEXT_BUFFER(state->buffer), &start, &end);
    gtk_text_buffer_select_range(GTK_TEXT_BUFFER(state->buffer), &start, &end);
}

static void action_undo(GSimpleAction *action, GVariant *parameter) { (void)parameter; gtk_text_buffer_undo(GTK_TEXT_BUFFER(action_state(action)->buffer)); }
static void action_redo(GSimpleAction *action, GVariant *parameter) { (void)parameter; gtk_text_buffer_redo(GTK_TEXT_BUFFER(action_state(action)->buffer)); }

/* Keep submenu navigation in one native popup. Nested grabbing surfaces can
 * leave a failed Wayland popup holding focus when switching menus quickly. */
static void configure_menu_popovers(GtkWidget *widget) {
    if (GTK_IS_POPOVER_MENU(widget)) {
#if GTK_CHECK_VERSION(4, 14, 0)
        gtk_popover_menu_set_flags(GTK_POPOVER_MENU(widget), 0);
#endif
        return;
    }
    for (GtkWidget *child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child))
        configure_menu_popovers(child);
}

static void collect_open_menus(GtkWidget *widget, GPtrArray *menus) {
    for (GtkWidget *child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child))
        collect_open_menus(child, menus);
    if (GTK_IS_POPOVER_MENU(widget) && gtk_widget_get_visible(widget))
        g_ptr_array_add(menus, g_object_ref(widget));
}

static gboolean dismiss_editor_menus(AppState *state) {
    GPtrArray *menus = g_ptr_array_new_with_free_func(g_object_unref);
    collect_open_menus(state->window, menus);
    gboolean open = menus->len != 0;
    /* Children first. Hide immediately so the next popup cannot race a
     * popdown transition that still owns a native input grab. */
    for (guint i = 0; i < menus->len; i++) {
        GtkWidget *menu = g_ptr_array_index(menus, i);
        gtk_popover_popdown(GTK_POPOVER(menu));
        gtk_widget_set_visible(menu, FALSE);
    }
    g_ptr_array_unref(menus);
    return open;
}

static gboolean menu_escape(GtkEventControllerKey *controller, guint keyval, guint keycode,
                            GdkModifierType modifiers, gpointer data) {
    (void)controller; (void)keycode; (void)modifiers;
    AppState *state = data;
    if (keyval != GDK_KEY_Escape) return FALSE;
    GtkWidget *focus = gtk_root_get_focus(GTK_ROOT(state->window));
    gboolean menu_focus = focus && (gtk_widget_get_ancestor(focus, GTK_TYPE_POPOVER_MENU) ||
                                    gtk_widget_get_ancestor(focus, GTK_TYPE_POPOVER_MENU_BAR));
    if (!dismiss_editor_menus(state) && !menu_focus) return FALSE;
    gtk_widget_grab_focus(GTK_WIDGET(state->view));
    return TRUE;
}

static void spelling_actions_enabled(AppState *state, gboolean enabled) {
    const char *names[] = {"replace-spelling", "spelling-add", "spelling-ignore"};
    for (guint i = 0; i < G_N_ELEMENTS(names); i++) {
        GAction *action = g_action_map_lookup_action(G_ACTION_MAP(state->app), names[i]);
        if (action) g_simple_action_set_enabled(G_SIMPLE_ACTION(action), enabled);
    }
}

static void replace_spelling(GSimpleAction *action, GVariant *parameter) {
    AppState *state = action_state(action);
    guint64 revision;
    gint start_offset, end_offset;
    const gchar *original, *replacement;
    g_variant_get(parameter, "(tii&s&s)", &revision, &start_offset, &end_offset, &original, &replacement);
    if (revision != state->document_revision || !gtk_text_view_get_editable(GTK_TEXT_VIEW(state->view))) return;
    dismiss_editor_menus(state);
    gtk_widget_grab_focus(GTK_WIDGET(state->view));
    GtkTextBuffer *buffer = GTK_TEXT_BUFFER(state->buffer);
    GtkTextIter start, end;
    gtk_text_buffer_get_iter_at_offset(buffer, &start, start_offset);
    gtk_text_buffer_get_iter_at_offset(buffer, &end, end_offset);
    gchar *current = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
    if (g_str_equal(current, original)) {
        gtk_text_buffer_begin_user_action(buffer);
        gtk_text_buffer_delete(buffer, &start, &end);
        gtk_text_buffer_insert(buffer, &start, replacement, -1);
        gtk_text_buffer_end_user_action(buffer);
    }
    g_free(current);
}

static void prepare_spelling_menu(AppState *state, const GtkTextIter *position) {
    dismiss_editor_menus(state);
    spelling_actions_enabled(state, !state->loading);
    g_menu_remove_all(state->spelling_menu);
    if (!position || !state->spell_enabled || state->encoding == ENCODING_BYTES || !state->dictionary || !state->dict_suggest || !state->dict_free_string_list
        || !gtk_text_view_get_editable(GTK_TEXT_VIEW(state->view))) return;
    GtkTextIter start = *position, end = *position;
    if (!g_unichar_isalpha(gtk_text_iter_get_char(&start))) return;
    int length = 0;
    while (!gtk_text_iter_is_start(&start)) {
        if (++length > 1024) return;
        GtkTextIter previous = start;
        gtk_text_iter_backward_char(&previous);
        if (!g_unichar_isalpha(gtk_text_iter_get_char(&previous))) break;
        start = previous;
    }
    while (g_unichar_isalpha(gtk_text_iter_get_char(&end))) {
        if (++length > 1024) return;
        gtk_text_iter_forward_char(&end);
    }
    gchar *word = gtk_text_buffer_get_text(GTK_TEXT_BUFFER(state->buffer), &start, &end, FALSE);
    if (g_utf8_strlen(word, -1) > 1 && state->dict_check(state->dictionary, word, strlen(word)) > 0) {
        size_t count = 0;
        char **suggestions = state->dict_suggest(state->dictionary, word, strlen(word), &count);
        GMenu *section = g_menu_new();
        for (size_t i = 0; suggestions && i < count && i < 5; i++) {
            GMenuItem *item = g_menu_item_new(suggestions[i], NULL);
            g_menu_item_set_action_and_target_value(item, "app.replace-spelling",
                g_variant_new("(tiiss)", state->document_revision, gtk_text_iter_get_offset(&start),
                              gtk_text_iter_get_offset(&end), word, suggestions[i]));
            g_menu_append_item(section, item);
            g_object_unref(item);
        }
        if (!suggestions || count == 0) g_menu_append(section, "No spelling suggestions", NULL);
        const char *labels[] = {"Add to Dictionary", "Ignore Word"};
        const char *actions[] = {"app.spelling-add", "app.spelling-ignore"};
        for (int i = 0; i < 2; i++) {
            if ((i == 0 && !state->dict_add) || (i == 1 && !state->dict_add_to_session)) continue;
            GMenuItem *item = g_menu_item_new(labels[i], NULL);
            g_menu_item_set_action_and_target_value(item, actions[i], g_variant_new("(ts)", state->document_revision, word));
            g_menu_append_item(section, item); g_object_unref(item);
        }
        g_menu_append_section(state->spelling_menu, "Spelling", G_MENU_MODEL(section));
        g_object_unref(section);
        if (suggestions) state->dict_free_string_list(state->dictionary, suggestions);
    }
    g_free(word);
}

static void spelling_context_pressed(GtkGestureClick *gesture, int n_press, double x, double y, gpointer data) {
    (void)gesture; (void)n_press;
    AppState *state = data;
    int buffer_x, buffer_y;
    GtkTextIter position;
    gtk_text_view_window_to_buffer_coords(GTK_TEXT_VIEW(state->view), GTK_TEXT_WINDOW_WIDGET,
                                         (int)x, (int)y, &buffer_x, &buffer_y);
    gboolean over_text = gtk_text_view_get_iter_at_position(GTK_TEXT_VIEW(state->view), &position,
                                                           NULL, buffer_x, buffer_y);
    prepare_spelling_menu(state, over_text ? &position : NULL);
    /* Let GtkTextView open its usual menu, including clipboard actions. */
}

static gboolean spelling_context_key(GtkEventControllerKey *controller, guint keyval,
                                     guint keycode, GdkModifierType modifiers, gpointer data) {
    (void)controller; (void)keycode;
    if (keyval == GDK_KEY_Menu || (keyval == GDK_KEY_F10 && (modifiers & GDK_SHIFT_MASK))) {
        AppState *state = data;
        GtkTextIter position;
        gtk_text_buffer_get_iter_at_mark(GTK_TEXT_BUFFER(state->buffer), &position,
            gtk_text_buffer_get_insert(GTK_TEXT_BUFFER(state->buffer)));
        if (gtk_text_iter_ends_word(&position)) gtk_text_iter_backward_char(&position);
        prepare_spelling_menu(state, &position);
    }
    return FALSE;
}

static void close_find(GtkButton *button, gpointer data) {
    (void)button;
    AppState *state = data;
    gtk_widget_set_visible(state->find_bar, FALSE);
    gtk_widget_grab_focus(GTK_WIDGET(state->view));
}

static gboolean find_text(AppState *state) {
    const char *query = gtk_editable_get_text(GTK_EDITABLE(state->find_entry));
    if (!query[0]) {
        gtk_widget_set_visible(state->find_bar, TRUE);
        gtk_widget_grab_focus(state->find_entry);
        return FALSE;
    }
    GtkTextBuffer *buffer = GTK_TEXT_BUFFER(state->buffer);
    gboolean up = gtk_check_button_get_active(GTK_CHECK_BUTTON(state->search_up));
    gboolean wrap = gtk_check_button_get_active(GTK_CHECK_BUTTON(state->search_wrap));
    GtkTextSearchFlags flags = GTK_TEXT_SEARCH_TEXT_ONLY;
    if (!gtk_check_button_get_active(GTK_CHECK_BUTTON(state->match_case))) flags |= GTK_TEXT_SEARCH_CASE_INSENSITIVE;
    GtkTextIter begin, selection_start, selection_end, match_start, match_end;
    if (gtk_text_buffer_get_selection_bounds(buffer, &selection_start, &selection_end)) begin = up ? selection_start : selection_end;
    else gtk_text_buffer_get_iter_at_mark(buffer, &begin, gtk_text_buffer_get_insert(buffer));
    gboolean found = up ? gtk_text_iter_backward_search(&begin, query, flags, &match_start, &match_end, NULL)
                        : gtk_text_iter_forward_search(&begin, query, flags, &match_start, &match_end, NULL);
    gboolean wrapped = FALSE;
    if (!found && wrap) {
        if (up) gtk_text_buffer_get_end_iter(buffer, &begin);
        else gtk_text_buffer_get_start_iter(buffer, &begin);
        found = up ? gtk_text_iter_backward_search(&begin, query, flags, &match_start, &match_end, NULL)
                   : gtk_text_iter_forward_search(&begin, query, flags, &match_start, &match_end, NULL);
        wrapped = found;
    }
    if (found) {
        gtk_text_buffer_select_range(buffer, &match_end, &match_start);
        gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(state->view), &match_start, 0.1, FALSE, 0, 0);
        gtk_label_set_text(GTK_LABEL(state->search_message), wrapped ? "Search wrapped around." : "");
    } else {
        gtk_label_set_text(GTK_LABEL(state->search_message), "Text not found.");
        gtk_widget_set_visible(state->find_bar, TRUE);
    }
    return found;
}

static void find_next(GtkWidget *widget, gpointer data) { (void)widget; find_text(data); }

static void update_replace_toggle(GtkWidget *button, gboolean expanded) {
    const char *label = expanded ? "Hide replacement controls" : "Show replacement controls";
    gtk_button_set_icon_name(GTK_BUTTON(button), expanded ? "pan-down-symbolic" : "pan-end-symbolic");
    gtk_widget_set_tooltip_text(button, expanded ? label : "Show replacement controls (Ctrl+H)");
    gtk_accessible_update_property(GTK_ACCESSIBLE(button), GTK_ACCESSIBLE_PROPERTY_LABEL, label, -1);
    gtk_accessible_update_state(GTK_ACCESSIBLE(button), GTK_ACCESSIBLE_STATE_EXPANDED, expanded, -1);
}

static void toggle_replace(GtkToggleButton *button, gpointer data) {
    AppState *state = data;
    gboolean replace = gtk_toggle_button_get_active(button);
    update_replace_toggle(GTK_WIDGET(button), replace);
    gtk_widget_set_visible(state->replace_row, replace);
    /* Selecting an entry's text can take the primary selection from the match. */
    gtk_entry_grab_focus_without_selecting(GTK_ENTRY(replace ? state->replace_entry : state->find_entry));
}

static void show_find(AppState *state, gboolean replace) {
    gboolean was_visible = gtk_widget_get_visible(state->find_bar);
    GtkTextIter start, end;
    if (!was_visible && gtk_text_buffer_get_selection_bounds(GTK_TEXT_BUFFER(state->buffer), &start, &end)) {
        gchar *text = gtk_text_buffer_get_text(GTK_TEXT_BUFFER(state->buffer), &start, &end, FALSE);
        if (!strchr(text, '\n')) gtk_editable_set_text(GTK_EDITABLE(state->find_entry), text);
        g_free(text);
    }
    gtk_widget_set_visible(state->find_bar, TRUE);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(state->replace_toggle), replace);
    gtk_label_set_text(GTK_LABEL(state->search_message), "");
    if (was_visible)
        gtk_entry_grab_focus_without_selecting(GTK_ENTRY(replace && *gtk_editable_get_text(GTK_EDITABLE(state->find_entry))
                                                         ? state->replace_entry : state->find_entry));
    else
        gtk_widget_grab_focus(state->find_entry);
}

static void action_find(GSimpleAction *action, GVariant *parameter) { (void)parameter; show_find(action_state(action), FALSE); }
static void action_replace(GSimpleAction *action, GVariant *parameter) { (void)parameter; show_find(action_state(action), TRUE); }
static void action_find_next(GSimpleAction *action, GVariant *parameter) { (void)parameter; find_text(action_state(action)); }

static void replace_one(GtkWidget *widget, gpointer data) {
    (void)widget;
    AppState *state = data;
    GtkTextBuffer *buffer = GTK_TEXT_BUFFER(state->buffer);
    const char *query = gtk_editable_get_text(GTK_EDITABLE(state->find_entry));
    if (!query[0]) return;
    GtkTextIter start, end;
    if (gtk_text_buffer_get_selection_bounds(buffer, &start, &end)) {
        gchar *text = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
        gchar *a = g_utf8_casefold(text, -1), *b = g_utf8_casefold(query, -1);
        gboolean matches = gtk_check_button_get_active(GTK_CHECK_BUTTON(state->match_case)) ? g_str_equal(text, query) : g_str_equal(a, b);
        g_free(text); g_free(a); g_free(b);
        if (matches) {
            int offset = gtk_text_iter_get_offset(&start);
            gtk_text_buffer_begin_user_action(buffer);
            gtk_text_buffer_delete(buffer, &start, &end);
            gtk_text_buffer_insert(buffer, &start, gtk_editable_get_text(GTK_EDITABLE(state->replace_entry)), -1);
            if (gtk_check_button_get_active(GTK_CHECK_BUTTON(state->search_up))) gtk_text_buffer_get_iter_at_offset(buffer, &start, offset);
            gtk_text_buffer_place_cursor(buffer, &start);
            gtk_text_buffer_end_user_action(buffer);
        }
    }
    find_text(state);
}

static void replace_all(GtkWidget *widget, gpointer data) {
    (void)widget;
    AppState *state = data;
    const char *query = gtk_editable_get_text(GTK_EDITABLE(state->find_entry));
    if (!query[0]) return;
    GtkTextBuffer *buffer = GTK_TEXT_BUFFER(state->buffer);
    GtkTextSearchFlags flags = GTK_TEXT_SEARCH_TEXT_ONLY;
    if (!gtk_check_button_get_active(GTK_CHECK_BUTTON(state->match_case))) flags |= GTK_TEXT_SEARCH_CASE_INSENSITIVE;
    GtkTextIter cursor, start, end;
    gtk_text_buffer_get_start_iter(buffer, &cursor);
    guint count = 0;
    gtk_text_buffer_begin_user_action(buffer);
    while (gtk_text_iter_forward_search(&cursor, query, flags, &start, &end, NULL)) {
        gtk_text_buffer_delete(buffer, &start, &end);
        gtk_text_buffer_insert(buffer, &start, gtk_editable_get_text(GTK_EDITABLE(state->replace_entry)), -1);
        cursor = start; /* Skip inserted text, even when it contains the query. */
        count++;
    }
    gtk_text_buffer_end_user_action(buffer);
    gchar *message = g_strdup_printf("Replaced %u occurrence%s.", count, count == 1 ? "" : "s");
    gtk_label_set_text(GTK_LABEL(state->search_message), message); g_free(message);
}

static gboolean search_key(GtkEventControllerKey *controller, guint keyval, guint keycode, GdkModifierType modifiers, gpointer data) {
    (void)controller; (void)keycode; (void)modifiers;
    if (keyval != GDK_KEY_Escape) return FALSE;
    close_find(NULL, data); return TRUE;
}

static void set_wrap_state(GSimpleAction *action, GVariant *value, gpointer data) {
    AppState *state = data;
    state->word_wrap = g_variant_get_boolean(value);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(state->view), state->word_wrap ? GTK_WRAP_WORD_CHAR : GTK_WRAP_NONE);
    g_simple_action_set_state(action, value);
}

static void set_status_state(GSimpleAction *action, GVariant *value, gpointer data) {
    AppState *state = data;
    state->show_status = g_variant_get_boolean(value);
    gtk_widget_set_visible(state->status, state->show_status);
    g_simple_action_set_state(action, value);
}

static void set_rtl_state(GSimpleAction *action, GVariant *value, gpointer data) {
    AppState *state = data;
    gboolean rtl = g_variant_get_boolean(value);
    gtk_widget_set_direction(GTK_WIDGET(state->view), rtl ? GTK_TEXT_DIR_RTL : GTK_TEXT_DIR_LTR);
    gtk_text_view_set_justification(GTK_TEXT_VIEW(state->view), rtl ? GTK_JUSTIFY_RIGHT : GTK_JUSTIFY_LEFT);
    g_simple_action_set_state(action, value);
}

static void action_delete(GSimpleAction *action, GVariant *parameter) {
    (void)parameter;
    GtkTextBuffer *buffer = GTK_TEXT_BUFFER(action_state(action)->buffer);
    GtkTextIter start, end;
    if (!gtk_text_buffer_get_selection_bounds(buffer, &start, &end)) {
        gtk_text_buffer_get_iter_at_mark(buffer, &start, gtk_text_buffer_get_insert(buffer));
        end = start; gtk_text_iter_forward_cursor_position(&end);
    }
    gtk_text_buffer_begin_user_action(buffer);
    gtk_text_buffer_delete(buffer, &start, &end);
    gtk_text_buffer_end_user_action(buffer);
}

static void action_time_date(GSimpleAction *action, GVariant *parameter) {
    (void)parameter;
    AppState *state = action_state(action);
    GtkTextBuffer *buffer = GTK_TEXT_BUFFER(state->buffer);
    gchar *stamp = time_date();
    gtk_text_buffer_begin_user_action(buffer);
    gtk_text_buffer_delete_selection(buffer, TRUE, TRUE);
    gtk_text_buffer_insert_at_cursor(buffer, stamp, -1);
    gtk_text_buffer_end_user_action(buffer);
    g_free(stamp);
    gtk_widget_grab_focus(GTK_WIDGET(state->view));
}

static gboolean go_to_line(AppState *state, int line) {
    GtkTextBuffer *buffer = GTK_TEXT_BUFFER(state->buffer);
    if (line < 1 || line > gtk_text_buffer_get_line_count(buffer)) return FALSE;
    GtkTextIter iter;
    gtk_text_buffer_get_iter_at_line(buffer, &iter, line - 1);
    gtk_text_buffer_place_cursor(buffer, &iter);
    gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(state->view), &iter, 0.1, FALSE, 0, 0);
    gtk_widget_grab_focus(GTK_WIDGET(state->view));
    return TRUE;
}

static void go_to_accept(GtkWidget *widget, gpointer data) {
    (void)widget;
    GtkWidget *window = data;
    AppState *state = g_object_get_data(G_OBJECT(window), "state");
    GtkSpinButton *spin = g_object_get_data(G_OBJECT(window), "line");
    gtk_spin_button_update(spin);
    if (go_to_line(state, gtk_spin_button_get_value_as_int(spin))) gtk_window_destroy(GTK_WINDOW(window));
}

static void action_go_to(GSimpleAction *action, GVariant *parameter) {
    (void)parameter;
    AppState *state = action_state(action);
    GtkWidget *window = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(window), "Go To Line");
    gtk_window_set_transient_for(GTK_WINDOW(window), GTK_WINDOW(state->window));
    gtk_window_set_modal(GTK_WINDOW(window), TRUE);
    gtk_window_set_destroy_with_parent(GTK_WINDOW(window), TRUE);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_top(box, 16); gtk_widget_set_margin_bottom(box, 16);
    gtk_widget_set_margin_start(box, 16); gtk_widget_set_margin_end(box, 16);
    gtk_window_set_child(GTK_WINDOW(window), box);
    GtkWidget *label = gtk_label_new("Line number:");
    gtk_box_append(GTK_BOX(box), label);
    GtkWidget *spin = gtk_spin_button_new_with_range(1, gtk_text_buffer_get_line_count(GTK_TEXT_BUFFER(state->buffer)), 1);
    GtkTextIter iter;
    gtk_text_buffer_get_iter_at_mark(GTK_TEXT_BUFFER(state->buffer), &iter, gtk_text_buffer_get_insert(GTK_TEXT_BUFFER(state->buffer)));
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin), gtk_text_iter_get_line(&iter) + 1);
    gtk_box_append(GTK_BOX(box), spin);
    GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *cancel = gtk_button_new_with_label("Cancel"), *go = gtk_button_new_with_label("Go To");
    gtk_box_append(GTK_BOX(buttons), cancel); gtk_box_append(GTK_BOX(buttons), go); gtk_box_append(GTK_BOX(box), buttons);
    g_object_set_data(G_OBJECT(window), "state", state); g_object_set_data(G_OBJECT(window), "line", spin);
    g_signal_connect(go, "clicked", G_CALLBACK(go_to_accept), window);
    g_signal_connect_swapped(cancel, "clicked", G_CALLBACK(gtk_window_destroy), window);
    gtk_window_set_default_widget(GTK_WINDOW(window), go);
    gtk_widget_set_receives_default(spin, TRUE);
    gtk_window_present(GTK_WINDOW(window));
}

static void action_page_setup(GSimpleAction *action, GVariant *parameter) {
    (void)parameter; AppState *state = action_state(action);
    show_page_setup(GTK_WINDOW(state->window), &state->printing);
}

static void action_print(GSimpleAction *action, GVariant *parameter) {
    (void)parameter; AppState *state = action_state(action);
    if (state->busy) return;
    state->busy = TRUE;
    gchar *name = state->filename ? g_path_get_basename(state->filename) : g_strdup("Untitled");
    GError *error = NULL;
    if (!print_document(GTK_WINDOW(state->window), state->view, state->font, name, &state->printing, NULL, &error)) {
        show_error(state, "Could not print document", error); g_clear_error(&error);
    }
    g_free(name); state->busy = FALSE;
}

static void action_help(GSimpleAction *action, GVariant *parameter) {
    (void)parameter; AppState *state = action_state(action);
    GtkAlertDialog *dialog = gtk_alert_dialog_new("Quillmote Help");
    gtk_alert_dialog_set_detail(dialog,
        "File: Ctrl+N New, Ctrl+O Open, Ctrl+S Save, Ctrl+Shift+S Save As, Ctrl+P Print.\n"
        "Edit: Ctrl+Z Undo, Ctrl+Shift+Z Redo, Ctrl+X/C/V Cut/Copy/Paste, Ctrl+A Select All.\n"
        "Search: Ctrl+F Find, F3 Find Next, Ctrl+H Replace, Ctrl+G Go To Line. Escape closes Find/Replace.\n"
        "F5 inserts the current time and date. A file starting with .LOG on its own line gets a timestamp when opened.\n\n"
        "Format chooses font, word wrap, and spelling language/on-off. View controls the status bar, reading order, and zoom. Appearance follows the system.\n"
        "Zoom: Ctrl++ (or Ctrl+=), Ctrl+-, Ctrl+0 to reset. Zoom does not change the selected font or printing size.\n"
        "Right-click an underlined word for suggestions, Add to Dictionary, or Ignore Word; Shift+F10 opens the same menu.\n\n"
        "Drop a file onto the window to open it. Loading can be cancelled without losing the previous document.\n"
        "Launch with a new filename to create a document; Save creates the file.\n"
        "Unsaved work is periodically backed up for crash recovery. A normal launch restores it; when opening a file, use the Recover banner.\n\n"
        "Open shows all files and detects the encoding automatically. Non-text files use a reversible raw-byte view.\n"
        "Control bytes appear as symbols; saving a raw-byte view preserves those bytes.\n"
        "Save As offers encoding choices. ANSI means Windows-1252; choose Unicode for other scripts.\n"
        "Page Setup controls paper, orientation, margins, headers, and footers. Print uses the desktop print dialog, including Print to File/PDF where available.");
    gtk_alert_dialog_show(dialog, GTK_WINDOW(state->window)); g_object_unref(dialog);
}

static void action_about(GSimpleAction *action, GVariant *parameter) {
    (void)parameter; AppState *state = action_state(action);
    gtk_show_about_dialog(GTK_WINDOW(state->window), "program-name", "Quillmote", "version", "1.0",
        "comments", "A native plain-text editor inspired by Windows XP Notepad.\nBuilt with GTK and GtkSourceView.", NULL);
}

static void system_theme_changed(GtkSettings *settings, GParamSpec *pspec, gpointer data) {
    (void)pspec;
    AppState *state = data;
    gboolean dark = FALSE;
#if GTK_CHECK_VERSION(4, 20, 0)
    GtkInterfaceColorScheme scheme;
    g_object_get(settings, "gtk-interface-color-scheme", &scheme, NULL);
    if (scheme == GTK_INTERFACE_COLOR_SCHEME_DARK || scheme == GTK_INTERFACE_COLOR_SCHEME_LIGHT) {
        dark = scheme == GTK_INTERFACE_COLOR_SCHEME_DARK;
    } else
#endif
    {
        gchar *theme = NULL;
        g_object_get(settings, "gtk-application-prefer-dark-theme", &dark, "gtk-theme-name", &theme, NULL);
        gchar *lower = g_ascii_strdown(theme ? theme : "", -1);
        dark = dark || strstr(lower, "dark") != NULL;
        g_free(lower);
        g_free(theme);
    }
    state->dark_mode = dark;
    apply_editor_style(state);
}

static void font_dialog_done(GObject *source, GAsyncResult *result, gpointer data) {
    AppState *state = data;
    GError *error = NULL;
    PangoFontDescription *font = gtk_font_dialog_choose_font_finish(GTK_FONT_DIALOG(source), result, &error);
    if (font) {
        g_clear_pointer(&state->font, pango_font_description_free);
        state->font = font;
        apply_editor_style(state);
    } else if (error && !g_error_matches(error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_DISMISSED)
               && !g_error_matches(error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_CANCELLED)) {
        show_error(state, "Could not select font", error);
    }
    g_clear_error(&error);
}

static void action_font(GSimpleAction *action, GVariant *parameter) {
    (void)parameter;
    AppState *state = action_state(action);
    GtkFontDialog *dialog = gtk_font_dialog_new();
    gtk_font_dialog_set_title(dialog, "Font");
    gtk_font_dialog_set_modal(dialog, TRUE);
    gtk_font_dialog_choose_font(dialog, GTK_WINDOW(state->window), state->font, NULL, font_dialog_done, state);
    g_object_unref(dialog);
}

static void dictionary_found(const char *tag, const char *provider, const char *description,
                             const char *file, void *data) {
    (void)provider; (void)description; (void)file;
    GPtrArray *languages = data;
    for (guint i = 0; i < languages->len; i++)
        if (g_str_equal(tag, g_ptr_array_index(languages, i))) return;
    g_ptr_array_add(languages, g_strdup(tag));
}

static gint compare_languages(gconstpointer a, gconstpointer b) {
    return g_strcmp0(*(char * const *)a, *(char * const *)b);
}

static gboolean enchant_setup(AppState *state) {
    state->spell_languages = g_ptr_array_new_with_free_func(g_free);
    state->enchant_library = dlopen("libenchant-2.so.2", RTLD_LAZY);
    if (!state->enchant_library) return FALSE;
    state->broker_init = dlsym(state->enchant_library, "enchant_broker_init");
    state->broker_request_dict = dlsym(state->enchant_library, "enchant_broker_request_dict");
    state->dict_check = dlsym(state->enchant_library, "enchant_dict_check");
    state->dict_suggest = dlsym(state->enchant_library, "enchant_dict_suggest");
    state->dict_free_string_list = dlsym(state->enchant_library, "enchant_dict_free_string_list");
    state->broker_free = dlsym(state->enchant_library, "enchant_broker_free");
    state->broker_free_dict = dlsym(state->enchant_library, "enchant_broker_free_dict");
    state->broker_list_dicts = dlsym(state->enchant_library, "enchant_broker_list_dicts");
    state->dict_add = dlsym(state->enchant_library, "enchant_dict_add");
    state->dict_add_to_session = dlsym(state->enchant_library, "enchant_dict_add_to_session");
    if (!state->broker_init || !state->broker_request_dict || !state->dict_check ||
        !state->broker_free || !state->broker_free_dict) return FALSE;
    state->broker = state->broker_init();
    if (!state->broker) return FALSE;
    if (state->broker_list_dicts) state->broker_list_dicts(state->broker, dictionary_found, state->spell_languages);
    g_ptr_array_sort(state->spell_languages, compare_languages);
    if (!state->spell_language) {
        const gchar * const *preferred = g_get_language_names();
        for (int j = 0; preferred[j] && !state->spell_language; j++) {
            gchar *locale = g_strdup(preferred[j]);
            locale[strcspn(locale, ".@")] = 0;
            for (guint i = 0; i < state->spell_languages->len; i++) {
                const char *tag = g_ptr_array_index(state->spell_languages, i);
                if (g_str_equal(tag, locale)) { state->spell_language = g_strdup(tag); break; }
            }
            g_free(locale);
        }
        if (!state->spell_language) state->spell_language = g_strdup("en_US");
    }
    state->dictionary = state->broker_request_dict(state->broker, state->spell_language);
    return state->dictionary != NULL;
}

static gboolean spell_scan_idle(gpointer data) {
    AppState *state = data;
    GtkTextIter start, end;
    GtkTextBuffer *buffer = GTK_TEXT_BUFFER(state->buffer);
    if (state->spell_offset == 0) {
        gtk_text_buffer_get_bounds(buffer, &start, &end);
        gtk_text_buffer_remove_tag_by_name(buffer, "misspelled", &start, &end);
    }
    if (!state->spell_enabled || !state->dictionary || state->encoding == ENCODING_BYTES || state->loading) {
        state->spell_idle = 0; return G_SOURCE_REMOVE;
    }
    gtk_text_buffer_get_iter_at_offset(buffer, &start, state->spell_offset);
    gint64 deadline = g_get_monotonic_time() + 3000;
    while (!gtk_text_iter_is_end(&start)) {
        int skipped = 0;
        while (!gtk_text_iter_is_end(&start) && !g_unichar_isalpha(gtk_text_iter_get_char(&start))) {
            gtk_text_iter_forward_char(&start);
            if (++skipped >= 1024) break;
        }
        if (gtk_text_iter_is_end(&start)) break;
        if (g_unichar_isalpha(gtk_text_iter_get_char(&start))) {
            GtkTextIter word_start = start, word_end = start;
            int letters = 0;
            while (!gtk_text_iter_is_end(&word_end) && g_unichar_isalpha(gtk_text_iter_get_char(&word_end)) && letters < 1024) {
                gtk_text_iter_forward_char(&word_end); letters++;
            }
            /* Skip pathological words, including continuation chunks. */
            GtkTextIter previous = word_start;
            gboolean continuation = gtk_text_iter_backward_char(&previous) && g_unichar_isalpha(gtk_text_iter_get_char(&previous));
            if (letters < 1024 && !continuation) {
                gchar *word = gtk_text_buffer_get_text(buffer, &word_start, &word_end, FALSE);
                if (letters > 1 && state->dict_check(state->dictionary, word, strlen(word)) > 0)
                    gtk_text_buffer_apply_tag_by_name(buffer, "misspelled", &word_start, &word_end);
                g_free(word);
            }
            start = word_end;
        }
        if (g_get_monotonic_time() >= deadline) {
            state->spell_offset = gtk_text_iter_get_offset(&start); return G_SOURCE_CONTINUE;
        }
    }
    state->spell_idle = 0; return G_SOURCE_REMOVE;
}

static void schedule_spell_scan(GtkTextBuffer *buffer, gpointer data) {
    (void)buffer;
    AppState *state = data;
    state->document_revision++;
    state->spell_offset = 0;
    /* Do not destroy a model button during its own activation. The menu is
     * a snapshot until the next opening; revision guards reject old targets. */
    spelling_actions_enabled(state, FALSE);
    if (!state->spell_idle) state->spell_idle = g_idle_add(spell_scan_idle, state);
    update_title(state); update_status(state);
}

static void set_spelling_state(GSimpleAction *action, GVariant *value, gpointer data) {
    AppState *state = data;
    dismiss_editor_menus(state); gtk_widget_grab_focus(GTK_WIDGET(state->view));
    state->spell_enabled = g_variant_get_boolean(value);
    g_simple_action_set_state(action, value);
    schedule_spell_scan(NULL, state);
}

static void set_language_state(GSimpleAction *action, GVariant *value, gpointer data) {
    AppState *state = data;
    const char *language = g_variant_get_string(value, NULL);
    if (!state->broker) return;
    dismiss_editor_menus(state); gtk_widget_grab_focus(GTK_WIDGET(state->view));
    void *dictionary = state->broker_request_dict(state->broker, language);
    if (!dictionary) return;
    if (state->dictionary) state->broker_free_dict(state->broker, state->dictionary);
    state->dictionary = dictionary;
    g_free(state->spell_language); state->spell_language = g_strdup(language);
    g_simple_action_set_state(action, value);
    schedule_spell_scan(NULL, state);
}

static void spelling_word_action(GSimpleAction *action, GVariant *value) {
    AppState *state = action_state(action);
    guint64 revision; const char *word;
    g_variant_get(value, "(t&s)", &revision, &word);
    if (!state->dictionary || !state->spell_enabled || revision != state->document_revision) return;
    dismiss_editor_menus(state); gtk_widget_grab_focus(GTK_WIDGET(state->view));
    gboolean permanent = g_str_equal(g_action_get_name(G_ACTION(action)), "spelling-add");
    if (permanent && state->dict_add) state->dict_add(state->dictionary, word, -1);
    else if (!permanent && state->dict_add_to_session) state->dict_add_to_session(state->dictionary, word, -1);
    schedule_spell_scan(NULL, state);
}

static void action_zoom(GSimpleAction *action, GVariant *parameter) {
    (void)parameter;
    AppState *state = action_state(action);
    const char *name = g_action_get_name(G_ACTION(action));
    state->zoom = g_str_equal(name, "zoom-reset") ? 100 :
        CLAMP(state->zoom + (g_str_equal(name, "zoom-in") ? 10 : -10), 50, 300);
    apply_editor_style(state); update_status(state);
}

static void cursor_moved(GtkTextBuffer *buffer, GtkTextIter *location, GtkTextMark *mark, gpointer data) {
    (void)buffer; (void)location; (void)mark;
    update_status(data);
}

static void add_action(AppState *state, const char *name, GCallback callback) {
    GSimpleAction *action = g_simple_action_new(name, NULL);
    g_object_set_data(G_OBJECT(action), "quillmote-state", state);
    g_signal_connect(action, "activate", callback, NULL);
    g_action_map_add_action(G_ACTION_MAP(state->app), G_ACTION(action));
    g_object_unref(action);
}

static gchar *preferences_path(void) {
    return g_build_filename(g_get_user_config_dir(), "quillmote", "settings.ini", NULL);
}

static void load_preferences(AppState *state) {
    state->font = pango_font_description_from_string("Monospace 12");
    state->word_wrap = TRUE; state->show_status = TRUE; state->spell_enabled = TRUE;
    state->window_width = 920; state->window_height = 620; state->zoom = 100;
    print_options_init(&state->printing);
    GKeyFile *settings = g_key_file_new();
    gchar *path = preferences_path();
    /* Preserve preferences from the app's former name on the first launch. */
    if (!g_file_test(path, G_FILE_TEST_EXISTS)) {
        g_free(path);
        path = g_build_filename(g_get_user_config_dir(), "notepad", "settings.ini", NULL);
    }
    if (g_key_file_load_from_file(settings, path, G_KEY_FILE_NONE, NULL)) {
        gchar *font = g_key_file_get_string(settings, "Editor", "font", NULL);
        if (font) {
            PangoFontDescription *description = pango_font_description_from_string(font);
            if (pango_font_description_get_family(description) && pango_font_description_get_size(description) > 0) {
                pango_font_description_free(state->font); state->font = description;
            } else pango_font_description_free(description);
            g_free(font);
        }
        if (g_key_file_has_key(settings, "Editor", "word-wrap", NULL)) state->word_wrap = g_key_file_get_boolean(settings, "Editor", "word-wrap", NULL);
        if (g_key_file_has_key(settings, "Editor", "status-bar", NULL)) state->show_status = g_key_file_get_boolean(settings, "Editor", "status-bar", NULL);
        if (g_key_file_has_key(settings, "Editor", "spell-enabled", NULL)) state->spell_enabled = g_key_file_get_boolean(settings, "Editor", "spell-enabled", NULL);
        state->spell_language = g_key_file_get_string(settings, "Editor", "spell-language", NULL);
        int width = g_key_file_get_integer(settings, "Window", "width", NULL);
        int height = g_key_file_get_integer(settings, "Window", "height", NULL);
        if (width >= 320 && width <= 16384) state->window_width = width;
        if (height >= 200 && height <= 16384) state->window_height = height;
        state->maximized = g_key_file_get_boolean(settings, "Window", "maximized", NULL);
        GtkPageSetup *page = gtk_page_setup_new_from_key_file(settings, "Page Setup", NULL);
        if (page) { g_set_object(&state->printing.page_setup, page); g_object_unref(page); }
        const char *keys[] = {"header", "footer"};
        gchar **fields[] = {&state->printing.header, &state->printing.footer};
        for (int i = 0; i < 2; i++) {
            gchar *value = g_key_file_get_string(settings, "Print", keys[i], NULL);
            if (value) { g_free(*fields[i]); *fields[i] = value; }
        }
    }
    g_free(path); g_key_file_unref(settings); state->preferences_ready = TRUE;
}

static void save_preferences(AppState *state) {
    if (!state->preferences_ready) return;
    GKeyFile *settings = g_key_file_new();
    gchar *font = pango_font_description_to_string(state->font);
    g_key_file_set_string(settings, "Editor", "font", font); g_free(font);
    g_key_file_set_boolean(settings, "Editor", "word-wrap", state->word_wrap);
    g_key_file_set_boolean(settings, "Editor", "status-bar", state->show_status);
    g_key_file_set_boolean(settings, "Editor", "spell-enabled", state->spell_enabled);
    if (state->spell_language) g_key_file_set_string(settings, "Editor", "spell-language", state->spell_language);
    g_key_file_set_integer(settings, "Window", "width", state->window_width);
    g_key_file_set_integer(settings, "Window", "height", state->window_height);
    g_key_file_set_boolean(settings, "Window", "maximized", state->maximized);
    gtk_page_setup_to_key_file(state->printing.page_setup, settings, "Page Setup");
    g_key_file_set_string(settings, "Print", "header", state->printing.header);
    g_key_file_set_string(settings, "Print", "footer", state->printing.footer);
    gchar *path = preferences_path(), *directory = g_path_get_dirname(path);
    GError *error = NULL;
    if (g_mkdir_with_parents(directory, 0700) == 0 && !g_key_file_save_to_file(settings, path, &error)) {
        g_printerr("Could not save preferences: %s\n", error->message); g_clear_error(&error);
    }
    g_free(path); g_free(directory); g_key_file_unref(settings);
}

static void modified_changed(GtkTextBuffer *buffer, gpointer data) {
    AppState *state = data;
    if (!gtk_text_buffer_get_modified(buffer)) recovery_clear(state);
    update_title(state);
}

static void add_toggle(AppState *state, const char *name, gboolean initial, GCallback callback) {
    GSimpleAction *action = g_simple_action_new_stateful(name, NULL, g_variant_new_boolean(initial));
    g_signal_connect(action, "change-state", callback, state);
    g_action_map_add_action(G_ACTION_MAP(state->app), G_ACTION(action)); g_object_unref(action);
}

static GtkWidget *action_button(const char *label, const char *action) {
    GtkWidget *button = gtk_button_new_with_label(label);
    gtk_actionable_set_action_name(GTK_ACTIONABLE(button), action);
    return button;
}

static void build_search_bar(AppState *state) {
    state->find_bar = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_add_css_class(state->find_bar, "find-bar");
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    state->replace_toggle = gtk_toggle_button_new();
    update_replace_toggle(state->replace_toggle, FALSE);
    gtk_box_append(GTK_BOX(row), state->replace_toggle);
    state->find_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(state->find_entry), "Find");
    gtk_widget_set_hexpand(state->find_entry, TRUE);
    gtk_box_append(GTK_BOX(row), state->find_entry);
    gtk_box_append(GTK_BOX(row), action_button("Find Next", "app.find-next"));
    GtkWidget *close = gtk_button_new_with_label("Close");
    gtk_box_append(GTK_BOX(row), close);
    gtk_box_append(GTK_BOX(state->find_bar), row);
    state->replace_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    state->replace_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(state->replace_entry), "Replace with");
    gtk_widget_set_hexpand(state->replace_entry, TRUE);
    gtk_box_append(GTK_BOX(state->replace_row), state->replace_entry);
    GtkWidget *one = gtk_button_new_with_label("Replace"), *all = gtk_button_new_with_label("Replace All");
    gtk_box_append(GTK_BOX(state->replace_row), one); gtk_box_append(GTK_BOX(state->replace_row), all);
    gtk_box_append(GTK_BOX(state->find_bar), state->replace_row);
    GtkWidget *options = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    state->match_case = gtk_check_button_new_with_label("Match case");
    state->search_up = gtk_check_button_new_with_label("Search up");
    state->search_wrap = gtk_check_button_new_with_label("Wrap around");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(state->search_wrap), TRUE);
    gtk_box_append(GTK_BOX(options), state->match_case); gtk_box_append(GTK_BOX(options), state->search_up);
    gtk_box_append(GTK_BOX(options), state->search_wrap); gtk_box_append(GTK_BOX(state->find_bar), options);
    state->search_message = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(state->search_message), 0);
    gtk_box_append(GTK_BOX(state->find_bar), state->search_message);
    gtk_widget_set_visible(state->find_bar, FALSE);
    gtk_widget_set_visible(state->replace_row, FALSE);
    g_signal_connect(close, "clicked", G_CALLBACK(close_find), state);
    g_signal_connect(state->replace_toggle, "toggled", G_CALLBACK(toggle_replace), state);
    g_signal_connect(state->find_entry, "activate", G_CALLBACK(find_next), state);
    g_signal_connect(state->replace_entry, "activate", G_CALLBACK(replace_one), state);
    g_signal_connect(one, "clicked", G_CALLBACK(replace_one), state); g_signal_connect(all, "clicked", G_CALLBACK(replace_all), state);
    GtkEventController *keys = gtk_event_controller_key_new();
    g_signal_connect(keys, "key-pressed", G_CALLBACK(search_key), state);
    gtk_widget_add_controller(state->find_bar, keys);
}

static void remember_window(GObject *window, GParamSpec *spec, gpointer data) {
    (void)spec; AppState *state = data;
    gtk_window_get_default_size(GTK_WINDOW(window), &state->window_width, &state->window_height);
    state->maximized = gtk_window_is_maximized(GTK_WINDOW(window));
}

static gboolean file_dropped(GtkDropTarget *target, const GValue *value, double x, double y, gpointer data) {
    (void)target; (void)x; (void)y;
    AppState *state = data;
    if (state->busy || !G_VALUE_HOLDS(value, GDK_TYPE_FILE_LIST)) return FALSE;
    GSList *files = gdk_file_list_get_files(g_value_get_boxed(value));
    if (!files) return FALSE;
    if (files->next) {
        GError *error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                                           "Drop one file per Quillmote window.");
        show_error(state, "Could not open files", error); g_error_free(error); return TRUE;
    }
    gchar *path = g_file_get_path(files->data);
    if (!path) return FALSE;
    request_document_change(state, PENDING_PATH, path); g_free(path);
    return TRUE;
}

static void recovery_clear(AppState *state) {
    state->recovery_epoch++;
    state->recovery_revision = 0;
    state->recovered = FALSE;
    if (state->recovery_idle) { g_source_remove(state->recovery_idle); state->recovery_idle = 0; }
    if (state->snapshot) { g_string_free(state->snapshot, TRUE); state->snapshot = NULL; }
    if (state->recovery) g_unlink(state->recovery->path);
}

static void recovery_refresh(AppState *state) {
    if (!state->recovery_directory || !state->recovery_bar) return;
    gchar *path = recovery_find(state->recovery_directory);
    gtk_widget_set_visible(state->recovery_bar, path != NULL);
    g_free(path);
}

static void recover_clicked(GtkButton *button, gpointer data) {
    (void)button; AppState *state = data;
    if (state->busy) return;
    gchar *path = recovery_find(state->recovery_directory);
    if (path) request_document_change(state, PENDING_RECOVER, path);
    else recovery_refresh(state);
    g_free(path);
}

static gboolean startup_recovery(gpointer data) {
    AppState *state = data;
    state->recovery_start_idle = 0;
    recovery_refresh(state);
    if (!state->busy && !state->filename && !gtk_text_buffer_get_modified(GTK_TEXT_BUFFER(state->buffer)) &&
        gtk_text_buffer_get_char_count(GTK_TEXT_BUFFER(state->buffer)) == 0) recover_clicked(NULL, state);
    return G_SOURCE_REMOVE;
}

typedef struct {
    AppState *state;
    RecoveryDocument *document;
    gchar *path;
    guint64 epoch, revision;
} SnapshotJob;

static void snapshot_written(GObject *source, GAsyncResult *result, gpointer data) {
    (void)source; SnapshotJob *job = data;
    AppState *state = job->state;
    GError *error = NULL;
    gchar *temporary = g_task_propagate_pointer(G_TASK(result), &error);
    if (job->epoch == state->recovery_epoch) {
        if (temporary && g_rename(temporary, job->path) == 0) {
            state->recovery_revision = job->revision;
            state->recovery_failed = FALSE;
        } else {
            state->recovery_failed = TRUE;
            g_warning("Could not save recovery snapshot: %s", error ? error->message : g_strerror(errno));
        }
        update_status(state);
    }
    if (temporary) g_unlink(temporary);
    g_free(temporary); g_clear_error(&error);
    recovery_document_free(job->document); g_free(job->path); g_free(job);
    state->recovery_writing = FALSE;
    g_application_release(G_APPLICATION(state->app));
}

static void snapshot_thread(GTask *task, gpointer source, gpointer data, GCancellable *cancel) {
    (void)source; (void)cancel; SnapshotJob *job = data;
    GError *error = NULL;
    gchar *path = recovery_write_temporary(job->path, job->document, &error);
    if (path) g_task_return_pointer(task, path, g_free);
    else g_task_return_error(task, error);
}

static gboolean snapshot_chunk(gpointer data) {
    AppState *state = data;
    if (state->snapshot_revision != state->document_revision || state->loading) {
        g_string_free(state->snapshot, TRUE); state->snapshot = NULL;
        state->recovery_idle = 0; return G_SOURCE_REMOVE;
    }
    GtkTextBuffer *buffer = GTK_TEXT_BUFFER(state->buffer);
    GtkTextIter start, end;
    gtk_text_buffer_get_iter_at_offset(buffer, &start, state->snapshot_offset);
    end = start; gtk_text_iter_forward_chars(&end, 32768);
    gchar *chunk = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
    g_string_append(state->snapshot, chunk); g_free(chunk);
    state->snapshot_offset = gtk_text_iter_get_offset(&end);
    if (!gtk_text_iter_is_end(&end)) return G_SOURCE_CONTINUE;
    SnapshotJob *job = g_new0(SnapshotJob, 1);
    job->state = state; job->epoch = state->recovery_epoch; job->revision = state->snapshot_revision;
    job->path = g_strdup(state->recovery->path);
    job->document = g_new0(RecoveryDocument, 1);
    job->document->text = g_string_free(state->snapshot, FALSE); state->snapshot = NULL;
    job->document->filename = g_strdup(state->filename);
    job->document->encoding = state->encoding; job->document->ending = state->ending;
    gtk_text_buffer_get_iter_at_mark(buffer, &start, gtk_text_buffer_get_insert(buffer));
    job->document->cursor = gtk_text_iter_get_offset(&start);
    state->recovery_idle = 0; state->recovery_writing = TRUE;
    g_application_hold(G_APPLICATION(state->app));
    GTask *task = g_task_new(NULL, NULL, snapshot_written, job);
    g_task_set_task_data(task, job, NULL); g_task_run_in_thread(task, snapshot_thread); g_object_unref(task);
    return G_SOURCE_REMOVE;
}

static gboolean recovery_tick(gpointer data) {
    AppState *state = data;
    if (state->loading || state->recovery_writing || state->recovery_idle || !state->recovery ||
        !gtk_text_buffer_get_modified(GTK_TEXT_BUFFER(state->buffer)) || state->document_revision == state->recovery_revision)
        return G_SOURCE_CONTINUE;
    state->snapshot_revision = state->document_revision; state->snapshot_offset = 0;
    state->snapshot = g_string_new(NULL);
    state->recovery_idle = g_idle_add(snapshot_chunk, state);
    return G_SOURCE_CONTINUE;
}

static void activate(GtkApplication *app, gpointer data) {
    AppState *state = data;
    if (state->window) { gtk_window_present(GTK_WINDOW(state->window)); return; }
    state->app = app;
    load_preferences(state);
    enchant_setup(state);
    state->window = gtk_application_window_new(app);
    g_signal_connect(state->window, "close-request", G_CALLBACK(close_requested), state);
    gtk_window_set_default_size(GTK_WINDOW(state->window), state->window_width, state->window_height);
    if (state->maximized) gtk_window_maximize(GTK_WINDOW(state->window));
    g_signal_connect(state->window, "notify::default-width", G_CALLBACK(remember_window), state);
    g_signal_connect(state->window, "notify::default-height", G_CALLBACK(remember_window), state);
    g_signal_connect(state->window, "notify::maximized", G_CALLBACK(remember_window), state);
    gtk_window_set_titlebar(GTK_WINDOW(state->window), gtk_header_bar_new());
    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_window_set_child(GTK_WINDOW(state->window), root);

    GMenu *file = g_menu_new();
    g_menu_append(file, "_New", "app.new"); g_menu_append(file, "_Open...", "app.open");
    g_menu_append(file, "_Save", "app.save"); g_menu_append(file, "Save _As...", "app.save-as");
    g_menu_append(file, "Page Set_up...", "app.page-setup"); g_menu_append(file, "_Print...", "app.print"); g_menu_append(file, "E_xit", "app.quit");
    GMenu *edit = g_menu_new();
    g_menu_append(edit, "_Undo", "app.undo"); g_menu_append(edit, "_Redo", "app.redo");
    g_menu_append(edit, "Cu_t", "app.cut"); g_menu_append(edit, "_Copy", "app.copy"); g_menu_append(edit, "_Paste", "app.paste");
    g_menu_append(edit, "De_lete", "app.delete"); g_menu_append(edit, "_Find...", "app.find");
    g_menu_append(edit, "Find _Next", "app.find-next"); g_menu_append(edit, "R_eplace...", "app.replace");
    g_menu_append(edit, "_Go To...", "app.go-to"); g_menu_append(edit, "Select _All", "app.select-all"); g_menu_append(edit, "Time/_Date", "app.time-date");
    GMenu *format = g_menu_new(); g_menu_append(format, "_Word Wrap", "app.wrap"); g_menu_append(format, "_Font...", "app.font");
    GMenu *spelling = g_menu_new(), *languages = g_menu_new();
    g_menu_append(spelling, "Check Spelling", "app.spell-enabled");
    for (guint i = 0; i < state->spell_languages->len; i++) {
        const char *tag = g_ptr_array_index(state->spell_languages, i);
        gchar *label = g_strdup(tag); g_strdelimit(label, "_", '-');
        GMenuItem *item = g_menu_item_new(label, NULL); g_free(label);
        g_menu_item_set_action_and_target(item, "app.spell-language", "s", tag);
        g_menu_append_item(languages, item); g_object_unref(item);
    }
    if (!state->spell_languages->len) g_menu_append(languages, "No dictionaries installed", NULL);
    g_menu_append_submenu(spelling, "Language", G_MENU_MODEL(languages));
    g_menu_append_submenu(format, "Spelling", G_MENU_MODEL(spelling));
    g_object_unref(spelling); g_object_unref(languages);
    GMenu *view = g_menu_new(); g_menu_append(view, "_Status Bar", "app.status-bar"); g_menu_append(view, "_Right-to-Left Reading Order", "app.rtl");
    GMenu *zoom = g_menu_new();
    g_menu_append(zoom, "Zoom In", "app.zoom-in"); g_menu_append(zoom, "Zoom Out", "app.zoom-out");
    g_menu_append(zoom, "Reset Zoom", "app.zoom-reset");
    g_menu_append_submenu(view, "Zoom", G_MENU_MODEL(zoom)); g_object_unref(zoom);
    GMenu *help = g_menu_new(); g_menu_append(help, "View _Help", "app.help"); g_menu_append(help, "_About Quillmote", "app.about");
    GMenu *menubar = g_menu_new();
    g_menu_append_submenu(menubar, "_File", G_MENU_MODEL(file)); g_menu_append_submenu(menubar, "_Edit", G_MENU_MODEL(edit));
    g_menu_append_submenu(menubar, "F_ormat", G_MENU_MODEL(format)); g_menu_append_submenu(menubar, "_View", G_MENU_MODEL(view)); g_menu_append_submenu(menubar, "_Help", G_MENU_MODEL(help));
    GtkWidget *menu_bar = gtk_popover_menu_bar_new_from_model(G_MENU_MODEL(menubar));
    configure_menu_popovers(menu_bar);
    gtk_box_append(GTK_BOX(root), menu_bar);
    GtkEventController *escape = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(escape, GTK_PHASE_CAPTURE);
    g_signal_connect(escape, "key-pressed", G_CALLBACK(menu_escape), state);
    gtk_widget_add_controller(state->window, escape);
    g_object_unref(file); g_object_unref(edit); g_object_unref(format); g_object_unref(view); g_object_unref(help); g_object_unref(menubar);

    state->loading_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    state->loading_label = gtk_label_new("Loading…");
    gtk_box_append(GTK_BOX(state->loading_bar), state->loading_label);
    GtkWidget *cancel = gtk_button_new_with_label("Cancel");
    gtk_box_append(GTK_BOX(state->loading_bar), cancel);
    g_signal_connect(cancel, "clicked", G_CALLBACK(cancel_load), state);
    gtk_box_append(GTK_BOX(root), state->loading_bar); gtk_widget_set_visible(state->loading_bar, FALSE);
    state->recovery_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_append(GTK_BOX(state->recovery_bar), gtk_label_new("Unsaved work from a previous session is available."));
    GtkWidget *recover = gtk_button_new_with_label("Recover");
    gtk_box_append(GTK_BOX(state->recovery_bar), recover);
    g_signal_connect(recover, "clicked", G_CALLBACK(recover_clicked), state);
    gtk_box_append(GTK_BOX(root), state->recovery_bar); gtk_widget_set_visible(state->recovery_bar, FALSE);

    state->buffer = gtk_source_buffer_new(NULL);
    state->css = gtk_css_provider_new();
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(state->css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    state->view = GTK_SOURCE_VIEW(gtk_source_view_new_with_buffer(state->buffer));
    gtk_widget_add_css_class(GTK_WIDGET(state->view), "quillmote-editor");
    GtkDropTarget *drop = gtk_drop_target_new(GDK_TYPE_FILE_LIST, GDK_ACTION_COPY);
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(drop), GTK_PHASE_CAPTURE);
    g_signal_connect(drop, "drop", G_CALLBACK(file_dropped), state);
    gtk_widget_add_controller(state->window, GTK_EVENT_CONTROLLER(drop));
    gtk_source_view_set_show_line_numbers(state->view, FALSE);
    gtk_source_view_set_highlight_current_line(state->view, TRUE);
    gtk_source_view_set_tab_width(state->view, 8);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(state->view), state->word_wrap ? GTK_WRAP_WORD_CHAR : GTK_WRAP_NONE);
    state->scroller = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(state->scroller), GTK_WIDGET(state->view));
    gtk_widget_set_vexpand(state->scroller, TRUE);
    build_search_bar(state);
    gtk_box_append(GTK_BOX(root), state->find_bar);
    gtk_box_append(GTK_BOX(root), state->scroller);

    state->spelling_menu = g_menu_new();
    GMenu *context = g_menu_new(), *direction = g_menu_new();
    g_menu_append_section(context, NULL, G_MENU_MODEL(state->spelling_menu));
    g_menu_append(direction, "Right-to-Left Reading Order", "app.rtl");
    g_menu_append_section(context, NULL, G_MENU_MODEL(direction));
    gtk_text_view_set_extra_menu(GTK_TEXT_VIEW(state->view), G_MENU_MODEL(context));
    g_object_unref(context); g_object_unref(direction);
    GtkGesture *context_click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(context_click), GDK_BUTTON_SECONDARY);
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(context_click), GTK_PHASE_CAPTURE);
    g_signal_connect(context_click, "pressed", G_CALLBACK(spelling_context_pressed), state);
    gtk_widget_add_controller(GTK_WIDGET(state->view), GTK_EVENT_CONTROLLER(context_click));
    GtkEventController *context_key = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(context_key, GTK_PHASE_CAPTURE);
    g_signal_connect(context_key, "key-pressed", G_CALLBACK(spelling_context_key), state);
    gtk_widget_add_controller(GTK_WIDGET(state->view), context_key);
    GSimpleAction *replace = g_simple_action_new("replace-spelling", G_VARIANT_TYPE("(tiiss)"));
    g_object_set_data(G_OBJECT(replace), "quillmote-state", state);
    g_signal_connect(replace, "activate", G_CALLBACK(replace_spelling), NULL);
    g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(replace)); g_object_unref(replace);
    GdkRGBA spell_color; gdk_rgba_parse(&spell_color, "#e33b3b");
    gtk_text_buffer_create_tag(GTK_TEXT_BUFFER(state->buffer), "misspelled", "underline", PANGO_UNDERLINE_ERROR, "underline-rgba", &spell_color, NULL);
    state->status = gtk_label_new(NULL);
    gtk_widget_add_css_class(state->status, "quillmote-status"); gtk_label_set_xalign(GTK_LABEL(state->status), 0);
    gtk_box_append(GTK_BOX(root), state->status); gtk_widget_set_visible(state->status, state->show_status);

    const struct { const char *name; GCallback callback; const char *shortcut; } actions[] = {
        {"zoom-in", G_CALLBACK(action_zoom), "<Primary>plus"},
        {"zoom-out", G_CALLBACK(action_zoom), "<Primary>minus"},
        {"zoom-reset", G_CALLBACK(action_zoom), "<Primary>0"},
        {"new", G_CALLBACK(action_new), "<Primary>n"}, {"open", G_CALLBACK(action_open), "<Primary>o"},
        {"save", G_CALLBACK(action_save), "<Primary>s"}, {"save-as", G_CALLBACK(action_save_as), "<Primary><Shift>s"},
        {"quit", G_CALLBACK(action_quit), "<Primary>q"}, {"cut", G_CALLBACK(action_cut), NULL},
        {"copy", G_CALLBACK(action_copy), NULL}, {"paste", G_CALLBACK(action_paste), NULL},
        {"select-all", G_CALLBACK(action_select_all), NULL}, {"undo", G_CALLBACK(action_undo), NULL},
        {"redo", G_CALLBACK(action_redo), NULL}, {"delete", G_CALLBACK(action_delete), NULL},
        {"find", G_CALLBACK(action_find), "<Primary>f"}, {"find-next", G_CALLBACK(action_find_next), "F3"},
        {"replace", G_CALLBACK(action_replace), "<Primary>h"}, {"go-to", G_CALLBACK(action_go_to), "<Primary>g"},
        {"time-date", G_CALLBACK(action_time_date), "F5"}, {"font", G_CALLBACK(action_font), NULL},
        {"page-setup", G_CALLBACK(action_page_setup), NULL}, {"print", G_CALLBACK(action_print), "<Primary>p"},
        {"help", G_CALLBACK(action_help), "F1"}, {"about", G_CALLBACK(action_about), NULL}
    };
    for (guint i = 0; i < G_N_ELEMENTS(actions); i++) {
        add_action(state, actions[i].name, actions[i].callback);
        if (actions[i].shortcut) {
            gchar *name = g_strdup_printf("app.%s", actions[i].name);
            const char *keys[] = {actions[i].shortcut, NULL};
            gtk_application_set_accels_for_action(app, name, keys); g_free(name);
        }
    }
    const char *zoom_in_keys[] = {"<Primary>plus", "<Primary>equal", "<Primary>KP_Add", NULL};
    const char *zoom_out_keys[] = {"<Primary>minus", "<Primary>KP_Subtract", NULL};
    const char *zoom_reset_keys[] = {"<Primary>0", "<Primary>KP_0", NULL};
    gtk_application_set_accels_for_action(app, "app.zoom-in", zoom_in_keys);
    gtk_application_set_accels_for_action(app, "app.zoom-out", zoom_out_keys);
    gtk_application_set_accels_for_action(app, "app.zoom-reset", zoom_reset_keys);
    add_toggle(state, "spell-enabled", state->spell_enabled, G_CALLBACK(set_spelling_state));
    GSimpleAction *language = g_simple_action_new_stateful("spell-language", G_VARIANT_TYPE_STRING,
        g_variant_new_string(state->spell_language ? state->spell_language : ""));
    g_signal_connect(language, "change-state", G_CALLBACK(set_language_state), state);
    g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(language)); g_object_unref(language);
    const char *word_actions[] = {"spelling-add", "spelling-ignore"};
    for (int i = 0; i < 2; i++) {
        GSimpleAction *word_action = g_simple_action_new(word_actions[i], G_VARIANT_TYPE("(ts)"));
        g_object_set_data(G_OBJECT(word_action), "quillmote-state", state);
        g_signal_connect(word_action, "activate", G_CALLBACK(spelling_word_action), NULL);
        g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(word_action)); g_object_unref(word_action);
    }
    add_toggle(state, "wrap", state->word_wrap, G_CALLBACK(set_wrap_state));
    add_toggle(state, "status-bar", state->show_status, G_CALLBACK(set_status_state));
    add_toggle(state, "rtl", FALSE, G_CALLBACK(set_rtl_state));
    g_signal_connect(state->buffer, "changed", G_CALLBACK(schedule_spell_scan), state);
    g_signal_connect(state->buffer, "modified-changed", G_CALLBACK(modified_changed), state);
    g_signal_connect(state->buffer, "mark-set", G_CALLBACK(cursor_moved), state);
    GtkSettings *settings = gtk_settings_get_default();
#if GTK_CHECK_VERSION(4, 20, 0)
    g_signal_connect(settings, "notify::gtk-interface-color-scheme", G_CALLBACK(system_theme_changed), state);
#endif
    g_signal_connect(settings, "notify::gtk-theme-name", G_CALLBACK(system_theme_changed), state);
    g_signal_connect(settings, "notify::gtk-application-prefer-dark-theme", G_CALLBACK(system_theme_changed), state);
    system_theme_changed(settings, NULL, state);
    update_title(state); update_status(state);
    gtk_window_present(GTK_WINDOW(state->window)); gtk_widget_grab_focus(GTK_WIDGET(state->view));
    state->recovery_directory = g_build_filename(g_get_user_config_dir(), "quillmote", "recovery", NULL);
    GError *recovery_error = NULL;
    state->recovery = recovery_session_new(state->recovery_directory, &recovery_error);
    if (!state->recovery) {
        state->recovery_failed = TRUE; update_status(state);
        g_warning("Crash recovery unavailable: %s", recovery_error->message); g_clear_error(&recovery_error);
    }
    state->recovery_timer = g_timeout_add_seconds(2, recovery_tick, state);
    state->recovery_start_idle = g_idle_add(startup_recovery, state);
}

static void open_files(GApplication *application, GFile **files, gint count, const gchar *hint, gpointer data) {
    (void)hint;
    AppState *state = data;
    activate(GTK_APPLICATION(application), state);
    if (count != 1) {
        GError *error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "Open one document per Quillmote window.");
        show_error(state, "Could not open files", error); g_error_free(error); return;
    }
    gchar *path = g_file_get_path(files[0]);
    if (path) request_document_change(state, PENDING_CREATE, path);
    else {
        GError *error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "Choose a file on a local or mounted drive.");
        show_error(state, "Could not open file", error); g_error_free(error);
    }
    g_free(path);
}

static void shutdown_app(GApplication *application, gpointer data) {
    (void)application; AppState *state = data;
    save_preferences(state);
    if (state->recovery_start_idle) g_source_remove(state->recovery_start_idle);
    if (state->recovery_timer) g_source_remove(state->recovery_timer);
    if (state->recovery_idle) g_source_remove(state->recovery_idle);
    if (state->snapshot) g_string_free(state->snapshot, TRUE);
    recovery_session_free(state->recovery); g_free(state->recovery_directory);
    g_clear_pointer(&state->spell_languages, g_ptr_array_unref); g_free(state->spell_language);
    print_options_clear(&state->printing);
    g_clear_pointer(&state->pending_path, g_free);
    g_signal_handlers_disconnect_by_data(gtk_settings_get_default(), state);
    if (state->spell_idle) g_source_remove(state->spell_idle);
    g_clear_object(&state->spelling_menu);
    if (state->broker_free_dict && state->dictionary) state->broker_free_dict(state->broker, state->dictionary);
    if (state->broker_free && state->broker) state->broker_free(state->broker);
    if (state->enchant_library) dlclose(state->enchant_library);
    if (state->css) gtk_style_context_remove_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(state->css));
    g_clear_object(&state->css);
    g_clear_object(&state->buffer);
    g_clear_pointer(&state->font, pango_font_description_free);
    g_free(state->filename);
}

int main(int argc, char **argv) {
    g_set_application_name("Quillmote");
    AppState state = {0};
    GtkApplication *app = gtk_application_new("com.example.Quillmote", G_APPLICATION_HANDLES_OPEN | G_APPLICATION_NON_UNIQUE);
    g_signal_connect(app, "open", G_CALLBACK(open_files), &state);
    g_signal_connect(app, "activate", G_CALLBACK(activate), &state); g_signal_connect(app, "shutdown", G_CALLBACK(shutdown_app), &state);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app); return status;
}
