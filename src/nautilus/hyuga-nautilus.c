/*
 * hyuga-nautilus.c — "Import in Hyuga Convert" Nautilus context action.
 *
 * Copyright (C) 2026 Telona Hyuga
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <glib.h>
#include <gio/gio.h>
#include <nautilus-extension.h>
#include <string.h>

#include "../input_formats.h"

typedef struct {
    GObject parent_instance;
} HyugaNautilusProvider;

typedef struct {
    GObjectClass parent_class;
} HyugaNautilusProviderClass;

static void hyuga_nautilus_provider_iface_init(NautilusMenuProviderInterface *iface);

G_DEFINE_DYNAMIC_TYPE_EXTENDED(
    HyugaNautilusProvider,
    hyuga_nautilus_provider,
    G_TYPE_OBJECT,
    0,
    G_IMPLEMENT_INTERFACE_DYNAMIC(NAUTILUS_TYPE_MENU_PROVIDER,
                          hyuga_nautilus_provider_iface_init))

static gboolean extension_in_list(const gchar *extension,
                                 const gchar *const *list)
{
    for (gint i = 0; list[i] != NULL; i++) {
        if (g_ascii_strcasecmp(extension, list[i]) == 0)
            return TRUE;
    }
    return FALSE;
}

typedef enum {
    FILE_KIND_NONE = 0,
    FILE_KIND_IMAGE,
    FILE_KIND_VIDEO,
    FILE_KIND_GIF
} FileKind;

static FileKind file_name_kind(const gchar *name)
{
    if (!name)
        return FILE_KIND_NONE;

    const gchar *dot = strrchr(name, '.');
    if (!dot || dot[1] == '\0')
        return FILE_KIND_NONE;

    gchar *extension = g_ascii_strdown(dot + 1, -1);
    FileKind kind = FILE_KIND_NONE;
    if (g_strcmp0(extension, "gif") == 0)
        kind = FILE_KIND_GIF;
    else if (extension_in_list(extension, hyuga_image_inputs))
        kind = FILE_KIND_IMAGE;
    else if (extension_in_list(extension, hyuga_video_inputs))
        kind = FILE_KIND_VIDEO;
    g_free(extension);
    return kind;
}

static gchar *resolve_app_path(void)
{
    /* make install places the binary in /usr or /usr/local, both are in PATH.
     * Keeping the lookup dynamic also makes the extension independent of the
     * install prefix used when it was built. */
    return g_find_program_in_path("hyuga-convert");
}

static void on_child_exit(GPid pid, gint status, gpointer data)
{
    (void) status;
    (void) data;
    g_spawn_close_pid(pid);
}

static void on_import_activate(NautilusMenuItem *item, gpointer data)
{
    (void) data;
    gchar **paths = g_object_get_data(G_OBJECT(item), "hyuga-paths");
    if (!paths || !paths[0])
        return;

    gchar *app_path = resolve_app_path();
    if (!app_path) {
        g_warning("Hyuga Convert: executable not found");
        return;
    }

    GPtrArray *argv = g_ptr_array_new();
    g_ptr_array_add(argv, app_path);
    for (gint i = 0; paths[i] != NULL; i++)
        g_ptr_array_add(argv, paths[i]);
    g_ptr_array_add(argv, NULL);

    GError *error = NULL;
    GPid pid = 0;
    gboolean spawned = g_spawn_async(
        NULL,
        (gchar **) argv->pdata,
        NULL,
        G_SPAWN_DO_NOT_REAP_CHILD,
        NULL,
        NULL,
        &pid,
        &error);

    if (!spawned) {
        g_warning("Hyuga Convert: cannot launch the application: %s",
                  error ? error->message : "unknown error");
        g_clear_error(&error);
    } else {
        g_child_watch_add(pid, on_child_exit, NULL);
    }

    g_ptr_array_free(argv, TRUE);
    g_free(app_path);
}

static GList *get_file_items(NautilusMenuProvider *provider, GList *files)
{
    (void) provider;

    if (!files)
        return NULL;

    gchar *app_path = resolve_app_path();
    if (!app_path)
        return NULL;
    g_free(app_path);

    GPtrArray *paths = g_ptr_array_new_with_free_func(g_free);
    gboolean has_image = FALSE;
    gboolean has_video = FALSE;
    gboolean valid = TRUE;

    for (GList *node = files; node != NULL; node = node->next) {
        NautilusFileInfo *file_info = node->data;
        if (nautilus_file_info_is_directory(file_info)) {
            valid = FALSE;
            break;
        }

        gchar *name = nautilus_file_info_get_name(file_info);
        FileKind kind = file_name_kind(name);
        g_free(name);
        if (kind == FILE_KIND_NONE) {
            valid = FALSE;
            break;
        }

        gchar *uri = nautilus_file_info_get_uri(file_info);
        if (!uri || !g_str_has_prefix(uri, "file://")) {
            g_free(uri);
            valid = FALSE;
            break;
        }

        gchar *path = g_filename_from_uri(uri, NULL, NULL);
        g_free(uri);
        if (!path) {
            valid = FALSE;
            break;
        }

        g_ptr_array_add(paths, path);
        if (kind == FILE_KIND_IMAGE)
            has_image = TRUE;
        else if (kind == FILE_KIND_VIDEO)
            has_video = TRUE;
    }

    if (!valid || paths->len == 0 || (has_image && has_video)) {
        g_ptr_array_free(paths, TRUE);
        return NULL;
    }

    gchar **pathv = g_new0(gchar *, paths->len + 1);
    for (guint i = 0; i < paths->len; i++)
        pathv[i] = g_strdup(g_ptr_array_index(paths, i));
    pathv[paths->len] = NULL;
    g_ptr_array_free(paths, TRUE);

    NautilusMenuItem *item = nautilus_menu_item_new(
        "HyugaConvert::import",
        "Импортировать в Hyuga Convert",
        "Открыть выбранные файлы в Hyuga Convert для пакетной конвертации",
        "hyuga-convert");
    g_object_set_data_full(G_OBJECT(item), "hyuga-paths",
                           pathv, (GDestroyNotify) g_strfreev);
    g_signal_connect(item, "activate", G_CALLBACK(on_import_activate), NULL);

    return g_list_append(NULL, item);
}

static void hyuga_nautilus_provider_iface_init(
    NautilusMenuProviderInterface *iface)
{
    iface->get_file_items = get_file_items;
    iface->get_background_items = NULL;
}

static void hyuga_nautilus_provider_class_init(
    HyugaNautilusProviderClass *klass)
{
    (void) klass;
}

static void hyuga_nautilus_provider_class_finalize(
    HyugaNautilusProviderClass *klass)
{
    (void) klass;
}

static void hyuga_nautilus_provider_init(HyugaNautilusProvider *provider)
{
    (void) provider;
}

void nautilus_module_initialize(GTypeModule *module)
{
    hyuga_nautilus_provider_register_type(module);
}

void nautilus_module_shutdown(void)
{
}

void nautilus_module_list_types(const GType **types, int *num_types)
{
    static GType type_list[1];

    type_list[0] = hyuga_nautilus_provider_get_type();
    *types = type_list;
    *num_types = 1;
}
