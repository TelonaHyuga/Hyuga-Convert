/*
 * main.c — Hyuga Convert, a small GTK3 front-end for FFmpeg.
 *
 * Copyright (C) 2026 Telona Hyuga
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "media.h"

#include <errno.h>
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <gtk/gtk.h>
#include <locale.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#ifndef VERSION
#define VERSION "1.0.0"
#endif

#ifndef APP_ID
#define APP_ID "com.hyuga.HyugaConvert"
#endif

#define RESOURCE_PREFIX "/com/hyuga/HyugaConvert"

typedef enum {
    STATUS_INFO,
    STATUS_SUCCESS,
    STATUS_ERROR
} StatusKind;

typedef struct {
    gchar *path;
    MediaKind kind;
    MediaInfo info;
} SourceItem;

typedef struct _App App;

struct _App {
    GtkApplication *application;
    GtkWidget *window;

    GtkWidget *source_card;
    GtkWidget *source_entry;
    GtkWidget *source_button;
    GtkWidget *source_clear_button;
    GtkWidget *source_info;
    GtkWidget *source_list;
    GtkWidget *source_scroller;
    GtkWidget *format_combo;
    GtkWidget *format_desc;
    GtkWidget *quality_combo;
    GtkWidget *folder_entry;
    GtkWidget *folder_button;
    GtkWidget *output_label;
    GtkWidget *convert_button;
    GtkWidget *cancel_button;
    GtkWidget *reveal_button;
    GtkWidget *progress;
    GtkWidget *status_label;
    GtkWidget *log_expander;
    GtkWidget *log_view;
    GtkTextBuffer *log_buffer;
    GtkWidget *info_dialog;
    GtkWidget *theme_switch;

    GdkPixbuf *icon;
    GPtrArray *sources;
    MediaKind batch_kind;
    SourceItem *active_source;
    gdouble current_fraction;
    guint batch_index;
    guint batch_success;
    guint batch_failed;
    guint next_item_source_id;
    gchar *output_path;
    gchar *last_open_dir;

    gboolean ffmpeg_available;
    gboolean ffprobe_available;
    gboolean dark_theme;
    gboolean converting;
    gboolean cancel_requested;
    gboolean quit_after_finish;
    gboolean updating_ui;

    GSubprocess *proc;
    GCancellable *cancellable;
    GDataInputStream *stderr_stream;
    gboolean proc_done;
    gboolean stderr_done;
    gchar *wait_error;
    gchar *error_tail;
    gchar *speed_text;
};

static void update_format_ui(App *app);
static void update_output_path(App *app);
static void set_status(App *app, StatusKind kind, const gchar *text);
static gboolean add_source_paths(App *app, GPtrArray *paths, gboolean replace);
static void rebuild_source_list(App *app);
static void start_conversion(App *app);
static void cancel_conversion(App *app);
static void on_window_destroy(GtkWidget *widget, gpointer data);
static void finalize_conversion(App *app);
static void start_next_batch_item(App *app);
static void finish_batch(App *app);
static void show_info_dialog(App *app);
static void show_license_dialog(App *app);

/* ------------------------------------------------------------------------- */
/* Small helpers                                                             */
/* ------------------------------------------------------------------------- */

static gchar *expand_path(const gchar *path)
{
    if (!path || !*path)
        return g_strdup("");

    gchar *expanded = NULL;
    if (path[0] == '~' && (path[1] == '/' || path[1] == '\0')) {
        const gchar *home = g_get_home_dir();
        if (!home || !*home)
            home = g_get_user_name();
        expanded = path[1] == '\0'
            ? g_strdup(home)
            : g_build_filename(home, path + 2, NULL);
    } else {
        expanded = g_strdup(path);
    }

    gchar *canonical = g_canonicalize_filename(expanded, NULL);
    g_free(expanded);
    return canonical;
}

static gboolean file_is_readable(const gchar *path)
{
    return g_file_test(path, G_FILE_TEST_IS_REGULAR) && g_access(path, R_OK) == 0;
}

static GtkWidget *make_label(const gchar *text, const gchar *css_class)
{
    GtkWidget *label = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    if (css_class)
        gtk_style_context_add_class(gtk_widget_get_style_context(label), css_class);
    return label;
}

static GtkWidget *make_section_header(const gchar *title, const gchar *hint)
{
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *title_label = make_label(title, "section-title");
    gtk_widget_set_halign(title_label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(row), title_label, FALSE, FALSE, 0);

    if (hint) {
        GtkWidget *hint_label = make_label(hint, "hint");
        gtk_label_set_xalign(GTK_LABEL(hint_label), 1.0f);
        gtk_label_set_ellipsize(GTK_LABEL(hint_label), PANGO_ELLIPSIZE_END);
        gtk_widget_set_hexpand(hint_label, TRUE);
        gtk_box_pack_end(GTK_BOX(row), hint_label, TRUE, TRUE, 0);
    }
    return row;
}

static GtkWidget *make_form_label(const gchar *text)
{
    GtkWidget *label = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    gtk_widget_set_valign(label, GTK_ALIGN_CENTER);
    gtk_style_context_add_class(gtk_widget_get_style_context(label), "form-label");
    return label;
}

static GtkWidget *make_button_with_icon(const gchar *label,
                                        const gchar *icon_name)
{
    GtkWidget *button = gtk_button_new_with_mnemonic(label);
    if (icon_name) {
        GtkWidget *image = gtk_image_new_from_icon_name(icon_name, GTK_ICON_SIZE_BUTTON);
        gtk_button_set_image(GTK_BUTTON(button), image);
        gtk_button_set_always_show_image(GTK_BUTTON(button), TRUE);
    }
    return button;
}

static void set_tooltip(GtkWidget *widget, const gchar *text)
{
    gtk_widget_set_tooltip_text(widget, text);
}

/* ------------------------------------------------------------------------- */
/* Source batch                                                              */
/* ------------------------------------------------------------------------- */

static void source_item_free(gpointer data)
{
    SourceItem *item = data;
    if (!item)
        return;
    g_free(item->path);
    media_info_clear(&item->info);
    g_free(item);
}

static const gchar *files_word(guint count)
{
    guint last_two = count % 100;
    guint last = count % 10;
    if (last == 1 && last_two != 11)
        return "файл";
    if (last >= 2 && last <= 4 && (last_two < 12 || last_two > 14))
        return "файла";
    return "файлов";
}

static MediaKind sources_group_kind(const GPtrArray *sources)
{
    gboolean has_image = FALSE;
    gboolean has_video = FALSE;
    gboolean has_gif = FALSE;

    if (!sources)
        return MEDIA_NONE;

    for (guint i = 0; i < sources->len; i++) {
        const SourceItem *item = g_ptr_array_index(sources, i);
        switch (item->kind) {
        case MEDIA_IMAGE:
            has_image = TRUE;
            break;
        case MEDIA_VIDEO:
            has_video = TRUE;
            break;
        case MEDIA_GIF:
            has_gif = TRUE;
            break;
        default:
            break;
        }
    }

    if (has_image && has_video)
        return MEDIA_NONE;
    if (has_video)
        return MEDIA_VIDEO;
    if (has_image)
        return MEDIA_IMAGE;
    if (has_gif)
        return MEDIA_GIF;
    return MEDIA_NONE;
}

static gboolean batch_kind_allows(MediaKind current, MediaKind candidate)
{
    if (candidate == MEDIA_GIF)
        return TRUE;
    if (current == MEDIA_NONE || current == MEDIA_GIF)
        return TRUE;
    return current == candidate;
}

static const gchar *batch_kind_label(MediaKind kind)
{
    switch (kind) {
    case MEDIA_IMAGE:
        return "изображения";
    case MEDIA_VIDEO:
        return "видео";
    case MEDIA_GIF:
        return "GIF";
    default:
        return "файлы";
    }
}

static guint sources_total_count(const App *app)
{
    return app->sources ? app->sources->len : 0;
}

static guint64 sources_total_size(const App *app)
{
    guint64 total = 0;
    for (guint i = 0; i < sources_total_count(app); i++) {
        const SourceItem *item = g_ptr_array_index(app->sources, i);
        GStatBuf st;
        if (g_stat(item->path, &st) == 0 && st.st_size > 0)
            total += (guint64) st.st_size;
    }
    return total;
}

/* ------------------------------------------------------------------------- */
/* Theme preference                                                          */
/* ------------------------------------------------------------------------- */

#define THEME_CONFIG_DIR "hyuga-convert"
#define THEME_CONFIG_FILE "settings.ini"

static gchar *theme_config_path(void)
{
    return g_build_filename(g_get_user_config_dir(), THEME_CONFIG_DIR,
                            THEME_CONFIG_FILE, NULL);
}

static gboolean load_theme_preference(gboolean *dark)
{
    g_return_val_if_fail(dark != NULL, FALSE);

    gchar *path = theme_config_path();
    GKeyFile *key_file = g_key_file_new();
    gboolean found = FALSE;

    if (g_key_file_load_from_file(key_file, path, G_KEY_FILE_NONE, NULL)) {
        GError *error = NULL;
        gboolean value = g_key_file_get_boolean(key_file, "ui", "dark-theme",
                                                &error);
        if (!error) {
            *dark = value;
            found = TRUE;
        }
        g_clear_error(&error);
    }

    g_key_file_free(key_file);
    g_free(path);
    return found;
}

static void save_theme_preference(gboolean dark)
{
    gchar *folder = g_build_filename(g_get_user_config_dir(),
                                     THEME_CONFIG_DIR, NULL);
    if (g_mkdir_with_parents(folder, 0700) != 0) {
        g_free(folder);
        return;
    }

    GKeyFile *key_file = g_key_file_new();
    g_key_file_set_boolean(key_file, "ui", "dark-theme", dark);

    gsize length = 0;
    gchar *data = g_key_file_to_data(key_file, &length, NULL);
    gchar *path = g_build_filename(folder, THEME_CONFIG_FILE, NULL);

    if (data != NULL) {
        GError *error = NULL;
        if (!g_file_set_contents(path, data, (gssize) length, &error)) {
            g_clear_error(&error);
        }
    }

    g_free(path);
    g_free(data);
    g_key_file_free(key_file);
    g_free(folder);
}

static void apply_dark_theme(App *app, gboolean dark, gboolean persist)
{
    GtkSettings *settings = gtk_settings_get_default();
    if (settings) {
        g_object_set(settings,
                     "gtk-application-prefer-dark-theme", dark,
                     NULL);
    }

    app->dark_theme = dark;
    if (persist)
        save_theme_preference(dark);
}

static gboolean on_theme_switch_state_set(GtkSwitch *widget,
                                          gboolean state,
                                          gpointer data)
{
    (void) widget;
    App *app = data;
    apply_dark_theme(app, state, TRUE);
    return FALSE;
}

/* ------------------------------------------------------------------------- */
/* Command line mode                                                         */
/* ------------------------------------------------------------------------- */

static void print_input_list(const gchar *title, const gchar *const *items)
{
    GString *line = g_string_new("  ");
    g_string_append_printf(line, "%-12s ", title);
    for (gint i = 0; items[i] != NULL; i++) {
        if (i > 0)
            g_string_append(line, ", ");
        g_string_append(line, items[i]);
    }
    g_print("%s\n", line->str);
    g_string_free(line, TRUE);
}

static gint cli_list_formats(void)
{
    g_print("Hyuga Convert %s\n\n", VERSION);
    g_print("Входные форматы:\n");
    print_input_list("Изображения:", media_supported_image_inputs());
    print_input_list("Видео:", media_supported_video_inputs());
    g_print("\nВыходные форматы (идентификатор для --format):\n");
    for (gint i = 0; i < TARGET_COUNT; i++) {
        const FormatSpec *spec = media_format_spec((TargetFormat) i);
        g_print("  %-12s %s\n", media_target_id((TargetFormat) i), spec->description);
    }
    g_print("\nДля GIF идентификатор зависит от входа: gif-image или gif-video.\n");
    g_print("Короткое имя --format gif выбирается автоматически.\n");
    return 0;
}

