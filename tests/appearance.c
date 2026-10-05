#define main quillmote_main
#include "../main.c"
#undef main

static void flush_events(void) {
    /* CSS changes are applied on the next frame, not just the next idle. */
    gint64 deadline = g_get_monotonic_time() + 100 * G_TIME_SPAN_MILLISECOND;
    do {
        while (g_main_context_iteration(NULL, FALSE)) {}
        g_usleep(1000);
    } while (g_get_monotonic_time() < deadline);
}

static void assert_font(AppState *state, const PangoFontDescription *rendered_font) {
    const PangoFontDescription *actual = pango_context_get_font_description(
        gtk_widget_get_pango_context(GTK_WIDGET(state->view)));
    g_assert_cmpstr(pango_font_description_get_family(actual), ==, "Monospace");
    g_assert_true(pango_font_description_equal(actual, rendered_font));
    g_assert_cmpint(pango_font_description_get_size(state->font), ==, 22 * PANGO_SCALE);
    g_assert_cmpint(pango_font_description_get_weight(actual), ==, PANGO_WEIGHT_BOLD);
    g_assert_cmpint(pango_font_description_get_style(actual), ==, PANGO_STYLE_ITALIC);
}

static void simulate_system_theme(gboolean dark) {
    /* This overrides settings only inside the test process, not on the desktop. */
#if GTK_CHECK_VERSION(4, 20, 0)
    g_object_set(gtk_settings_get_default(), "gtk-interface-color-scheme",
                 dark ? GTK_INTERFACE_COLOR_SCHEME_DARK : GTK_INTERFACE_COLOR_SCHEME_LIGHT, NULL);
#else
    g_object_set(gtk_settings_get_default(), "gtk-application-prefer-dark-theme", dark,
                 "gtk-theme-name", dark ? "Adwaita-dark" : "Adwaita", NULL);
#endif
}

static gchar *document_text(AppState *state) {
    GtkTextIter start, end;
    gtk_text_buffer_get_bounds(GTK_TEXT_BUFFER(state->buffer), &start, &end);
    return gtk_text_buffer_get_text(GTK_TEXT_BUFFER(state->buffer), &start, &end, FALSE);
}

