#include "document.h"
#include <string.h>

const char *encoding_label(TextEncoding encoding) {
    static const char *labels[] = { "UTF-8", "UTF-8 with BOM", "Unicode (UTF-16 LE)",
                                   "Unicode big endian (UTF-16 BE)", "ANSI (Windows-1252)", "Raw bytes (Latin-1)" };
    return labels[encoding];
}

static gchar *decode_bytes(const gchar *bytes, gsize length, TextEncoding *encoding, LineEnding *ending, GError **error) {
    GString *text = g_string_sized_new(length);
    for (gsize i = 0; i < length; i++) {
        gunichar value = (guint8)bytes[i];
        /* Visible control pictures keep NUL and CR out of GTK's text storage,
         * while retaining a reversible, one-character-per-byte representation. */
        if (value < 0x20 && value != '\n' && value != '\t') value += 0x2400;
        else if (value == 0x7f) value = 0x2421;
        g_string_append_unichar(text, value);
        if (text->len > G_MAXINT) {
            g_set_error_literal(error, G_CONVERT_ERROR, G_CONVERT_ERROR_FAILED, "This file is too large to display.");
            g_string_free(text, TRUE); return NULL;
        }
    }
    *encoding = ENCODING_BYTES; *ending = ENDING_LF;
    return g_string_free(text, FALSE);
}

gchar *decode_document_owned(gchar *bytes, gsize length, int requested_encoding,
                       TextEncoding *encoding, LineEnding *ending, GError **error) {
    if (length > G_MAXINT) {
        g_set_error_literal(error, G_CONVERT_ERROR, G_CONVERT_ERROR_FAILED, "This file is too large to display.");
        g_free(bytes); return NULL;
    }
    if (requested_encoding == ENCODING_BYTES) {
        gchar *text = decode_bytes(bytes, length, encoding, ending, error);
        g_free(bytes); return text;
    }
    gsize skip = 0;
    TextEncoding detected = ENCODING_UTF8;
    if (length >= 3 && memcmp(bytes, "\xef\xbb\xbf", 3) == 0) {
        detected = ENCODING_UTF8_BOM; skip = 3;
    } else if (length >= 2 && memcmp(bytes, "\xff\xfe", 2) == 0) {
        detected = ENCODING_UTF16_LE; skip = 2;
    } else if (length >= 2 && memcmp(bytes, "\xfe\xff", 2) == 0) {
        detected = ENCODING_UTF16_BE; skip = 2;
    } else if (requested_encoding < 0 && !g_utf8_validate(bytes, length, NULL)) {
        detected = ENCODING_ANSI;
    }
    if (requested_encoding >= 0) {
        if (detected != (TextEncoding)requested_encoding &&
            !(detected == ENCODING_UTF8_BOM && requested_encoding == ENCODING_UTF8)) skip = 0;
        detected = requested_encoding;
    }
    gchar *decoded;
    gsize written = length - skip;
    const char *charset = detected == ENCODING_UTF16_LE ? "UTF-16LE" :
                          detected == ENCODING_UTF16_BE ? "UTF-16BE" :
                          detected == ENCODING_ANSI ? "WINDOWS-1252" : "UTF-8";
    GError *conversion_error = NULL;
    if ((detected == ENCODING_UTF8 || detected == ENCODING_UTF8_BOM) &&
        g_utf8_validate(bytes + skip, length - skip, NULL)) {
        /* Reuse the read buffer for ordinary UTF-8, including BOM removal. */
        decoded = bytes;
        memmove(decoded, decoded + skip, written);
        decoded[written] = '\0';
        bytes = NULL;
    } else {
        decoded = g_convert(bytes + skip, length - skip, "UTF-8", charset, NULL, &written, &conversion_error);
    }
    if (!decoded || !g_utf8_validate(decoded, written, NULL)) {
        if (requested_encoding < 0) {
            g_clear_error(&conversion_error); g_free(decoded);
            gchar *text = decode_bytes(bytes, length, encoding, ending, error);
            g_free(bytes); return text;
        }
        if (conversion_error) g_propagate_error(error, conversion_error);
        else g_set_error_literal(error, G_CONVERT_ERROR, G_CONVERT_ERROR_ILLEGAL_SEQUENCE, "This file contains non-text data.");
        g_free(bytes); g_free(decoded); return NULL;
    }
    g_free(bytes);
    if (written > G_MAXINT) {
        g_set_error_literal(error, G_CONVERT_ERROR, G_CONVERT_ERROR_ILLEGAL_SEQUENCE,
                            "This file is too large to display.");
        g_free(decoded);
        return NULL;
    }
    gsize normalized = 0;
    gboolean found_ending = FALSE;
    *ending = ENDING_LF;
    for (gsize i = 0; i < written; i++) {
        if (decoded[i] == '\r' || decoded[i] == '\n') {
            gboolean crlf = decoded[i] == '\r' && i + 1 < written && decoded[i + 1] == '\n';
            if (!found_ending) {
                *ending = crlf ? ENDING_CRLF : decoded[i] == '\r' ? ENDING_CR : ENDING_LF;
                found_ending = TRUE;
            }
            if (crlf) i++;
            decoded[normalized++] = '\n';
        } else {
            decoded[normalized++] = decoded[i];
        }
    }
    decoded[normalized] = '\0';
    *encoding = detected;
    return decoded;
}