static gint cli_run(gint argc, gchar **argv)
{
    gchar *input = NULL;
    gchar *output = NULL;
    gchar *format = NULL;
    gchar *quality_text = NULL;
    gboolean list_formats = FALSE;
    gboolean dry_run = FALSE;
    gboolean show_version = FALSE;

    GOptionEntry entries[] = {
        { "input", 'i', 0, G_OPTION_ARG_FILENAME, &input,
          "Исходный файл", "FILE" },
        { "format", 'f', 0, G_OPTION_ARG_STRING, &format,
          "Выходной формат (например mp4, png, gif)", "FORMAT" },
        { "output", 'o', 0, G_OPTION_ARG_FILENAME, &output,
          "Выходной файл (по умолчанию — рядом с исходным)", "FILE" },
        { "quality", 'q', 0, G_OPTION_ARG_STRING, &quality_text,
          "Качество: high, balanced или small", "PRESET" },
        { "list-formats", 0, 0, G_OPTION_ARG_NONE, &list_formats,
          "Показать поддерживаемые форматы", NULL },
        { "dry-run", 0, 0, G_OPTION_ARG_NONE, &dry_run,
          "Показать команду FFmpeg, но не запускать её", NULL },
        { "version", 0, 0, G_OPTION_ARG_NONE, &show_version,
          "Показать версию", NULL },
        { NULL, 0, 0, 0, NULL, NULL, NULL }
    };

    GError *error = NULL;
    GOptionContext *context = g_option_context_new(
        "— конвертация видео и изображений без графического интерфейса");
    g_option_context_set_summary(
        context,
        "Пример: hyuga-convert --cli -i video.mkv -f mp4 -q balanced");
    g_option_context_add_main_entries(context, entries, NULL);

    if (!g_option_context_parse(context, &argc, &argv, &error)) {
        g_printerr("Ошибка аргументов: %s\n", error->message);
        g_clear_error(&error);
        g_option_context_free(context);
        g_free(input);
        g_free(output);
        g_free(format);
        g_free(quality_text);
        return 2;
    }
    g_option_context_free(context);

    if (show_version) {
        g_print("Hyuga Convert %s\n", VERSION);
        g_free(input);
        g_free(output);
        g_free(format);
        g_free(quality_text);
        return 0;
    }

    if (list_formats) {
        gint result = cli_list_formats();
        g_free(input);
        g_free(output);
        g_free(format);
        g_free(quality_text);
        return result;
    }

    if (!input) {
        g_printerr("Укажите исходный файл: --input FILE\n");
        g_free(output);
        g_free(format);
        g_free(quality_text);
        return 2;
    }
    if (!format) {
        g_printerr("Укажите выходной формат: --format FORMAT\n");
        g_free(input);
        g_free(output);
        g_free(quality_text);
        return 2;
    }

    gchar *input_path = expand_path(input);
    if (!file_is_readable(input_path)) {
        g_printerr("Файл не найден или недоступен: %s\n", input_path);
        g_free(input);
        g_free(output);
        g_free(format);
        g_free(quality_text);
        g_free(input_path);
        return 1;
    }

    MediaKind kind = media_detect_kind(input_path);
    if (kind == MEDIA_NONE) {
        g_printerr("Не удалось определить тип файла: %s\n", input_path);
        g_free(input);
        g_free(output);
        g_free(format);
        g_free(quality_text);
        g_free(input_path);
        return 1;
    }

    TargetFormat target;
    if (g_strcmp0(format, "gif-image") == 0 || g_strcmp0(format, "gif-video") == 0) {
        target = media_target_from_id(format);
    } else {
        target = media_target_for_extension(format, kind);
    }
    if (target == TARGET_NONE || !media_target_allowed(target, kind)) {
        g_printerr("Формат «%s» не подходит для этого файла.\n", format);
        g_printerr("Посмотрите доступные варианты: hyuga-convert --cli --list-formats\n");
        g_free(input);
        g_free(output);
        g_free(format);
        g_free(quality_text);
        g_free(input_path);
        return 1;
    }

    QualityPreset quality = quality_text
        ? media_quality_from_id(quality_text)
        : QUALITY_BALANCED;

    gchar *output_path = NULL;
    if (output) {
        output_path = expand_path(output);
    } else {
        gchar *folder = g_path_get_dirname(input_path);
        gchar *stem = media_stem(input_path);
        const FormatSpec *spec = media_format_spec(target);
        output_path = media_unique_output_path(folder, stem, spec->extension);
        g_free(stem);
        g_free(folder);
    }

    gchar *output_folder = g_path_get_dirname(output_path);
    if (!g_file_test(output_folder, G_FILE_TEST_IS_DIR)) {
        g_printerr("Папка назначения не существует: %s\n", output_folder);
        g_free(input);
        g_free(output);
        g_free(format);
        g_free(quality_text);
        g_free(input_path);
        g_free(output_path);
        g_free(output_folder);
        return 1;
    }
    g_free(output_folder);

    GPtrArray *ffmpeg_argv = media_build_ffmpeg_argv(
        input_path, output_path, target, quality, FALSE);
    if (!ffmpeg_argv) {
        g_printerr("Не удалось построить команду FFmpeg.\n");
        g_free(input);
        g_free(output);
        g_free(format);
        g_free(quality_text);
        g_free(input_path);
        g_free(output_path);
        return 1;
    }

    if (dry_run) {
        gchar *command = media_argv_to_string(ffmpeg_argv);
        g_print("%s\n", command);
        g_print("Результат: %s\n", output_path);
        g_free(command);
        g_ptr_array_free(ffmpeg_argv, TRUE);
        g_free(input);
        g_free(output);
        g_free(format);
        g_free(quality_text);
        g_free(input_path);
        g_free(output_path);
        return 0;
    }

    if (!g_find_program_in_path("ffmpeg")) {
        g_printerr("FFmpeg не найден в PATH. Установите пакет ffmpeg.\n");
        g_ptr_array_free(ffmpeg_argv, TRUE);
        g_free(input);
        g_free(output);
        g_free(format);
        g_free(quality_text);
        g_free(input_path);
        g_free(output_path);
        return 1;
    }

    GSubprocess *process = g_subprocess_newv(
        (const gchar *const *) ffmpeg_argv->pdata,
        G_SUBPROCESS_FLAGS_NONE,
        &error);
    g_ptr_array_free(ffmpeg_argv, TRUE);

    if (!process) {
        g_printerr("Не удалось запустить FFmpeg: %s\n", error->message);
        g_clear_error(&error);
        g_free(input);
        g_free(output);
        g_free(format);
        g_free(quality_text);
        g_free(input_path);
        g_free(output_path);
        return 1;
    }

    gboolean waited = g_subprocess_wait(process, NULL, &error);
    gboolean successful = waited && g_subprocess_get_successful(process);
    gint exit_status = g_subprocess_get_if_exited(process)
        ? g_subprocess_get_exit_status(process)
        : 1;

    if (!successful) {
        if (error) {
            g_printerr("FFmpeg завершился с ошибкой: %s\n", error->message);
            g_clear_error(&error);
        } else {
            g_printerr("FFmpeg завершился с кодом %d.\n", exit_status);
        }
    } else {
        g_print("Готово: %s\n", output_path);
    }

    g_clear_object(&process);
    g_free(input);
    g_free(output);
    g_free(format);
    g_free(quality_text);
    g_free(input_path);
    g_free(output_path);
    return successful ? 0 : 1;
}

/* ------------------------------------------------------------------------- */
/* Status and log                                                            */
/* ------------------------------------------------------------------------- */

static void set_status(App *app, StatusKind kind, const gchar *text)
{
    if (!app || !app->status_label)
        return;

    GtkStyleContext *context = gtk_widget_get_style_context(app->status_label);
    gtk_style_context_remove_class(context, "status-info");
    gtk_style_context_remove_class(context, "status-success");
    gtk_style_context_remove_class(context, "status-error");

    switch (kind) {
    case STATUS_SUCCESS:
        gtk_style_context_add_class(context, "status-success");
        break;
    case STATUS_ERROR:
        gtk_style_context_add_class(context, "status-error");
        break;
    default:
        gtk_style_context_add_class(context, "status-info");
        break;
    }

    gtk_label_set_text(GTK_LABEL(app->status_label), text ? text : "");
    set_tooltip(app->status_label, text);
}

static void log_clear(App *app)
{
    if (app->log_buffer)
        gtk_text_buffer_set_text(app->log_buffer, "", 0);
}

static void log_append(App *app, const gchar *text)
{
    if (!app->log_buffer || !text)
        return;

    GtkTextIter end;
    gtk_text_buffer_get_end_iter(app->log_buffer, &end);
    gtk_text_buffer_insert(app->log_buffer, &end, text, -1);
}

/* ------------------------------------------------------------------------- */
/* Format and output UI                                                      */
/* ------------------------------------------------------------------------- */

static TargetFormat current_target(App *app)
{
    if (!app->format_combo)
        return TARGET_NONE;
    const gchar *id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(app->format_combo));
    TargetFormat target = media_target_from_id(id);
    if (!media_target_allowed(target, app->batch_kind))
        return TARGET_NONE;
    return target;
}

static QualityPreset current_quality(App *app)
{
    if (!app->quality_combo)
        return QUALITY_BALANCED;
    const gchar *id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(app->quality_combo));
    return media_quality_from_id(id);
}

static void update_convert_sensitivity(App *app)
{
    gboolean ready = sources_total_count(app) > 0 &&
        app->batch_kind != MEDIA_NONE &&
        current_target(app) != TARGET_NONE &&
        app->ffmpeg_available &&
        !app->converting;
    gtk_widget_set_sensitive(app->convert_button, ready);
}

static void update_format_ui(App *app)
{
    TargetFormat target = current_target(app);
    const FormatSpec *spec = media_format_spec(target);

    if (!spec) {
        gtk_label_set_text(GTK_LABEL(app->format_desc),
                           "Добавьте файлы — подходящие форматы появятся автоматически.");
        gtk_widget_set_sensitive(app->quality_combo, FALSE);
        set_tooltip(app->quality_combo, "Сначала добавьте файлы.");
    } else {
        gtk_label_set_text(GTK_LABEL(app->format_desc), spec->description);
        gboolean quality_available = media_quality_supported(target) &&
            !app->converting;
        gtk_widget_set_sensitive(app->quality_combo, quality_available);
        if (media_quality_supported(target)) {
            set_tooltip(app->quality_combo, "Влияет на размер и качество результата.");
        } else {
            set_tooltip(app->quality_combo,
                        "Для этого формата качество не настраивается.");
        }
    }

    update_output_path(app);
}

static void update_output_path(App *app)
{
    guint count = sources_total_count(app);
    if (count == 0 || app->batch_kind == MEDIA_NONE) {
        g_free(app->output_path);
        app->output_path = NULL;
        gtk_label_set_text(GTK_LABEL(app->output_label), "—");
        set_tooltip(app->output_label, NULL);
        update_convert_sensitivity(app);
        return;
    }

    TargetFormat target = current_target(app);
    const FormatSpec *spec = media_format_spec(target);
    const gchar *folder_text = gtk_entry_get_text(GTK_ENTRY(app->folder_entry));
    if (!spec || !folder_text || !*folder_text) {
        g_free(app->output_path);
        app->output_path = NULL;
        gtk_label_set_text(GTK_LABEL(app->output_label), "Укажите папку назначения");
        set_tooltip(app->output_label, NULL);
        update_convert_sensitivity(app);
        return;
    }

    gchar *folder = expand_path(folder_text);
    if (count == 1) {
        const SourceItem *item = g_ptr_array_index(app->sources, 0);
        gchar *stem = media_stem(item->path);
        gchar *path = media_unique_output_path(folder, stem, spec->extension);
        g_free(stem);
        gtk_label_set_text(GTK_LABEL(app->output_label), path);
        set_tooltip(app->output_label, path);
        g_free(path);
    } else {
        gchar *summary = g_strdup_printf("Будет создано: %u %s в выбранной папке",
                                         count, files_word(count));
        gtk_label_set_text(GTK_LABEL(app->output_label), summary);
        set_tooltip(app->output_label, folder);
        g_free(summary);
    }
    g_free(folder);

    g_free(app->output_path);
    app->output_path = NULL;
    update_convert_sensitivity(app);
}

static TargetFormat default_target_for_batch(App *app)
{
    guint count = sources_total_count(app);
    if (count == 0)
        return TARGET_NONE;

    const SourceItem *first = g_ptr_array_index(app->sources, 0);
    if (app->batch_kind == MEDIA_GIF) {
        if (first->kind == MEDIA_GIF && first->info.ok && first->info.duration <= 0.0)
            return TARGET_PNG;
        return TARGET_MP4;
    }

    return media_default_target(app->batch_kind, first->path);
}

