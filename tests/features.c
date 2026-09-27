#define main quillmote_main
#include "../main.c"
#undef main

#include "ui.h"

static void wait_for_load(AppState *state) {
    gint64 deadline = g_get_monotonic_time() + 20 * G_TIME_SPAN_SECOND;
    while (state->loading && g_get_monotonic_time() < deadline) {
        g_main_context_iteration(NULL, FALSE); g_usleep(1000);
    }
    g_assert_null(state->loading);
}

static void assert_text(AppState *state, const char *expected) {
    gchar *actual = buffer_text(state);
    g_assert_cmpstr(actual, ==, expected); g_free(actual);
}

static void caret(AppState *state, int offset) {
    GtkTextIter iter;
    gtk_text_buffer_get_iter_at_offset(GTK_TEXT_BUFFER(state->buffer), &iter, offset);
    gtk_text_buffer_place_cursor(GTK_TEXT_BUFFER(state->buffer), &iter);
}

static int selection_start(AppState *state) {
    GtkTextIter start, end;
    g_assert_true(gtk_text_buffer_get_selection_bounds(GTK_TEXT_BUFFER(state->buffer), &start, &end));
    return gtk_text_iter_get_offset(&start);
}

static void check_search(AppState *state) {
    GtkTextBuffer *buffer = GTK_TEXT_BUFFER(state->buffer);
    gtk_text_buffer_set_text(buffer, "café Cat cat\ncat", -1);
    caret(state, 0);
    gtk_editable_set_text(GTK_EDITABLE(state->find_entry), "cat");
    show_find(state, FALSE);
    g_assert_true(find_text(state)); g_assert_cmpint(selection_start(state), ==, 5);
    /* Expand Replace after a case-insensitive match without adopting its casing. */
    g_signal_emit_by_name(state->replace_toggle, "clicked");
    gtk_editable_set_text(GTK_EDITABLE(state->replace_entry), "dog");
    g_assert_true(gtk_widget_get_visible(state->replace_row));
    g_assert_true(gtk_widget_is_ancestor(gtk_window_get_focus(GTK_WINDOW(state->window)), state->replace_entry));
    g_assert_cmpstr(gtk_editable_get_text(GTK_EDITABLE(state->find_entry)), ==, "cat");
    g_assert_cmpint(selection_start(state), ==, 5);
    g_action_group_activate_action(G_ACTION_GROUP(state->app), "find", NULL);
    g_assert_false(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(state->replace_toggle)));
    g_assert_false(gtk_widget_get_visible(state->replace_row));
    g_action_group_activate_action(G_ACTION_GROUP(state->app), "replace", NULL);
    g_assert_true(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(state->replace_toggle)));
    g_assert_true(gtk_widget_get_visible(state->replace_row));
    g_assert_cmpstr(gtk_editable_get_text(GTK_EDITABLE(state->find_entry)), ==, "cat");
    g_assert_cmpstr(gtk_editable_get_text(GTK_EDITABLE(state->replace_entry)), ==, "dog");
    replace_one(NULL, state);
    assert_text(state, "café dog cat\ncat");
    gtk_text_buffer_undo(buffer);
    caret(state, 0);
    g_assert_true(find_text(state)); g_assert_cmpint(selection_start(state), ==, 5);
    g_assert_true(find_text(state)); g_assert_cmpint(selection_start(state), ==, 9);
    gtk_check_button_set_active(GTK_CHECK_BUTTON(state->search_up), TRUE);
    g_assert_true(find_text(state)); g_assert_cmpint(selection_start(state), ==, 5);
    gtk_check_button_set_active(GTK_CHECK_BUTTON(state->match_case), TRUE);
    gtk_check_button_set_active(GTK_CHECK_BUTTON(state->search_wrap), FALSE);
    g_assert_false(find_text(state));
    gtk_check_button_set_active(GTK_CHECK_BUTTON(state->search_wrap), TRUE);
    g_assert_true(find_text(state)); g_assert_cmpint(selection_start(state), ==, 13);
    gtk_check_button_set_active(GTK_CHECK_BUTTON(state->search_up), FALSE);
    gtk_editable_set_text(GTK_EDITABLE(state->replace_entry), "dog");
    replace_one(NULL, state);
    assert_text(state, "café Cat cat\ndog");
    gtk_text_buffer_undo(buffer); assert_text(state, "café Cat cat\ncat");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(state->match_case), FALSE);
    gtk_editable_set_text(GTK_EDITABLE(state->replace_entry), "catcat");
    replace_all(NULL, state);
    assert_text(state, "café catcat catcat\ncatcat");
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(state->search_message)), ==, "Replaced 3 occurrences.");
    gtk_text_buffer_undo(buffer); assert_text(state, "café Cat cat\ncat");
    gtk_editable_set_text(GTK_EDITABLE(state->replace_entry), "");
    replace_all(NULL, state); assert_text(state, "café  \n");
    gtk_text_buffer_undo(buffer);
    gtk_editable_set_text(GTK_EDITABLE(state->find_entry), "");
    replace_all(NULL, state); assert_text(state, "café Cat cat\ncat");
    g_assert_true(go_to_line(state, 2)); g_assert_false(go_to_line(state, 3)); g_assert_false(go_to_line(state, 0));
    GtkTextIter iter;
    gtk_text_buffer_get_iter_at_mark(buffer, &iter, gtk_text_buffer_get_insert(buffer));
    g_assert_cmpint(gtk_text_iter_get_line(&iter), ==, 1);
    g_action_group_activate_action(G_ACTION_GROUP(state->app), "time-date", NULL);
    gchar *text = buffer_text(state);
    g_assert_cmpuint(strlen(text), >, strlen("café Cat cat\ncat")); g_free(text);
    gtk_text_buffer_undo(buffer); assert_text(state, "café Cat cat\ncat");
    caret(state, 3); /* Delete the entire accented character. */
    g_action_group_activate_action(G_ACTION_GROUP(state->app), "delete", NULL);
    assert_text(state, "caf Cat cat\ncat"); gtk_text_buffer_undo(buffer);
    g_action_group_change_action_state(G_ACTION_GROUP(state->app), "status-bar", g_variant_new_boolean(FALSE));
    g_assert_false(gtk_widget_get_visible(state->status));
    g_action_group_change_action_state(G_ACTION_GROUP(state->app), "wrap", g_variant_new_boolean(FALSE));
    g_assert_cmpint(gtk_text_view_get_wrap_mode(GTK_TEXT_VIEW(state->view)), ==, GTK_WRAP_NONE);
    g_action_group_change_action_state(G_ACTION_GROUP(state->app), "rtl", g_variant_new_boolean(TRUE));
    g_assert_cmpint(gtk_widget_get_direction(GTK_WIDGET(state->view)), ==, GTK_TEXT_DIR_RTL);
    g_action_group_change_action_state(G_ACTION_GROUP(state->app), "rtl", g_variant_new_boolean(FALSE));
    g_action_group_activate_action(G_ACTION_GROUP(state->app), "go-to", NULL);
}

