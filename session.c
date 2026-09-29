#include "session.h"
#include <glib/gstdio.h>
#include <errno.h>

static gchar *session_path(void) {
    return g_build_filename(g_get_user_config_dir(), "quillmote", "session.ini", NULL);
}

static void tab_free(gpointer data) {
    SessionTab *tab = data;
    g_free(tab->path); g_free(tab);
}

Session *session_new(void) {
    Session *session = g_new0(Session, 1);
    session->tabs = g_ptr_array_new_with_free_func(tab_free);
    session->active = -1;
    return session;
}

void session_free(Session *session) {
    if (!session) return;
    g_ptr_array_unref(session->tabs); g_free(session);
}

Session *session_load(void) {
    Session *session = session_new();
    GKeyFile *key = g_key_file_new();
    gchar *path = session_path();
    if (g_key_file_load_from_file(key, path, G_KEY_FILE_NONE, NULL)) {
        int count = CLAMP(g_key_file_get_integer(key, "Session", "count", NULL), 0, 10000);
        session->active = g_key_file_get_integer(key, "Session", "active", NULL);
        for (int i = 0; i < count; i++) {
            gchar *group = g_strdup_printf("Tab %d", i);
            SessionTab *tab = g_new0(SessionTab, 1);
            tab->path = g_key_file_get_string(key, group, "path", NULL);
            if (tab->path && !g_path_is_absolute(tab->path)) g_clear_pointer(&tab->path, g_free);
            tab->cursor = MAX(0, g_key_file_get_integer(key, group, "cursor", NULL));
            tab->zoom = g_key_file_get_integer(key, group, "zoom", NULL);
            if (tab->zoom < 50 || tab->zoom > 300) tab->zoom = 100;
            g_ptr_array_add(session->tabs, tab); g_free(group);
        }
    }
    g_key_file_unref(key); g_free(path);
    return session;
}

gboolean session_save(const Session *session, GError **error) {
    GKeyFile *key = g_key_file_new();
    g_key_file_set_integer(key, "Session", "count", session->tabs->len);
    g_key_file_set_integer(key, "Session", "active", session->active);
    for (guint i = 0; i < session->tabs->len; i++) {
        SessionTab *tab = g_ptr_array_index(session->tabs, i);
        gchar *group = g_strdup_printf("Tab %u", i);
        if (tab->path) g_key_file_set_string(key, group, "path", tab->path);
        g_key_file_set_integer(key, group, "cursor", tab->cursor);
        g_key_file_set_integer(key, group, "zoom", tab->zoom);
        g_free(group);
    }
    gchar *path = session_path(), *directory = g_path_get_dirname(path);
    gsize length;
    gchar *contents = g_key_file_to_data(key, &length, NULL);
    gboolean saved = FALSE;
    if (g_mkdir_with_parents(directory, 0700) != 0)
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno), "Could not create session folder: %s", g_strerror(errno));
    else saved = g_file_set_contents_full(path, contents, length,
        G_FILE_SET_CONTENTS_CONSISTENT | G_FILE_SET_CONTENTS_DURABLE, 0600, error);
    g_free(contents); g_free(path); g_free(directory); g_key_file_unref(key);
    return saved;
}