static void rebuild_format_combo(App *app, TargetFormat desired)
{
    app->updating_ui = TRUE;
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(app->format_combo));

    if (app->batch_kind == MEDIA_NONE || sources_total_count(app) == 0) {
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(app->format_combo),
                                  "none", "Сначала добавьте файлы");
        gtk_combo_box_set_active(GTK_COMBO_BOX(app->format_combo), 0);
        gtk_widget_set_sensitive(app->format_combo, FALSE);
    } else {
        for (gint i = 0; i < TARGET_COUNT; i++) {
            TargetFormat target = (TargetFormat) i;
            if (!media_target_allowed(target, app->batch_kind))
                continue;
            const FormatSpec *spec = media_format_spec(target);
            gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(app->format_combo),
                                      media_target_id(target), spec->label);
        }

        if (desired == TARGET_NONE || !media_target_allowed(desired, app->batch_kind))
            desired = default_target_for_batch(app);

        if (desired != TARGET_NONE && media_target_allowed(desired, app->batch_kind)) {
            gtk_combo_box_set_active_id(GTK_COMBO_BOX(app->format_combo),
                                        media_target_id(desired));
        } else {
            gtk_combo_box_set_active(GTK_COMBO_BOX(app->format_combo), 0);
        }
        gtk_widget_set_sensitive(app->format_combo, !app->converting);
    }

    app->updating_ui = FALSE;
    update_format_ui(app);
}

static void clear_success_state(App *app)
{
    if (app->converting)
        return;
    if (!gtk_widget_get_visible(app->reveal_button))
        return;

    gtk_widget_set_visible(app->reveal_button, FALSE);
    gtk_widget_set_visible(app->progress, FALSE);
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(app->progress), 0.0);
    set_status(app, STATUS_INFO, "Настройки изменены. Нажмите «Конвертировать».");
}

static void on_format_changed(GtkComboBox *combo, gpointer data)
{
    (void) combo;
    App *app = data;
    if (app->updating_ui || app->converting)
        return;
    clear_success_state(app);
    update_format_ui(app);
}

static void on_quality_changed(GtkComboBox *combo, gpointer data)
{
    (void) combo;
    App *app = data;
    if (app->updating_ui || app->converting)
        return;
    clear_success_state(app);
}

static void on_folder_changed(GtkEditable *editable, gpointer data)
{
    (void) editable;
    App *app = data;
    if (app->updating_ui || app->converting)
        return;
    clear_success_state(app);
    update_output_path(app);
}

/* ------------------------------------------------------------------------- */
/* Source loading                                                            */
/* ------------------------------------------------------------------------- */

static void update_source_summary(App *app)
{
    guint count = sources_total_count(app);

    if (count == 0) {
        gtk_label_set_text(
            GTK_LABEL(app->source_info),
            "Файлы не выбраны. Нажмите «Добавить файлы…» или перетащите их в окно.");
        set_tooltip(app->source_info, NULL);
    } else if (count == 1) {
        const SourceItem *item = g_ptr_array_index(app->sources, 0);
        gchar *text = media_info_label(item->path, item->kind, &item->info);
        gtk_label_set_text(GTK_LABEL(app->source_info), text);
        set_tooltip(app->source_info, item->path);
        g_free(text);
    } else {
        guint64 size = sources_total_size(app);
        gchar *size_text = g_format_size(size);
        gchar *text = g_strdup_printf(
            "%u %s · %s · %s",
            count, files_word(count), batch_kind_label(app->batch_kind), size_text);
        gtk_label_set_text(GTK_LABEL(app->source_info), text);
        set_tooltip(app->source_info, "Выбрано несколько файлов для пакетной конвертации.");
        g_free(text);
        g_free(size_text);
    }

    gtk_widget_set_sensitive(
        app->source_clear_button,
        count > 0 && !app->converting);
}

static void on_remove_source(GtkButton *button, gpointer data)
{
    App *app = data;
    const gchar *path = g_object_get_data(G_OBJECT(button), "source-path");
    if (!path || app->converting)
        return;

    TargetFormat desired = current_target(app);

    for (guint i = 0; i < sources_total_count(app); i++) {
        SourceItem *item = g_ptr_array_index(app->sources, i);
        if (g_strcmp0(item->path, path) == 0) {
            g_ptr_array_remove_index(app->sources, i);
            break;
        }
    }

    app->batch_kind = sources_group_kind(app->sources);
    gtk_widget_set_visible(app->reveal_button, FALSE);
    gtk_widget_set_visible(app->progress, FALSE);
    gtk_widget_set_visible(app->log_expander, FALSE);
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(app->progress), 0.0);

    rebuild_source_list(app);
    rebuild_format_combo(app, desired);

    if (sources_total_count(app) == 0) {
        set_status(app, STATUS_INFO, "Список файлов очищен. Добавьте файлы для конвертации.");
    } else {
        set_status(app, STATUS_INFO, "Файл убран из списка.");
    }
}

static void rebuild_source_list(App *app)
{
    GList *children = gtk_container_get_children(GTK_CONTAINER(app->source_list));
    for (GList *child = children; child != NULL; child = child->next)
        gtk_widget_destroy(GTK_WIDGET(child->data));
    g_list_free(children);

    guint count = sources_total_count(app);
    for (guint i = 0; i < count; i++) {
        SourceItem *item = g_ptr_array_index(app->sources, i);
        GtkWidget *row = gtk_list_box_row_new();
        gtk_list_box_row_set_selectable(GTK_LIST_BOX_ROW(row), FALSE);
        gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);

        GtkWidget *content = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        gtk_widget_set_margin_start(content, 8);
        gtk_widget_set_margin_end(content, 6);
        gtk_widget_set_margin_top(content, 6);
        gtk_widget_set_margin_bottom(content, 6);
        gtk_container_add(GTK_CONTAINER(row), content);

        GtkWidget *text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);
        gtk_widget_set_hexpand(text, TRUE);
        gchar *base = g_path_get_basename(item->path);
        GtkWidget *name = make_label(base, "source-name");
        gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_MIDDLE);
        gtk_label_set_max_width_chars(GTK_LABEL(name), 70);
        g_free(base);

        gchar *details = media_info_label(item->path, item->kind, &item->info);
        GtkWidget *detail = make_label(details, "source-detail");
        gtk_label_set_ellipsize(GTK_LABEL(detail), PANGO_ELLIPSIZE_END);
        gtk_label_set_max_width_chars(GTK_LABEL(detail), 75);
        g_free(details);

        gtk_box_pack_start(GTK_BOX(text), name, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(text), detail, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(content), text, TRUE, TRUE, 0);

        GtkWidget *remove = gtk_button_new_from_icon_name(
            "list-remove-symbolic", GTK_ICON_SIZE_BUTTON);
        gtk_style_context_add_class(
            gtk_widget_get_style_context(remove), "flat");
        gtk_widget_set_valign(remove, GTK_ALIGN_CENTER);
        set_tooltip(remove, "Убрать файл из списка");
        g_object_set_data_full(G_OBJECT(remove), "source-path",
                               g_strdup(item->path), g_free);
        g_signal_connect(remove, "clicked", G_CALLBACK(on_remove_source), app);
        gtk_box_pack_end(GTK_BOX(content), remove, FALSE, FALSE, 0);

        gtk_container_add(GTK_CONTAINER(app->source_list), row);
        gtk_widget_show_all(row);
    }

    gtk_widget_set_visible(app->source_scroller, count > 0);
    gtk_widget_set_sensitive(app->source_list, !app->converting);
    gint list_height = CLAMP((gint) count * 44 + 6, 50, 182);
    gtk_widget_set_size_request(app->source_scroller, -1, list_height);

    gchar *button_label = count > 1
        ? g_strdup_printf("_Конвертировать (%u)", count)
        : g_strdup("_Конвертировать");
    gtk_button_set_label(GTK_BUTTON(app->convert_button), button_label);
    g_free(button_label);

    update_source_summary(app);
}

static gboolean add_source_paths(App *app, GPtrArray *paths, gboolean replace)
{
    if (app->converting) {
        set_status(app, STATUS_INFO,
                   "Дождитесь завершения текущей конвертации или отмените её.");
        return FALSE;
    }

    if (!paths || paths->len == 0) {
        set_status(app, STATUS_ERROR, "Сначала выберите хотя бы один файл.");
        return FALSE;
    }

    TargetFormat desired = current_target(app);
    guint count_before = sources_total_count(app);
    gboolean was_empty = count_before == 0;
    guint added = 0;
    guint failed = 0;
    guint duplicates = 0;
    GString *errors = g_string_new(NULL);

    if (replace) {
        g_ptr_array_set_size(app->sources, 0);
        app->batch_kind = MEDIA_NONE;
        was_empty = TRUE;
        gtk_entry_set_text(GTK_ENTRY(app->folder_entry), "");
        app->updating_ui = FALSE;
    }

    for (guint i = 0; i < paths->len; i++) {
        const gchar *raw_path = g_ptr_array_index(paths, i);
        if (!raw_path || !*raw_path)
            continue;

        gchar *path = expand_path(raw_path);
        if (!file_is_readable(path)) {
            const gchar *message = g_file_test(path, G_FILE_TEST_IS_DIR)
                ? "это папка, а не файл"
                : "файл не найден или недоступен для чтения";
            g_string_append_printf(errors, "%s — %s\n", path, message);
            failed++;
            g_free(path);
            continue;
        }

        MediaKind kind = media_detect_kind(path);
        if (kind == MEDIA_NONE) {
            g_string_append_printf(errors, "%s — формат не поддерживается\n", path);
            failed++;
            g_free(path);
            continue;
        }

        if (!batch_kind_allows(sources_group_kind(app->sources), kind)) {
            g_string_append_printf(
                errors,
                "%s — нельзя смешивать изображения и видео в одном пакете\n",
                path);
            failed++;
            g_free(path);
            continue;
        }

        gboolean already_added = FALSE;
        for (guint j = 0; j < sources_total_count(app); j++) {
            const SourceItem *existing = g_ptr_array_index(app->sources, j);
            if (g_strcmp0(existing->path, path) == 0) {
                already_added = TRUE;
                break;
            }
        }
        if (already_added) {
            duplicates++;
            g_free(path);
            continue;
        }

        MediaInfo info = { 0 };
        media_probe(path, &info);

        SourceItem *item = g_new0(SourceItem, 1);
        item->path = path; /* ownership moves to item */
        item->kind = kind;
        item->info = info;
        info.codec = NULL;
        g_ptr_array_add(app->sources, item);
        added++;

        if (was_empty && added == 1) {
            gchar *folder = g_path_get_dirname(item->path);
            gtk_entry_set_text(GTK_ENTRY(app->folder_entry), folder);
            gtk_editable_set_position(GTK_EDITABLE(app->folder_entry), 0);
            g_free(app->last_open_dir);
            app->last_open_dir = g_strdup(folder);
            g_free(folder);
        }
    }

    if (added > 0) {
        app->batch_kind = sources_group_kind(app->sources);
        gtk_widget_set_visible(app->reveal_button, FALSE);
        gtk_widget_set_visible(app->progress, FALSE);
        gtk_widget_set_visible(app->log_expander, FALSE);
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(app->progress), 0.0);

        gboolean target_allowed = desired != TARGET_NONE &&
            media_target_allowed(desired, app->batch_kind);
        rebuild_source_list(app);
        rebuild_format_combo(app, target_allowed ? desired : TARGET_NONE);

        gtk_entry_set_text(GTK_ENTRY(app->source_entry), "");
        gtk_widget_set_sensitive(app->folder_entry, TRUE);
        gtk_widget_set_sensitive(app->folder_button, TRUE);

        if (!app->ffmpeg_available) {
            set_status(app, STATUS_ERROR,
                       "FFmpeg не найден. Установите пакет ffmpeg, затем перезапустите программу.");
        } else if (failed > 0) {
            gchar *text = g_strdup_printf(
                "Добавлено: %u, пропущено: %u. Подробности во всплывающей подсказке.",
                added, failed);
            set_status(app, STATUS_ERROR, text);
            set_tooltip(app->status_label, errors->str);
            g_free(text);
        } else {
            gchar *text = g_strdup_printf("Добавлено: %u %s.",
                                          added, files_word(added));
            set_status(app, STATUS_INFO, text);
            g_free(text);
        }
    } else if (failed > 0) {
        set_status(app, STATUS_ERROR, "Не удалось добавить файлы.");
        set_tooltip(app->status_label, errors->str);
    } else if (duplicates > 0) {
        set_status(app, STATUS_INFO, "Эти файлы уже добавлены в список.");
    } else {
        set_status(app, STATUS_ERROR, "Не удалось добавить файлы.");
    }

    if (replace && added == 0) {
        app->batch_kind = sources_group_kind(app->sources);
        rebuild_source_list(app);
        rebuild_format_combo(app, TARGET_NONE);
    }

    g_string_free(errors, TRUE);
    return added > 0;
}

static void on_source_activate(GtkEntry *entry, gpointer data)
{
    (void) entry;
    App *app = data;
    const gchar *text = gtk_entry_get_text(GTK_ENTRY(app->source_entry));
    if (!text || !*text)
        return;

    GPtrArray *paths = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(paths, g_strdup(text));
    add_source_paths(app, paths, FALSE);
    g_ptr_array_free(paths, TRUE);
}

