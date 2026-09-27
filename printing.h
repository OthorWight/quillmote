#ifndef QUILLMOTE_PRINTING_H
#define QUILLMOTE_PRINTING_H
#include <gtksourceview/gtksource.h>

typedef struct {
    GtkPageSetup *page_setup;
    GtkPrintSettings *settings;
    gchar *header;
    gchar *footer;
} PrintOptions;

void print_options_init(PrintOptions *options);
void print_options_clear(PrintOptions *options);
void show_page_setup(GtkWindow *parent, PrintOptions *options);
gchar **format_print_band(const char *format, const char *filename, int page, GDateTime *now);
gboolean print_document(GtkWindow *parent, GtkSourceView *view, const PangoFontDescription *font,
                        const char *filename, PrintOptions *options, const char *export_path, GError **error);
#endif
