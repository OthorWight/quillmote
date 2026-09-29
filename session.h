#ifndef QUILLMOTE_SESSION_H
#define QUILLMOTE_SESSION_H
#include <glib.h>

typedef struct {
    gchar *path;
    int cursor, zoom;
} SessionTab;

typedef struct {
    GPtrArray *tabs;
    int active;
} Session;

Session *session_new(void);
Session *session_load(void);
gboolean session_save(const Session *session, GError **error);
void session_free(Session *session);
#endif