gchar *decode_document(const gchar *bytes, gsize length, int requested_encoding,
                       TextEncoding *encoding, LineEnding *ending, GError **error) {
    if (length > G_MAXINT) {
        g_set_error_literal(error, G_CONVERT_ERROR, G_CONVERT_ERROR_FAILED, "This file is too large to display.");
        return NULL;
    }
    gchar *owned = g_malloc(length + 1);
    memcpy(owned, bytes, length); owned[length] = '\0';
    return decode_document_owned(owned, length, requested_encoding, encoding, ending, error);
}

GBytes *encode_document_chunk(const gchar *text, TextEncoding encoding, LineEnding ending,
                              gboolean include_bom, GError **error) {
    if (!g_utf8_validate(text, -1, NULL)) {
        g_set_error_literal(error, G_CONVERT_ERROR, G_CONVERT_ERROR_ILLEGAL_SEQUENCE, "This text is not valid UTF-8.");
        return NULL;
    }
    if (encoding == ENCODING_BYTES) {
        GByteArray *bytes = g_byte_array_new();
        for (const char *p = text; *p; p = g_utf8_next_char(p)) {
            gunichar value = g_utf8_get_char(p);
            if (value >= 0x2400 && value <= 0x241f) value -= 0x2400;
            else if (value == 0x2421) value = 0x7f;
            if (value > 0xff) {
                g_set_error_literal(error, G_CONVERT_ERROR, G_CONVERT_ERROR_ILLEGAL_SEQUENCE,
                    "This character cannot be stored as a single byte. Choose UTF-8 in Save As to save the displayed text instead.");
                g_byte_array_unref(bytes); return NULL;
            }
            guint8 byte = value;
            g_byte_array_append(bytes, &byte, 1);
        }
        return g_byte_array_free_to_bytes(bytes);
    }
    if ((encoding == ENCODING_UTF8 || encoding == ENCODING_UTF8_BOM) &&
        ending == ENDING_LF && (!include_bom || encoding == ENCODING_UTF8))
        return g_bytes_new(text, strlen(text));
    GString *lines = g_string_sized_new(strlen(text));
    for (const char *p = text; *p; p++) {
        if (*p == '\n') g_string_append(lines, ending == ENDING_CRLF ? "\r\n" : ending == ENDING_CR ? "\r" : "\n");
        else g_string_append_c(lines, *p);
    }
    const char *charset = encoding == ENCODING_UTF16_LE ? "UTF-16LE" :
                          encoding == ENCODING_UTF16_BE ? "UTF-16BE" :
                          encoding == ENCODING_ANSI ? "WINDOWS-1252" : "UTF-8";
    gsize length;
    gchar *converted = g_convert(lines->str, lines->len, charset, "UTF-8", NULL, &length, error);
    g_string_free(lines, TRUE);
    if (!converted) {
        if (encoding == ENCODING_ANSI)
            g_prefix_error(error, "Some characters cannot be saved as ANSI (Windows-1252). Choose UTF-8 or Unicode in Save As. ");
        return NULL;
    }
    GByteArray *result = g_byte_array_new();
    if (include_bom && encoding == ENCODING_UTF8_BOM) g_byte_array_append(result, (const guint8 *)"\xef\xbb\xbf", 3);
    if (include_bom && encoding == ENCODING_UTF16_LE) g_byte_array_append(result, (const guint8 *)"\xff\xfe", 2);
    if (include_bom && encoding == ENCODING_UTF16_BE) g_byte_array_append(result, (const guint8 *)"\xfe\xff", 2);
    g_byte_array_append(result, (const guint8 *)converted, length);
    g_free(converted);
    return g_byte_array_free_to_bytes(result);
}

GBytes *encode_document(const gchar *text, TextEncoding encoding, LineEnding ending, GError **error) {
    return encode_document_chunk(text, encoding, ending, TRUE, error);
}