static void check_spelling(AppState *state) {
    g_assert_nonnull(state->dictionary);
    GtkTextBuffer *buffer = GTK_TEXT_BUFFER(state->buffer);
    const char *original = "café speling other speling";
    gtk_text_buffer_set_text(buffer, original, -1);
    flush_events();
    GtkTextIter position;
    gtk_text_buffer_get_iter_at_offset(buffer, &position, 7);
    GdkRectangle rect;
    gtk_text_view_get_iter_location(GTK_TEXT_VIEW(state->view), &position, &rect);
    int x, y;
    gtk_text_view_buffer_to_window_coords(GTK_TEXT_VIEW(state->view), GTK_TEXT_WINDOW_WIDGET,
                                         rect.x + rect.width / 2, rect.y + rect.height / 2, &x, &y);
    /* Emit through the installed capture controller, with the cursor at EOF. */
    GListModel *controllers = gtk_widget_observe_controllers(GTK_WIDGET(state->view));
    gboolean emitted = FALSE;
    for (guint i = 0; i < g_list_model_get_n_items(controllers); i++) {
        GObject *controller = g_list_model_get_item(controllers, i);
        if (GTK_IS_GESTURE_CLICK(controller) &&
            gtk_event_controller_get_propagation_phase(GTK_EVENT_CONTROLLER(controller)) == GTK_PHASE_CAPTURE &&
            gtk_gesture_single_get_button(GTK_GESTURE_SINGLE(controller)) == GDK_BUTTON_SECONDARY) {
            g_signal_emit_by_name(controller, "pressed", 1, (double)x, (double)y);
            emitted = TRUE;
        }
        g_object_unref(controller);
    }
    g_object_unref(controllers);
    g_assert_true(emitted);
    GMenuModel *extra = gtk_text_view_get_extra_menu(GTK_TEXT_VIEW(state->view));
    GMenuModel *spelling = g_menu_model_get_item_link(extra, 0, G_MENU_LINK_SECTION);
    g_assert_true(spelling == G_MENU_MODEL(state->spelling_menu));
    g_object_unref(spelling);
    g_assert_cmpint(g_menu_model_get_n_items(G_MENU_MODEL(state->spelling_menu)), ==, 1);
    GMenuModel *section = g_menu_model_get_item_link(G_MENU_MODEL(state->spelling_menu), 0, G_MENU_LINK_SECTION);
    g_assert_cmpint(g_menu_model_get_n_items(section), >, 0);
    GVariant *target = g_menu_model_get_item_attribute_value(section, 0, G_MENU_ATTRIBUTE_TARGET, G_VARIANT_TYPE("(tiiss)"));
    g_assert_nonnull(target);
    guint64 revision;
    int start, end;
    const char *word, *replacement;
    g_variant_get(target, "(tii&s&s)", &revision, &start, &end, &word, &replacement);
    g_assert_cmpint(start, ==, 5);
    g_assert_cmpint(end, ==, 12);
    g_assert_cmpstr(word, ==, "speling");
    gchar *expected = g_strdup_printf("café %s other speling", replacement);
    g_action_group_activate_action(G_ACTION_GROUP(state->app), "replace-spelling", target);
    gchar *text = document_text(state);
    g_assert_cmpstr(text, ==, expected);
    g_free(text);
    g_free(expected);
    gtk_text_buffer_undo(buffer);
    text = document_text(state);
    g_assert_cmpstr(text, ==, original);
    g_free(text);

    /* An old suggestion cannot modify a document edited since the menu opened. */
    gtk_text_buffer_set_text(buffer, "A different document", -1);
    g_action_group_activate_action(G_ACTION_GROUP(state->app), "replace-spelling", target);
    text = document_text(state);
    g_assert_cmpstr(text, ==, "A different document");
    g_free(text);
    g_variant_unref(target);
    g_object_unref(section);
    g_assert_false(g_action_group_get_action_enabled(G_ACTION_GROUP(state->app), "replace-spelling"));

    gtk_text_buffer_set_text(buffer, "word speling", -1);
    gtk_text_buffer_get_iter_at_offset(buffer, &position, 1);
    prepare_spelling_menu(state, &position);
    g_assert_cmpint(g_menu_model_get_n_items(G_MENU_MODEL(state->spelling_menu)), ==, 0);
    gtk_text_buffer_get_iter_at_offset(buffer, &position, 4);
    prepare_spelling_menu(state, &position);
    g_assert_cmpint(g_menu_model_get_n_items(G_MENU_MODEL(state->spelling_menu)), ==, 0);
    /* Shift+F10 also offers suggestions at the caret, including a word's end. */
    spelling_context_key(NULL, GDK_KEY_F10, 0, GDK_SHIFT_MASK, state);
    g_assert_cmpint(g_menu_model_get_n_items(G_MENU_MODEL(state->spelling_menu)), ==, 1);
    spelling_context_pressed(NULL, 1, 5000, 5000, state);
    g_assert_cmpint(g_menu_model_get_n_items(G_MENU_MODEL(state->spelling_menu)), ==, 0);
    void *dictionary = state->dictionary;
    state->dictionary = NULL;
    gtk_text_buffer_get_iter_at_offset(buffer, &position, 7);
    prepare_spelling_menu(state, &position);
    g_assert_cmpint(g_menu_model_get_n_items(G_MENU_MODEL(state->spelling_menu)), ==, 0);
    state->dictionary = dictionary;
    flush_events();
}

static GtkWidget *menu_button_with_text(GtkWidget *widget, const char *text) {
    if (GTK_IS_LABEL(widget) && g_strcmp0(gtk_label_get_text(GTK_LABEL(widget)), text) == 0) {
        for (GtkWidget *parent = gtk_widget_get_parent(widget); parent; parent = gtk_widget_get_parent(parent))
            if (g_str_equal(G_OBJECT_TYPE_NAME(parent), "GtkModelButton")) return parent;
    }
    for (GtkWidget *child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child)) {
        GtkWidget *found = menu_button_with_text(child, text);
        if (found) return found;
    }
    return NULL;
}

