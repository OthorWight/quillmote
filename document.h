#ifndef QUILLMOTE_DOCUMENT_H
#define QUILLMOTE_DOCUMENT_H

#include <glib.h>

typedef enum { ENCODING_UTF8, ENCODING_UTF8_BOM, ENCODING_UTF16_LE, ENCODING_UTF16_BE, ENCODING_ANSI, ENCODING_BYTES } TextEncoding;
typedef enum { ENDING_LF, ENDING_CRLF, ENDING_CR } LineEnding;

const char *encoding_label(TextEncoding encoding);
gchar *decode_document(const gchar *bytes, gsize length, int requested_encoding,
                       TextEncoding *encoding, LineEnding *ending, GError **error);
/* Consumes a g_malloc allocation with length + 1 bytes, even on failure. */
gchar *decode_document_owned(gchar *bytes, gsize length, int requested_encoding,
                             TextEncoding *encoding, LineEnding *ending, GError **error);
GBytes *encode_document(const gchar *text, TextEncoding encoding, LineEnding ending, GError **error);
/* Chunks end on UTF-8 character boundaries; only the first includes a BOM. */
GBytes *encode_document_chunk(const gchar *text, TextEncoding encoding, LineEnding ending,
                              gboolean include_bom, GError **error);

#endif
