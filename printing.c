#include "printing.h"
#include <pango/pangocairo.h>

void print_options_init(PrintOptions *options) {
    options->page_setup = gtk_page_setup_new();
    options->settings = gtk_print_settings_new();
    options->header = g_strdup("&f");
    options->footer = g_strdup("Page &p");
}

void print_options_clear(PrintOptions *options) {
    g_clear_object(&options->page_setup);
    g_clear_object(&options->settings);
    g_clear_pointer(&options->header, g_free);
    g_clear_pointer(&options->footer, g_free);
}

gchar **format_print_band(const char *format, const char *filename, int page, GDateTime *now) {
    GString *parts[] = {g_string_new(NULL), g_string_new(NULL), g_string_new(NULL)};
    int alignment = 1;
    for (const char *p = format; *p; p++) {
        if (*p != '&' || !p[1]) { g_string_append_c(parts[alignment], *p); continue; }
        p++;
        gchar *value = NULL;
        switch (g_ascii_tolower(*p)) {
        case 'l': alignment = 0; break;
        case 'c': alignment = 1; break;
        case 'r': alignment = 2; break;
        case 'f': g_string_append(parts[alignment], filename); break;
        case 'p': g_string_append_printf(parts[alignment], "%d", page); break;
        case 'd': value = g_date_time_format(now, "%x"); break;
        case 't': value = g_date_time_format(now, "%X"); break;
        case '&': g_string_append_c(parts[alignment], '&'); break;
        default: g_string_append_c(parts[alignment], '&'); g_string_append_c(parts[alignment], *p); break;
        }
        if (value) { g_string_append(parts[alignment], value); g_free(value); }
    }
    gchar **result = g_new0(gchar *, 4);
    for (int i = 0; i < 3; i++) result[i] = g_string_free(parts[i], FALSE);
    return result;
}

typedef struct {
    PrintOptions *options;
    GtkWidget *window, *paper, *orientation, *margins[4], *header, *footer, *error;
    GList *papers;
} PageDialog;

static void free_page_dialog(gpointer data) {
    PageDialog *dialog = data;
    g_list_free_full(dialog->papers, (GDestroyNotify)gtk_paper_size_free);
    g_free(dialog);
}

static void page_setup_save(GtkButton *button, gpointer data) {
    (void)button;
    PageDialog *dialog = data;
    GtkPageSetup *setup = gtk_page_setup_new();
    GtkPaperSize *paper = g_list_nth_data(dialog->papers, gtk_drop_down_get_selected(GTK_DROP_DOWN(dialog->paper)));
    gtk_page_setup_set_paper_size(setup, paper);
    gtk_page_setup_set_orientation(setup, gtk_drop_down_get_selected(GTK_DROP_DOWN(dialog->orientation)));
    double m[4];
    for (int i = 0; i < 4; i++) m[i] = gtk_spin_button_get_value(GTK_SPIN_BUTTON(dialog->margins[i]));
    gtk_page_setup_set_top_margin(setup, m[0], GTK_UNIT_MM);
    gtk_page_setup_set_bottom_margin(setup, m[1], GTK_UNIT_MM);
    gtk_page_setup_set_left_margin(setup, m[2], GTK_UNIT_MM);
    gtk_page_setup_set_right_margin(setup, m[3], GTK_UNIT_MM);
    if (gtk_page_setup_get_page_width(setup, GTK_UNIT_MM) < 10 || gtk_page_setup_get_page_height(setup, GTK_UNIT_MM) < 30) {
        gtk_label_set_text(GTK_LABEL(dialog->error), "Margins leave too little room for text.");
        g_object_unref(setup);
        return;
    }
    g_set_object(&dialog->options->page_setup, setup);
    g_object_unref(setup);
    g_free(dialog->options->header);
    g_free(dialog->options->footer);
    dialog->options->header = g_strdup(gtk_editable_get_text(GTK_EDITABLE(dialog->header)));
    dialog->options->footer = g_strdup(gtk_editable_get_text(GTK_EDITABLE(dialog->footer)));
    gtk_window_destroy(GTK_WINDOW(dialog->window));
}

