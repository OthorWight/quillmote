#ifndef QUILLMOTE_FILEIO_H
#define QUILLMOTE_FILEIO_H
#include <glib.h>

typedef struct { int fd; gchar *temporary, *destination; } AtomicFile;
gboolean atomic_file_begin(AtomicFile *file, const char *path, gboolean private_file, GError **error);
gboolean file_write_all(int fd, const void *data, gsize length, GError **error);
gboolean atomic_file_commit(AtomicFile *file, GError **error);
void atomic_file_abort(AtomicFile *file);
#endif
