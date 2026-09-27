#ifndef QUILLMOTE_DOCUMENT_H
#define QUILLMOTE_DOCUMENT_H

#include <glib.h>

typedef enum { ENCODING_UTF8, ENCODING_UTF8_BOM, ENCODING_UTF16_LE, ENCODING_UTF16_BE, ENCODING_ANSI, ENCODING_BYTES } TextEncoding;
typedef enum { ENDING_LF, ENDING_CRLF, ENDING_CR } LineEnding;

const char *encoding_label(TextEncoding encoding);
gchar *decode_document(const gchar *bytes, gsize length, int requested_encoding,
                       TextEncoding *encoding, LineEnding *ending, GError **error);
GBytes *encode_document(const gchar *text, TextEncoding encoding, LineEnding ending, GError **error);

#endif
