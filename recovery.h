#ifndef QUILLMOTE_RECOVERY_H
#define QUILLMOTE_RECOVERY_H

#include "document.h"

typedef struct {
    gchar *path;
    int lock_fd;
} RecoverySession;

typedef struct {
    gchar *text, *filename;
    TextEncoding encoding;
    LineEnding ending;
    int cursor;
} RecoveryDocument;

RecoverySession *recovery_session_new(const char *directory, GError **error);
RecoverySession *recovery_session_claim(const char *path, GError **error);
void recovery_session_free(RecoverySession *session);
gchar *recovery_find(const char *directory);
RecoveryDocument *recovery_read(const char *path, GError **error);
void recovery_document_free(RecoveryDocument *document);
/* Write a private temporary snapshot; the UI commits it only if still current. */
gchar *recovery_write_temporary(const char *path, const RecoveryDocument *document, GError **error);

#endif
