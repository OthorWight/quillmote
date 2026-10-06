#define main quillmote_main
#include "../main.c"
#undef main
#include "ui.h"
#include <stdio.h>
#include <sys/stat.h>
#include <utime.h>

static void spin(void) { g_main_context_iteration(NULL, FALSE); g_usleep(1000); }

static void settle(AppState *root) {
    gint64 deadline = g_get_monotonic_time() + 15 * G_TIME_SPAN_SECOND;
    gboolean busy;
    do {
        spin(); busy = FALSE;
        for (guint i = 0; i < root->tabs->len; i++) {
            AppState *tab = g_ptr_array_index(root->tabs, i);
            if (!tab->disposed) busy |= tab->loading || tab->disk_cancel || tab->disk_debounce ||
                tab->recovery_start_idle || tab->recovery_idle || tab->recovery_writing;
        }
        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
    } while (busy);
}

static void wait_changed(AppState *tab, gboolean unavailable) {
    gint64 deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
    while (!tab->disk_changed || tab->disk_unavailable != unavailable) {
        g_assert_cmpint(g_get_monotonic_time(), <, deadline); spin();
    }
    settle(window_state(tab));
    g_assert_true(gtk_widget_get_visible(tab->change_bar));
}

static void assert_text(AppState *tab, const char *expected) {
    gchar *text = buffer_text(tab); g_assert_cmpstr(text, ==, expected); g_free(text);
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
        GtkWidget *found = find_button(child, label); if (found) return found;
    }
    return NULL;
}

static void answer(AppState *root, const char *label) {
    gint64 deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
    do {
        spin(); GListModel *windows = gtk_window_get_toplevels();
        for (guint i = 0; i < g_list_model_get_n_items(windows); i++) {
            GtkWidget *window = g_list_model_get_item(windows, i);
            GtkWidget *button = window != root->window ? find_button(window, label) : NULL;
            if (button) g_signal_emit_by_name(button, "clicked");
            g_object_unref(window);
            if (button) { settle(root); return; }
        }
    } while (g_get_monotonic_time() < deadline);
    g_error("Missing dialog button: %s", label);
}

static void write_in_place(const char *path, const char *text) {
    FILE *file = fopen(path, "wb"); g_assert_nonnull(file);
    g_assert_cmpuint(fwrite(text, 1, strlen(text), file), ==, strlen(text));
    g_assert_cmpint(fclose(file), ==, 0);
}

