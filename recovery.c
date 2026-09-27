#define _GNU_SOURCE
#include "recovery.h"
#include <glib/gstdio.h>
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

RecoverySession *recovery_session_claim(const char *path, GError **error) {
    gchar *lock = g_strconcat(path, ".lock", NULL);
    int fd = g_open(lock, O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    g_free(lock);
    if (fd < 0 || flock(fd, LOCK_EX | LOCK_NB) != 0) {
        int saved_errno = errno;
        if (fd >= 0) close(fd);
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(saved_errno),
                    "Could not access recovery snapshot: %s", g_strerror(saved_errno));
        return NULL;
    }
    RecoverySession *session = g_new0(RecoverySession, 1);
    session->path = g_strdup(path); session->lock_fd = fd;
    return session;
}

RecoverySession *recovery_session_new(const char *directory, GError **error) {
    if (g_mkdir_with_parents(directory, 0700) != 0) {
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
                    "Could not create recovery directory: %s", g_strerror(errno));
        return NULL;
    }
    gchar *id = g_uuid_string_random(), *name = g_strconcat(id, ".ini", NULL);
    gchar *path = g_build_filename(directory, name, NULL);
    RecoverySession *session = recovery_session_claim(path, error);
    g_free(id); g_free(name); g_free(path);
    return session;
}

void recovery_session_free(RecoverySession *session) {
    if (!session) return;
    /* Keep lock inodes for existing snapshots: unlinking them can split a lock
     * between a concurrent scanner and a newly opened descriptor. */
    close(session->lock_fd);
    g_free(session->path); g_free(session);
}

gchar *recovery_find(const char *directory) {
    GDir *dir = g_dir_open(directory, 0, NULL);
    if (!dir) return NULL;
    const char *name;
    gchar *found = NULL;
    while ((name = g_dir_read_name(dir))) {
        if (!g_str_has_suffix(name, ".ini")) continue;
        gchar *path = g_build_filename(directory, name, NULL);
        RecoverySession *session = recovery_session_claim(path, NULL);
        if (session && g_file_test(path, G_FILE_TEST_IS_REGULAR)) found = g_strdup(path);
        recovery_session_free(session); g_free(path);
        if (found) break;
    }
    g_dir_close(dir); return found;
}

void recovery_document_free(RecoveryDocument *document) {
    if (!document) return;
    g_free(document->text); g_free(document->filename); g_free(document);
}

RecoveryDocument *recovery_read(const char *path, GError **error) {
    GKeyFile *key = g_key_file_new();
    RecoveryDocument *doc = NULL;
    if (!g_key_file_load_from_file(key, path, G_KEY_FILE_NONE, error)) goto done;
    doc = g_new0(RecoveryDocument, 1);
    doc->text = g_key_file_get_string(key, "Document", "text", error);
    if (!doc->text) goto invalid;
    doc->filename = g_key_file_get_string(key, "Document", "filename", NULL);
    doc->encoding = g_key_file_get_integer(key, "Document", "encoding", NULL);
    doc->ending = g_key_file_get_integer(key, "Document", "ending", NULL);
    doc->cursor = g_key_file_get_integer(key, "Document", "cursor", NULL);
    if (!g_utf8_validate(doc->text, -1, NULL) || strlen(doc->text) > G_MAXINT ||
        doc->encoding < ENCODING_UTF8 || doc->encoding > ENCODING_BYTES ||
        doc->ending < ENDING_LF || doc->ending > ENDING_CR) {
        g_set_error_literal(error, G_FILE_ERROR, G_FILE_ERROR_INVAL, "Invalid recovery snapshot.");
        goto invalid;
    }
    goto done;
invalid:
    recovery_document_free(doc); doc = NULL;
done:
    g_key_file_unref(key); return doc;
}

gchar *recovery_write_temporary(const char *path, const RecoveryDocument *doc, GError **error) {
    GKeyFile *key = g_key_file_new();
    g_key_file_set_string(key, "Document", "text", doc->text);
    if (doc->filename) g_key_file_set_string(key, "Document", "filename", doc->filename);
    g_key_file_set_integer(key, "Document", "encoding", doc->encoding);
    g_key_file_set_integer(key, "Document", "ending", doc->ending);
    g_key_file_set_integer(key, "Document", "cursor", doc->cursor);
    gsize length;
    gchar *contents = g_key_file_to_data(key, &length, NULL);
    gchar *id = g_uuid_string_random(), *temporary = g_strconcat(path, ".", id, ".tmp", NULL);
    gboolean ok = g_file_set_contents_full(temporary, contents, length,
        G_FILE_SET_CONTENTS_CONSISTENT | G_FILE_SET_CONTENTS_DURABLE, 0600, error);
    g_free(id); g_free(contents); g_key_file_unref(key);
    if (!ok) { g_free(temporary); return NULL; }
    return temporary;
}
