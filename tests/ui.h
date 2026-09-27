#ifndef QUILLMOTE_TEST_UI_H
#define QUILLMOTE_TEST_UI_H

static void flush_events(void) {
    gint64 deadline = g_get_monotonic_time() + 100 * G_TIME_SPAN_MILLISECOND;
    do {
        while (g_main_context_iteration(NULL, FALSE)) {}
        g_usleep(1000);
    } while (g_get_monotonic_time() < deadline);
}

static void capture_window(GtkWindow *window, const char *directory, const char *name) {
    flush_events();
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
    GskRenderer *renderer = gtk_native_get_renderer(GTK_NATIVE(GTK_WIDGET(window)));
    graphene_rect_t viewport = GRAPHENE_RECT_INIT(0, 0, width, height);
    GdkTexture *texture = gsk_renderer_render_texture(renderer, node, &viewport);
    gchar *path = g_build_filename(directory, name, NULL);
    g_assert_true(gdk_texture_save_to_png(texture, path));
    g_print("Screenshot: %s\n", path);
    g_free(path); g_object_unref(texture); gsk_render_node_unref(node); g_object_unref(paintable);
}

#endif
