#define main quillmote_main
#include "../main.c"
#undef main
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

static void spin(void) {
    g_main_context_iteration(NULL, FALSE);
    g_usleep(100);
}

static void settle(AppState *state) {
    AppState *window = window_state(state);
    gint64 deadline = g_get_monotonic_time() + 30 * G_TIME_SPAN_SECOND;
    gboolean pending;
    do {
        spin(); pending = FALSE;
        for (guint i = 0; window->tabs && i < window->tabs->len; i++) {
            AppState *tab = g_ptr_array_index(window->tabs, i);
            if (!tab->disposed) pending |= tab->loading || tab->recovery_writing || tab->recovery_idle || tab->recovery_start_idle;
        }
        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
    } while (pending);
}

static void assert_text(AppState *state, const char *expected) {
    gchar *text = buffer_text(state);
    g_assert_cmpstr(text, ==, expected); g_free(text);
}

static GtkApplication *start(AppState *state) {
    gtk_init();
    static guint serial = 0;
    gchar *id = g_strdup_printf(QUILLMOTE_APP_ID ".WorkflowTest%u", serial++);
    GtkApplication *app = gtk_application_new(id, G_APPLICATION_NON_UNIQUE); g_free(id);
    GError *error = NULL;
    g_assert_true(g_application_register(G_APPLICATION(app), NULL, &error)); g_assert_no_error(error);
    activate(app, state);
    return app;
}

static GtkWidget *find_button(GtkWidget *widget, const char *label) {
    if (GTK_IS_BUTTON(widget)) {
        const char *raw = gtk_button_get_label(GTK_BUTTON(widget));
        GString *plain = g_string_new(NULL);
        for (const char *p = raw ? raw : ""; *p; p++) if (*p != '_') g_string_append_c(plain, *p);
        gboolean match = g_str_equal(plain->str, label); g_string_free(plain, TRUE);
        if (match) return widget;
    }
    for (GtkWidget *child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child)) {
        GtkWidget *found = find_button(child, label);
        if (found) return found;
    }
    return NULL;
}

static void answer(AppState *state, const char *label) {
    gint64 deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
    while (g_get_monotonic_time() < deadline) {
        spin();
        GListModel *windows = gtk_window_get_toplevels();
        for (guint i = 0; i < g_list_model_get_n_items(windows); i++) {
            GtkWidget *window = g_list_model_get_item(windows, i);
            GtkWidget *button = window != state->window ? find_button(window, label) : NULL;
            if (button) g_signal_emit_by_name(button, "clicked");
            g_object_unref(window);
            if (button) { settle(state); return; }
        }
    }
    g_error("Missing dialog button %s", label);
}

static void crash_writer(const char *directory) {
    g_setenv("XDG_CONFIG_HOME", directory, TRUE);
    AppState state = {0};
    start(&state); settle(&state);
    state.filename = g_build_filename(directory, "original.txt", NULL);
    g_assert_true(g_file_set_contents(state.filename, "original on disk", -1, NULL));
    gtk_text_buffer_set_text(GTK_TEXT_BUFFER(state.buffer), "Unsaved café\nsecond line", -1);
    state.encoding = ENCODING_UTF16_LE; state.ending = ENDING_CRLF;
    GtkTextIter cursor;
    gtk_text_buffer_get_iter_at_offset(GTK_TEXT_BUFFER(state.buffer), &cursor, 5);
    gtk_text_buffer_place_cursor(GTK_TEXT_BUFFER(state.buffer), &cursor);
    recovery_tick(&state); settle(&state);
    g_assert_true(g_file_test(state.recovery->path, G_FILE_TEST_IS_REGULAR));
    struct stat st;
    g_assert_cmpint(g_stat(state.recovery->path, &st), ==, 0);
    g_assert_cmpint(st.st_mode & 0777, ==, 0600);
    raise(SIGKILL);
    g_assert_not_reached();
}