static void assert_same_shortcut(const char *actual, const char *expected) {
    guint actual_key, expected_key;
    GdkModifierType actual_mods, expected_mods;
    g_assert_true(gtk_accelerator_parse(actual, &actual_key, &actual_mods));
    g_assert_true(gtk_accelerator_parse(expected, &expected_key, &expected_mods));
    g_assert_cmpuint(actual_key, ==, expected_key);
    g_assert_cmpuint(actual_mods, ==, expected_mods);
}

static void check_menu_shortcuts(GMenuModel *menu, GtkApplication *app, guint *hints) {
    for (int i = 0; i < g_menu_model_get_n_items(menu); i++) {
        gchar *action = NULL, *accel = NULL;
        g_menu_model_get_item_attribute(menu, i, G_MENU_ATTRIBUTE_ACTION, "s", &action);
        g_menu_model_get_item_attribute(menu, i, "accel", "s", &accel);
        if (action) {
            gchar **keys = gtk_application_get_accels_for_action(app, action);
            if (keys[0]) {
                g_assert_nonnull(accel);
                assert_same_shortcut(accel, keys[0]);
            }
            if (accel) (*hints)++;
            g_strfreev(keys);
        }
        g_free(action); g_free(accel);
        const char *links[] = {G_MENU_LINK_SECTION, G_MENU_LINK_SUBMENU};
        for (guint j = 0; j < G_N_ELEMENTS(links); j++) {
            GMenuModel *child = g_menu_model_get_item_link(menu, i, links[j]);
            if (child) { check_menu_shortcuts(child, app, hints); g_object_unref(child); }
        }
    }
}

static GtkWidget *find_label(GtkWidget *widget, const char *text) {
    if (GTK_IS_LABEL(widget) && g_strcmp0(gtk_label_get_text(GTK_LABEL(widget)), text) == 0) return widget;
    for (GtkWidget *child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child)) {
        GtkWidget *found = find_label(child, text);
        if (found) return found;
    }
    return NULL;
}

static void assert_rendered_shortcut(GtkWidget *button, const char *shortcut) {
    guint key; GdkModifierType mods;
    g_assert_true(gtk_accelerator_parse(shortcut, &key, &mods));
    gchar *label = gtk_accelerator_get_label(key, mods);
    GtkWidget *hint = find_label(button, label);
    g_assert_nonnull(hint);
    for (int i = 0; i < 20 && !gtk_widget_get_mapped(hint); i++) flush_events();
    g_assert_true(gtk_widget_get_mapped(hint));
    g_free(label);
}

static void capture_menu(GtkWidget *button, const char *directory, const char *name) {
    GtkWidget *popup = gtk_widget_get_ancestor(button, GTK_TYPE_POPOVER);
    g_assert_nonnull(popup);
    GdkPaintable *paintable = gtk_widget_paintable_new(popup);
    GtkSnapshot *snapshot = gtk_snapshot_new();
    int width = gtk_widget_get_width(popup), height = gtk_widget_get_height(popup);
    gdk_paintable_snapshot(paintable, snapshot, width, height);
    GskRenderNode *node = gtk_snapshot_free_to_node(snapshot);
    g_assert_nonnull(node);
    graphene_rect_t viewport = GRAPHENE_RECT_INIT(0, 0, width, height);
    GdkTexture *texture = gsk_renderer_render_texture(gtk_native_get_renderer(GTK_NATIVE(popup)), node, &viewport);
    gchar *path = g_build_filename(directory, name, NULL);
    g_assert_true(gdk_texture_save_to_png(texture, path));
    g_print("Menu screenshot: %s\n", path);
    g_free(path); g_object_unref(texture); gsk_render_node_unref(node); g_object_unref(paintable);
}