static void on_clear_sources(GtkButton *button, gpointer data)
{
    (void) button;
    App *app = data;
    if (app->converting)
        return;

    g_ptr_array_set_size(app->sources, 0);
    app->batch_kind = MEDIA_NONE;
    gtk_widget_set_visible(app->reveal_button, FALSE);
    gtk_widget_set_visible(app->progress, FALSE);
    gtk_widget_set_visible(app->log_expander, FALSE);
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(app->progress), 0.0);

    rebuild_source_list(app);
    rebuild_format_combo(app, TARGET_NONE);
    set_status(app, STATUS_INFO, "Список файлов очищен. Добавьте файлы для конвертации.");
}

static GtkFileFilter *make_filter(const gchar *name,
                                  const gchar *const *image_exts,
                                  const gchar *const *video_exts)
{
    GtkFileFilter *filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, name);

    const gchar *const *lists[] = { image_exts, video_exts };
    for (gint list_index = 0; list_index < 2; list_index++) {
        const gchar *const *list = lists[list_index];
        if (!list)
            continue;
        for (gint i = 0; list[i] != NULL; i++) {
            gchar *pattern = g_strdup_printf("*.%s", list[i]);
            gtk_file_filter_add_pattern(filter, pattern);
            g_free(pattern);
        }
    }
    return filter;
}

static void choose_source(App *app)
{
    if (app->converting)
        return;

    GtkWidget *dialog = gtk_file_chooser_dialog_new(
        "Выберите файлы",
        GTK_WINDOW(app->window),
        GTK_FILE_CHOOSER_ACTION_OPEN,
        "_Отмена", GTK_RESPONSE_CANCEL,
        "_Добавить", GTK_RESPONSE_ACCEPT,
        NULL);
    gtk_file_chooser_set_local_only(GTK_FILE_CHOOSER(dialog), TRUE);
    gtk_file_chooser_set_select_multiple(GTK_FILE_CHOOSER(dialog), TRUE);
    gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);

    GtkFileFilter *all_filter = make_filter(
        "Все поддерживаемые файлы",
        media_supported_image_inputs(),
        media_supported_video_inputs());
    GtkFileFilter *image_filter = make_filter(
        "Изображения", media_supported_image_inputs(), NULL);
    GtkFileFilter *video_filter = make_filter(
        "Видео", NULL, media_supported_video_inputs());
    GtkFileFilter *any_filter = gtk_file_filter_new();
    gtk_file_filter_set_name(any_filter, "Все файлы");
    gtk_file_filter_add_pattern(any_filter, "*");

    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), all_filter);
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), image_filter);
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), video_filter);
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), any_filter);
    gtk_file_chooser_set_filter(GTK_FILE_CHOOSER(dialog), all_filter);

    const gchar *start_dir = NULL;
    gchar *source_dir = NULL;
    if (sources_total_count(app) > 0) {
        const SourceItem *first = g_ptr_array_index(app->sources, 0);
        source_dir = g_path_get_dirname(first->path);
        start_dir = source_dir;
    } else if (app->last_open_dir && g_file_test(app->last_open_dir, G_FILE_TEST_IS_DIR)) {
        start_dir = app->last_open_dir;
    }
    if (start_dir)
        gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dialog), start_dir);

    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        GSList *filenames = gtk_file_chooser_get_filenames(GTK_FILE_CHOOSER(dialog));
        if (filenames) {
            GPtrArray *paths = g_ptr_array_new_with_free_func(g_free);
            for (GSList *node = filenames; node != NULL; node = node->next) {
                g_ptr_array_add(paths, node->data);
                node->data = NULL;
            }
            g_slist_free(filenames);

            if (paths->len > 0) {
                const gchar *first = g_ptr_array_index(paths, 0);
                g_free(app->last_open_dir);
                app->last_open_dir = g_path_get_dirname(first);
                add_source_paths(app, paths, FALSE);
            }
            g_ptr_array_free(paths, TRUE);
        }
    }

    g_free(source_dir);
    gtk_widget_destroy(dialog);
}

static void choose_folder(App *app)
{
    if (app->converting)
        return;

    GtkWidget *dialog = gtk_file_chooser_dialog_new(
        "Выберите папку для результата",
        GTK_WINDOW(app->window),
        GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
        "_Отмена", GTK_RESPONSE_CANCEL,
        "_Выбрать", GTK_RESPONSE_ACCEPT,
        NULL);
    gtk_file_chooser_set_local_only(GTK_FILE_CHOOSER(dialog), TRUE);
    gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);

    gchar *current = expand_path(gtk_entry_get_text(GTK_ENTRY(app->folder_entry)));
    if (current && g_file_test(current, G_FILE_TEST_IS_DIR)) {
        gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dialog), current);
    } else {
        gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dialog),
                                            g_get_home_dir());
    }
    g_free(current);

    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        gchar *folder = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        if (folder) {
            gtk_entry_set_text(GTK_ENTRY(app->folder_entry), folder);
            g_free(folder);
        }
    }

    gtk_widget_destroy(dialog);
}

/* ------------------------------------------------------------------------- */
/* Drag and drop                                                             */
/* ------------------------------------------------------------------------- */

static void on_drag_motion(GtkWidget *widget, GdkDragContext *context,
                           gint x, gint y, guint time, gpointer data)
{
    (void) widget;
    (void) x;
    (void) y;
    (void) time;
    App *app = data;
    gtk_style_context_add_class(
        gtk_widget_get_style_context(app->source_card), "drop-active");
    gdk_drag_status(context, GDK_ACTION_COPY, time);
}

static void on_drag_leave(GtkWidget *widget, GdkDragContext *context,
                          guint time, gpointer data)
{
    (void) widget;
    (void) context;
    (void) time;
    App *app = data;
    gtk_style_context_remove_class(
        gtk_widget_get_style_context(app->source_card), "drop-active");
}

static void on_drag_data_received(GtkWidget *widget, GdkDragContext *context,
                                  gint x, gint y, GtkSelectionData *selection,
                                  guint info, guint time, gpointer data)
{
    (void) widget;
    (void) x;
    (void) y;
    (void) info;
    App *app = data;

    gtk_style_context_remove_class(
        gtk_widget_get_style_context(app->source_card), "drop-active");

    gchar **uris = gtk_selection_data_get_uris(selection);
    gboolean accepted = FALSE;
    if (uris && uris[0]) {
        GPtrArray *paths = g_ptr_array_new_with_free_func(g_free);
        for (gint i = 0; uris[i] != NULL; i++) {
            gchar *path = g_filename_from_uri(uris[i], NULL, NULL);
            if (path)
                g_ptr_array_add(paths, path);
        }
        if (paths->len > 0)
            accepted = add_source_paths(app, paths, FALSE);
        g_ptr_array_free(paths, TRUE);
    }
    if (uris)
        g_strfreev(uris);

    gtk_drag_finish(context, accepted, FALSE, time);
}

/* ------------------------------------------------------------------------- */
/* Conversion                                                                */
/* ------------------------------------------------------------------------- */

static void update_busy_widgets(App *app, gboolean busy)
{
    guint count = sources_total_count(app);
    gtk_widget_set_sensitive(app->source_entry, !busy);
    gtk_widget_set_sensitive(app->source_button, !busy);
    gtk_widget_set_sensitive(app->source_clear_button, !busy && count > 0);
    gtk_widget_set_sensitive(app->source_list, !busy && count > 0);
    gtk_widget_set_sensitive(app->format_combo,
                             !busy && app->batch_kind != MEDIA_NONE);
    gtk_widget_set_sensitive(app->folder_entry, !busy && count > 0);
    gtk_widget_set_sensitive(app->folder_button, !busy && count > 0);
    gtk_widget_set_sensitive(app->cancel_button, busy);
    gtk_widget_set_visible(app->cancel_button, busy);
    gtk_widget_set_sensitive(app->convert_button, FALSE);

    if (busy) {
        gtk_widget_set_sensitive(app->quality_combo, FALSE);
    } else {
        TargetFormat target = current_target(app);
        gtk_widget_set_sensitive(
            app->quality_combo,
            count > 0 && media_quality_supported(target));
        update_convert_sensitivity(app);
    }
}

static void append_error_line(App *app, const gchar *line)
{
    if (!line || !*line)
        return;

    log_append(app, line);
    log_append(app, "\n");
    g_free(app->error_tail);
    app->error_tail = g_strdup(line);
}

static gboolean is_progress_line(const gchar *line)
{
    static const gchar *const keys[] = {
        "frame=", "fps=", "bitrate=", "total_size=", "out_time_us=",
        "out_time_ms=", "out_time=", "dup_frames=", "drop_frames=",
        "speed=", "progress=", NULL
    };

    for (gint i = 0; keys[i] != NULL; i++) {
        if (g_str_has_prefix(line, keys[i]))
            return TRUE;
    }

    if (g_str_has_prefix(line, "stream_") && g_str_has_suffix(line, "_q="))
        return TRUE;

    return FALSE;
}

static void update_progress_text(App *app)
{
    if (!app->converting || app->cancel_requested || !app->active_source)
        return;

    guint total = sources_total_count(app);
    gdouble current = app->current_fraction;
    if (current < 0.0)
        current = 0.0;
    if (current > 1.0)
        current = 1.0;

    gdouble overall = total > 0
        ? ((gdouble) app->batch_index + current) / (gdouble) total
        : current;
    if (overall < 0.0)
        overall = 0.0;
    if (overall > 1.0)
        overall = 1.0;

    if (app->active_source->info.duration <= 0.0) {
        gtk_progress_bar_pulse(GTK_PROGRESS_BAR(app->progress));
        gchar *text;
        if (total > 1) {
            text = app->speed_text && *app->speed_text
                ? g_strdup_printf("Файл %u из %u · конвертация… · %s",
                                  app->batch_index + 1, total, app->speed_text)
                : g_strdup_printf("Файл %u из %u · конвертация…",
                                  app->batch_index + 1, total);
        } else {
            text = app->speed_text && *app->speed_text
                ? g_strdup_printf("Конвертация… · %s", app->speed_text)
                : g_strdup("Конвертация…");
        }
        set_status(app, STATUS_INFO, text);
        g_free(text);
        return;
    }

    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(app->progress), overall);
    gint percent = (gint) (overall * 100.0 + 0.5);
    if (percent > 100)
        percent = 100;

    gchar *text;
    if (total > 1) {
        text = app->speed_text && *app->speed_text
            ? g_strdup_printf("Файл %u из %u · %d%% · %s",
                              app->batch_index + 1, total, percent,
                              app->speed_text)
            : g_strdup_printf("Файл %u из %u · %d%%",
                              app->batch_index + 1, total, percent);
    } else {
        text = app->speed_text && *app->speed_text
            ? g_strdup_printf("Конвертация… %d%% · %s", percent, app->speed_text)
            : g_strdup_printf("Конвертация… %d%%", percent);
    }
    set_status(app, STATUS_INFO, text);
    g_free(text);
}

static void process_stderr_line(App *app, const gchar *line)
{
    if (!line || !*line)
        return;

    if (g_str_has_prefix(line, "out_time_us=")) {
        gint64 usec = g_ascii_strtoll(line + 12, NULL, 10);
        gdouble seconds = (gdouble) usec / 1000000.0;
        if (app->active_source && app->active_source->info.duration > 0.0) {
            gdouble fraction = seconds / app->active_source->info.duration;
            if (fraction > 1.0)
                fraction = 1.0;
            if (fraction < 0.0)
                fraction = 0.0;
            app->current_fraction = fraction;
            update_progress_text(app);
        }
        return;
    }

    if (g_str_has_prefix(line, "out_time=")) {
        const gchar *time = line + 9;
        gint hours = 0, minutes = 0;
        gdouble seconds = 0.0;
        if (sscanf(time, "%d:%d:%lf", &hours, &minutes, &seconds) == 3 &&
            app->active_source && app->active_source->info.duration > 0.0) {
            gdouble total = hours * 3600.0 + minutes * 60.0 + seconds;
            gdouble fraction = total / app->active_source->info.duration;
            if (fraction > 1.0)
                fraction = 1.0;
            app->current_fraction = fraction;
            update_progress_text(app);
        }
        return;
    }

    if (g_str_has_prefix(line, "speed=")) {
        g_free(app->speed_text);
        app->speed_text = g_strdup(line + 6);
        update_progress_text(app);
        return;
    }

    if (g_str_has_prefix(line, "progress=")) {
        update_progress_text(app);
        return;
    }

    if (is_progress_line(line))
        return;

    append_error_line(app, line);
}

