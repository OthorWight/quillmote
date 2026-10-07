#define _GNU_SOURCE
#include "../recovery.h"
#include "../fileio.h"
#include <glib/gstdio.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>

static void snapshot_round_trip(void) {
    gchar *directory = g_dir_make_tmp("quillmote-storage-XXXXXX", NULL);
    gchar *path = g_build_filename(directory, "snapshot.ini", NULL);
    const char *texts[] = {"", "  \tleading\nbackslash\\tab\treturn\r日本語 🙂\ntrailing  ", "\n[Document]\ntext=other\n#comment\n"};
    for (guint i = 0; i < G_N_ELEMENTS(texts); i++) {
        RecoveryDocument original = {.text = (gchar *)texts[i], .filename = "/tmp/space \\ newline\nnotes", .encoding = ENCODING_UTF16_BE, .ending = ENDING_CR, .cursor = 17};
        GError *error = NULL;
        gchar *temporary = recovery_write_temporary(path, &original, &error);
        g_assert_no_error(error); g_assert_nonnull(temporary);
        GStatBuf info; g_assert_cmpint(g_stat(temporary, &info), ==, 0);
        g_assert_cmpint(info.st_mode & 0777, ==, 0600);
        RecoveryDocument *read = recovery_read(temporary, &error);
        g_assert_no_error(error); g_assert_nonnull(read);
        g_assert_cmpstr(read->text, ==, original.text);
        g_assert_cmpstr(read->filename, ==, original.filename);
        g_assert_cmpint(read->encoding, ==, original.encoding);
        g_assert_cmpint(read->ending, ==, original.ending);
        g_assert_cmpint(read->cursor, ==, original.cursor);
        recovery_document_free(read); g_unlink(temporary); g_free(temporary);
    }
    GString *large = g_string_new("  ");
    for (int i = 0; i < 10000; i++) g_string_append(large, "café\\\t\r\n");
    RecoveryDocument original = {.text = large->str, .encoding = ENCODING_UTF8, .ending = ENDING_LF};
    GError *error = NULL;
    gchar *temporary = recovery_write_temporary(path, &original, &error);
    g_assert_no_error(error);
    RecoveryDocument *read = recovery_read(temporary, &error);
    g_assert_no_error(error); g_assert_cmpstr(read->text, ==, large->str);
    recovery_document_free(read); g_unlink(temporary); g_free(temporary); g_string_free(large, TRUE);
    g_free(path); g_rmdir(directory); g_free(directory);
}

static void atomic_writes(void) {
    gchar *directory = g_dir_make_tmp("quillmote-atomic-XXXXXX", NULL);
    gchar *path = g_build_filename(directory, "document.txt", NULL);
    GError *error = NULL;
    g_assert_true(g_file_set_contents(path, "keep original", -1, &error)); g_assert_no_error(error);
    g_assert_cmpint(g_chmod(path, 0640), ==, 0);
    AtomicFile file;
    g_assert_true(atomic_file_begin(&file, path, FALSE, &error));
    g_assert_true(file_write_all(file.fd, "partial", 7, &error));
    gchar *temporary = g_strdup(file.temporary);
    atomic_file_abort(&file);
    g_assert_false(g_file_test(temporary, G_FILE_TEST_EXISTS)); g_free(temporary);
    gchar *text = NULL; g_assert_true(g_file_get_contents(path, &text, NULL, &error));
    g_assert_cmpstr(text, ==, "keep original"); g_free(text);
    g_assert_true(atomic_file_begin(&file, path, FALSE, &error));
    g_assert_true(file_write_all(file.fd, "replacement", 11, &error));
    g_assert_true(atomic_file_commit(&file, &error)); g_assert_no_error(error);
    atomic_file_abort(&file); /* Cleanup is safe after commit. */
    g_assert_true(g_file_get_contents(path, &text, NULL, &error));
    g_assert_cmpstr(text, ==, "replacement"); g_free(text);
    GStatBuf info; g_assert_cmpint(g_stat(path, &info), ==, 0);
    g_assert_cmpint(info.st_mode & 0777, ==, 0640);
    g_assert_false(atomic_file_begin(&file, directory, FALSE, &error));
    g_assert_nonnull(error); g_clear_error(&error);
    atomic_file_abort(&file);
    /* A failed commit removes its temporary file and preserves the destination. */
    g_assert_true(atomic_file_begin(&file, path, FALSE, &error));
    g_assert_true(file_write_all(file.fd, "incomplete", 10, &error));
    temporary = g_strdup(file.temporary);
    g_free(file.destination); file.destination = g_strdup(directory);
    g_assert_false(atomic_file_commit(&file, &error));
    g_assert_nonnull(error); g_clear_error(&error);
    g_assert_false(g_file_test(temporary, G_FILE_TEST_EXISTS)); g_free(temporary);
    g_assert_true(g_file_get_contents(path, &text, NULL, &error));
    g_assert_cmpstr(text, ==, "replacement"); g_free(text);
    /* Match the previous GLib writer's behavior when saving through a symlink. */
    gchar *old_link = g_build_filename(directory, "old-link", NULL);
    gchar *new_link = g_build_filename(directory, "new-link", NULL);
    g_assert_cmpint(symlink(path, old_link), ==, 0);
    g_assert_cmpint(symlink(path, new_link), ==, 0);
    g_assert_true(g_file_set_contents(old_link, "linked", -1, &error));
    g_assert_true(atomic_file_begin(&file, new_link, FALSE, &error));
    g_assert_true(file_write_all(file.fd, "linked", 6, &error));
    g_assert_true(atomic_file_commit(&file, &error)); g_assert_no_error(error);
    g_assert_cmpint(g_file_test(new_link, G_FILE_TEST_IS_SYMLINK), ==,
                    g_file_test(old_link, G_FILE_TEST_IS_SYMLINK));
    g_assert_true(g_file_get_contents(path, &text, NULL, &error));
    g_assert_cmpstr(text, ==, "replacement"); g_free(text);
    g_unlink(old_link); g_unlink(new_link); g_free(old_link); g_free(new_link);
    g_unlink(path); g_free(path); g_rmdir(directory); g_free(directory);
}

int main(int argc, char **argv) {
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/storage/snapshot-round-trip", snapshot_round_trip);
    g_test_add_func("/storage/atomic-writes", atomic_writes);
    return g_test_run();
}