static void check_visible_shortcuts(AppState *state, const char *directory) {
    GtkWidget *bar = gtk_widget_get_first_child(gtk_window_get_child(GTK_WINDOW(state->window)));
    GMenuModel *model = gtk_popover_menu_bar_get_menu_model(GTK_POPOVER_MENU_BAR(bar));
    guint hints = 0;
    check_menu_shortcuts(model, state->app, &hints);
    g_assert_cmpuint(hints, ==, 25);
    GtkWidget *file = gtk_widget_get_first_child(bar);
    GtkWidget *save = menu_button_with_text(bar, "Save");
    g_assert_nonnull(save);
    /* Screenshot popups must also map when the desktop keeps focus in another
     * application. Restore normal grabbing behavior for the lifecycle tests. */
    GtkPopover *file_popup = GTK_POPOVER(gtk_widget_get_ancestor(save, GTK_TYPE_POPOVER));
    gtk_popover_set_autohide(file_popup, FALSE);
    g_assert_true(gtk_widget_activate(file)); flush_events();
    assert_rendered_shortcut(save, "<Primary>s");
    capture_menu(save, directory, "file-menu.png");
    g_assert_true(menu_escape(NULL, GDK_KEY_Escape, 0, 0, state)); flush_events();
    gtk_popover_set_autohide(file_popup, TRUE);
    GtkWidget *undo = menu_button_with_text(bar, "Undo");
    g_assert_nonnull(undo);
    GtkPopover *edit_popup = GTK_POPOVER(gtk_widget_get_ancestor(undo, GTK_TYPE_POPOVER));
    gtk_popover_set_autohide(edit_popup, FALSE);
    g_assert_true(gtk_widget_activate(gtk_widget_get_next_sibling(file))); flush_events();
    const struct { const char *label, *action, *key; } editing[] = {
        {"Undo", "app.undo", "<Primary>z"}, {"Redo", "app.redo", "<Primary><Shift>z"},
        {"Cut", "app.cut", "<Primary>x"}, {"Copy", "app.copy", "<Primary>c"},
        {"Paste", "app.paste", "<Primary>v"}, {"Delete", "app.delete", "Delete"},
        {"Select All", "app.select-all", "<Primary>a"}
    };
    for (guint i = 0; i < G_N_ELEMENTS(editing); i++) {
        GtkWidget *button = menu_button_with_text(bar, editing[i].label);
        g_assert_nonnull(button);
        assert_rendered_shortcut(button, editing[i].key);
        /* These must remain native widget bindings, including inside Find entries. */
        gchar **keys = gtk_application_get_accels_for_action(state->app, editing[i].action);
        g_assert_null(keys[0]); g_strfreev(keys);
    }
    capture_menu(undo, directory, "edit-menu.png");
    g_assert_true(menu_escape(NULL, GDK_KEY_Escape, 0, 0, state)); flush_events();
    gtk_popover_set_autohide(edit_popup, TRUE);
    g_print("Menu shortcut hints, grouping, and native editing bindings passed.\n");
}

static void assert_editor_focus(AppState *state) {
    GPtrArray *menus = g_ptr_array_new_with_free_func(g_object_unref);
    collect_open_menus(state->window, menus);
    g_assert_cmpuint(menus->len, ==, 0); g_ptr_array_unref(menus);
    g_assert_true(gtk_root_get_focus(GTK_ROOT(state->window)) == GTK_WIDGET(state->view));
}