static void read_next_stderr_line(App *app);

static void on_stderr_line(GObject *source, GAsyncResult *result, gpointer data)
{
    (void) source;
    App *app = data;

    GError *error = NULL;
    gsize length = 0;
    gchar *line = g_data_input_stream_read_line_finish(
        app->stderr_stream, result, &length, &error);

    if (!line) {
        if (error && !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
            append_error_line(app, error->message);
        }
        g_clear_error(&error);
        app->stderr_done = TRUE;
        if (app->converting && app->proc_done)
            finalize_conversion(app);
        return;
    }

    gchar *valid = g_utf8_make_valid(line, (gssize) MIN(length, (gsize) G_MAXSSIZE));
    process_stderr_line(app, valid);
    g_free(valid);
    g_free(line);

    if (app->converting)
        read_next_stderr_line(app);
}

static void read_next_stderr_line(App *app)
{
    if (!app->stderr_stream || app->stderr_done)
        return;
    g_data_input_stream_read_line_async(
        app->stderr_stream, G_PRIORITY_DEFAULT, app->cancellable,
        on_stderr_line, app);
}

static void on_process_wait(GObject *source, GAsyncResult *result, gpointer data)
{
    (void) source;
    App *app = data;

    GError *error = NULL;
    if (!g_subprocess_wait_finish(app->proc, result, &error)) {
        if (error && !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
            g_free(app->wait_error);
            app->wait_error = g_strdup(error->message);
            append_error_line(app, error->message);
        }
        g_clear_error(&error);
    }

    app->proc_done = TRUE;
    if (app->stderr_done)
        finalize_conversion(app);
}

static void clear_process_state(App *app)
{
    g_clear_object(&app->cancellable);
    g_clear_object(&app->stderr_stream);
    g_clear_object(&app->proc);
    g_clear_pointer(&app->wait_error, g_free);
    app->proc_done = FALSE;
    app->stderr_done = FALSE;
}

static void finish_batch(App *app)
{
    if (app->next_item_source_id != 0) {
        g_source_remove(app->next_item_source_id);
        app->next_item_source_id = 0;
    }

    guint total = sources_total_count(app);
    gboolean canceled = app->cancel_requested;
    guint success = app->batch_success;
    guint failed = app->batch_failed;
    gchar *last_output = g_strdup(app->output_path);

    app->converting = FALSE;
    app->active_source = NULL;
    app->current_fraction = 0.0;
    clear_process_state(app);
    gtk_widget_set_sensitive(app->cancel_button, TRUE);
    gtk_widget_set_visible(app->cancel_button, FALSE);

    if (canceled) {
        gtk_widget_set_visible(app->progress, FALSE);
        gchar *text = g_strdup_printf(
            "Пакетная конвертация отменена. Готово: %u из %u.", success, total);
        set_status(app, STATUS_INFO, text);
        g_free(text);
    } else if (failed == 0) {
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(app->progress), 1.0);
        gtk_widget_set_visible(app->progress, TRUE);
        if (total == 1 && last_output) {
            const gchar *base = strrchr(last_output, G_DIR_SEPARATOR);
            base = base ? base + 1 : last_output;
            gchar *text = g_strdup_printf("Готово: %s", base);
            set_status(app, STATUS_SUCCESS, text);
            set_tooltip(app->status_label, last_output);
            g_free(text);
        } else {
            gchar *text = g_strdup_printf("Готово: %u %s.",
                                          success, files_word(success));
            set_status(app, STATUS_SUCCESS, text);
            set_tooltip(app->status_label, "Все файлы успешно сконвертированы.");
            g_free(text);
        }
    } else {
        gtk_progress_bar_set_fraction(
            GTK_PROGRESS_BAR(app->progress),
            total > 0 ? (gdouble) success / (gdouble) total : 0.0);
        gtk_widget_set_visible(app->progress, success > 0);

        gchar *text;
        if (success > 0) {
            text = g_strdup_printf("Завершено: %u, ошибок: %u.",
                                   success, failed);
        } else {
            text = g_strdup("Не удалось сконвертировать ни один файл.");
        }
        set_status(app, STATUS_ERROR, text);
        g_free(text);

        if (app->error_tail) {
            set_tooltip(app->status_label, app->error_tail);
        } else {
            set_tooltip(app->status_label, "Подробности в журнале FFmpeg.");
        }
        gtk_widget_set_visible(app->log_expander, TRUE);
        gtk_expander_set_expanded(GTK_EXPANDER(app->log_expander), TRUE);
    }

    if (success > 0)
        gtk_widget_set_visible(app->reveal_button, TRUE);

    if (total > 1) {
        gchar *summary;
        if (success == total) {
            summary = g_strdup_printf("Готово: %u %s", total, files_word(total));
        } else if (success > 0) {
            summary = g_strdup_printf("Обработано: %u из %u", success, total);
        } else {
            summary = g_strdup("Нет успешных результатов");
        }
        gtk_label_set_text(GTK_LABEL(app->output_label), summary);
        set_tooltip(app->output_label,
                    gtk_entry_get_text(GTK_ENTRY(app->folder_entry)));
        g_free(summary);
    } else if (success == 0) {
        gtk_label_set_text(GTK_LABEL(app->output_label), "—");
        set_tooltip(app->output_label, NULL);
    }

    g_clear_pointer(&last_output, g_free);
    g_clear_pointer(&app->error_tail, g_free);
    g_clear_pointer(&app->speed_text, g_free);
    app->cancel_requested = FALSE;
    app->batch_index = total;

    update_busy_widgets(app, FALSE);

    if (app->quit_after_finish) {
        gtk_widget_destroy(app->window);
        return;
    }
}

static gboolean start_next_batch_idle(gpointer data)
{
    App *app = data;
    app->next_item_source_id = 0;
    start_next_batch_item(app);
    return G_SOURCE_REMOVE;
}

static void start_next_batch_item(App *app)
{
    if (!app->converting)
        return;

    guint total = sources_total_count(app);
    if (app->cancel_requested || app->batch_index >= total) {
        finish_batch(app);
        return;
    }

    SourceItem *item = g_ptr_array_index(app->sources, app->batch_index);
    TargetFormat target = current_target(app);
    const FormatSpec *spec = media_format_spec(target);
    if (!spec) {
        app->batch_failed++;
        app->batch_index++;
        app->next_item_source_id = g_idle_add(start_next_batch_idle, app);
        return;
    }

    gchar *folder = expand_path(gtk_entry_get_text(GTK_ENTRY(app->folder_entry)));
    if (!folder || !g_file_test(folder, G_FILE_TEST_IS_DIR)) {
        gchar *message = g_strdup_printf(
            "Папка назначения недоступна: %s", folder ? folder : "");
        append_error_line(app, message);
        g_free(message);
        app->batch_failed++;
        app->batch_index++;
        g_free(folder);
        app->next_item_source_id = g_idle_add(start_next_batch_idle, app);
        return;
    }

    gchar *stem = media_stem(item->path);
    gchar *output_path = media_unique_output_path(folder, stem, spec->extension);
    g_free(stem);
    g_free(folder);

    g_free(app->output_path);
    app->output_path = output_path;
    gtk_label_set_text(GTK_LABEL(app->output_label), output_path);
    set_tooltip(app->output_label, output_path);

    QualityPreset quality = current_quality(app);
    GPtrArray *argv = media_build_ffmpeg_argv(
        item->path, output_path, target, quality, TRUE);
    if (!argv) {
        append_error_line(app, "Не удалось подготовить команду FFmpeg.");
        g_remove(output_path);
        g_free(app->output_path);
        app->output_path = NULL;
        app->batch_failed++;
        app->batch_index++;
        app->next_item_source_id = g_idle_add(start_next_batch_idle, app);
        return;
    }

    gchar *base = g_path_get_basename(item->path);
    gchar *header = g_strdup_printf("\n=== [%u/%u] %s ===\n",
                                    app->batch_index + 1, total, base);
    log_append(app, header);
    g_free(header);
    g_free(base);

    gchar *command = media_argv_to_string(argv);
    log_append(app, "$ ");
    log_append(app, command);
    log_append(app, "\n\n");
    g_free(command);

    GError *error = NULL;
    GSubprocess *process = g_subprocess_newv(
        (const gchar *const *) argv->pdata,
        G_SUBPROCESS_FLAGS_STDERR_PIPE | G_SUBPROCESS_FLAGS_STDOUT_SILENCE,
        &error);
    g_ptr_array_free(argv, TRUE);

    if (!process) {
        gchar *message = g_strdup_printf(
            "Не удалось запустить FFmpeg: %s",
            error ? error->message : "неизвестная ошибка");
        append_error_line(app, message);
        g_free(message);
        g_clear_error(&error);
        g_remove(output_path);
        g_free(app->output_path);
        app->output_path = NULL;
        app->batch_failed++;
        app->batch_index++;
        app->next_item_source_id = g_idle_add(start_next_batch_idle, app);
        return;
    }

    app->active_source = item;
    app->current_fraction = 0.0;
    app->proc = process;
    app->cancellable = g_cancellable_new();
    app->stderr_stream = g_data_input_stream_new(
        g_subprocess_get_stderr_pipe(process));
    g_data_input_stream_set_newline_type(app->stderr_stream,
                                         G_DATA_STREAM_NEWLINE_TYPE_ANY);

    app->proc_done = FALSE;
    app->stderr_done = FALSE;
    g_clear_pointer(&app->wait_error, g_free);
    g_clear_pointer(&app->error_tail, g_free);
    g_clear_pointer(&app->speed_text, g_free);

    gtk_widget_set_visible(app->reveal_button, FALSE);
    gtk_widget_set_visible(app->log_expander, FALSE);
    gtk_widget_set_visible(app->progress, TRUE);
    gtk_progress_bar_set_fraction(
        GTK_PROGRESS_BAR(app->progress),
        total > 0 ? (gdouble) app->batch_index / (gdouble) total : 0.0);
    gtk_widget_set_sensitive(app->cancel_button, TRUE);

    if (total > 1) {
        gchar *text = g_strdup_printf("Файл %u из %u · подготовка…",
                                      app->batch_index + 1, total);
        set_status(app, STATUS_INFO, text);
        g_free(text);
    } else {
        set_status(app, STATUS_INFO, "Запуск FFmpeg…");
    }

    read_next_stderr_line(app);
    g_subprocess_wait_async(process, app->cancellable, on_process_wait, app);
}

static void finalize_conversion(App *app)
{
    if (!app->converting || !app->proc_done || !app->stderr_done)
        return;

    gboolean exited = app->proc && g_subprocess_get_if_exited(app->proc);
    gboolean signaled = app->proc && g_subprocess_get_if_signaled(app->proc);
    gint exit_status = exited ? g_subprocess_get_exit_status(app->proc) : -1;
    gboolean success = exited && exit_status == 0 &&
        !app->cancel_requested && app->wait_error == NULL;
    gboolean output_exists = app->output_path &&
        g_file_test(app->output_path, G_FILE_TEST_IS_REGULAR);

    if (success && output_exists) {
        app->batch_success++;
    } else if (app->cancel_requested) {
        if (app->output_path)
            g_remove(app->output_path);
    } else {
        if (app->output_path)
            g_remove(app->output_path);
        app->batch_failed++;
        if (!app->error_tail) {
            if (signaled) {
                append_error_line(app, "FFmpeg был остановлен сигналом.");
            } else {
                gchar *detail = g_strdup_printf(
                    "FFmpeg завершился с кодом %d.", exit_status);
                append_error_line(app, detail);
                g_free(detail);
            }
        }
    }

    clear_process_state(app);
    app->active_source = NULL;
    app->current_fraction = 0.0;
    app->batch_index++;

    if (app->cancel_requested || app->batch_index >= sources_total_count(app)) {
        finish_batch(app);
        return;
    }

    start_next_batch_item(app);
}