static GtkWidget *find_button(GtkWidget *widget, const char *label) {
    if (GTK_IS_BUTTON(widget)) {
        const char *text = gtk_button_get_label(GTK_BUTTON(widget));
        GString *plain = g_string_new(NULL);
        for (const char *p = text ? text : ""; *p; p++) if (*p != '_') g_string_append_c(plain, *p);
        gboolean matches = g_str_equal(plain->str, label);
        g_string_free(plain, TRUE);
        if (matches) return widget;
    }
    for (GtkWidget *child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child)) {
        GtkWidget *found = find_button(child, label);
        if (found) return found;
    }
    return NULL;
}

static void answer_prompt(AppState *state, const char *label) {
    flush_events();
    GListModel *windows = gtk_window_get_toplevels();
    gboolean clicked = FALSE;
    for (guint i = 0; i < g_list_model_get_n_items(windows); i++) {
        GtkWidget *window = g_list_model_get_item(windows, i);
        GtkWidget *button = window != state->window ? find_button(window, label) : NULL;
        if (button) { g_signal_emit_by_name(button, "clicked"); clicked = TRUE; }
        g_object_unref(window);
        if (clicked) break;
    }
    if (!clicked) g_error("Could not find dialog button: %s", label);
    flush_events();
}

static void check_file_lifecycle(AppState *state, const char *directory) {
    GtkTextBuffer *buffer = GTK_TEXT_BUFFER(state->buffer);
    gchar *path = g_build_filename(directory, "document.txt", NULL);
    gtk_text_buffer_set_text(buffer, "saved café\n", -1);
    g_assert_true(save_contents(state, path, ENCODING_UTF16_LE, ENDING_CRLF));
    g_assert_false(gtk_text_buffer_get_modified(buffer));
    gtk_text_buffer_set_text(buffer, "unsaved edit", -1);
    request_document_change(state, PENDING_NEW);
    g_assert_true(state->busy); answer_prompt(state, "Cancel");
    assert_text(state, "unsaved edit"); g_assert_false(state->busy);
    request_document_change(state, PENDING_NEW); answer_prompt(state, "Save");
    assert_text(state, ""); g_assert_null(state->filename);
    g_assert_true(load_file(state, path, -1)); wait_for_load(state); buffer = GTK_TEXT_BUFFER(state->buffer); assert_text(state, "unsaved edit");
    g_assert_cmpint(state->encoding, ==, ENCODING_UTF16_LE);
    g_assert_false(gtk_text_buffer_get_can_undo(buffer));
    /* Failed conversion must not overwrite the file or continue New. */
    state->encoding = ENCODING_ANSI;
    gtk_text_buffer_set_text(buffer, "日本語", -1);
    request_document_change(state, PENDING_NEW); answer_prompt(state, "Save");
    assert_text(state, "日本語"); g_assert_cmpstr(state->filename, ==, path);
    g_assert_cmpint(state->pending, ==, PENDING_NONE);
    answer_prompt(state, "Close");
    g_assert_true(load_file(state, path, -1)); wait_for_load(state); buffer = GTK_TEXT_BUFFER(state->buffer); assert_text(state, "unsaved edit");
    gtk_text_buffer_set_text(buffer, "keep this", -1);
    g_action_group_activate_action(G_ACTION_GROUP(state->app), "save-as", NULL);
    g_assert_nonnull(state->file_dialog);
    flush_events(); /* Allow the native dialog's window export to finish before cancellation. */
    g_signal_emit_by_name(state->file_dialog, "response", GTK_RESPONSE_CANCEL);
    flush_events();
    g_assert_cmpstr(state->filename, ==, path); assert_text(state, "keep this");
    request_document_change(state, PENDING_NEW); answer_prompt(state, "Don't Save");
    assert_text(state, ""); g_assert_null(state->filename);
    gtk_text_buffer_set_text(buffer, "unnamed changes", -1);
    request_document_change(state, PENDING_NEW); answer_prompt(state, "Save");
    g_assert_nonnull(state->file_dialog); flush_events();
    g_signal_emit_by_name(state->file_dialog, "response", GTK_RESPONSE_CANCEL); flush_events();
    assert_text(state, "unnamed changes"); g_assert_cmpint(state->pending, ==, PENDING_NONE);
    gchar *log = g_build_filename(directory, "log.txt", NULL);
    GError *error = NULL;
    g_assert_true(g_file_set_contents(log, ".LOG\r\n", -1, &error)); g_assert_no_error(error);
    g_assert_true(load_file(state, log, -1)); wait_for_load(state); buffer = GTK_TEXT_BUFFER(state->buffer);
    gchar *text = buffer_text(state);
    g_assert_true(g_str_has_prefix(text, ".LOG\n"));
    g_assert_true(g_str_has_suffix(text, "\n")); g_assert_cmpuint(strlen(text), >, 10);
    g_free(text); g_assert_true(gtk_text_buffer_get_modified(buffer));
    g_assert_cmpint(state->ending, ==, ENDING_CRLF);
    /* A window-manager close gets the same cancelable save prompt. */
    gtk_window_close(GTK_WINDOW(state->window)); answer_prompt(state, "Cancel");
    g_assert_true(gtk_widget_get_visible(state->window));
    g_free(log); g_free(path);
}