static void check_menu_lifecycle(AppState *state) {
    GtkTextBuffer *buffer = GTK_TEXT_BUFFER(state->buffer);
    for (int i = 0; i < 4; i++) {
        gtk_text_buffer_set_text(buffer, "speling", -1);
        GtkTextIter position; gtk_text_buffer_get_start_iter(buffer, &position);
        prepare_spelling_menu(state, &position);
        GMenuModel *section = g_menu_model_get_item_link(G_MENU_MODEL(state->spelling_menu), 0, G_MENU_LINK_SECTION);
        gchar *replacement = NULL;
        g_assert_true(g_menu_model_get_item_attribute(section, 0, G_MENU_ATTRIBUTE_LABEL, "s", &replacement));
        g_assert_true(gtk_widget_activate_action(GTK_WIDGET(state->view), "menu.popup", NULL));
        flush_events();
        GtkWidget *button = menu_button_with_text(GTK_WIDGET(state->view), replacement);
        g_assert_nonnull(button);
        /* Activate the real menu button while its popup owns focus. */
        g_assert_true(gtk_widget_activate(button));
        flush_events();
        gchar *text = buffer_text(state); g_assert_cmpstr(text, ==, replacement); g_free(text);
        assert_editor_focus(state);
        gtk_text_buffer_insert_at_cursor(buffer, " typed", -1);
        text = buffer_text(state); g_assert_nonnull(strstr(text, " typed")); g_free(text);
        g_free(replacement); g_object_unref(section);

        gtk_text_buffer_get_start_iter(buffer, &position);
        prepare_spelling_menu(state, &position);
        g_assert_true(gtk_widget_activate_action(GTK_WIDGET(state->view), "menu.popup", NULL));
        flush_events();
        g_assert_true(menu_escape(NULL, GDK_KEY_Escape, 0, 0, state));
        assert_editor_focus(state);
        g_assert_false(menu_escape(NULL, GDK_KEY_Escape, 0, 0, state));
    }

    GtkWidget *bar = gtk_widget_get_first_child(gtk_window_get_child(GTK_WINDOW(state->window)));
    GtkWidget *format = gtk_widget_get_first_child(bar);
    format = gtk_widget_get_next_sibling(gtk_widget_get_next_sibling(format));
    g_assert_true(gtk_widget_activate(format)); flush_events();
    GtkWidget *spelling = menu_button_with_text(bar, "Spelling");
    g_assert_nonnull(spelling); g_assert_true(gtk_widget_activate(spelling)); flush_events();
    GtkWidget *language = menu_button_with_text(bar, "Language");
    g_assert_nonnull(language); g_assert_true(gtk_widget_activate(language)); flush_events();
    gchar *label = g_strdup(state->spell_language); g_strdelimit(label, "_", '-');
    GtkWidget *choice = menu_button_with_text(bar, label); g_free(label);
    g_assert_nonnull(choice); g_assert_true(gtk_widget_activate(choice)); flush_events();
    assert_editor_focus(state);
    /* Switching straight from a menu to context suggestions closes the old grab. */
    g_assert_true(gtk_widget_activate(format)); flush_events();
    GtkTextIter position; gtk_text_buffer_get_start_iter(buffer, &position);
    prepare_spelling_menu(state, &position);
    g_assert_true(gtk_widget_activate_action(GTK_WIDGET(state->view), "menu.popup", NULL)); flush_events();
    g_assert_true(menu_escape(NULL, GDK_KEY_Escape, 0, 0, state));
    assert_editor_focus(state);
    g_print("Native spelling/menu activation and Escape focus checks passed.\n");
}

