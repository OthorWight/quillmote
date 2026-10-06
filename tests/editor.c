#define main quillmote_main
#include "../main.c"
#undef main
#include <signal.h>

#include "ui.h"
static void spin(void) { g_main_context_iteration(NULL, FALSE); g_usleep(100); }
static void settle(AppState *state) {
    AppState *window = window_state(state);
    gint64 deadline = g_get_monotonic_time() + 30 * G_TIME_SPAN_SECOND;
    gboolean busy;
    do {
        spin(); busy = FALSE;
        for (guint i = 0; i < window->tabs->len; i++) {
            AppState *tab = g_ptr_array_index(window->tabs, i);
            if (tab->disposed) { busy |= tab->recovery_writing; continue; }
            busy |= tab->loading || tab->search_idle || tab->count_idle || tab->spell_idle || tab->recovery_start_idle || tab->recovery_idle || tab->recovery_writing;
            busy |= gtk_source_search_context_get_occurrences_count(tab->search_context) < 0;
        }
        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
    } while (busy);
}
static GtkApplication *start(AppState *state) {
    gtk_init();
    GtkApplication *app = gtk_application_new(QUILLMOTE_APP_ID ".EditorTest", G_APPLICATION_NON_UNIQUE);
    g_assert_true(g_application_register(G_APPLICATION(app), NULL, NULL));
    activate(app, state); settle(state); return app;
}
static void assert_text(AppState *state, const char *expected) {
    gchar *text = buffer_text(state); g_assert_cmpstr(text, ==, expected); g_free(text);
}
static void select_range(AppState *state, int start, int end) {
    GtkTextIter a, b; GtkTextBuffer *buffer = GTK_TEXT_BUFFER(state->buffer);
    gtk_text_buffer_get_iter_at_offset(buffer, &a, start); gtk_text_buffer_get_iter_at_offset(buffer, &b, end);
    gtk_text_buffer_select_range(buffer, &b, &a);
}
static void action(AppState *state, const char *name) {
    g_action_group_activate_action(G_ACTION_GROUP(state->app), name, NULL);
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
static void answer(AppState *state, const char *label) {
    gint64 deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
    do {
        spin();
        GListModel *windows = gtk_window_get_toplevels();
        for (guint i = 0; i < g_list_model_get_n_items(windows); i++) {
            GtkWidget *window = g_list_model_get_item(windows, i);
            GtkWidget *button = window != state->window ? button_named(window, label) : NULL;
            if (button) g_signal_emit_by_name(button, "clicked");
            g_object_unref(window);
            if (button) { settle(state); return; }
        }
    } while (g_get_monotonic_time() < deadline);
    g_error("Missing dialog button: %s", label);
}
static void check_counts_search(AppState *state) {
    const char *text = "café can't 123\nHola\t世界 e\314\201  !";
    gtk_text_buffer_set_text(GTK_TEXT_BUFFER(state->buffer), text, -1); settle(state);
    g_assert_cmpuint(state->document_count.words, ==, 6);
    g_assert_nonnull(strstr(gtk_label_get_text(GTK_LABEL(state->status_counts)), "6 words"));
    select_range(state, 0, 10); settle(state);
    g_assert_cmpuint(state->selection_count.words, ==, 2);
    g_assert_nonnull(strstr(gtk_label_get_text(GTK_LABEL(state->status_counts)), "Selection: 2 words · 10 characters"));
    gtk_text_buffer_set_modified(GTK_TEXT_BUFFER(state->buffer), FALSE);
    g_action_group_change_action_state(G_ACTION_GROUP(state->app), "invisible-characters", g_variant_new_boolean(TRUE));
    g_assert_true(gtk_source_space_drawer_get_enable_matrix(gtk_source_view_get_space_drawer(state->view)));
    g_assert_false(gtk_text_buffer_get_modified(GTK_TEXT_BUFFER(state->buffer))); assert_text(state, text);
    save_preferences(state);
    AppState prefs = {0}; load_preferences(&prefs);
    g_assert_true(prefs.show_invisibles); g_assert_true(prefs.show_word_count);
    pango_font_description_free(prefs.font); g_free(prefs.spell_language); print_options_clear(&prefs.printing);

    gtk_text_buffer_set_text(GTK_TEXT_BUFFER(state->buffer), "Cat cat cat", -1);
    select_range(state, 0, 0); show_find(state, FALSE);
    gtk_editable_set_text(GTK_EDITABLE(state->find_entry), "cat"); settle(state);
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(state->search_count)), ==, "3 matches");
    g_assert_true(gtk_source_search_context_get_highlight(state->search_context));
    find_text(state); settle(state);
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(state->search_count)), ==, "1 of 3");
    find_text(state); settle(state);
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(state->search_count)), ==, "2 of 3");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(state->match_case), TRUE); settle(state);
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(state->search_count)), ==, "1 of 2");
    gtk_editable_set_text(GTK_EDITABLE(state->replace_entry), "dog"); replace_all(NULL, state); settle(state);
    assert_text(state, "Cat dog dog");
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(state->search_count)), ==, "No matches");
    gtk_text_buffer_undo(GTK_TEXT_BUFFER(state->buffer)); settle(state);
    g_assert_cmpint(gtk_source_search_context_get_occurrences_count(state->search_context), ==, 2);
    close_find(NULL, state);
    g_assert_false(gtk_source_search_context_get_highlight(state->search_context));
    select_range(state, 0, 0);
    gtk_editable_set_text(GTK_EDITABLE(state->find_entry), ""); show_find(state, FALSE); settle(state);
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(state->search_count)), ==, "");
    close_find(NULL, state);
}
static GtkWidget *status_menu_item(GtkWidget *widget, const char *label) {
    if (GTK_IS_LABEL(widget) && g_strcmp0(gtk_label_get_text(GTK_LABEL(widget)), label) == 0) {
        for (GtkWidget *parent = gtk_widget_get_parent(widget); parent; parent = gtk_widget_get_parent(parent))
            if (g_str_equal(G_OBJECT_TYPE_NAME(parent), "GtkModelButton")) return parent;
    }
    for (GtkWidget *child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child)) {
        GtkWidget *found = status_menu_item(child, label); if (found) return found;
    }
    return NULL;
}
static void capture_status_widget(GtkWidget *widget, const char *directory, const char *name) {
    graphene_rect_t bounds; g_assert_true(gtk_widget_compute_bounds(widget, widget, &bounds));
    int width = (int)bounds.size.width, height = (int)bounds.size.height;
    GdkPaintable *paintable = gtk_widget_paintable_new(widget);
    GtkSnapshot *snapshot = gtk_snapshot_new();
    if (GTK_IS_POPOVER(widget)) gdk_paintable_snapshot(paintable, snapshot, width, height);
    else gtk_widget_snapshot_child(gtk_widget_get_parent(widget), widget, snapshot);
    GskRenderNode *node = gtk_snapshot_free_to_node(snapshot);
    g_assert_nonnull(node);
    graphene_rect_t viewport = GRAPHENE_RECT_INIT(0, 0, width, height);
    GdkTexture *texture = gsk_renderer_render_texture(gtk_native_get_renderer(gtk_widget_get_native(widget)), node, &viewport);
    gchar *path = g_build_filename(directory, name, NULL);
    g_assert_true(gdk_texture_save_to_png(texture, path));
    g_print("Status screenshot: %s\n", path);
    g_free(path); g_object_unref(texture); gsk_render_node_unref(node); g_object_unref(paintable);
}
static void check_status_controls(AppState *state, const char *directory) {
    GtkTextBuffer *buffer = GTK_TEXT_BUFFER(state->buffer);
    gtk_text_buffer_set_text(buffer, "Status bar controls\nZoom, wrap, and spelling", -1);
    gtk_text_buffer_set_modified(buffer, FALSE);
    select_range(state, 23, 23); settle(state);
    g_assert_cmpstr(gtk_button_get_label(GTK_BUTTON(state->status_position)), ==, "Ln 2, Col 4");
    PangoFontDescription *font = pango_font_description_copy(state->font);

    gtk_editable_set_text(GTK_EDITABLE(state->zoom_value), "137");
    gtk_spin_button_update(GTK_SPIN_BUTTON(state->zoom_value));
    g_assert_cmpint(state->zoom, ==, 137);
    g_signal_emit_by_name(state->zoom_in, "clicked");
    g_assert_cmpint(state->zoom, ==, 147);
    g_signal_emit_by_name(state->zoom_out, "clicked");
    g_assert_cmpint(state->zoom, ==, 137);
    g_assert_cmpstr(gtk_menu_button_get_label(GTK_MENU_BUTTON(state->status_zoom)), ==, "137%");
    action(state, "zoom-in");
    g_assert_cmpint(gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(state->zoom_value)), ==, 147);
    GtkWidget *reset = button_named(GTK_WIDGET(gtk_menu_button_get_popover(GTK_MENU_BUTTON(state->status_zoom))), "Reset to 100%");
    g_assert_nonnull(reset); g_signal_emit_by_name(reset, "clicked");
    g_assert_cmpint(state->zoom, ==, 100);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(state->zoom_value), 50);
    g_assert_false(gtk_widget_get_sensitive(state->zoom_out));
    action(state, "zoom-out"); g_assert_cmpint(state->zoom, ==, 50);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(state->zoom_value), 300);
    g_assert_false(gtk_widget_get_sensitive(state->zoom_in));
    action(state, "zoom-in"); g_assert_cmpint(state->zoom, ==, 300);
    action(state, "zoom-reset");
    g_assert_true(pango_font_description_equal(font, state->font));
    pango_font_description_free(font);
    g_assert_false(gtk_text_buffer_get_modified(buffer));

    gboolean wrap = state->word_wrap;
    g_signal_emit_by_name(state->status_wrap, "clicked");
    g_assert_cmpint(state->word_wrap, ==, !wrap);
    g_assert_cmpint(gtk_text_view_get_wrap_mode(GTK_TEXT_VIEW(state->view)), ==, !wrap ? GTK_WRAP_WORD_CHAR : GTK_WRAP_NONE);
    g_action_group_change_action_state(G_ACTION_GROUP(state->app), "wrap", g_variant_new_boolean(wrap));
    g_assert_cmpint(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(state->status_wrap)), ==, wrap);

    GtkPopover *popup = gtk_menu_button_get_popover(GTK_MENU_BUTTON(state->status_zoom));
    gtk_popover_set_autohide(popup, FALSE);
    gtk_menu_button_popup(GTK_MENU_BUTTON(state->status_zoom)); flush_events();
    g_assert_true(gtk_widget_get_mapped(GTK_WIDGET(popup)));
    capture_status_widget(GTK_WIDGET(popup), directory, "status-zoom.png");
    g_assert_true(menu_escape(NULL, GDK_KEY_Escape, 0, 0, state)); flush_events();
    g_assert_false(gtk_widget_get_visible(GTK_WIDGET(popup)));
    g_assert_true(gtk_root_get_focus(GTK_ROOT(state->window)) == GTK_WIDGET(state->view));
    gtk_popover_set_autohide(popup, TRUE);
    GtkPopover *spell_popup = gtk_menu_button_get_popover(GTK_MENU_BUTTON(state->status_spelling));
    gtk_popover_set_autohide(spell_popup, FALSE);
    gtk_menu_button_popup(GTK_MENU_BUTTON(state->status_spelling)); flush_events();
    GtkWidget *check = status_menu_item(GTK_WIDGET(spell_popup), "Check Spelling");
    g_assert_nonnull(check);
    gboolean spell = state->spell_enabled;
    g_assert_true(gtk_widget_activate(check)); flush_events();
    g_assert_cmpint(state->spell_enabled, ==, !spell);
    g_assert_false(gtk_widget_get_visible(GTK_WIDGET(spell_popup)));
    g_assert_true(gtk_root_get_focus(GTK_ROOT(state->window)) == GTK_WIDGET(state->view));
    g_action_group_change_action_state(G_ACTION_GROUP(state->app), "spell-enabled", g_variant_new_boolean(spell));
    gtk_popover_set_autohide(spell_popup, TRUE);
    g_signal_emit_by_name(state->status_position, "clicked"); answer(state, "Cancel");

    AppState *second = new_tab(state); settle(state);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(second->zoom_value), 90);
    select_tab(state);
    g_signal_emit_by_name(state->zoom_in, "clicked");
    g_assert_cmpint(state->zoom, ==, 110); g_assert_cmpint(second->zoom, ==, 90);
    select_tab(second); g_signal_emit_by_name(second->zoom_out, "clicked");
    g_assert_cmpint(second->zoom, ==, 80); g_assert_cmpint(state->zoom, ==, 110);
    close_tab(second); settle(state); action(state, "zoom-reset");

    TextEncoding encoding = state->encoding; LineEnding ending = state->ending;
    state->encoding = ENCODING_UTF16_LE; state->ending = ENDING_CRLF; update_status(state);
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(state->status_format)), ==, "UTF-16 LE · CRLF");
    state->encoding = ENCODING_BYTES; update_status(state);
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(state->status_format)), ==, "Raw bytes");
    g_assert_cmpstr(gtk_menu_button_get_label(GTK_MENU_BUTTON(state->status_spelling)), ==, "Spell: Off");
    state->encoding = encoding; state->ending = ending; update_status(state);
    g_action_group_change_action_state(G_ACTION_GROUP(state->app), "word-count", g_variant_new_boolean(FALSE));
    g_assert_false(gtk_widget_get_visible(state->status_counts));
    g_action_group_change_action_state(G_ACTION_GROUP(state->app), "word-count", g_variant_new_boolean(TRUE));
    settle(state);
    /* Allocate the bar directly because a tiling desktop can ignore window sizes. */
    flush_events();
    graphene_rect_t bounds;
    g_assert_true(gtk_widget_compute_bounds(state->status, state->status, &bounds));
    gtk_widget_allocate(state->status, 640, (int)bounds.size.height, -1, NULL);
    g_assert_cmpint(gtk_widget_get_width(state->status), <=, 640);
    g_assert_true(gtk_widget_compute_bounds(state->zoom_in, state->status, &bounds));
    g_assert_cmpfloat(bounds.origin.x + bounds.size.width, <=, gtk_widget_get_width(state->status));
    capture_status_widget(state->status, directory, "status-narrow.png");
    gtk_widget_queue_allocate(state->page); flush_events();
    g_assert_false(gtk_text_buffer_get_modified(buffer));
}
static void check_auto_indent(AppState *state) {
    const struct { const char *before; int cursor; const char *after; } cases[] = {
        {"    text", 8, "    text\n    "},
        {"\t\ttext", 6, "\t\ttext\n\t\t"},
        {" \t  text", 8, " \t  text\n \t  "},
        {"    left right", 8, "    left\n    right"},
        {"\t  ", 3, "\t  \n\t  "},
        {"text", 4, "text\n"},
        {"", 0, "\n"}
    };
    GtkTextBuffer *buffer = GTK_TEXT_BUFFER(state->buffer);
    GtkSourceIndenter *indenter = gtk_source_view_get_indenter(state->view);
    g_assert_true(gtk_source_view_get_auto_indent(state->view));
    for (guint i = 0; i < G_N_ELEMENTS(cases); i++) {
        gtk_text_buffer_set_text(buffer, cases[i].before, -1);
        select_range(state, cases[i].cursor, cases[i].cursor);
        GtkTextIter iter;
        gtk_text_buffer_get_iter_at_offset(buffer, &iter, cases[i].cursor);
        gboolean indent = gtk_source_indenter_is_trigger(indenter, state->view, &iter, 0, GDK_KEY_Return);
        g_assert_cmpint(gtk_source_indenter_is_trigger(indenter, state->view, &iter, 0, GDK_KEY_KP_Enter), ==, indent);
        g_assert_false(gtk_source_indenter_is_trigger(indenter, state->view, &iter, GDK_SHIFT_MASK, GDK_KEY_Return));
        /* Exercise the default indenter with the same post-newline iterator
         * and user-action boundary supplied by GtkSourceView for Enter. */
        gtk_text_buffer_begin_user_action(buffer);
        gtk_text_buffer_insert(buffer, &iter, "\n", 1);
        if (indent) gtk_source_indenter_indent(indenter, state->view, &iter);
        gtk_text_buffer_end_user_action(buffer);
        assert_text(state, cases[i].after);
        gtk_text_buffer_undo(buffer); assert_text(state, cases[i].before);
    }
}
static void check_tabs(AppState *root, const char *directory) {
    gtk_text_buffer_set_text(GTK_TEXT_BUFFER(root->buffer), "first draft", -1);
    select_range(root, 3, 3);
    action(root, "new"); AppState *second = active_state(root);
    g_assert_true(second != root); g_assert_cmpint(gtk_notebook_get_n_pages(GTK_NOTEBOOK(root->notebook)), ==, 2);
    assert_text(root, "first draft");
    g_assert_cmpint(gtk_notebook_get_n_pages(GTK_NOTEBOOK(root->tab_strip)), ==, 2);
    /* Clicking a header tab switches the actual editor; shortcuts select it back. */
    gtk_notebook_set_current_page(GTK_NOTEBOOK(root->tab_strip), 0);
    g_assert_true(active_state(root) == root);
    g_assert_cmpint(gtk_notebook_get_current_page(GTK_NOTEBOOK(root->notebook)), ==, 0);
    action(root, "next-tab");
    g_assert_true(active_state(root) == second);
    g_assert_cmpint(gtk_notebook_get_current_page(GTK_NOTEBOOK(root->tab_strip)), ==, 1);
    gtk_text_buffer_insert_at_cursor(GTK_TEXT_BUFFER(second->buffer), "second draft", -1);
    action(root, "previous-tab"); g_assert_true(active_state(root) == root);
    GtkTextIter cursor; gtk_text_buffer_get_iter_at_mark(GTK_TEXT_BUFFER(root->buffer), &cursor, gtk_text_buffer_get_insert(GTK_TEXT_BUFFER(root->buffer)));
    g_assert_cmpint(gtk_text_iter_get_offset(&cursor), ==, 3);
    action(root, "next-tab"); g_assert_true(active_state(root) == second);
    action(root, "undo"); assert_text(second, ""); assert_text(root, "first draft");
    action(root, "redo"); assert_text(second, "second draft");
    g_action_group_change_action_state(G_ACTION_GROUP(root->app), "invisible-characters", g_variant_new_boolean(FALSE));
    select_tab(root); g_assert_true(root->show_invisibles);
    GVariant *value = g_action_group_get_action_state(G_ACTION_GROUP(root->app), "invisible-characters");
    g_assert_true(g_variant_get_boolean(value)); g_variant_unref(value);
    select_tab(second); action(root, "close-tab"); answer(root, "Cancel");
    g_assert_false(second->closed); assert_text(second, "second draft");

    /* Save failure cannot close a tab or continue quitting. */
    second->filename = g_build_filename(directory, "missing-parent", "draft.txt", NULL);
    action(root, "close-tab"); answer(root, "Save"); answer(root, "Close");
    g_assert_false(second->closed); assert_text(second, "second draft");
    g_free(second->filename); second->filename = g_build_filename(directory, "second.txt", NULL);
    gchar *saved_path = g_strdup(second->filename);
    action(root, "close-tab"); answer(root, "Save"); g_assert_true(second->closed);
    g_assert_cmpint(gtk_notebook_get_n_pages(GTK_NOTEBOOK(root->tab_strip)), ==,
                    gtk_notebook_get_n_pages(GTK_NOTEBOOK(root->notebook)));
    gchar *disk; g_assert_true(g_file_get_contents(saved_path, &disk, NULL, NULL)); g_free(saved_path);
    g_assert_cmpstr(disk, ==, "second draft"); g_free(disk);

    gchar *one = g_build_filename(directory, "one.txt", NULL), *two = g_build_filename(directory, "two.txt", NULL);
    g_assert_true(g_file_set_contents(one, "one file", -1, NULL));
    g_assert_true(g_file_set_contents(two, "two file", -1, NULL));
    GFile *files[] = {g_file_new_for_path(one), g_file_new_for_path(two)};
    open_files(G_APPLICATION(root->app), files, 2, "", root); settle(root);
    AppState *last = active_state(root);
    g_assert_cmpint(gtk_notebook_get_n_pages(GTK_NOTEBOOK(root->notebook)), ==, 3);
    assert_text(last, "two file"); assert_text(root, "first draft");
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(last->tab_label)), ==, "two.txt");
    show_find(last, FALSE); gtk_editable_set_text(GTK_EDITABLE(last->find_entry), "file");
    g_action_group_change_action_state(G_ACTION_GROUP(root->app), "invisible-characters", g_variant_new_boolean(TRUE));
    settle(root); flush_events();
    for (int i = 0; i < 20 && pango_layout_is_ellipsized(gtk_label_get_layout(GTK_LABEL(last->tab_label))); i++) flush_events();
    g_assert_false(pango_layout_is_ellipsized(gtk_label_get_layout(GTK_LABEL(last->tab_label))));
    capture_window(GTK_WINDOW(root->window), directory, "tabs-search-counts.png");
    GValue drop = G_VALUE_INIT; g_value_init(&drop, GDK_TYPE_FILE_LIST);
    g_value_take_boxed(&drop, gdk_file_list_new_from_array(files, 2));
    g_assert_true(file_dropped(NULL, &drop, 0, 0, root)); settle(root);
    g_assert_cmpint(gtk_notebook_get_n_pages(GTK_NOTEBOOK(root->notebook)), ==, 3);
    g_value_unset(&drop); g_object_unref(files[0]); g_object_unref(files[1]);

    /* Closing an inactive tab keeps actions correctly routed to the survivor. */
    tab_close_clicked(NULL, root); answer(root, "Cancel");
    g_assert_true(active_state(root) == root);
    action(root, "quit"); answer(root, "Cancel");
    g_assert_false(root->closing_window); g_assert_cmpint(gtk_notebook_get_n_pages(GTK_NOTEBOOK(root->notebook)), ==, 3);
    select_tab(last); action(root, "close-tab"); g_assert_true(last->closed);
    action(root, "close-tab"); /* Other saved file. */
    g_assert_cmpint(gtk_notebook_get_n_pages(GTK_NOTEBOOK(root->notebook)), ==, 1);
    /* Force a new revision: the periodic writer may have saved the old one.
     * Start the worker without dispatching its completion before close. */
    gtk_text_buffer_insert_at_cursor(GTK_TEXT_BUFFER(root->buffer), " pending recovery", -1);
    recovery_tick(root);
    g_assert_cmpuint(root->recovery_idle, !=, 0);
    g_source_remove(root->recovery_idle);
    while (snapshot_chunk(root) == G_SOURCE_CONTINUE) {}
    g_assert_true(root->recovery_writing);
    action(root, "close-tab"); answer(root, "Don't Save");
    g_assert_true(root->closed);
    g_assert_cmpint(gtk_notebook_get_n_pages(GTK_NOTEBOOK(root->notebook)), ==, 1);
    assert_text(active_state(root), "");
    /* Discard a tab while the snapshot worker still owns its copy. Its callback
     * must release the copy without recreating recovery for the closed tab. */
    AppState *writing = new_tab(root);
    gtk_text_buffer_set_text(GTK_TEXT_BUFFER(writing->buffer), "Discard this snapshot", -1);
    gchar *snapshot_path = g_strdup(writing->recovery->path);
    recovery_tick(writing);
    g_assert_cmpuint(writing->recovery_idle, !=, 0);
    g_source_remove(writing->recovery_idle);
    while (snapshot_chunk(writing) == G_SOURCE_CONTINUE) {}
    close_tab(writing);
    g_assert_true(writing->recovery_writing);
    g_assert_null(writing->buffer);
    settle(root);
    g_assert_false(writing->recovery_writing);
    g_assert_false(g_file_test(snapshot_path, G_FILE_TEST_EXISTS));
    g_free(snapshot_path);
    action(root, "quit"); settle(root);
    g_assert_null(root->active);
    g_free(one); g_free(two);
}
static void crash_tabs(const char *directory) {
    g_setenv("XDG_CONFIG_HOME", directory, TRUE);
    AppState state = {0}; start(&state);
    gtk_text_buffer_set_text(GTK_TEXT_BUFFER(state.buffer), "recovered first", -1);
    AppState *second = new_tab(&state);
    gtk_text_buffer_set_text(GTK_TEXT_BUFFER(second->buffer), "recovered second", -1);
    second->encoding = ENCODING_UTF16_LE;
    recovery_tick(&state); recovery_tick(second); settle(&state);
    raise(SIGKILL);
}
int main(int argc, char **argv) {
    if (argc == 3 && g_str_equal(argv[1], "--crash")) { crash_tabs(argv[2]); return 1; }
    gchar *directory = g_dir_make_tmp("quillmote-editor-XXXXXX", NULL);
    g_setenv("XDG_CONFIG_HOME", directory, TRUE);
    GSubprocess *child = g_subprocess_new(G_SUBPROCESS_FLAGS_NONE, NULL, argv[0], "--crash", directory, NULL);
    g_assert_true(g_subprocess_wait(child, NULL, NULL));
    g_assert_cmpint(g_subprocess_get_term_sig(child), ==, SIGKILL); g_object_unref(child);
    AppState state = {0}; GtkApplication *app = start(&state);
    g_assert_cmpint(gtk_notebook_get_n_pages(GTK_NOTEBOOK(state.notebook)), ==, 2);
    gboolean first = FALSE, second = FALSE;
    for (guint i = 0; i < state.tabs->len; i++) {
        AppState *tab = g_ptr_array_index(state.tabs, i);
        gchar *text = buffer_text(tab);
        first |= g_str_equal(text, "recovered first");
        second |= g_str_equal(text, "recovered second") && tab->encoding == ENCODING_UTF16_LE;
        g_assert_true(gtk_text_buffer_get_modified(GTK_TEXT_BUFFER(tab->buffer))); g_free(text);
    }
    g_assert_true(first); g_assert_true(second);
    AppState *other = active_state(&state);
    if (other == &state) { action(&state, "next-tab"); other = active_state(&state); }
    action(&state, "close-tab"); answer(&state, "Don't Save");
    select_tab(&state);
    check_counts_search(&state);
    check_auto_indent(&state);
    check_status_controls(&state, directory);
    check_tabs(&state, directory);
    shutdown_app(G_APPLICATION(app), &state); g_object_unref(app); g_free(directory);
    g_print("Counts, invisible characters, search feedback, independent tabs, close/save/cancel, and multi-tab crash recovery passed.\n");
    return 0;
}