static void setup_row(GtkGrid *grid, int row, const char *title, GtkWidget *widget) {
    GtkWidget *label = gtk_label_new(title);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_grid_attach(grid, label, 0, row, 1, 1);
    gtk_widget_set_hexpand(widget, TRUE);
    gtk_grid_attach(grid, widget, 1, row, 1, 1);
}

void show_page_setup(GtkWindow *parent, PrintOptions *options) {
    PageDialog *dialog = g_new0(PageDialog, 1);
    dialog->options = options;
    dialog->window = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(dialog->window), "Page Setup");
    gtk_window_set_transient_for(GTK_WINDOW(dialog->window), parent);
    gtk_window_set_modal(GTK_WINDOW(dialog->window), TRUE);
    gtk_window_set_destroy_with_parent(GTK_WINDOW(dialog->window), TRUE);
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8); gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_widget_set_margin_top(grid, 16); gtk_widget_set_margin_bottom(grid, 16);
    gtk_widget_set_margin_start(grid, 16); gtk_widget_set_margin_end(grid, 16);
    gtk_window_set_child(GTK_WINDOW(dialog->window), grid);
    dialog->papers = gtk_paper_size_get_paper_sizes(TRUE);
    GtkPaperSize *current = gtk_page_setup_get_paper_size(options->page_setup);
    dialog->papers = g_list_prepend(dialog->papers, gtk_paper_size_copy(current));
    GtkStringList *names = gtk_string_list_new(NULL);
    for (GList *item = dialog->papers; item; item = item->next)
        gtk_string_list_append(names, gtk_paper_size_get_display_name(item->data));
    dialog->paper = gtk_drop_down_new(G_LIST_MODEL(names), NULL);
    const char *orientations[] = {"Portrait", "Landscape", "Reverse portrait", "Reverse landscape", NULL};
    dialog->orientation = gtk_drop_down_new_from_strings(orientations);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(dialog->orientation), gtk_page_setup_get_orientation(options->page_setup));
    setup_row(GTK_GRID(grid), 0, "Paper", dialog->paper);
    setup_row(GTK_GRID(grid), 1, "Orientation", dialog->orientation);
    const char *labels[] = {"Top margin (mm)", "Bottom margin (mm)", "Left margin (mm)", "Right margin (mm)"};
    double margins[] = {gtk_page_setup_get_top_margin(options->page_setup, GTK_UNIT_MM),
                        gtk_page_setup_get_bottom_margin(options->page_setup, GTK_UNIT_MM),
                        gtk_page_setup_get_left_margin(options->page_setup, GTK_UNIT_MM),
                        gtk_page_setup_get_right_margin(options->page_setup, GTK_UNIT_MM)};
    for (int i = 0; i < 4; i++) {
        dialog->margins[i] = gtk_spin_button_new_with_range(0, 500, 0.5);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(dialog->margins[i]), margins[i]);
        setup_row(GTK_GRID(grid), 2 + i, labels[i], dialog->margins[i]);
    }
    dialog->header = gtk_entry_new(); dialog->footer = gtk_entry_new();
    gtk_editable_set_text(GTK_EDITABLE(dialog->header), options->header);
    gtk_editable_set_text(GTK_EDITABLE(dialog->footer), options->footer);
    setup_row(GTK_GRID(grid), 6, "Header", dialog->header);
    setup_row(GTK_GRID(grid), 7, "Footer", dialog->footer);
    GtkWidget *hint = gtk_label_new("&f file   &p page   &d date   &t time\n&l left   &c center   &r right   && ampersand\nLeave a field empty to omit it.");
    gtk_grid_attach(GTK_GRID(grid), hint, 0, 8, 2, 1);
    dialog->error = gtk_label_new(NULL); gtk_widget_add_css_class(dialog->error, "error");
    gtk_grid_attach(GTK_GRID(grid), dialog->error, 0, 9, 2, 1);
    GtkWidget *cancel = gtk_button_new_with_label("Cancel"), *save = gtk_button_new_with_label("OK");
    gtk_grid_attach(GTK_GRID(grid), cancel, 0, 10, 1, 1); gtk_grid_attach(GTK_GRID(grid), save, 1, 10, 1, 1);
    g_signal_connect_swapped(cancel, "clicked", G_CALLBACK(gtk_window_destroy), dialog->window);
    g_signal_connect(save, "clicked", G_CALLBACK(page_setup_save), dialog);
    g_object_set_data_full(G_OBJECT(dialog->window), "page-dialog", dialog, free_page_dialog);
    gtk_window_present(GTK_WINDOW(dialog->window));
}

