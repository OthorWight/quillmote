#define main quillmote_main
#include "../main.c"
#undef main
#include "ui.h"

static GtkWidget *find_type(GtkWidget *widget, GType type) {
    if (g_type_is_a(G_OBJECT_TYPE(widget), type)) return widget;
    for (GtkWidget *child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child)) {
        GtkWidget *found = find_type(child, type); if (found) return found;
    }
    return NULL;
}

int main(void) {
    gchar *directory = g_dir_make_tmp("quillmote-help-XXXXXX", NULL);
    g_setenv("XDG_CONFIG_HOME", directory, TRUE);
    gtk_init();
    GtkApplication *app = gtk_application_new(QUILLMOTE_APP_ID ".HelpTest", G_APPLICATION_NON_UNIQUE);
    g_assert_true(g_application_register(G_APPLICATION(app), NULL, NULL));
    AppState state = {0}; activate(app, &state); flush_events();
    g_assert_true(gtk_icon_theme_has_icon(gtk_icon_theme_get_for_display(gdk_display_get_default()), QUILLMOTE_APP_ID));
    g_action_group_activate_action(G_ACTION_GROUP(app), "help", NULL); flush_events();
    GWeakRef *ref = g_object_get_data(G_OBJECT(state.window), "quillmote-help");
    g_assert_nonnull(ref);
    GtkWindow *help = g_weak_ref_get(ref); g_assert_nonnull(help);
    GtkStack *stack = GTK_STACK(find_type(GTK_WIDGET(help), GTK_TYPE_STACK)); g_assert_nonnull(stack);
    GtkSelectionModel *pages = gtk_stack_get_pages(stack);
    g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(pages)), ==, 8); g_object_unref(pages);
    show_help(GTK_WINDOW(state.window));
    GtkWindow *again = g_weak_ref_get(ref); g_assert_true(help == again); g_object_unref(again);
    gtk_stack_set_visible_child_name(stack, "search"); flush_events();
    capture_window(help, directory, "help.png");
    gtk_window_destroy(help); g_object_unref(help); flush_events();
    g_assert_null(g_weak_ref_get(ref));
    show_help(GTK_WINDOW(state.window)); flush_events();
    help = g_weak_ref_get(ref); g_assert_nonnull(help);
    gtk_window_destroy(help); g_object_unref(help);
    g_action_group_activate_action(G_ACTION_GROUP(app), "about", NULL); flush_events();
    GListModel *windows = gtk_window_get_toplevels(); gboolean found = FALSE;
    for (guint i = 0; i < g_list_model_get_n_items(windows); i++) {
        GtkWidget *window = g_list_model_get_item(windows, i);
        if (GTK_IS_ABOUT_DIALOG(window)) {
            g_assert_cmpstr(gtk_about_dialog_get_version(GTK_ABOUT_DIALOG(window)), ==, QUILLMOTE_VERSION);
            g_assert_cmpint(gtk_about_dialog_get_license_type(GTK_ABOUT_DIALOG(window)), ==, GTK_LICENSE_MIT_X11);
            g_assert_nonnull(strstr(gtk_about_dialog_get_system_information(GTK_ABOUT_DIALOG(window)), "GtkSourceView"));
            capture_window(GTK_WINDOW(window), directory, "about.png");
            gtk_window_destroy(GTK_WINDOW(window)); found = TRUE;
        }
        g_object_unref(window);
        if (found) break;
    }
    g_assert_true(found);
    gtk_window_destroy(GTK_WINDOW(state.window)); shutdown_app(G_APPLICATION(app), &state);
    g_object_unref(app); g_free(directory);
    g_print("Offline help, window reuse, About metadata, license, and embedded icon passed.\n");
    return 0;
}