static void check_printing(AppState *state, const char *directory) {
    GDateTime *now = g_date_time_new_local(2026, 9, 22, 10, 30, 0);
    gchar **parts = format_print_band("&l&f&cPage &p&r&& &d &t", "notes%#N.txt", 3, now);
    g_assert_cmpstr(parts[0], ==, "notes%#N.txt"); g_assert_cmpstr(parts[1], ==, "Page 3");
    g_assert_true(g_str_has_prefix(parts[2], "& ")); g_strfreev(parts); g_date_time_unref(now);
    GString *text = g_string_new("Printing regression\n");
    for (int i = 1; i <= 150; i++) g_string_append_printf(text, "Line %03d: café and plain black text.\n", i);
    gtk_text_buffer_set_text(GTK_TEXT_BUFFER(state->buffer), text->str, -1); g_string_free(text, TRUE);
    gchar *path = g_build_filename(directory, "printed.pdf", NULL);
    GError *error = NULL;
    g_assert_true(print_document(GTK_WINDOW(state->window), state->view, state->font, "printing.txt", &state->printing, path, &error));
    g_assert_no_error(error);
    gchar *pdf; gsize size;
    g_assert_true(g_file_get_contents(path, &pdf, &size, &error)); g_assert_no_error(error);
    g_assert_true(g_str_has_prefix(pdf, "%PDF-")); g_assert_cmpuint(size, >, 1000); g_free(pdf);
    g_print("PDF output: %s\n", path); g_free(path);
}