int main(void) {
    gchar *directory = g_dir_make_tmp("quillmote-file-changes-XXXXXX", NULL);
    g_setenv("XDG_CONFIG_HOME", directory, TRUE);
    gtk_init();
    AppState root = {0};
    GtkApplication *app = gtk_application_new(QUILLMOTE_APP_ID ".FileChangesTest", G_APPLICATION_NON_UNIQUE);
    g_assert_true(g_application_register(G_APPLICATION(app), NULL, NULL));
    activate(app, &root); settle(&root);
    gchar *one = g_build_filename(directory, "one.txt", NULL);
    gchar *two = g_build_filename(directory, "two.txt", NULL);
    gchar *copy = g_build_filename(directory, "copy.txt", NULL);
    g_assert_true(g_file_set_contents(one, "original\ntext", -1, NULL));
    g_assert_true(g_file_set_contents(two, "second file", -1, NULL));
    AppState *a = open_tab(&root, one, FALSE, FALSE); settle(&root);
    g_assert_nonnull(a->file_monitor);
    g_assert_false(a->disk_changed);
    g_assert_false(gtk_widget_get_visible(a->change_bar));

    /* In-place updates notify without changing the buffer or undo history. */
    GtkTextIter cursor;
    gtk_text_buffer_get_iter_at_offset(GTK_TEXT_BUFFER(a->buffer), &cursor, 5);
    gtk_text_buffer_place_cursor(GTK_TEXT_BUFFER(a->buffer), &cursor);
    a->zoom = 130; apply_editor_style(a);
    write_in_place(one, "updated\ntext"); wait_changed(a, FALSE);
    assert_text(a, "original\ntext");
    g_assert_nonnull(strstr(gtk_label_get_text(GTK_LABEL(a->tab_label)), "↻"));
    capture_window(GTK_WINDOW(root.window), directory, "external-change.png");
    g_action_group_activate_action(G_ACTION_GROUP(app), "refresh", NULL); settle(&root);
    assert_text(a, "updated\ntext"); g_assert_false(a->disk_changed);
    gtk_text_buffer_get_iter_at_mark(GTK_TEXT_BUFFER(a->buffer), &cursor, gtk_text_buffer_get_insert(GTK_TEXT_BUFFER(a->buffer)));
    g_assert_cmpint(gtk_text_iter_get_offset(&cursor), ==, 5);
    g_assert_cmpint(a->zoom, ==, 130);
    g_assert_false(gtk_text_buffer_get_modified(GTK_TEXT_BUFFER(a->buffer)));

    /* Cancelled refresh leaves the existing buffer and change notice intact. */
    g_assert_true(g_file_set_contents(one, "cancelled refresh", -1, NULL)); wait_changed(a, FALSE);
    GtkSourceBuffer *previous = a->buffer;
    refresh_clicked(NULL, a); cancel_load(NULL, a); settle(&root);
    g_assert_true(a->buffer == previous); assert_text(a, "updated\ntext");
    g_assert_true(a->disk_changed);

    /* Atomic replacement twice: the directory watch survives both saves. */
    g_assert_true(g_file_set_contents(one, "replacement 1", -1, NULL)); wait_changed(a, FALSE);
    refresh_clicked(NULL, a); settle(&root); assert_text(a, "replacement 1");
    g_assert_true(g_file_set_contents(one, "replacement 2", -1, NULL)); wait_changed(a, FALSE);
    refresh_clicked(NULL, a); settle(&root); assert_text(a, "replacement 2");

    /* Content, rather than timestamps or notifications, decides whether to warn. */
    g_assert_true(g_file_set_contents(one, "replacement 2", -1, NULL));
    queue_disk_check(a); settle(&root); g_assert_false(a->disk_changed);
    struct stat before; g_assert_cmpint(g_stat(one, &before), ==, 0);
    write_in_place(one, "replacement 3");
    struct utimbuf times = {.actime = before.st_atime, .modtime = before.st_mtime};
    g_assert_cmpint(utime(one, &times), ==, 0); wait_changed(a, FALSE);
    refresh_clicked(NULL, a); settle(&root);

    /* Dirty refresh cancellation preserves edits and undo; acceptance is explicit. */
    gtk_text_buffer_insert_at_cursor(GTK_TEXT_BUFFER(a->buffer), "local ", -1);
    gchar *edited = buffer_text(a);
    g_assert_true(g_file_set_contents(one, "latest on disk", -1, NULL)); wait_changed(a, FALSE);
    refresh_clicked(NULL, a); answer(&root, "Cancel");
    assert_text(a, edited); g_free(edited);
    g_assert_true(gtk_text_buffer_get_can_undo(GTK_TEXT_BUFFER(a->buffer)));
    g_assert_true(a->disk_changed);
    refresh_clicked(NULL, a); answer(&root, "Discard and Refresh");
    assert_text(a, "latest on disk"); g_assert_false(a->disk_changed);

    /* Known conflicts are also protected on Save, including cancellation. */
    gtk_text_buffer_set_text(GTK_TEXT_BUFFER(a->buffer), "my version", -1);
    g_assert_true(g_file_set_contents(one, "another version", -1, NULL)); wait_changed(a, FALSE);
    save_file(a); answer(&root, "Cancel"); assert_text(a, "my version");
    gchar *disk = NULL; g_assert_true(g_file_get_contents(one, &disk, NULL, NULL));
    g_assert_cmpstr(disk, ==, "another version"); g_free(disk);
    save_file(a); answer(&root, "Replace");
    g_assert_false(a->disk_changed);
    queue_disk_check(a); settle(&root); g_assert_false(a->disk_changed);
    g_assert_true(g_file_get_contents(one, &disk, NULL, NULL));
    g_assert_cmpstr(disk, ==, "my version"); g_free(disk);

    /* Background tabs get their own notice; switching never replaces text. */
    AppState *b = open_tab(&root, two, FALSE, FALSE); settle(&root);
    g_assert_true(g_file_set_contents(one, "background update", -1, NULL)); wait_changed(a, FALSE);
    g_assert_true(root.active == b); g_assert_false(b->disk_changed);
    assert_text(b, "second file"); select_tab(a);
    refresh_clicked(NULL, a); settle(&root);

    /* Focus fallback catches missed events, including changes to background tabs. */
    g_file_monitor_cancel(a->file_monitor);
    g_file_monitor_cancel(b->file_monitor);
    g_assert_true(g_file_set_contents(one, "missed event", -1, NULL));
    g_assert_true(g_file_set_contents(two, "missed background event", -1, NULL));
    gtk_window_present(GTK_WINDOW(root.window));
    gint64 deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
    while (!gtk_window_is_active(GTK_WINDOW(root.window))) {
        g_assert_cmpint(g_get_monotonic_time(), <, deadline); spin();
    }
    window_focus_changed(G_OBJECT(root.window), NULL, &root);
    wait_changed(a, FALSE); wait_changed(b, FALSE);
    refresh_clicked(NULL, a); settle(&root);
    refresh_clicked(NULL, b); settle(&root); select_tab(a);

    /* A failed refresh must keep the old buffer and its unsaved edits. */
    gtk_text_buffer_insert_at_cursor(GTK_TEXT_BUFFER(a->buffer), "keep ", -1);
    edited = buffer_text(a);
    g_assert_cmpint(g_unlink(one), ==, 0); wait_changed(a, TRUE);
    g_assert_false(gtk_widget_get_sensitive(a->refresh_button));
    refresh_clicked(NULL, a); answer(&root, "Discard and Refresh"); answer(&root, "Close");
    assert_text(a, edited); g_free(edited);
    g_assert_true(gtk_text_buffer_get_modified(GTK_TEXT_BUFFER(a->buffer)));
    g_assert_true(g_file_set_contents(one, "recreated", -1, NULL)); wait_changed(a, FALSE);
    refresh_clicked(NULL, a); answer(&root, "Discard and Refresh"); assert_text(a, "recreated");

    /* Save As moves the watch; writes to the old path are ignored. */
    g_assert_true(save_contents(a, copy, a->encoding, a->ending)); settle(&root);
    g_assert_true(g_file_set_contents(one, "old path change", -1, NULL));
    queue_disk_check(a); settle(&root); g_assert_false(a->disk_changed);
    g_assert_true(g_file_set_contents(copy, "copy change", -1, NULL)); wait_changed(a, FALSE);

    /* A new named file expects absence until another process creates it. */
    gchar *missing = g_build_filename(directory, "new.txt", NULL);
    AppState *c = open_tab(&root, missing, TRUE, FALSE); settle(&root);
    g_assert_false(c->disk_changed);
    g_assert_true(g_file_set_contents(missing, "created elsewhere", -1, NULL)); wait_changed(c, FALSE);

    /* Retire a tab during a pending check: no callback can reach freed state. */
    queue_disk_check(c);
    g_source_remove(c->disk_debounce); c->disk_debounce = 0;
    check_disk(c); /* Leave a worker in flight. */
    close_tab(c);
    shutdown_app(G_APPLICATION(app), &root);
    deadline = g_get_monotonic_time() + 300 * G_TIME_SPAN_MILLISECOND;
    while (g_get_monotonic_time() < deadline) spin();
    g_object_unref(app);
    g_free(one); g_free(two); g_free(copy); g_free(missing); g_free(directory);
    g_print("File notifications, focus fallback, refresh/save conflicts, deletion, and watcher disposal passed.\n");
    return 0;
}