typedef struct {
    GtkSourcePrintCompositor *compositor;
    PrintOptions *options;
    const char *filename;
    GDateTime *now;
} PrintJob;

static void begin_print(GtkPrintOperation *operation, GtkPrintContext *context, gpointer data) {
    (void)operation;
    PrintJob *job = data;
    GtkPageSetup *setup = gtk_print_context_get_page_setup(context);
    gtk_source_print_compositor_set_top_margin(job->compositor,
        gtk_page_setup_get_top_margin(setup, GTK_UNIT_POINTS) + (job->options->header[0] ? 24 : 0), GTK_UNIT_POINTS);
    gtk_source_print_compositor_set_bottom_margin(job->compositor,
        gtk_page_setup_get_bottom_margin(setup, GTK_UNIT_POINTS) + (job->options->footer[0] ? 24 : 0), GTK_UNIT_POINTS);
    gtk_source_print_compositor_set_left_margin(job->compositor, gtk_page_setup_get_left_margin(setup, GTK_UNIT_POINTS), GTK_UNIT_POINTS);
    gtk_source_print_compositor_set_right_margin(job->compositor, gtk_page_setup_get_right_margin(setup, GTK_UNIT_POINTS), GTK_UNIT_POINTS);
}

static gboolean paginate(GtkPrintOperation *operation, GtkPrintContext *context, gpointer data) {
    PrintJob *job = data;
    gboolean done = gtk_source_print_compositor_paginate(job->compositor, context);
    if (done) gtk_print_operation_set_n_pages(operation, gtk_source_print_compositor_get_n_pages(job->compositor));
    return done;
}

static void draw_band(GtkPrintContext *context, PrintJob *job, const char *format, int page, double y) {
    gchar **parts = format_print_band(format, job->filename, page, job->now);
    cairo_t *cr = gtk_print_context_get_cairo_context(context);
    PangoLayout *layout = gtk_print_context_create_pango_layout(context);
    PangoFontDescription *font = pango_font_description_from_string("Sans 9");
    pango_layout_set_font_description(layout, font);
    pango_font_description_free(font);
    GtkPageSetup *setup = gtk_print_context_get_page_setup(context);
    double width = gtk_page_setup_get_page_width(setup, GTK_UNIT_POINTS);
    double left = gtk_page_setup_get_left_margin(setup, GTK_UNIT_POINTS);
    pango_layout_set_width(layout, width * PANGO_SCALE);
    pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
    for (int i = 0; i < 3; i++) {
        pango_layout_set_text(layout, parts[i], -1);
        pango_layout_set_alignment(layout, i == 0 ? PANGO_ALIGN_LEFT : i == 1 ? PANGO_ALIGN_CENTER : PANGO_ALIGN_RIGHT);
        cairo_move_to(cr, left, y);
        pango_cairo_show_layout(cr, layout);
    }
    g_object_unref(layout); g_strfreev(parts);
}