static void check_arbitrary_file(AppState *state, const char *directory) {
    gchar input[256];
    for (int i = 0; i < 256; i++) input[i] = i;
    gchar *path = g_build_filename(directory, "arbitrary.bin", NULL);
    GError *error = NULL;
    g_assert_true(g_file_set_contents(path, input, sizeof input, &error)); g_assert_no_error(error);
    g_assert_true(load_file(state, path, -1)); wait_for_load(state);
    g_assert_cmpint(state->encoding, ==, ENCODING_BYTES);
    g_assert_cmpint(gtk_text_buffer_get_char_count(GTK_TEXT_BUFFER(state->buffer)), ==, 256);
    g_assert_true(save_contents(state, path, state->encoding, state->ending));
    gchar *saved; gsize length;
    g_assert_true(g_file_get_contents(path, &saved, &length, &error)); g_assert_no_error(error);
    g_assert_cmpmem(saved, length, input, sizeof input); g_free(saved);
    caret(state, 65);
    g_action_group_activate_action(G_ACTION_GROUP(state->app), "delete", NULL);
    gtk_text_buffer_insert_at_cursor(GTK_TEXT_BUFFER(state->buffer), "Z", -1);
    input[65] = 'Z';
    g_assert_true(save_contents(state, path, state->encoding, state->ending));
    g_assert_true(g_file_get_contents(path, &saved, &length, &error)); g_assert_no_error(error);
    g_assert_cmpmem(saved, length, input, sizeof input); g_free(saved);
    flush_events();
    GtkTextIter iter;
    gtk_text_buffer_get_start_iter(GTK_TEXT_BUFFER(state->buffer), &iter);
    g_assert_false(gtk_text_iter_has_tag(&iter, gtk_text_tag_table_lookup(gtk_text_buffer_get_tag_table(GTK_TEXT_BUFFER(state->buffer)), "misspelled")));
    g_free(path);
    request_document_change(state, PENDING_NEW);
    g_assert_cmpint(state->encoding, ==, ENCODING_UTF8);
}


int main(void) {
    GError *error = NULL;
    gchar *directory = g_dir_make_tmp("quillmote-features-XXXXXX", &error); g_assert_no_error(error);
    g_setenv("XDG_CONFIG_HOME", directory, TRUE);
    gchar *settings_directory = g_build_filename(directory, "quillmote", NULL);
    g_assert_cmpint(g_mkdir_with_parents(settings_directory, 0700), ==, 0);
    gchar *settings_path = g_build_filename(settings_directory, "settings.ini", NULL);
    g_assert_true(g_file_set_contents(settings_path, "[Editor]\nfont=Monospace 12\n[Print]\nheader=Notes &f\n", -1, &error));
    g_assert_no_error(error); g_free(settings_directory); g_free(settings_path);
    gtk_init();
    AppState state = {0};
    GtkApplication *app = gtk_application_new(QUILLMOTE_APP_ID ".FeatureTest", G_APPLICATION_NON_UNIQUE);
    g_assert_true(g_application_register(G_APPLICATION(app), NULL, &error)); g_assert_no_error(error);
    activate(app, &state); flush_events();
    g_assert_cmpstr(state.printing.header, ==, "Notes &f");
    g_assert_nonnull(strstr(gtk_window_get_title(GTK_WINDOW(state.window)), "Quillmote"));
    check_search(&state);
    answer_prompt(&state, "Go To");
    g_action_group_activate_action(G_ACTION_GROUP(app), "page-setup", NULL);
    answer_prompt(&state, "OK");
    check_file_lifecycle(&state, directory);
    check_arbitrary_file(&state, directory);
    check_printing(&state, directory);
    save_preferences(&state);
    AppState restored = {0}; load_preferences(&restored);
    g_assert_false(restored.word_wrap); g_assert_false(restored.show_status);
    g_assert_true(pango_font_description_equal(restored.font, state.font));
    g_free(restored.spell_language); pango_font_description_free(restored.font); print_options_clear(&restored.printing);
    gtk_text_buffer_set_text(GTK_TEXT_BUFFER(state.buffer), "Quillmote\n\nA native text editor with system appearance.\nFind and replace, go to a line, or press F5 for the time and date.\nRight-click an underlined word for spelling suggestions.", -1);
    g_action_group_change_action_state(G_ACTION_GROUP(app), "status-bar", g_variant_new_boolean(TRUE));
    gtk_editable_set_text(GTK_EDITABLE(state.find_entry), "text");
    show_find(&state, TRUE);
    capture_window(GTK_WINDOW(state.window), directory, "quillmote.png");
    close_find(NULL, &state);
    flush_events();
    while (state.recovery_writing || state.recovery_idle) g_main_context_iteration(NULL, TRUE);
    gtk_window_destroy(GTK_WINDOW(state.window)); shutdown_app(G_APPLICATION(app), &state);
    g_object_unref(app); g_free(directory);
    g_print("Editing, file lifecycle, preferences, and printing checks passed.\n");
    return 0;
}