static void start_conversion(App *app)
{
    if (app->converting)
        return;

    if (!app->ffmpeg_available) {
        set_status(app, STATUS_ERROR,
                   "FFmpeg не найден. Установите пакет ffmpeg и перезапустите программу.");
        return;
    }

    if (sources_total_count(app) == 0) {
        set_status(app, STATUS_ERROR, "Сначала добавьте файлы для конвертации.");
        return;
    }

    TargetFormat target = current_target(app);
    const FormatSpec *spec = media_format_spec(target);
    if (!spec) {
        set_status(app, STATUS_ERROR, "Выберите выходной формат.");
        return;
    }

    const gchar *folder_text = gtk_entry_get_text(GTK_ENTRY(app->folder_entry));
    gchar *folder = expand_path(folder_text);
    if (!folder || !*folder) {
        set_status(app, STATUS_ERROR, "Укажите папку назначения.");
        g_free(folder);
        return;
    }
    if (!g_file_test(folder, G_FILE_TEST_IS_DIR)) {
        set_status(app, STATUS_ERROR, "Папка назначения не существует.");
        set_tooltip(app->status_label, folder);
        g_free(folder);
        return;
    }
    if (g_access(folder, W_OK) != 0) {
        set_status(app, STATUS_ERROR, "Нет прав на запись в папку назначения.");
        set_tooltip(app->status_label, folder);
        g_free(folder);
        return;
    }
    g_free(folder);

    app->converting = TRUE;
    app->cancel_requested = FALSE;
    app->batch_index = 0;
    app->batch_success = 0;
    app->batch_failed = 0;
    app->active_source = NULL;
    if (app->next_item_source_id != 0) {
        g_source_remove(app->next_item_source_id);
        app->next_item_source_id = 0;
    }
    app->current_fraction = 0.0;
    g_clear_pointer(&app->wait_error, g_free);
    g_clear_pointer(&app->error_tail, g_free);
    g_clear_pointer(&app->speed_text, g_free);

    log_clear(app);
    gchar *header = g_strdup_printf(
        "Пакетная конвертация: %u %s → %s\n",
        sources_total_count(app), files_word(sources_total_count(app)),
        spec->label);
    log_append(app, header);
    g_free(header);

    gtk_widget_set_visible(app->reveal_button, FALSE);
    gtk_widget_set_visible(app->log_expander, FALSE);
    gtk_widget_set_visible(app->progress, TRUE);
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(app->progress), 0.0);
    gtk_widget_set_sensitive(app->cancel_button, TRUE);
    update_busy_widgets(app, TRUE);

    if (sources_total_count(app) > 1) {
        set_status(app, STATUS_INFO, "Запуск пакетной конвертации…");
    } else {
        set_status(app, STATUS_INFO, "Запуск FFmpeg…");
    }

    start_next_batch_item(app);
}

static void cancel_conversion(App *app)
{
    if (!app->converting || app->cancel_requested)
        return;

    app->cancel_requested = TRUE;
    if (sources_total_count(app) > 1) {
        set_status(app, STATUS_INFO, "Останавливаем пакетную конвертацию…");
    } else {
        set_status(app, STATUS_INFO, "Останавливаем FFmpeg…");
    }
    gtk_widget_set_sensitive(app->cancel_button, FALSE);

    if (app->cancellable)
        g_cancellable_cancel(app->cancellable);
    if (app->proc) {
        g_subprocess_force_exit(app->proc);
    } else {
        finish_batch(app);
    }
}

static void reveal_output(App *app)
{
    const gchar *folder_text = gtk_entry_get_text(GTK_ENTRY(app->folder_entry));
    gchar *folder = expand_path(folder_text);
    if (!folder || !*folder || !g_file_test(folder, G_FILE_TEST_IS_DIR)) {
        set_status(app, STATUS_ERROR, "Папка результата недоступна.");
        g_free(folder);
        return;
    }

    gchar *uri = g_filename_to_uri(folder, NULL, NULL);
    GError *error = NULL;
    if (uri && !g_app_info_launch_default_for_uri(uri, NULL, &error)) {
        set_status(app, STATUS_ERROR, "Не удалось открыть папку.");
        if (error) {
            set_tooltip(app->status_label, error->message);
            g_clear_error(&error);
        }
    }
    g_free(uri);
    g_free(folder);
}

/* ------------------------------------------------------------------------- */
/* Information window                                                        */
/* ------------------------------------------------------------------------- */

static void on_info_destroy(GtkWidget *widget, gpointer data)
{
    (void) widget;
    App *app = data;
    app->info_dialog = NULL;
    app->theme_switch = NULL;
}

static void on_info_response(GtkDialog *dialog, gint response, gpointer data)
{
    (void) response;
    (void) data;
    gtk_widget_destroy(GTK_WIDGET(dialog));
}

static GtkWidget *info_row(GtkGrid *grid, gint row,
                           const gchar *key, GtkWidget *value)
{
    GtkWidget *key_label = make_label(key, "info-key");
    gtk_widget_set_valign(key_label, GTK_ALIGN_START);
    gtk_grid_attach(grid, key_label, 0, row, 1, 1);

    gtk_widget_set_halign(value, GTK_ALIGN_START);
    gtk_widget_set_valign(value, GTK_ALIGN_START);
    gtk_widget_set_hexpand(value, TRUE);
    gtk_grid_attach(grid, value, 1, row, 1, 1);
    return value;
}

static void show_license_dialog(App *app)
{
    GtkWidget *parent = app->info_dialog ? app->info_dialog : app->window;
    GtkWidget *dialog = gtk_dialog_new_with_buttons(
        "GNU General Public License v3.0",
        GTK_WINDOW(parent),
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "_Закрыть", GTK_RESPONSE_CLOSE,
        NULL);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 760, 650);
    gtk_window_set_position(GTK_WINDOW(dialog), GTK_WIN_POS_CENTER_ON_PARENT);
    gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_CLOSE);

    GtkWidget *scroller = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_hexpand(scroller, TRUE);
    gtk_widget_set_vexpand(scroller, TRUE);

    GtkWidget *text_view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(text_view), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(text_view), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(text_view), GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(text_view), 16);
    gtk_text_view_set_right_margin(GTK_TEXT_VIEW(text_view), 16);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(text_view), 14);
    gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(text_view), 14);

    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(text_view));
    GError *error = NULL;
    GBytes *license_bytes = g_resources_lookup_data(
        RESOURCE_PREFIX "/GPL-3.0.txt", G_RESOURCE_LOOKUP_FLAGS_NONE, &error);
    if (license_bytes) {
        gsize length = 0;
        const gchar *text = g_bytes_get_data(license_bytes, &length);
        gtk_text_buffer_set_text(buffer, text, (gint) length);
        g_bytes_unref(license_bytes);
    } else {
        gtk_text_buffer_set_text(
            buffer,
            "Не удалось загрузить текст лицензии. "
            "Полный текст GNU GPL v3.0 доступен на https://www.gnu.org/licenses/gpl-3.0.html",
            -1);
        g_clear_error(&error);
    }

    gtk_container_add(GTK_CONTAINER(scroller), text_view);
    gtk_container_add(GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(dialog))),
                      scroller);
    gtk_widget_set_margin_start(scroller, 12);
    gtk_widget_set_margin_end(scroller, 12);
    gtk_widget_set_margin_top(scroller, 12);
    gtk_widget_set_margin_bottom(scroller, 12);
    gtk_widget_show_all(dialog);
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
}

static void show_info_dialog(App *app)
{
    if (app->info_dialog) {
        gtk_window_present(GTK_WINDOW(app->info_dialog));
        return;
    }

    GtkWidget *dialog = gtk_dialog_new_with_buttons(
        "Информация о Hyuga Convert",
        GTK_WINDOW(app->window),
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "_Закрыть", GTK_RESPONSE_CLOSE,
        NULL);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 620, 500);
    gtk_window_set_position(GTK_WINDOW(dialog), GTK_WIN_POS_CENTER_ON_PARENT);
    gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_CLOSE);

    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    gtk_container_set_border_width(GTK_CONTAINER(content), 22);

    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
    gtk_container_add(GTK_CONTAINER(content), outer);

    GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 18);
    GtkWidget *logo = gtk_image_new();
    if (app->icon) {
        GdkPixbuf *scaled = gdk_pixbuf_scale_simple(
            app->icon, 104, 104, GDK_INTERP_BILINEAR);
        if (scaled) {
            gtk_image_set_from_pixbuf(GTK_IMAGE(logo), scaled);
            g_object_unref(scaled);
        }
    } else {
        gtk_image_set_from_icon_name(GTK_IMAGE(logo), "image-missing",
                                     GTK_ICON_SIZE_DIALOG);
        gtk_image_set_pixel_size(GTK_IMAGE(logo), 104);
    }
    gtk_widget_set_valign(logo, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(header), logo, FALSE, FALSE, 0);

    GtkWidget *headline = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
    GtkWidget *title = make_label("Hyuga Convert", "info-title");
    GtkWidget *version = make_label("Версия " VERSION, "info-version");
    GtkWidget *summary = make_label(
        "Локальный конвертер видео и изображений на C, GTK 3 и FFmpeg.",
        "info-desc");
    gtk_label_set_line_wrap(GTK_LABEL(summary), TRUE);
    gtk_label_set_line_wrap_mode(GTK_LABEL(summary), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_max_width_chars(GTK_LABEL(summary), 42);
    gtk_box_pack_start(GTK_BOX(headline), title, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(headline), version, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(headline), summary, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(header), headline, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(outer), header, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(outer), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL),
                       FALSE, FALSE, 0);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 9);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 22);
    gtk_box_pack_start(GTK_BOX(outer), grid, FALSE, FALSE, 0);

    GtkWidget *author = make_label("Telona Hyuga", "info-value");
    gtk_label_set_selectable(GTK_LABEL(author), TRUE);
    info_row(GTK_GRID(grid), 0, "Автор", author);

    GtkWidget *website = gtk_link_button_new_with_label(
        "https://telona.xo.je", "https://telona.xo.je");
    gtk_widget_set_halign(website, GTK_ALIGN_START);
    info_row(GTK_GRID(grid), 1, "Сайт", website);

    GtkWidget *copyright = make_label(
        "© 2026 Telona Hyuga", "info-value");
    gtk_label_set_selectable(GTK_LABEL(copyright), TRUE);
    info_row(GTK_GRID(grid), 2, "Copyright", copyright);

    GtkWidget *license_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget *license = make_label(
        "GNU GPL v3.0 или новее (GPL-3.0-or-later)", "info-value");
    gtk_label_set_selectable(GTK_LABEL(license), TRUE);
    GtkWidget *license_button = gtk_button_new_with_label("Полный текст");
    set_tooltip(license_button, "Открыть полный текст GNU GPL v3.0");
    g_signal_connect_swapped(license_button, "clicked",
                             G_CALLBACK(show_license_dialog), app);
    gtk_box_pack_start(GTK_BOX(license_row), license, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(license_row), license_button, FALSE, FALSE, 0);
    info_row(GTK_GRID(grid), 3, "Лицензия", license_row);

    GtkWidget *theme_panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_style_context_add_class(
        gtk_widget_get_style_context(theme_panel), "info-panel");
    GtkWidget *theme_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
    GtkWidget *theme_text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    GtkWidget *theme_title = make_label("Тёмная тема", "info-value");
    GtkWidget *theme_hint = make_label(
        "Оформление применяется сразу и запоминается для следующих запусков.",
        "hint");
    gtk_label_set_line_wrap(GTK_LABEL(theme_hint), TRUE);
    gtk_label_set_line_wrap_mode(GTK_LABEL(theme_hint), PANGO_WRAP_WORD_CHAR);
    gtk_box_pack_start(GTK_BOX(theme_text), theme_title, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(theme_text), theme_hint, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(theme_row), theme_text, TRUE, TRUE, 0);

    GtkWidget *theme_switch = gtk_switch_new();
    gtk_switch_set_active(GTK_SWITCH(theme_switch), app->dark_theme);
    gtk_widget_set_valign(theme_switch, GTK_ALIGN_CENTER);
    g_signal_connect(theme_switch, "state-set",
                     G_CALLBACK(on_theme_switch_state_set), app);
    gtk_box_pack_end(GTK_BOX(theme_row), theme_switch, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(theme_panel), theme_row, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(outer), theme_panel, FALSE, FALSE, 0);

    GtkWidget *footer = make_label(
        "Исходный код распространяется на условиях GNU GPL v3.0 или новее.",
        "hint");
    gtk_label_set_line_wrap(GTK_LABEL(footer), TRUE);
    gtk_label_set_line_wrap_mode(GTK_LABEL(footer), PANGO_WRAP_WORD_CHAR);
    gtk_box_pack_start(GTK_BOX(outer), footer, FALSE, FALSE, 0);

    app->info_dialog = dialog;
    app->theme_switch = theme_switch;
    g_signal_connect(dialog, "destroy", G_CALLBACK(on_info_destroy), app);
    g_signal_connect(dialog, "response", G_CALLBACK(on_info_response), app);

    gtk_widget_show_all(dialog);
}

/* ------------------------------------------------------------------------- */
/* Window and application                                                    */
/* ------------------------------------------------------------------------- */

