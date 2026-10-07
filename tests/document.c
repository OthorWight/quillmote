#include "../document.h"
#include <string.h>

static void round_trip(void) {
    const char *text = "café costs €5\nsecond line\n";
    for (int encoding = ENCODING_UTF8; encoding <= ENCODING_ANSI; encoding++) {
        for (int ending = ENDING_LF; ending <= ENDING_CR; ending++) {
            GError *error = NULL;
            GBytes *bytes = encode_document(text, encoding, ending, &error);
            g_assert_no_error(error); g_assert_nonnull(bytes);
            gsize length;
            const char *data = g_bytes_get_data(bytes, &length);
            TextEncoding actual_encoding;
            LineEnding actual_ending;
            gchar *decoded = decode_document(data, length, -1, &actual_encoding, &actual_ending, &error);
            g_assert_no_error(error); g_assert_cmpstr(decoded, ==, text);
            g_assert_cmpint(actual_encoding, ==, encoding); g_assert_cmpint(actual_ending, ==, ending);
            g_free(decoded); g_bytes_unref(bytes);
        }
    }
}

static void unicode_and_invalid(void) {
    for (int encoding = ENCODING_UTF8; encoding <= ENCODING_UTF16_BE; encoding++) {
        GError *error = NULL;
        const char *text = "日本語 🙂\n";
        GBytes *bytes = encode_document(text, encoding, ENDING_CRLF, &error);
        g_assert_no_error(error);
        gsize length; const char *data = g_bytes_get_data(bytes, &length);
        TextEncoding detected; LineEnding ending;
        gchar *decoded = decode_document(data, length, -1, &detected, &ending, &error);
        g_assert_no_error(error); g_assert_cmpstr(decoded, ==, text);
        g_free(decoded); g_bytes_unref(bytes);
    }
    GError *error = NULL;
    g_assert_null(encode_document("日本語", ENCODING_ANSI, ENDING_LF, &error));
    g_assert_nonnull(error); g_clear_error(&error);
    TextEncoding encoding; LineEnding ending;
    g_assert_null(decode_document("abc\0def", 7, ENCODING_UTF8, &encoding, &ending, &error));
    g_assert_nonnull(error); g_clear_error(&error);
    g_assert_null(decode_document("\xff\xfe\x61", 3, ENCODING_UTF16_LE, &encoding, &ending, &error));
    g_assert_nonnull(error); g_clear_error(&error);
    gchar *text = decode_document("", 0, -1, &encoding, &ending, &error);
    g_assert_no_error(error); g_assert_cmpstr(text, ==, ""); g_free(text);
    text = decode_document("a\r\nb\rc\n", 7, -1, &encoding, &ending, &error);
    g_assert_no_error(error); g_assert_cmpstr(text, ==, "a\nb\nc\n"); g_free(text);
    text = decode_document("\x61\0\x62\0", 4, ENCODING_UTF16_LE, &encoding, &ending, &error);
    g_assert_no_error(error); g_assert_cmpstr(text, ==, "ab"); g_free(text);
}

static void raw_bytes(void) {
    gchar bytes[256];
    for (int i = 0; i < 256; i++) bytes[i] = i;
    GError *error = NULL;
    TextEncoding encoding; LineEnding ending;
    gchar *text = decode_document(bytes, sizeof bytes, -1, &encoding, &ending, &error);
    g_assert_no_error(error); g_assert_nonnull(text);
    g_assert_cmpint(encoding, ==, ENCODING_BYTES);
    g_assert_true(g_utf8_validate(text, -1, NULL));
    g_assert_cmpint(g_utf8_strlen(text, -1), ==, 256);
    g_assert_nonnull(strstr(text, "␀"));
    GBytes *encoded = encode_document(text, encoding, ENDING_CRLF, &error);
    g_assert_no_error(error);
    gsize length; const char *data = g_bytes_get_data(encoded, &length);
    g_assert_cmpmem(data, length, bytes, sizeof bytes);
    g_bytes_unref(encoded); g_free(text);

    /* A damaged Unicode BOM must not prevent opening arbitrary files. */
    text = decode_document("\xff\xfe\x61", 3, -1, &encoding, &ending, &error);
    g_assert_no_error(error); g_assert_cmpint(encoding, ==, ENCODING_BYTES);
    encoded = encode_document(text, encoding, ending, &error); g_assert_no_error(error);
    data = g_bytes_get_data(encoded, &length); g_assert_cmpmem(data, length, "\xff\xfe\x61", 3);
    g_bytes_unref(encoded); g_free(text);
    text = decode_document("before\0after", 12, -1, &encoding, &ending, &error);
    g_assert_no_error(error); g_assert_cmpstr(text, ==, "before␀after"); g_free(text);
    g_assert_null(encode_document("日本語", ENCODING_BYTES, ENDING_LF, &error));
    g_assert_nonnull(error); g_clear_error(&error);
}

static void owned_and_chunked(void) {
    TextEncoding encoding; LineEnding ending; GError *error = NULL;
    gchar *owned = g_strdup("\xef\xbb\xbf" "café\r\nsecond\rthird\n");
    gchar *text = decode_document_owned(owned, strlen(owned), -1, &encoding, &ending, &error);
    g_assert_no_error(error); g_assert_true(text == owned);
    g_assert_cmpstr(text, ==, "café\nsecond\nthird\n");
    g_assert_cmpint(encoding, ==, ENCODING_UTF8_BOM); g_assert_cmpint(ending, ==, ENDING_CRLF);
    g_free(text);
    const char *chunks[] = {"café\n", "costs €5\n", ""};
    for (int format = ENCODING_UTF8; format <= ENCODING_ANSI; format++) {
        for (int lines = ENDING_LF; lines <= ENDING_CR; lines++) {
            GByteArray *output = g_byte_array_new();
            for (guint j = 0; j < G_N_ELEMENTS(chunks); j++) {
                GBytes *bytes = encode_document_chunk(chunks[j], format, lines, j == 0, &error);
                g_assert_no_error(error); g_assert_nonnull(bytes);
                gsize length; const guint8 *data = g_bytes_get_data(bytes, &length);
                g_byte_array_append(output, data, length); g_bytes_unref(bytes);
            }
            text = decode_document((const char *)output->data, output->len, -1, &encoding, &ending, &error);
            g_assert_no_error(error); g_assert_cmpstr(text, ==, "café\ncosts €5\n");
            g_assert_cmpint(encoding, ==, format); g_assert_cmpint(ending, ==, lines);
            g_free(text); g_byte_array_unref(output);
        }
    }
}

int main(int argc, char **argv) {
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/document/round-trip", round_trip);
    g_test_add_func("/document/unicode-and-invalid", unicode_and_invalid);
    g_test_add_func("/document/raw-bytes", raw_bytes);
    g_test_add_func("/document/owned-and-chunked", owned_and_chunked);
    return g_test_run();
}
