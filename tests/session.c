#define main quillmote_main
#include "../main.c"
#undef main

static void spin(void) { g_main_context_iteration(NULL, FALSE); g_usleep(1000); }
static void settle(AppState *root) {
    gint64 deadline = g_get_monotonic_time() + 15 * G_TIME_SPAN_SECOND;
    gboolean busy;
    do {
        spin(); busy = root->session_restore_pending;
        for (guint i = 0; i < root->tabs->len; i++) {
            AppState *tab = g_ptr_array_index(root->tabs, i);
            busy |= tab->recovery_writing;
            if (!tab->disposed) busy |= tab->loading || tab->recovery_start_idle || tab->count_idle || tab->search_idle || tab->recovery_idle;
        }
        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
    } while (busy);
}
static GtkApplication *start(AppState *root) {
    static guint launch;
    gchar *id = g_strdup_printf(QUILLMOTE_APP_ID ".SessionTest%u", ++launch);
    GtkApplication *app = gtk_application_new(id, G_APPLICATION_NON_UNIQUE); g_free(id);
    GError *error = NULL;
    g_assert_true(g_application_register(G_APPLICATION(app), NULL, &error));
    g_assert_no_error(error);
    activate(app, root); settle(root); return app;
}
static void stop(AppState *root, GtkApplication *app) {
    quit_window(root); settle(root);
    g_assert_null(root->active);
    shutdown_app(G_APPLICATION(app), root); g_object_unref(app);
}
static AppState *page(AppState *root, int index) {
    return g_object_get_data(G_OBJECT(gtk_notebook_get_nth_page(GTK_NOTEBOOK(root->notebook), index)), "document");
}
static int cursor(AppState *tab) {
    GtkTextIter iter;
    gtk_text_buffer_get_iter_at_mark(GTK_TEXT_BUFFER(tab->buffer), &iter, gtk_text_buffer_get_insert(GTK_TEXT_BUFFER(tab->buffer)));
    return gtk_text_iter_get_offset(&iter);
}
static void move_cursor(AppState *tab, int offset) {
    GtkTextIter iter;
    gtk_text_buffer_get_iter_at_offset(GTK_TEXT_BUFFER(tab->buffer), &iter, offset);
    gtk_text_buffer_place_cursor(GTK_TEXT_BUFFER(tab->buffer), &iter);
}
static GtkWidget *button_named(GtkWidget *widget, const char *label) {
    if (GTK_IS_BUTTON(widget)) {
        const char *raw = gtk_button_get_label(GTK_BUTTON(widget));
        GString *plain = g_string_new(NULL);
        for (const char *p = raw ? raw : ""; *p; p++) if (*p != '_') g_string_append_c(plain, *p);
        gboolean match = g_str_equal(plain->str, label); g_string_free(plain, TRUE);
        if (match) return widget;
    }
    for (GtkWidget *child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child)) {
        GtkWidget *found = button_named(child, label); if (found) return found;
    }
    return NULL;
}
static void answer(AppState *root, const char *label) {
    gint64 deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
    do {
        spin(); GListModel *windows = gtk_window_get_toplevels();
        for (guint i = 0; i < g_list_model_get_n_items(windows); i++) {
            GtkWidget *window = g_list_model_get_item(windows, i);
            GtkWidget *button = window != root->window ? button_named(window, label) : NULL;
            if (button) g_signal_emit_by_name(button, "clicked");
            g_object_unref(window);
            if (button) { settle(root); return; }
        }
    } while (g_get_monotonic_time() < deadline);
    g_error("Missing dialog button: %s", label);
}

