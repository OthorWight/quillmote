#define main quillmote_main
#include "../main.c"
#undef main
#include "ui.h"

/* Actual rendered backgrounds catch a light buffer under dark native chrome. */
static GdkTexture *window_texture(GtkWindow *window) {
    GdkPaintable *paintable = gtk_widget_paintable_new(GTK_WIDGET(window));
    GtkSnapshot *snapshot = gtk_snapshot_new();
    int width = gtk_widget_get_width(GTK_WIDGET(window)), height = gtk_widget_get_height(GTK_WIDGET(window));
    gdk_paintable_snapshot(paintable, snapshot, width, height);
    GskRenderNode *node = gtk_snapshot_free_to_node(snapshot);
    for (int attempt = 0; !node && attempt < 20; attempt++) {
        flush_events();
        snapshot = gtk_snapshot_new();
        gdk_paintable_snapshot(paintable, snapshot, width, height);
        node = gtk_snapshot_free_to_node(snapshot);
    }
    g_assert_nonnull(node);
    graphene_rect_t viewport = GRAPHENE_RECT_INIT(0, 0, width, height);
    GdkTexture *texture = gsk_renderer_render_texture(gtk_native_get_renderer(GTK_NATIVE(window)), node, &viewport);
    gsk_render_node_unref(node); g_object_unref(paintable);
    return texture;
}

static void assert_background(GdkTexture *texture, GtkWidget *window, GtkWidget *widget,
                              float x_fraction, float y_fraction, gboolean dark) {
    graphene_point_t point = GRAPHENE_POINT_INIT(gtk_widget_get_width(widget) * x_fraction,
                                                gtk_widget_get_height(widget) * y_fraction), at;
    g_assert_true(gtk_widget_compute_point(widget, window, &point, &at));
    int width = gdk_texture_get_width(texture), height = gdk_texture_get_height(texture);
    int x = CLAMP((int)at.x, 0, width - 1), y = CLAMP((int)at.y, 0, height - 1);
    guchar *pixels = g_malloc((gsize)width * height * 4);
    gdk_texture_download(texture, pixels, width * 4);
    guchar *pixel = pixels + ((gsize)y * width + x) * 4;
    double brightness = (pixel[0] + pixel[1] + pixel[2]) / (3.0 * 255.0);
    if (dark) g_assert_cmpfloat(brightness, <, 0.45);
    else g_assert_cmpfloat(brightness, >, 0.55);
    g_free(pixels);
}

static void assert_foreground(GtkWidget *widget, gboolean dark) {
    GdkRGBA color;
    gtk_widget_get_color(widget, &color);
    double brightness = (color.red + color.green + color.blue) / 3;
    if (dark) g_assert_cmpfloat(brightness, >, 0.55);
    else g_assert_cmpfloat(brightness, <, 0.45);
}

static GtkWidget *find_type(GtkWidget *widget, GType type) {
    if (g_type_is_a(G_OBJECT_TYPE(widget), type)) return widget;
    for (GtkWidget *child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child)) {
        GtkWidget *found = find_type(child, type);
        if (found) return found;
    }
    return NULL;
}

