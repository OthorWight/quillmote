#define _GNU_SOURCE
#include "fileio.h"
#include <glib/gstdio.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

static gboolean io_error(GError **error) {
    int saved = errno;
    g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(saved), "%s", g_strerror(saved));
    return FALSE;
}

void atomic_file_abort(AtomicFile *file) {
    if (file->fd >= 0) { close(file->fd); file->fd = -1; }
    if (file->temporary) g_unlink(file->temporary);
    g_clear_pointer(&file->temporary, g_free);
    g_clear_pointer(&file->destination, g_free);
}

gboolean atomic_file_begin(AtomicFile *file, const char *path, gboolean private_file, GError **error) {
    *file = (AtomicFile){.fd = -1};
    GStatBuf info;
    gboolean exists = g_stat(path, &info) == 0;
    if (!exists && errno != ENOENT) return io_error(error);
    if (exists && !S_ISREG(info.st_mode)) {
        g_set_error_literal(error, G_FILE_ERROR, G_FILE_ERROR_INVAL, "Choose a regular file.");
        return FALSE;
    }
    gchar *directory = g_path_get_dirname(path);
    file->temporary = g_build_filename(directory, ".quillmote-write-XXXXXX", NULL);
    g_free(directory);
    file->destination = g_strdup(path);
    file->fd = g_mkstemp_full(file->temporary, O_WRONLY | O_CLOEXEC, private_file ? 0600 : 0666);
    if (file->fd < 0) { io_error(error); g_clear_pointer(&file->temporary, g_free); atomic_file_abort(file); return FALSE; }
    if (exists && !private_file && fchmod(file->fd, info.st_mode & 0777) != 0) {
        io_error(error); atomic_file_abort(file); return FALSE;
    }
    return TRUE;
}

gboolean file_write_all(int fd, const void *data, gsize length, GError **error) {
    const char *bytes = data;
    while (length) {
        ssize_t count = write(fd, bytes, MIN(length, (gsize)G_MAXSSIZE));
        if (count < 0) { if (errno == EINTR) continue; return io_error(error); }
        if (count == 0) { errno = EIO; return io_error(error); }
        bytes += count; length -= count;
    }
    return TRUE;
}

gboolean atomic_file_commit(AtomicFile *file, GError **error) {
    int result;
    do { result = fsync(file->fd); } while (result != 0 && errno == EINTR);
    if (result != 0 && errno != EINVAL && errno != ENOTSUP) { io_error(error); atomic_file_abort(file); return FALSE; }
    result = close(file->fd); file->fd = -1;
    if (result != 0 || g_rename(file->temporary, file->destination) != 0) {
        io_error(error); atomic_file_abort(file); return FALSE;
    }
    /* Persist the directory entry as well as the data. */
    gchar *directory = g_path_get_dirname(file->destination);
    int directory_fd = g_open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC, 0);
    g_free(directory);
    gboolean durable = TRUE;
    if (directory_fd >= 0) {
        do { result = fsync(directory_fd); } while (result != 0 && errno == EINTR);
        if (result != 0 && errno != EINVAL && errno != ENOTSUP) { io_error(error); durable = FALSE; }
        close(directory_fd);
    } else if (errno != EACCES) { io_error(error); durable = FALSE; }
    g_clear_pointer(&file->temporary, g_free);
    g_clear_pointer(&file->destination, g_free);
    return durable;
}