static void draw_page(GtkPrintOperation *operation, GtkPrintContext *context, int page, gpointer data) {
    (void)operation;
    PrintJob *job = data;
    cairo_t *cr = gtk_print_context_get_cairo_context(context);
    GtkPageSetup *setup = gtk_print_context_get_page_setup(context);
    cairo_save(cr);
    /* GtkSourcePrintCompositor positions text relative to the printable origin. */
    cairo_translate(cr, gtk_page_setup_get_left_margin(setup, GTK_UNIT_POINTS),
                    gtk_page_setup_get_top_margin(setup, GTK_UNIT_POINTS));
    gtk_source_print_compositor_draw_page(job->compositor, context, page);
    cairo_restore(cr);
    cairo_save(cr); cairo_set_source_rgb(cr, 0, 0, 0);
    draw_band(context, job, job->options->header, page + 1, gtk_page_setup_get_top_margin(setup, GTK_UNIT_POINTS));
    draw_band(context, job, job->options->footer, page + 1,
              gtk_print_context_get_height(context) - gtk_page_setup_get_bottom_margin(setup, GTK_UNIT_POINTS) - 14);
    cairo_restore(cr);
}

gboolean print_document(GtkWindow *parent, GtkSourceView *view, const PangoFontDescription *font,
                        const char *filename, PrintOptions *options, const char *export_path, GError **error) {
    GtkTextBuffer *source = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view));
    GtkTextIter start, end;
    gtk_text_buffer_get_bounds(source, &start, &end);
    gchar *text = gtk_text_buffer_get_text(source, &start, &end, FALSE);
    /* A plain snapshot keeps dark colors and spelling underlines off the paper. */
    GtkSourceBuffer *snapshot = gtk_source_buffer_new(NULL);
    gtk_text_buffer_set_text(GTK_TEXT_BUFFER(snapshot), text, -1); g_free(text);
    GtkSourcePrintCompositor *compositor = gtk_source_print_compositor_new(snapshot);
    g_object_unref(snapshot);
    gchar *font_name = pango_font_description_to_string(font);
    gtk_source_print_compositor_set_body_font_name(compositor, font_name); g_free(font_name);
    gtk_source_print_compositor_set_wrap_mode(compositor, GTK_WRAP_WORD_CHAR);
    gtk_source_print_compositor_set_tab_width(compositor, gtk_source_view_get_tab_width(view));
    gtk_source_print_compositor_set_highlight_syntax(compositor, FALSE);
    GtkPrintOperation *operation = gtk_print_operation_new();
    gtk_print_operation_set_default_page_setup(operation, options->page_setup);
    gtk_print_operation_set_print_settings(operation, options->settings);
    gtk_print_operation_set_unit(operation, GTK_UNIT_POINTS);
    gtk_print_operation_set_use_full_page(operation, TRUE);
    gtk_print_operation_set_job_name(operation, filename);
    if (export_path) gtk_print_operation_set_export_filename(operation, export_path);
    PrintJob job = {compositor, options, filename, g_date_time_new_now_local()};
    g_signal_connect(operation, "begin-print", G_CALLBACK(begin_print), &job);
    g_signal_connect(operation, "paginate", G_CALLBACK(paginate), &job);
    g_signal_connect(operation, "draw-page", G_CALLBACK(draw_page), &job);
    GtkPrintOperationResult result = gtk_print_operation_run(operation,
        export_path ? GTK_PRINT_OPERATION_ACTION_EXPORT : GTK_PRINT_OPERATION_ACTION_PRINT_DIALOG, parent, error);
    if (result == GTK_PRINT_OPERATION_RESULT_APPLY) {
        g_set_object(&options->settings, gtk_print_operation_get_print_settings(operation));
        g_set_object(&options->page_setup, gtk_print_operation_get_default_page_setup(operation));
    }
    g_date_time_unref(job.now); g_object_unref(compositor); g_object_unref(operation);
    return result != GTK_PRINT_OPERATION_RESULT_ERROR;
}