static void check_colors(AppState *window, AppState *tab, gboolean dark) {
    flush_events();
    g_assert_cmpint(tab->dark_mode, ==, dark);
    g_assert_cmpstr(gtk_source_style_scheme_get_id(gtk_source_buffer_get_style_scheme(tab->buffer)),
                    ==, dark ? "classic-dark" : "classic");
    GtkWidget *header = gtk_window_get_titlebar(GTK_WINDOW(window->window));
    GtkWidget *menu = find_type(header, GTK_TYPE_POPOVER_MENU_BAR);
    assert_foreground(menu, dark);
    assert_foreground(tab->tab_label, dark);
    assert_foreground(tab->status_position, dark);
    assert_foreground(tab->find_entry, dark);
    GdkTexture *texture = window_texture(GTK_WINDOW(window->window));
    assert_background(texture, window->window, header, .8, .5, dark);
    assert_background(texture, window->window, GTK_WIDGET(tab->view), .8, .7, dark);
    assert_background(texture, window->window, tab->status, .5, .5, dark);
    assert_background(texture, window->window, tab->find_bar, .9, .9, dark);
    g_object_unref(texture);

    g_assert_true(gtk_widget_activate(gtk_widget_get_first_child(menu)));
    flush_events();
    GtkWidget *popup = find_type(menu, GTK_TYPE_POPOVER);
    g_assert_nonnull(popup); assert_foreground(popup, dark);
    /* Check the native popover surface separately from the main window. */
    GdkRGBA background;
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    g_assert_true(gtk_style_context_lookup_color(gtk_widget_get_style_context(popup), "theme_bg_color", &background));
    G_GNUC_END_IGNORE_DEPRECATIONS
    double brightness = (background.red + background.green + background.blue) / 3;
    g_assert_true(dark ? brightness < .45 : brightness > .55);
    gtk_popover_popdown(GTK_POPOVER(popup)); flush_events();
}

static void set_desktop_preference(GSettings *desktop, gboolean dark) {
    if (desktop) {
        g_assert_true(g_settings_set_string(desktop, "color-scheme", dark ? "prefer-dark" : "prefer-light"));
    } else {
#if GTK_CHECK_VERSION(4, 20, 0)
        g_object_set(gtk_settings_get_default(), "gtk-interface-color-scheme",
                     dark ? GTK_INTERFACE_COLOR_SCHEME_DARK : GTK_INTERFACE_COLOR_SCHEME_LIGHT, NULL);
#endif
        g_object_set(gtk_settings_get_default(), "gtk-theme-name", dark ? "Adwaita-dark" : "Adwaita", NULL);
    }
}

static void conflicting_gtk_theme(gboolean dark) {
    g_object_set(gtk_settings_get_default(), "gtk-theme-name", dark ? "Adwaita" : "Adwaita-dark", NULL);
#if GTK_CHECK_VERSION(4, 20, 0)
    g_object_set(gtk_settings_get_default(), "gtk-interface-color-scheme",
                 dark ? GTK_INTERFACE_COLOR_SCHEME_LIGHT : GTK_INTERFACE_COLOR_SCHEME_DARK, NULL);
#endif
}