int main(void) {
    GError *setup_error = NULL;
    gchar *test_directory = g_dir_make_tmp("quillmote-tests-XXXXXX", &setup_error);
    g_assert_no_error(setup_error);
    g_setenv("XDG_CONFIG_HOME", test_directory, TRUE);
    gtk_init();
    AppState state = {0};
    GtkApplication *app = gtk_application_new(QUILLMOTE_APP_ID ".AppearanceTest", G_APPLICATION_NON_UNIQUE);
    GError *error = NULL;
    g_assert_true(g_application_register(G_APPLICATION(app), NULL, &error));
    g_assert_no_error(error);
    simulate_system_theme(TRUE);
    activate(app, &state);
    g_assert_true(state.dark_mode);
    flush_events();

    GtkWidget *root = gtk_window_get_child(GTK_WINDOW(state.window));
    g_assert_true(GTK_IS_HEADER_BAR(gtk_window_get_titlebar(GTK_WINDOW(state.window))));
    GtkWidget *menubar = gtk_widget_get_first_child(root);
    GMenuModel *menu = gtk_popover_menu_bar_get_menu_model(GTK_POPOVER_MENU_BAR(menubar));
    GMenuModel *format = g_menu_model_get_item_link(menu, 2, G_MENU_LINK_SUBMENU);
    gchar *font_action = NULL;
    g_assert_true(g_menu_model_get_item_attribute(format, 1, G_MENU_ATTRIBUTE_ACTION, "s", &font_action));
    g_assert_cmpstr(font_action, ==, "app.font");
    g_assert_nonnull(g_action_map_lookup_action(G_ACTION_MAP(app), "font"));
    g_assert_cmpint(g_menu_model_get_n_items(format), ==, 4);
    g_assert_null(g_action_map_lookup_action(G_ACTION_MAP(app), "dark-mode"));
    g_assert_null(g_action_map_lookup_action(G_ACTION_MAP(app), "correct"));
    GMenuModel *edit = g_menu_model_get_item_link(menu, 1, G_MENU_LINK_SUBMENU);
    g_assert_cmpint(g_menu_model_get_n_items(edit), ==, 4);
    const int group_sizes[] = {2, 5, 4, 1};
    for (int i = 0; i < 4; i++) {
        GMenuModel *section = g_menu_model_get_item_link(edit, i, G_MENU_LINK_SECTION);
        g_assert_nonnull(section);
        g_assert_cmpint(g_menu_model_get_n_items(section), ==, group_sizes[i]);
        g_object_unref(section);
    }
    g_object_unref(edit);
    g_free(font_action);
    g_object_unref(format);

    g_clear_pointer(&state.font, pango_font_description_free);
    state.font = pango_font_description_from_string("Monospace Bold Italic 22");
    apply_editor_style(&state);
    flush_events();
    PangoFontDescription *rendered_font = pango_font_description_copy(pango_context_get_font_description(
        gtk_widget_get_pango_context(GTK_WIDGET(state.view))));
    gtk_text_buffer_set_text(GTK_TEXT_BUFFER(state.buffer), "Retain this document", -1);
    for (int i = 0; i < 4; i++) {
        gboolean dark = i % 2 == 0;
        simulate_system_theme(dark);
        flush_events();
        assert_font(&state, rendered_font);
        GtkSourceStyleScheme *scheme = gtk_source_buffer_get_style_scheme(state.buffer);
        g_assert_nonnull(scheme);
        g_assert_cmpstr(gtk_source_style_scheme_get_id(scheme), ==, dark ? "classic-dark" : "classic");
        GtkSourceStyle *text_style = gtk_source_style_scheme_get_style(scheme, "text");
        gchar *background = NULL;
        g_object_get(text_style, "background", &background, NULL);
        GdkRGBA color;
        g_assert_true(gdk_rgba_parse(&color, background));
        g_assert_true(dark ? color.red < 0.3 && color.green < 0.3 && color.blue < 0.3
                           : color.red > 0.9 && color.green > 0.9 && color.blue > 0.9);
        g_free(background);
        GtkTextIter start, end;
        gtk_text_buffer_get_bounds(GTK_TEXT_BUFFER(state.buffer), &start, &end);
        gchar *text = gtk_text_buffer_get_text(GTK_TEXT_BUFFER(state.buffer), &start, &end, FALSE);
        g_assert_cmpstr(text, ==, "Retain this document");
        g_free(text);
    }

    check_visible_shortcuts(&state, test_directory);
    check_spelling(&state);
    check_menu_lifecycle(&state);
    pango_font_description_free(rendered_font);
    while (state.recovery_writing || state.recovery_idle) g_main_context_iteration(NULL, TRUE);
    gtk_window_destroy(GTK_WINDOW(state.window));
    shutdown_app(G_APPLICATION(app), &state);
    g_free(test_directory);
    g_object_unref(app);
    g_print("System theme and spelling regression checks passed.\n");
    return 0;
}