static void load_css(void)
{
    GError *error = NULL;
    GBytes *bytes = g_resources_lookup_data(
        RESOURCE_PREFIX "/style.css", G_RESOURCE_LOOKUP_FLAGS_NONE, &error);
    if (!bytes) {
        g_warning("Не удалось загрузить стили: %s", error->message);
        g_clear_error(&error);
        return;
    }

    GtkCssProvider *provider = gtk_css_provider_new();
    gtk_css_provider_load_from_data(
        provider,
        g_bytes_get_data(bytes, NULL),
        (gssize) g_bytes_get_size(bytes),
        &error);
    if (error) {
        g_warning("Ошибка CSS: %s", error->message);
        g_clear_error(&error);
    } else {
        GdkScreen *screen = gdk_screen_get_default();
        if (screen) {
            gtk_style_context_add_provider_for_screen(
                screen, GTK_STYLE_PROVIDER(provider),
                GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        }
    }

    g_object_unref(provider);
    g_bytes_unref(bytes);
}

static GdkPixbuf *load_app_icon(void)
{
    GError *error = NULL;
    GdkPixbuf *icon = gdk_pixbuf_new_from_resource_at_scale(
        RESOURCE_PREFIX "/hyuga-convert.svg", 256, 256, TRUE, &error);
    if (!icon)
        g_clear_error(&error);
    return icon;
}

static void build_ui(App *app)
{
    app->window = gtk_application_window_new(app->application);
    gtk_window_set_title(GTK_WINDOW(app->window), "Hyuga Convert");
    gtk_window_set_default_size(GTK_WINDOW(app->window), 800, 760);
    gtk_window_set_position(GTK_WINDOW(app->window), GTK_WIN_POS_CENTER);

    GdkGeometry geometry = { 0 };
    geometry.min_width = 640;
    geometry.min_height = 560;
    gtk_window_set_geometry_hints(GTK_WINDOW(app->window), NULL, &geometry,
                                  GDK_HINT_MIN_SIZE);

    app->icon = load_app_icon();
    if (app->icon) {
        gtk_window_set_default_icon(app->icon);
        gtk_window_set_icon(GTK_WINDOW(app->window), app->icon);
    }

    GtkWidget *header = gtk_header_bar_new();
    gtk_header_bar_set_show_close_button(GTK_HEADER_BAR(header), TRUE);
    gtk_header_bar_set_title(GTK_HEADER_BAR(header), "Hyuga Convert");
    gtk_header_bar_set_subtitle(GTK_HEADER_BAR(header),
                                "Видео и изображения · FFmpeg");
    gtk_window_set_titlebar(GTK_WINDOW(app->window), header);

    if (app->icon) {
        GdkPixbuf *header_pixbuf = gdk_pixbuf_scale_simple(
            app->icon, 24, 24, GDK_INTERP_BILINEAR);
        if (header_pixbuf) {
            GtkWidget *header_icon = gtk_image_new_from_pixbuf(header_pixbuf);
            gtk_widget_set_tooltip_text(header_icon, "Hyuga Convert");
            gtk_header_bar_pack_start(GTK_HEADER_BAR(header), header_icon);
            g_object_unref(header_pixbuf);
        }
    }

    GtkWidget *menu_button = gtk_menu_button_new();
    gtk_button_set_image(GTK_BUTTON(menu_button),
                         gtk_image_new_from_icon_name("open-menu-symbolic",
                                                      GTK_ICON_SIZE_BUTTON));
    gtk_button_set_always_show_image(GTK_BUTTON(menu_button), TRUE);
    set_tooltip(menu_button, "Меню");
    GMenu *menu = g_menu_new();
    GMenu *file_section = g_menu_new();
    g_menu_append(file_section, "Открыть файлы…", "app.open");
    g_menu_append(file_section, "Конвертировать", "app.convert");
    g_menu_append(file_section, "Отменить конвертацию", "app.cancel");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(file_section));
    GMenu *help_section = g_menu_new();
    g_menu_append(help_section, "Информация", "app.about");
    g_menu_append(help_section, "Выход", "app.quit");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(help_section));
    gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(menu_button), G_MENU_MODEL(menu));
    gtk_header_bar_pack_end(GTK_HEADER_BAR(header), menu_button);
    g_object_unref(file_section);
    g_object_unref(help_section);
    g_object_unref(menu);

    GtkWidget *scroller = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(app->window), scroller);

    GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
    gtk_widget_set_margin_start(content, 22);
    gtk_widget_set_margin_end(content, 22);
    gtk_widget_set_margin_top(content, 18);
    gtk_widget_set_margin_bottom(content, 18);
    gtk_container_add(GTK_CONTAINER(scroller), content);

    GtkWidget *intro = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    GtkWidget *hero = make_label("Конвертация видео и изображений", "hero-title");
    GtkWidget *subtitle = make_label(
        "Выберите один или несколько файлов — остальное сделает локальный FFmpeg.",
        "hint");
    gtk_box_pack_start(GTK_BOX(intro), hero, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(intro), subtitle, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(content), intro, FALSE, FALSE, 0);

    /* Source card */
    app->source_card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_style_context_add_class(gtk_widget_get_style_context(app->source_card), "card");
    gtk_box_pack_start(GTK_BOX(content), app->source_card, FALSE, FALSE, 0);

    GtkWidget *source_header = make_section_header(
        "1 · Исходные файлы", "можно перетащить несколько");
    gtk_box_pack_start(GTK_BOX(app->source_card), source_header, FALSE, FALSE, 0);

    GtkWidget *source_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    app->source_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(app->source_entry),
                                   "Добавить путь к файлу и нажать Enter");
    gtk_widget_set_hexpand(app->source_entry, TRUE);
    set_tooltip(app->source_entry,
                "Введите путь и нажмите Enter или выберите несколько файлов кнопкой «Добавить файлы».");
    app->source_button = make_button_with_icon(
        "_Добавить файлы…", "document-open-symbolic");
    app->source_clear_button = gtk_button_new_with_label("Очистить");
    gtk_style_context_add_class(
        gtk_widget_get_style_context(app->source_clear_button), "flat");
    set_tooltip(app->source_clear_button, "Убрать все файлы из списка");
    gtk_box_pack_start(GTK_BOX(source_row), app->source_entry, TRUE, TRUE, 0);
    gtk_box_pack_end(GTK_BOX(source_row), app->source_clear_button, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(source_row), app->source_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(app->source_card), source_row, FALSE, FALSE, 0);

    app->source_scroller = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(app->source_scroller),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(app->source_scroller, -1, 116);
    gtk_style_context_add_class(
        gtk_widget_get_style_context(app->source_scroller), "source-list");
    app->source_list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(app->source_list),
                                    GTK_SELECTION_NONE);
    gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(app->source_list), FALSE);
    gtk_style_context_add_class(
        gtk_widget_get_style_context(app->source_list), "source-list-box");
    gtk_container_add(GTK_CONTAINER(app->source_scroller), app->source_list);
    gtk_box_pack_start(GTK_BOX(app->source_card), app->source_scroller,
                       FALSE, FALSE, 0);

    app->source_info = make_label("Файлы не выбраны.", "info");
    gtk_label_set_ellipsize(GTK_LABEL(app->source_info), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(app->source_info), 100);
    gtk_box_pack_start(GTK_BOX(app->source_card), app->source_info, FALSE, FALSE, 0);

    /* Output card */
    GtkWidget *output_card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_style_context_add_class(gtk_widget_get_style_context(output_card), "card");
    gtk_box_pack_start(GTK_BOX(content), output_card, FALSE, FALSE, 0);

    GtkWidget *output_header = make_section_header(
        "2 · Формат результата", "качество и папка");
    gtk_box_pack_start(GTK_BOX(output_card), output_header, FALSE, FALSE, 0);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 10);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_box_pack_start(GTK_BOX(output_card), grid, FALSE, FALSE, 0);

    app->format_combo = gtk_combo_box_text_new();
    gtk_widget_set_hexpand(app->format_combo, TRUE);
    gtk_widget_set_size_request(app->format_combo, 280, 34);
    gtk_grid_attach(GTK_GRID(grid), make_form_label("Формат"), 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), app->format_combo, 1, 0, 2, 1);

    app->quality_combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(app->quality_combo),
                              "high", media_quality_name(QUALITY_HIGH));
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(app->quality_combo),
                              "balanced", media_quality_name(QUALITY_BALANCED));
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(app->quality_combo),
                              "small", media_quality_name(QUALITY_SMALL));
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(app->quality_combo), "balanced");
    gtk_widget_set_size_request(app->quality_combo, 230, 34);
    gtk_widget_set_halign(app->quality_combo, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), make_form_label("Качество"), 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), app->quality_combo, 1, 1, 1, 1);

    GtkWidget *folder_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    app->folder_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(app->folder_entry),
                                   "Папка для результата");
    gtk_widget_set_hexpand(app->folder_entry, TRUE);
    app->folder_button = gtk_button_new_from_icon_name(
        "folder-open-symbolic", GTK_ICON_SIZE_BUTTON);
    set_tooltip(app->folder_button, "Выбрать папку");
    gtk_box_pack_start(GTK_BOX(folder_row), app->folder_entry, TRUE, TRUE, 0);
    gtk_box_pack_end(GTK_BOX(folder_row), app->folder_button, FALSE, FALSE, 0);
    gtk_grid_attach(GTK_GRID(grid), make_form_label("Папка"), 0, 2, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), folder_row, 1, 2, 2, 1);

    GtkWidget *output_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(output_box), "output-box");
    app->output_label = make_label("—", "output-path");
    gtk_label_set_ellipsize(GTK_LABEL(app->output_label), PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_selectable(GTK_LABEL(app->output_label), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(app->output_label), 60);
    gtk_widget_set_hexpand(app->output_label, TRUE);
    gtk_box_pack_start(GTK_BOX(output_box), app->output_label, TRUE, TRUE, 0);
    gtk_grid_attach(GTK_GRID(grid), make_form_label("Результат"), 0, 3, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), output_box, 1, 3, 2, 1);

    app->format_desc = make_label(
        "Добавьте файлы — подходящие форматы появятся автоматически.",
        "format-desc");
    gtk_label_set_ellipsize(GTK_LABEL(app->format_desc), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(app->format_desc), 95);
    gtk_box_pack_start(GTK_BOX(output_card), app->format_desc, FALSE, FALSE, 0);

    /* Actions */
    GtkWidget *actions = gtk_box_new(GTK_ORIENTATION_VERTICAL, 9);
    gtk_box_pack_start(GTK_BOX(content), actions, FALSE, FALSE, 0);

    app->progress = gtk_progress_bar_new();
    gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(app->progress), FALSE);
    gtk_widget_set_size_request(app->progress, -1, 6);
    gtk_box_pack_start(GTK_BOX(actions), app->progress, FALSE, FALSE, 0);

    GtkWidget *action_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    app->status_label = make_label("Готово к работе.", "status-info");
    gtk_label_set_ellipsize(GTK_LABEL(app->status_label), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(app->status_label), 55);
    gtk_widget_set_hexpand(app->status_label, TRUE);
    gtk_box_pack_start(GTK_BOX(action_row), app->status_label, TRUE, TRUE, 0);

    app->reveal_button = make_button_with_icon("Открыть папку", "folder-open-symbolic");
    gtk_button_set_label(GTK_BUTTON(app->reveal_button), "Открыть папку");
    gtk_style_context_add_class(gtk_widget_get_style_context(app->reveal_button), "flat");

    app->cancel_button = gtk_button_new_with_label("Отмена");

    app->convert_button = make_button_with_icon(
        "_Конвертировать", "document-save-symbolic");
    gtk_style_context_add_class(
        gtk_widget_get_style_context(app->convert_button), "suggested-action");
    gtk_widget_set_size_request(app->convert_button, 172, 36);

    /* pack_end() prepends, so pack in reverse visual order to get:
     * [status] ... [Открыть папку] [Отмена] [Конвертировать] */
    gtk_box_pack_end(GTK_BOX(action_row), app->convert_button, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(action_row), app->cancel_button, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(action_row), app->reveal_button, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(actions), action_row, FALSE, FALSE, 0);

    /* Log */
    app->log_expander = gtk_expander_new("Журнал FFmpeg");
    GtkWidget *log_scroller = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(log_scroller),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(log_scroller, -1, 140);
    app->log_view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(app->log_view), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(app->log_view), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(app->log_view), TRUE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(app->log_view), GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(app->log_view), 8);
    gtk_text_view_set_right_margin(GTK_TEXT_VIEW(app->log_view), 8);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(app->log_view), 8);
    gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(app->log_view), 8);
    app->log_buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(app->log_view));
    gtk_style_context_add_class(gtk_widget_get_style_context(app->log_view), "log-view");
    gtk_container_add(GTK_CONTAINER(log_scroller), app->log_view);
    gtk_container_add(GTK_CONTAINER(app->log_expander), log_scroller);
    gtk_box_pack_start(GTK_BOX(content), app->log_expander, FALSE, FALSE, 0);

    GtkWidget *footer = make_label(
        "Обработка выполняется локально. Ничего не загружается в интернет.",
        "footer");
    gtk_widget_set_halign(footer, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(content), footer, FALSE, FALSE, 4);

    g_signal_connect(app->source_button, "clicked", G_CALLBACK(choose_source), app);
    g_signal_connect(app->source_clear_button, "clicked",
                     G_CALLBACK(on_clear_sources), app);
    g_signal_connect(app->folder_button, "clicked", G_CALLBACK(choose_folder), app);
    g_signal_connect(app->source_entry, "activate", G_CALLBACK(on_source_activate), app);
    g_signal_connect(app->format_combo, "changed", G_CALLBACK(on_format_changed), app);
    g_signal_connect(app->quality_combo, "changed", G_CALLBACK(on_quality_changed), app);
    g_signal_connect(app->folder_entry, "changed", G_CALLBACK(on_folder_changed), app);
    g_signal_connect(app->convert_button, "clicked", G_CALLBACK(start_conversion), app);
    g_signal_connect(app->cancel_button, "clicked", G_CALLBACK(cancel_conversion), app);
    g_signal_connect(app->reveal_button, "clicked", G_CALLBACK(reveal_output), app);
    g_signal_connect(app->window, "destroy", G_CALLBACK(on_window_destroy), app);

    GtkTargetEntry drop_targets[] = {
        { (gchar *) "text/uri-list", 0, 0 }
    };
    gtk_drag_dest_set(app->window, GTK_DEST_DEFAULT_ALL, drop_targets, 1,
                      GDK_ACTION_COPY);
    g_signal_connect(app->window, "drag-motion", G_CALLBACK(on_drag_motion), app);
    g_signal_connect(app->window, "drag-leave", G_CALLBACK(on_drag_leave), app);
    g_signal_connect(app->window, "drag-data-received",
                     G_CALLBACK(on_drag_data_received), app);

    gtk_widget_show_all(app->window);
    gtk_widget_set_visible(app->progress, FALSE);
    gtk_widget_set_visible(app->cancel_button, FALSE);
    gtk_widget_set_visible(app->reveal_button, FALSE);
    gtk_widget_set_visible(app->log_expander, FALSE);
    gtk_widget_set_visible(app->log_view, TRUE);

    gtk_widget_set_sensitive(app->folder_entry, FALSE);
    gtk_widget_set_sensitive(app->folder_button, FALSE);
    gtk_widget_set_sensitive(app->quality_combo, FALSE);
    gtk_widget_set_sensitive(app->convert_button, FALSE);
    gtk_widget_set_sensitive(app->source_clear_button, FALSE);

    rebuild_source_list(app);
    rebuild_format_combo(app, TARGET_NONE);

    app->ffmpeg_available = g_find_program_in_path("ffmpeg") != NULL;
    app->ffprobe_available = g_find_program_in_path("ffprobe") != NULL;
    if (!app->ffmpeg_available) {
        set_status(app, STATUS_ERROR,
                   "FFmpeg не найден. Установите пакет ffmpeg, затем перезапустите программу.");
    }
}