static void check_startup(gboolean dark, GSettings *desktop, const char *directory) {
    set_desktop_preference(desktop, dark);
    if (desktop) conflicting_gtk_theme(dark);
    AppState state = {.skip_session_restore = TRUE};
    GtkApplication *app = gtk_application_new(dark ? QUILLMOTE_APP_ID ".DarkStartup" : QUILLMOTE_APP_ID ".LightStartup",
                                             G_APPLICATION_NON_UNIQUE);
    g_assert_true(g_application_register(G_APPLICATION(app), NULL, NULL));
    activate(app, &state);
    gtk_widget_set_visible(state.find_bar, TRUE);
    check_colors(&state, &state, dark);
    capture_window(GTK_WINDOW(state.window), directory, dark ? "startup-dark.png" : "startup-light.png");
    AppState *second = new_tab(&state);
    gtk_widget_set_visible(second->find_bar, TRUE);
    gtk_text_buffer_set_text(GTK_TEXT_BUFFER(second->buffer), "Preserve my edits", -1);
    PangoFontDescription *font = pango_font_description_copy(second->font);
    int zoom = second->zoom;
    for (int i = 0; i < 4; i++) {
        gboolean next_dark = i % 2 == 0;
        set_desktop_preference(desktop, next_dark);
        if (desktop) conflicting_gtk_theme(next_dark);
        check_colors(&state, second, next_dark);
        g_assert_cmpint(state.dark_mode, ==, next_dark); /* Also the inactive tab. */
        g_assert_cmpstr(gtk_source_style_scheme_get_id(gtk_source_buffer_get_style_scheme(state.buffer)),
                        ==, next_dark ? "classic-dark" : "classic");
        g_assert_true(pango_font_description_equal(font, second->font));
        g_assert_cmpint(second->zoom, ==, zoom);
        GtkTextIter start, end;
        gtk_text_buffer_get_bounds(GTK_TEXT_BUFFER(second->buffer), &start, &end);
        gchar *text = gtk_text_buffer_get_text(GTK_TEXT_BUFFER(second->buffer), &start, &end, FALSE);
        g_assert_cmpstr(text, ==, "Preserve my edits"); g_free(text);
        show_help(GTK_WINDOW(state.window)); flush_events();
        GWeakRef *ref = g_object_get_data(G_OBJECT(state.window), "quillmote-help");
        GtkWindow *help = g_weak_ref_get(ref); g_assert_nonnull(help);
        assert_foreground(gtk_window_get_titlebar(help), next_dark);
        gtk_window_destroy(help); g_object_unref(help);
    }
    /* Closing the original document must not disconnect the window subscription. */
    close_tab(&state); flush_events();
    g_assert_true(state.disposed);
    set_desktop_preference(desktop, TRUE);
    check_colors(&state, second, TRUE);
#if GTK_CHECK_VERSION(4, 20, 0)
    /* With no explicit desktop preference, fall back to GTK's portal setting. */
    if (desktop) g_assert_true(g_settings_set_string(desktop, "color-scheme", "default"));
    g_object_set(gtk_settings_get_default(), "gtk-theme-name", "Adwaita-dark",
                 "gtk-interface-color-scheme", GTK_INTERFACE_COLOR_SCHEME_LIGHT, NULL);
    check_colors(&state, second, FALSE);
    g_object_set(gtk_settings_get_default(), "gtk-theme-name", "Adwaita",
                 "gtk-interface-color-scheme", GTK_INTERFACE_COLOR_SCHEME_DARK, NULL);
    check_colors(&state, second, TRUE);
    g_object_set(gtk_settings_get_default(), "gtk-interface-color-scheme", GTK_INTERFACE_COLOR_SCHEME_UNSUPPORTED, NULL);
#endif
    /* Desktops without a color-scheme setting still use the native theme. */
    if (desktop) g_assert_true(g_settings_set_string(desktop, "color-scheme", "default"));
    g_object_set(gtk_settings_get_default(), "gtk-theme-name", "Adwaita-dark", NULL);
    check_colors(&state, second, TRUE);
    g_assert_nonnull(state.theme_css);
    g_object_set(gtk_settings_get_default(), "gtk-theme-name", "Adwaita", NULL);
    check_colors(&state, second, FALSE);
    pango_font_description_free(font);
    while (second->recovery_writing || second->recovery_idle) g_main_context_iteration(NULL, TRUE);
    gtk_window_destroy(GTK_WINDOW(state.window)); shutdown_app(G_APPLICATION(app), &state); g_object_unref(app);
}

int main(void) {
    gchar *directory = g_dir_make_tmp("quillmote-themes-XXXXXX", NULL);
    g_assert_nonnull(directory);
    g_setenv("XDG_CONFIG_HOME", directory, TRUE);
    g_setenv("GSETTINGS_BACKEND", "memory", TRUE);
    g_unsetenv("GTK_THEME");
    gtk_init();
    /* Test final colors, independently of the desktop's transition duration. */
    g_object_set(gtk_settings_get_default(), "gtk-enable-animations", FALSE, NULL);
    GSettingsSchemaSource *source = g_settings_schema_source_get_default();
    GSettingsSchema *schema = source ? g_settings_schema_source_lookup(source, "org.gnome.desktop.interface", TRUE) : NULL;
    GSettings *desktop = schema && g_settings_schema_has_key(schema, "color-scheme") ? g_settings_new_full(schema, NULL, NULL) : NULL;
    if (schema) g_settings_schema_unref(schema);
    check_startup(FALSE, desktop, directory);
    check_startup(TRUE, desktop, directory);
    g_clear_object(&desktop); g_free(directory);
    g_print("Light/dark startup, native surfaces, live changes, dialogs and tab lifecycle passed.\n");
    return 0;
}