int main(void) {
    gchar *directory = g_dir_make_tmp("quillmote-session-XXXXXX", NULL);
    g_setenv("XDG_CONFIG_HOME", directory, TRUE);
    gtk_init();
    gchar *one = g_build_filename(directory, "one;[é].txt", NULL);
    gchar *two = g_build_filename(directory, "two.txt", NULL);
    gchar *open_folder = g_build_filename(directory, "open-folder", NULL);
    gchar *save_folder = g_build_filename(directory, "save-folder", NULL);
    g_assert_cmpint(g_mkdir(open_folder, 0700), ==, 0);
    g_assert_cmpint(g_mkdir(save_folder, 0700), ==, 0);
    gchar *closed = g_build_filename(open_folder, "closed.txt", NULL);
    gchar *copy = g_build_filename(save_folder, "copy.txt", NULL);
    g_assert_true(g_file_set_contents(one, "abcédefghij", -1, NULL));
    g_assert_true(g_file_set_contents(two, "second file", -1, NULL));
    g_assert_true(g_file_set_contents(closed, "closed file", -1, NULL));

    AppState first = {0}; GtkApplication *app = start(&first);
    g_assert_cmpint(gtk_notebook_get_n_pages(GTK_NOTEBOOK(first.notebook)), ==, 1);
    AppState *a = open_tab(&first, one, FALSE, FALSE); settle(&first);
    move_cursor(a, 5); a->zoom = 130;
    AppState *b = open_tab(&first, two, FALSE, FALSE); settle(&first);
    move_cursor(b, 8); b->zoom = 90;
    remember_file_folder(first.active, closed, FALSE);
    AppState *c = open_tab(&first, closed, FALSE, FALSE); settle(&first);
    g_assert_cmpstr(c->filename, ==, closed);
    g_assert_cmpstr(first.last_open_folder, ==, open_folder);
    g_assert_true(g_file_set_contents(copy, "", 0, NULL));
    remember_file_folder(c, copy, TRUE);
    g_assert_true(save_contents(c, copy, c->encoding, c->ending));
    g_assert_cmpstr(c->filename, ==, copy);
    g_assert_cmpstr(first.last_save_folder, ==, save_folder);
    close_tab(c);
    /* Header drag order must become the document order persisted on quit. */
    gtk_notebook_reorder_child(GTK_NOTEBOOK(first.tab_strip), b->tab_page, 0);
    g_assert_cmpint(gtk_notebook_page_num(GTK_NOTEBOOK(first.notebook), b->page), ==, 0);
    select_tab(a);
    stop(&first, app);
    Session *saved = session_load();
    g_assert_cmpuint(saved->tabs->len, ==, 2); g_assert_cmpint(saved->active, ==, 1);
    g_assert_cmpstr(((SessionTab *)g_ptr_array_index(saved->tabs, 0))->path, ==, two);
    g_assert_cmpstr(((SessionTab *)g_ptr_array_index(saved->tabs, 1))->path, ==, one);
    session_free(saved);

    AppState second = {0}; app = start(&second);
    g_assert_cmpint(gtk_notebook_get_n_pages(GTK_NOTEBOOK(second.notebook)), ==, 2);
    a = page(&second, 1); b = page(&second, 0);
    g_assert_cmpstr(a->filename, ==, one); g_assert_cmpstr(b->filename, ==, two);
    g_assert_true(second.active == a);
    g_assert_cmpint(gtk_notebook_page_num(GTK_NOTEBOOK(second.tab_strip), b->tab_page), ==, 0);
    g_assert_cmpint(gtk_notebook_get_current_page(GTK_NOTEBOOK(second.tab_strip)), ==, 1);
    g_assert_cmpint(cursor(a), ==, 5); g_assert_cmpint(cursor(b), ==, 8);
    g_assert_cmpint(a->zoom, ==, 130); g_assert_cmpint(b->zoom, ==, 90);
    g_assert_cmpstr(second.last_open_folder, ==, open_folder);
    g_assert_cmpstr(second.last_save_folder, ==, save_folder);
    gchar *folder = file_picker_folder(a, FALSE);
    g_assert_cmpstr(folder, ==, open_folder); g_free(folder);
    folder = file_picker_folder(a, TRUE);
    g_assert_cmpstr(folder, ==, directory); g_free(folder);
    AppState untitled = {.owner = &second};
    folder = file_picker_folder(&untitled, TRUE);
    g_assert_cmpstr(folder, ==, save_folder); g_free(folder);
    gchar *remembered_save = second.last_save_folder;
    second.last_save_folder = g_build_filename(directory, "missing-folder", NULL);
    folder = file_picker_folder(&untitled, TRUE);
    g_assert_cmpstr(folder, ==, open_folder); g_free(folder);
    g_free(second.last_save_folder); second.last_save_folder = remembered_save;
    /* Cancelling quit must not publish a partially closed session. */
    gtk_text_buffer_insert_at_cursor(GTK_TEXT_BUFFER(a->buffer), " changed", -1);
    quit_window(&second); answer(&second, "Cancel");
    g_assert_false(second.closing_window); g_assert_null(second.closing_session);
    saved = session_load(); g_assert_cmpuint(saved->tabs->len, ==, 2); session_free(saved);
    /* Simulate Save As completing before the final close decision. */
    quit_window(&second);
    gchar *renamed = g_build_filename(directory, "renamed.txt", NULL);
    g_assert_true(save_contents(a, renamed, a->encoding, a->ending));
    answer(&second, "Don't Save");
    g_assert_null(second.active);
    shutdown_app(G_APPLICATION(app), &second); g_object_unref(app);
    saved = session_load();
    g_assert_cmpstr(((SessionTab *)g_ptr_array_index(saved->tabs, 1))->path, ==, renamed);
    session_free(saved);

    /* Missing files are skipped, and changed lengths clamp the saved cursor. */
    g_unlink(two); g_assert_true(g_file_set_contents(renamed, "é", -1, NULL));
    AppState third = {0}; app = start(&third);
    g_assert_cmpint(gtk_notebook_get_n_pages(GTK_NOTEBOOK(third.notebook)), ==, 1);
    g_assert_cmpstr(third.active->filename, ==, renamed);
    g_assert_cmpint(cursor(third.active), ==, 1);
    stop(&third, app);

    /* Recovery of a named file must win over the older on-disk session copy. */
    gchar *recovery_dir = g_build_filename(directory, "quillmote", "recovery", NULL);
    RecoverySession *stale = recovery_session_new(recovery_dir, NULL);
    RecoveryDocument document = {.text = "Recovered unsaved text", .filename = renamed,
        .encoding = ENCODING_UTF8, .ending = ENDING_LF, .cursor = 7};
    gchar *temporary = recovery_write_temporary(stale->path, &document, NULL);
    g_assert_cmpint(g_rename(temporary, stale->path), ==, 0);
    g_free(temporary); recovery_session_free(stale);
    AppState fourth = {0}; app = start(&fourth);
    g_assert_cmpint(gtk_notebook_get_n_pages(GTK_NOTEBOOK(fourth.notebook)), ==, 1);
    gchar *text = buffer_text(fourth.active);
    g_assert_cmpstr(text, ==, document.text); g_free(text);
    g_assert_cmpint(cursor(fourth.active), ==, 7);
    g_assert_true(gtk_text_buffer_get_modified(GTK_TEXT_BUFFER(fourth.active->buffer)));
    quit_window(&fourth); answer(&fourth, "Don't Save");
    shutdown_app(G_APPLICATION(app), &fourth); g_object_unref(app);

    /* Explicit command-line opens do not pull in an unrelated old session. */
    AppState fifth = {0};
    app = gtk_application_new(QUILLMOTE_APP_ID ".SessionCLI", G_APPLICATION_NON_UNIQUE);
    g_assert_true(g_application_register(G_APPLICATION(app), NULL, NULL));
    GFile *explicit_file = g_file_new_for_path(one);
    open_files(G_APPLICATION(app), &explicit_file, 1, "", &fifth); settle(&fifth);
    g_assert_cmpint(gtk_notebook_get_n_pages(GTK_NOTEBOOK(fifth.notebook)), ==, 1);
    g_assert_cmpstr(fifth.active->filename, ==, one); g_object_unref(explicit_file);
    /* Explicitly closing the last saved tab leaves an empty next session. */
    close_tab(fifth.active); stop(&fifth, app);
    AppState sixth = {0}; app = start(&sixth);
    g_assert_cmpint(gtk_notebook_get_n_pages(GTK_NOTEBOOK(sixth.notebook)), ==, 1);
    g_assert_null(sixth.active->filename); stop(&sixth, app);
    g_free(open_folder); g_free(save_folder); g_free(copy);
    g_free(one); g_free(two); g_free(closed); g_free(renamed); g_free(recovery_dir); g_free(directory);
    g_print("Session order, active tab, cursor/zoom, folders, cancelled quit, Save As, missing files, recovery precedence, and CLI isolation passed.\n");
    return 0;
}