static void test_zoom_spelling(AppState *state) {
    PangoFontDescription *font = pango_font_description_copy(state->font);
    g_action_group_activate_action(G_ACTION_GROUP(state->app), "zoom-in", NULL);
    g_assert_cmpint(state->zoom, ==, 110);
    g_assert_true(pango_font_description_equal(state->font, font));
    system_theme_changed(gtk_settings_get_default(), NULL, state);
    g_assert_cmpint(state->zoom, ==, 110);
    g_action_group_activate_action(G_ACTION_GROUP(state->app), "zoom-reset", NULL);
    g_assert_cmpint(state->zoom, ==, 100);
    for (int i = 0; i < 40; i++) g_action_group_activate_action(G_ACTION_GROUP(state->app), "zoom-out", NULL);
    g_assert_cmpint(state->zoom, ==, 50);
    g_action_group_activate_action(G_ACTION_GROUP(state->app), "zoom-reset", NULL);
    pango_font_description_free(font);
    gchar **keys = gtk_application_get_accels_for_action(state->app, "app.zoom-in");
    g_assert_cmpuint(g_strv_length(keys), >=, 2); g_strfreev(keys);

    for (guint i = 0; i < state->spell_languages->len; i++) {
        const char *tag = g_ptr_array_index(state->spell_languages, i);
        g_action_group_activate_action(G_ACTION_GROUP(state->app), "spell-language", g_variant_new_string(tag));
        g_assert_cmpstr(state->spell_language, ==, tag);
        g_assert_nonnull(state->dictionary);
    }
    if (state->dictionary) {
        const char *word = "quillmotexyzzy";
        gtk_text_buffer_set_text(GTK_TEXT_BUFFER(state->buffer), word, -1);
        g_assert_cmpint(state->dict_check(state->dictionary, word, strlen(word)), >, 0);
        spelling_actions_enabled(state, TRUE);
        g_action_group_activate_action(G_ACTION_GROUP(state->app), "spelling-ignore",
            g_variant_new("(ts)", state->document_revision, word));
        g_assert_cmpint(state->dict_check(state->dictionary, word, strlen(word)), ==, 0);
        const char *added = "quillmotepersistword";
        spelling_actions_enabled(state, TRUE);
        g_action_group_activate_action(G_ACTION_GROUP(state->app), "spelling-add",
            g_variant_new("(ts)", state->document_revision, added));
        g_assert_cmpint(state->dict_check(state->dictionary, added, strlen(added)), ==, 0);
    }
    g_action_group_change_action_state(G_ACTION_GROUP(state->app), "spell-enabled", g_variant_new_boolean(FALSE));
    while (state->spell_idle) spin();
    GtkTextIter start;
    gtk_text_buffer_get_start_iter(GTK_TEXT_BUFFER(state->buffer), &start);
    prepare_spelling_menu(state, &start);
    g_assert_cmpint(g_menu_model_get_n_items(G_MENU_MODEL(state->spelling_menu)), ==, 0);
    g_assert_false(gtk_text_iter_has_tag(&start, gtk_text_tag_table_lookup(
        gtk_text_buffer_get_tag_table(GTK_TEXT_BUFFER(state->buffer)), "misspelled")));
}

static gboolean heartbeat(gpointer data) {
    (*(int *)data)++; return G_SOURCE_CONTINUE;
}

