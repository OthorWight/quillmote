#define main quillmote_main
#include "../main.c"
#undef main
#include <stdio.h>

static void pump(void) {
    g_main_context_iteration(NULL, FALSE);
    g_usleep(100);
}

static void wait_loaded(AppState *state, guint mib) {
    gint64 deadline = g_get_monotonic_time() + (gint64)MAX(120, mib * 4) * G_TIME_SPAN_SECOND;
    while (!state->disposed && (state->loading || state->count_idle || state->search_idle)) {
        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
        pump();
    }
}

static guint64 memory_report(const char *stage) {
    guint64 bytes = 0;
    gchar *contents = NULL;
    if (g_file_get_contents("/proc/self/status", &contents, NULL, NULL)) {
        const char *rss = strstr(contents, "VmRSS:");
        if (rss) {
            bytes = g_ascii_strtoull(rss + strlen("VmRSS:"), NULL, 10) * 1024;
            g_print("%s: %.*s\n", stage, (int)strcspn(rss, "\n"), rss);
        }
        g_free(contents);
    }
    return bytes;
}

static void wait_released(void) {
    gint64 deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
#ifdef __GLIBC__
    while (memory_release_idle) {
        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
        pump();
    }
#endif
    /* Give GTK's deferred widget cleanup a turn on other allocators, too. */
    deadline = g_get_monotonic_time() + 200 * G_TIME_SPAN_MILLISECOND;
    while (g_get_monotonic_time() < deadline) pump();
}

int main(void) {
    gchar *directory = g_dir_make_tmp("quillmote-memory-XXXXXX", NULL);
    g_setenv("XDG_CONFIG_HOME", directory, TRUE);
    gtk_init();
    AppState root = {0};
    GtkApplication *app = gtk_application_new(QUILLMOTE_APP_ID ".MemoryTest", G_APPLICATION_NON_UNIQUE);
    g_assert_true(g_application_register(G_APPLICATION(app), NULL, NULL));
    activate(app, &root);
    root.spell_enabled = FALSE;
    save_preferences(&root);
    while (root.recovery_start_idle || root.count_idle) pump();
    gchar *path = g_build_filename(directory, "large.txt", NULL);
    FILE *file = fopen(path, "wb"); g_assert_nonnull(file);
    char block[4096]; memset(block, 'a', sizeof block);
    for (gsize i = 127; i < sizeof block; i += 128) block[i] = '\n';
    const char *size = g_getenv("QUILLMOTE_MEMORY_TEST_MIB");
    guint mib = size ? (guint)g_ascii_strtoull(size, NULL, 10) : 32;
    g_assert_cmpuint(mib, >=, 32); g_assert_cmpuint(mib, <=, 1024);
    for (guint i = 0; i < mib * 256; i++) g_assert_cmpuint(fwrite(block, 1, sizeof block, file), ==, sizeof block);
    g_assert_cmpint(fclose(file), ==, 0);
    memory_report("Before load");
    for (int i = 0; i < 2; i++) {
        AppState *tab = open_tab(&root, path, FALSE, FALSE);
        wait_loaded(tab, mib);
        gpointer buffer = tab->buffer, view = tab->view, page = tab->page, search = tab->search_context;
        g_object_add_weak_pointer(G_OBJECT(buffer), &buffer);
        g_object_add_weak_pointer(G_OBJECT(view), &view);
        g_object_add_weak_pointer(G_OBJECT(page), &page);
        g_object_add_weak_pointer(G_OBJECT(search), &search);
        if (i == 1) {
            /* Selection, undo history, and a background search must not keep
             * the closed document alive, even when another tab is active. */
            gtk_text_buffer_insert_at_cursor(GTK_TEXT_BUFFER(tab->buffer), "edited", -1);
            g_assert_true(gtk_text_buffer_get_can_undo(GTK_TEXT_BUFFER(tab->buffer)));
            GtkTextIter start, end;
            gtk_text_buffer_get_start_iter(GTK_TEXT_BUFFER(tab->buffer), &start);
            end = start; gtk_text_iter_forward_chars(&end, 6);
            gtk_text_buffer_select_range(GTK_TEXT_BUFFER(tab->buffer), &start, &end);
            gtk_source_search_settings_set_search_text(
                gtk_source_search_context_get_settings(tab->search_context), "missing");
            AppState *survivor = new_tab(&root);
            gtk_text_buffer_set_text(GTK_TEXT_BUFFER(survivor->buffer), "Keep this tab", -1);
        }
        g_print("File size: %u MiB\n", mib);
        guint64 loaded = memory_report("Loaded");
        close_tab(tab);
        wait_released();
        g_print("Retained objects: buffer=%d view=%d page=%d search=%d\n", buffer != NULL, view != NULL, page != NULL, search != NULL);
        guint64 closed = memory_report("Closed (automatic release)");
#if defined(__GLIBC__) && defined(__linux__)
        /* Allow for font/GPU caches and desktop activity while catching the
         * original regression, where essentially none of the RSS was freed. */
        g_assert_cmpuint(loaded, >, closed + (guint64)mib * 1024 * 1024 / 2);
#else
        (void)loaded; (void)closed;
#endif
        g_assert_null(view); g_assert_null(page); g_assert_null(search);
        /* Some GTK versions retain a primary-selection buffer reference. It
         * must be an empty shell, with no file contents or undo/redo history. */
        if (buffer) {
            g_assert_cmpint(gtk_text_buffer_get_char_count(GTK_TEXT_BUFFER(buffer)), ==, 0);
            g_assert_false(gtk_text_buffer_get_enable_undo(GTK_TEXT_BUFFER(buffer)));
            g_assert_false(gtk_text_buffer_get_can_undo(GTK_TEXT_BUFFER(buffer)));
            g_assert_false(gtk_text_buffer_get_can_redo(GTK_TEXT_BUFFER(buffer)));
            g_assert_false(gtk_text_buffer_get_has_selection(GTK_TEXT_BUFFER(buffer)));
            g_object_remove_weak_pointer(G_OBJECT(buffer), &buffer);
        }
    }
    gchar *surviving_text = buffer_text(active_state(&root));
    g_assert_cmpstr(surviving_text, ==, "Keep this tab"); g_free(surviving_text);
    /* Cancelling a load through Close must release its staging data, too. */
    AppState *cancelled = open_tab(&root, path, FALSE, FALSE);
    request_document_change(cancelled, PENDING_CLOSE);
    wait_loaded(cancelled, mib); wait_released();
    g_assert_true(cancelled->closed); g_assert_null(cancelled->loading);
    g_assert_null(cancelled->load_cancel); g_assert_null(cancelled->buffer);
    gtk_window_destroy(GTK_WINDOW(root.window));
    shutdown_app(G_APPLICATION(app), &root); g_object_unref(app);
    g_unlink(path); g_free(path); g_free(directory);
    g_print("Closed document memory checks passed.\n");
    return 0;
}