static void install_action(App *app, const gchar *name, GCallback callback)
{
    GSimpleAction *action = g_simple_action_new(name, NULL);
    g_signal_connect(action, "activate", callback, app);
    g_action_map_add_action(G_ACTION_MAP(app->application), G_ACTION(action));
    g_object_unref(action);
}

static void action_open(GSimpleAction *action, GVariant *parameter, gpointer data)
{
    (void) action;
    (void) parameter;
    choose_source((App *) data);
}

static void action_convert(GSimpleAction *action, GVariant *parameter, gpointer data)
{
    (void) action;
    (void) parameter;
    start_conversion((App *) data);
}

static void action_cancel(GSimpleAction *action, GVariant *parameter, gpointer data)
{
    (void) action;
    (void) parameter;
    cancel_conversion((App *) data);
}

static void action_about(GSimpleAction *action, GVariant *parameter, gpointer data)
{
    (void) action;
    (void) parameter;
    show_info_dialog((App *) data);
}

static void action_quit(GSimpleAction *action, GVariant *parameter, gpointer data)
{
    (void) action;
    (void) parameter;
    App *app = data;

    if (app->info_dialog) {
        GtkWidget *info_dialog = app->info_dialog;
        app->info_dialog = NULL;
        app->theme_switch = NULL;
        gtk_widget_destroy(info_dialog);
    }
    if (app->window)
        gtk_window_close(GTK_WINDOW(app->window));
}

static void install_actions(App *app)
{
    install_action(app, "open", G_CALLBACK(action_open));
    install_action(app, "convert", G_CALLBACK(action_convert));
    install_action(app, "cancel", G_CALLBACK(action_cancel));
    install_action(app, "about", G_CALLBACK(action_about));
    install_action(app, "quit", G_CALLBACK(action_quit));

    const gchar *open_accels[] = { "<Primary>o", NULL };
    const gchar *convert_accels[] = { "<Primary>Return", NULL };
    const gchar *cancel_accels[] = { "Escape", NULL };
    const gchar *quit_accels[] = { "<Primary>q", NULL };

    gtk_application_set_accels_for_action(app->application, "app.open", open_accels);
    gtk_application_set_accels_for_action(app->application, "app.convert", convert_accels);
    gtk_application_set_accels_for_action(app->application, "app.cancel", cancel_accels);
    gtk_application_set_accels_for_action(app->application, "app.quit", quit_accels);
}

static gboolean on_delete_event(GtkWidget *widget, GdkEvent *event, gpointer data)
{
    (void) widget;
    (void) event;
    App *app = data;

    if (!app->converting)
        return FALSE;

    GtkWidget *dialog = gtk_message_dialog_new(
        GTK_WINDOW(app->window),
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        GTK_MESSAGE_QUESTION,
        GTK_BUTTONS_NONE,
        "Идёт конвертация");
    gtk_message_dialog_format_secondary_text(
        GTK_MESSAGE_DIALOG(dialog),
        "Если выйти сейчас, FFmpeg будет остановлен, а недописанный файл удалён.");
    gtk_dialog_add_button(GTK_DIALOG(dialog), "Продолжить конвертацию",
                          GTK_RESPONSE_CANCEL);
    GtkWidget *stop_button = gtk_dialog_add_button(
        GTK_DIALOG(dialog), "Остановить и выйти", GTK_RESPONSE_ACCEPT);
    gtk_style_context_add_class(
        gtk_widget_get_style_context(stop_button), "destructive-action");
    gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_CANCEL);

    gint response = gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);

    if (response == GTK_RESPONSE_ACCEPT) {
        if (app->converting) {
            app->quit_after_finish = TRUE;
            cancel_conversion(app);
        } else {
            gtk_widget_destroy(app->window);
        }
    }

    return TRUE;
}

static App *create_app(GtkApplication *application)
{
    App *app = g_new0(App, 1);
    app->application = application;
    app->batch_kind = MEDIA_NONE;
    app->sources = g_ptr_array_new_with_free_func(source_item_free);

    GtkSettings *settings = gtk_settings_get_default();
    gboolean system_dark = FALSE;
    if (settings) {
        g_object_get(settings,
                     "gtk-application-prefer-dark-theme", &system_dark,
                     NULL);
    }
    gboolean saved_dark = FALSE;
    app->dark_theme = load_theme_preference(&saved_dark)
        ? saved_dark
        : system_dark;
    apply_dark_theme(app, app->dark_theme, FALSE);

    load_css();
    build_ui(app);
    install_actions(app);

    g_signal_connect(app->window, "delete-event", G_CALLBACK(on_delete_event), app);

    g_object_set_data(G_OBJECT(application), "hyuga-app", app);
    return app;
}

static App *ensure_app(GtkApplication *application)
{
    App *app = g_object_get_data(G_OBJECT(application), "hyuga-app");
    if (!app)
        app = create_app(application);
    return app;
}

static void on_window_destroy(GtkWidget *widget, gpointer data)
{
    (void) widget;
    App *app = data;

    if (app->info_dialog) {
        GtkWidget *info_dialog = app->info_dialog;
        app->info_dialog = NULL;
        app->theme_switch = NULL;
        gtk_widget_destroy(info_dialog);
    }

    if (app->next_item_source_id != 0) {
        g_source_remove(app->next_item_source_id);
        app->next_item_source_id = 0;
    }
    if (app->cancellable)
        g_cancellable_cancel(app->cancellable);
    if (app->proc)
        g_subprocess_force_exit(app->proc);
    g_clear_object(&app->cancellable);
    g_clear_object(&app->stderr_stream);
    g_clear_object(&app->proc);

    g_object_set_data(G_OBJECT(app->application), "hyuga-app", NULL);
    static const gchar *const action_names[] = {
        "open", "convert", "cancel", "about", "quit", NULL
    };
    for (gint i = 0; action_names[i] != NULL; i++)
        g_action_map_remove_action(G_ACTION_MAP(app->application), action_names[i]);

    if (app->sources)
        g_ptr_array_free(app->sources, TRUE);
    g_free(app->output_path);
    g_free(app->last_open_dir);
    g_free(app->wait_error);
    g_free(app->error_tail);
    g_free(app->speed_text);
    if (app->icon)
        g_object_unref(app->icon);
    g_free(app);
}

static void on_activate(GtkApplication *application, gpointer data)
{
    (void) data;
    App *app = ensure_app(application);
    gtk_widget_show(app->window);
    gtk_window_present(GTK_WINDOW(app->window));
}

static void on_open(GtkApplication *application, GFile **files, gint n_files,
                    const gchar *hint, gpointer data)
{
    (void) hint;
    (void) data;
    App *app = ensure_app(application);
    gtk_widget_show(app->window);

    if (n_files > 0) {
        GPtrArray *paths = g_ptr_array_new_with_free_func(g_free);
        gint non_local = 0;
        for (gint i = 0; i < n_files; i++) {
            gchar *path = g_file_get_path(files[i]);
            if (path)
                g_ptr_array_add(paths, path);
            else
                non_local++;
        }

        if (paths->len > 0) {
            add_source_paths(app, paths, TRUE);
        } else {
            set_status(app, STATUS_ERROR,
                       "Поддерживаются только локальные файлы.");
        }
        if (non_local > 0 && paths->len > 0) {
            set_tooltip(app->status_label,
                        "Некоторые файлы не являются локальными и были пропущены.");
        }
        g_ptr_array_free(paths, TRUE);
    }

    gtk_window_present(GTK_WINDOW(app->window));
}

static void on_startup(GtkApplication *application, gpointer data)
{
    (void) application;
    (void) data;
    g_set_application_name("Hyuga Convert");
}

static void print_usage(const gchar *program)
{
    g_print("Hyuga Convert %s\n\n", VERSION);
    g_print("Графический интерфейс:\n  %s [ФАЙЛ…]\n\n", program);
    g_print("Командная строка:\n  %s --cli --input FILE --format FORMAT [--output FILE]\n", program);
    g_print("  %s --cli --list-formats\n", program);
    g_print("  %s --cli --help\n", program);
}

int main(int argc, char **argv)
{
    setlocale(LC_ALL, "");

    if (argc == 2 && (g_strcmp0(argv[1], "--help") == 0 ||
                      g_strcmp0(argv[1], "-h") == 0)) {
        print_usage(argv[0]);
        return 0;
    }
    if (argc == 2 && g_strcmp0(argv[1], "--version") == 0) {
        g_print("Hyuga Convert %s\n", VERSION);
        return 0;
    }

    if (argc > 1 && g_strcmp0(argv[1], "--cli") == 0) {
        gint option_count = argc - 2;
        gchar **cli_argv = g_new0(gchar *, (gsize) option_count + 2);
        cli_argv[0] = (gchar *) "hyuga-convert";
        for (gint i = 0; i < option_count; i++)
            cli_argv[i + 1] = argv[i + 2];
        gint result = cli_run(option_count + 1, cli_argv);
        g_free(cli_argv);
        return result;
    }

    GtkApplication *application = gtk_application_new(
        APP_ID, G_APPLICATION_HANDLES_OPEN);
    g_signal_connect(application, "startup", G_CALLBACK(on_startup), NULL);
    g_signal_connect(application, "activate", G_CALLBACK(on_activate), NULL);
    g_signal_connect(application, "open", G_CALLBACK(on_open), NULL);

    gint status = g_application_run(G_APPLICATION(application), argc, argv);
    g_object_unref(application);
    return status;
}