static void test_loading(AppState *window, const char *directory) {
    AppState *state = active_state(window);
    gchar *missing = g_build_filename(directory, "new-document.txt", NULL);
    gtk_text_buffer_set_modified(GTK_TEXT_BUFFER(state->buffer), FALSE);
    GFile *file = g_file_new_for_path(missing);
    open_files(G_APPLICATION(state->app), &file, 1, "", window); settle(window);
    state = active_state(window);
    g_assert_cmpstr(state->filename, ==, missing); assert_text(state, "");
    g_assert_false(g_file_test(missing, G_FILE_TEST_EXISTS));
    g_action_group_activate_action(G_ACTION_GROUP(state->app), "save", NULL);
    g_assert_true(g_file_test(missing, G_FILE_TEST_EXISTS));
    g_object_unref(file);

    gchar *path = g_build_filename(directory, "large.txt", NULL);
    GString *large = g_string_sized_new(4 * 1024 * 1024);
    for (int i = 0; i < 100000; i++) g_string_append(large, "A UTF-8 café line to load without freezing.\r\n");
    g_assert_true(g_file_set_contents(path, large->str, large->len, NULL)); g_string_free(large, TRUE);
    gtk_text_buffer_set_text(GTK_TEXT_BUFFER(state->buffer), "keep this on cancellation", -1);
    g_assert_true(load_file(state, path, -1));
    g_assert_true(state->busy);
    g_assert_false(g_action_group_get_action_enabled(G_ACTION_GROUP(state->app), "save"));
    cancel_load(NULL, state); settle(state);
    assert_text(state, "keep this on cancellation");
    g_assert_cmpstr(state->filename, ==, missing);
    g_assert_true(gtk_text_buffer_get_modified(GTK_TEXT_BUFFER(state->buffer)));

    g_assert_true(load_file(state, path, -1));
    gint64 deadline = g_get_monotonic_time() + 10 * G_TIME_SPAN_SECOND;
    while (state->loading && state->loading->offset == 0) {
        spin(); g_assert_cmpint(g_get_monotonic_time(), <, deadline);
    }
    g_assert_nonnull(state->loading);
    cancel_load(NULL, state); settle(state);
    assert_text(state, "keep this on cancellation");

    int ticks = 0;
    guint timer = g_timeout_add(1, heartbeat, &ticks);
    g_assert_true(load_file(state, path, -1)); settle(state);
    g_source_remove(timer);
    g_assert_cmpint(ticks, >, 5);
    g_assert_cmpint(state->ending, ==, ENDING_CRLF);
    g_assert_cmpint(gtk_text_buffer_get_line_count(GTK_TEXT_BUFFER(state->buffer)), ==, 100001);
    g_assert_false(gtk_text_buffer_get_can_undo(GTK_TEXT_BUFFER(state->buffer)));
    g_assert_true(g_action_group_get_action_enabled(G_ACTION_GROUP(state->app), "save"));
    g_print("Large-file loading yielded to %d UI timer ticks.\n", ticks);

    /* Dropping an already-open file selects it without losing edits. */
    gtk_text_buffer_set_text(GTK_TEXT_BUFFER(state->buffer), "unsaved before drop", -1);
    file = g_file_new_for_path(path);
    GdkFileList *list = gdk_file_list_new_from_array(&file, 1);
    GValue value = G_VALUE_INIT;
    g_value_init(&value, GDK_TYPE_FILE_LIST); g_value_take_boxed(&value, list);
    g_assert_true(file_dropped(NULL, &value, 0, 0, window)); settle(window);
    g_assert_true(active_state(window) == state);
    assert_text(state, "unsaved before drop");
    g_value_unset(&value); g_object_unref(file);

    gchar *absent = g_build_filename(directory, "does-not-exist.txt", NULL);
    g_assert_true(load_file(state, absent, -1)); settle(state);
    g_assert_cmpstr(state->filename, ==, path);
    answer(state, "Close");
    g_free(absent); g_free(path); g_free(missing);
}

static void test_recovery_with_explicit_file(AppState *active, const char *directory) {
    RecoverySession *stale = recovery_session_new(active->recovery_directory, NULL);
    RecoveryDocument document = {.text = "␀A␍\n", .encoding = ENCODING_BYTES, .ending = ENDING_LF, .cursor = 1};
    gchar *temp = recovery_write_temporary(stale->path, &document, NULL);
    g_assert_nonnull(temp); g_assert_cmpint(g_rename(temp, stale->path), ==, 0); g_free(temp);
    recovery_session_free(stale);

    AppState other = {0}; GtkApplication *app = start(&other);
    gchar *path = g_build_filename(directory, "explicit.txt", NULL);
    g_assert_true(g_file_set_contents(path, "requested file", -1, NULL));
    GFile *file = g_file_new_for_path(path);
    open_files(G_APPLICATION(app), &file, 1, "", &other); g_object_unref(file);
    settle(&other); assert_text(&other, "requested file");
    g_assert_true(gtk_widget_get_visible(other.recovery_bar));
    recover_clicked(NULL, &other); settle(&other);
    AppState *recovered = active_state(&other);
    assert_text(&other, "requested file");
    assert_text(recovered, document.text);
    g_assert_null(recovered->filename); g_assert_cmpint(recovered->encoding, ==, ENCODING_BYTES);
    g_assert_true(recovered->recovered);
    recovery_tick(recovered); settle(recovered);
    g_assert_null(recovery_find(active->recovery_directory));
    RecoveryDocument *snapshot = recovery_read(recovered->recovery->path, NULL);
    g_assert_nonnull(snapshot); g_assert_cmpstr(snapshot->text, ==, document.text);
    g_assert_cmpint(snapshot->encoding, ==, ENCODING_BYTES); recovery_document_free(snapshot);
    /* Explicit discard removes only this window's work. */
    recovered->pending = PENDING_NEW; perform_pending(recovered);
    g_assert_false(g_file_test(recovered->recovery->path, G_FILE_TEST_EXISTS));
    settle(recovered);
    gtk_window_destroy(GTK_WINDOW(other.window)); shutdown_app(G_APPLICATION(app), &other);
    g_object_unref(app); g_free(path);
}

int main(int argc, char **argv) {
    if (argc == 3 && g_str_equal(argv[1], "--crash")) { crash_writer(argv[2]); return 1; }
    gchar *directory = g_dir_make_tmp("quillmote-workflows-XXXXXX", NULL);
    GError *error = NULL;
    GSubprocess *child = g_subprocess_new(G_SUBPROCESS_FLAGS_NONE, &error, argv[0], "--crash", directory, NULL);
    g_assert_no_error(error); g_assert_nonnull(child);
    g_assert_true(g_subprocess_wait(child, NULL, &error)); g_assert_no_error(error);
    g_assert_true(g_subprocess_get_if_signaled(child));
    g_assert_cmpint(g_subprocess_get_term_sig(child), ==, SIGKILL); g_object_unref(child);

    g_setenv("XDG_CONFIG_HOME", directory, TRUE);
    AppState state = {0}; GtkApplication *app = start(&state); settle(&state);
    assert_text(&state, "Unsaved café\nsecond line");
    g_assert_true(state.recovered);
    g_assert_true(gtk_text_buffer_get_modified(GTK_TEXT_BUFFER(state.buffer)));
    g_assert_cmpint(state.encoding, ==, ENCODING_UTF16_LE); g_assert_cmpint(state.ending, ==, ENDING_CRLF);
    GtkTextIter cursor;
    gtk_text_buffer_get_iter_at_mark(GTK_TEXT_BUFFER(state.buffer), &cursor,
                                    gtk_text_buffer_get_insert(GTK_TEXT_BUFFER(state.buffer)));
    g_assert_cmpint(gtk_text_iter_get_offset(&cursor), ==, 5);
    gchar *disk = NULL;
    g_assert_true(g_file_get_contents(state.filename, &disk, NULL, NULL));
    g_assert_cmpstr(disk, ==, "original on disk"); g_free(disk);
    /* An active window's recovery file must never be claimed by another. */
    g_assert_null(recovery_find(state.recovery_directory));
    recovery_tick(&state); settle(&state);
    g_assert_true(g_file_test(state.recovery->path, G_FILE_TEST_EXISTS));
    g_assert_true(save_contents(&state, state.filename, state.encoding, state.ending));
    g_assert_false(g_file_test(state.recovery->path, G_FILE_TEST_EXISTS));

    g_print("Crash recovery after SIGKILL passed.\n");
    test_recovery_with_explicit_file(&state, directory);
    test_zoom_spelling(&state);
    test_loading(&state, directory);
    select_tab(&state);
    gtk_window_set_default_size(GTK_WINDOW(state.window), 1050, 740);
    remember_window(G_OBJECT(state.window), NULL, &state);
    save_preferences(&state);
    AppState restored = {0}; load_preferences(&restored);
    g_assert_cmpint(restored.window_width, ==, 1050); g_assert_cmpint(restored.window_height, ==, 740);
    g_assert_false(restored.spell_enabled); g_assert_cmpstr(restored.spell_language, ==, state.spell_language);
    g_free(restored.spell_language); pango_font_description_free(restored.font); print_options_clear(&restored.printing);

    /* A pending snapshot cannot resurrect work explicitly discarded by New. */
    gtk_text_buffer_set_text(GTK_TEXT_BUFFER(state.buffer), "discard this snapshot", -1);
    recovery_tick(&state);
    while (!state.recovery_writing) spin();
    state.pending = PENDING_NEW; perform_pending(&state); settle(&state);
    g_assert_false(g_file_test(state.recovery->path, G_FILE_TEST_EXISTS));
    assert_text(&state, "");
    gtk_window_destroy(GTK_WINDOW(state.window)); shutdown_app(G_APPLICATION(app), &state);
    g_object_unref(app); g_free(directory);
    g_print("Crash recovery, zoom, languages, window size, drop, CLI, and async loading checks passed.\n");
    return 0;
}
