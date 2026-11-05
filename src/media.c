/*
 * media.c — format catalog, probing and FFmpeg command construction.
 *
 * Copyright (C) 2026 Telona Hyuga
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "media.h"
#include "input_formats.h"

#include <gio/gio.h>
#include <glib/gstdio.h>
#include <stdlib.h>
#include <string.h>

static const FormatSpec format_specs[TARGET_COUNT] = {
    [TARGET_PNG] = {
        TARGET_PNG, "png", "PNG",
        "Без потерь · поддержка прозрачности",
        TRUE, FALSE
    },
    [TARGET_JPEG] = {
        TARGET_JPEG, "jpg", "JPEG",
        "Компактный формат для фотографий · без прозрачности",
        TRUE, FALSE
    },
    [TARGET_WEBP] = {
        TARGET_WEBP, "webp", "WebP",
        "Малый размер · поддержка прозрачности",
        TRUE, FALSE
    },
    [TARGET_BMP] = {
        TARGET_BMP, "bmp", "BMP",
        "Без сжатия · большой, но простой файл",
        TRUE, FALSE
    },
    [TARGET_TIFF] = {
        TARGET_TIFF, "tiff", "TIFF",
        "Без потерь · печать и архивирование",
        TRUE, FALSE
    },
    [TARGET_GIF_IMAGE] = {
        TARGET_GIF_IMAGE, "gif", "GIF",
        "256 цветов · статичное изображение",
        TRUE, FALSE
    },
    [TARGET_MP4] = {
        TARGET_MP4, "mp4", "MP4",
        "H.264 + AAC · максимальная совместимость",
        FALSE, TRUE
    },
    [TARGET_MKV] = {
        TARGET_MKV, "mkv", "MKV",
        "H.264 + AAC · гибкий контейнер",
        FALSE, TRUE
    },
    [TARGET_WEBM] = {
        TARGET_WEBM, "webm", "WebM",
        "VP9 + Opus · открытый современный формат",
        FALSE, TRUE
    },
    [TARGET_AVI] = {
        TARGET_AVI, "avi", "AVI",
        "MPEG-4 + MP3 · совместимость со старыми плеерами",
        FALSE, TRUE
    },
    [TARGET_MOV] = {
        TARGET_MOV, "mov", "MOV",
        "H.264 + AAC · формат экосистемы Apple",
        FALSE, TRUE
    },
    [TARGET_GIF_VIDEO] = {
        TARGET_GIF_VIDEO, "gif", "GIF (анимация)",
        "Анимированный GIF без звука · 256 цветов",
        FALSE, TRUE
    }
};

static const gchar *const x264_presets[QUALITY_COUNT] = {
    "slow", "medium", "veryfast"
};
static const gchar *const x264_crf[QUALITY_COUNT] = {
    "19", "23", "29"
};
static const gchar *const aac_bitrate[QUALITY_COUNT] = {
    "192k", "160k", "96k"
};
static const gchar *const vp9_crf[QUALITY_COUNT] = {
    "26", "32", "38"
};
static const gchar *const vp9_cpu_used[QUALITY_COUNT] = {
    "2", "4", "5"
};
static const gchar *const opus_bitrate[QUALITY_COUNT] = {
    "160k", "128k", "96k"
};
static const gchar *const mpeg4_qscale[QUALITY_COUNT] = {
    "2", "5", "9"
};
static const gchar *const mp3_bitrate[QUALITY_COUNT] = {
    "192k", "128k", "96k"
};
static const gchar *const jpeg_qscale[QUALITY_COUNT] = {
    "2", "4", "8"
};
static const gchar *const webp_quality[QUALITY_COUNT] = {
    "92", "82", "68"
};

static const gchar *const gif_filters[QUALITY_COUNT] = {
    "fps=15,scale='min(720,iw)':-2:flags=lanczos,"
    "split[s0][s1];[s0]palettegen=max_colors=256:stats_mode=diff[p];"
    "[s1][p]paletteuse=dither=bayer:bayer_scale=4:diff_mode=rectangle",
    "fps=12,scale='min(480,iw)':-2:flags=lanczos,"
    "split[s0][s1];[s0]palettegen=max_colors=256:stats_mode=diff[p];"
    "[s1][p]paletteuse=dither=bayer:bayer_scale=4:diff_mode=rectangle",
    "fps=10,scale='min(360,iw)':-2:flags=lanczos,"
    "split[s0][s1];[s0]palettegen=max_colors=256:stats_mode=diff[p];"
    "[s1][p]paletteuse=dither=bayer:bayer_scale=4:diff_mode=rectangle"
};

static gint quality_index(QualityPreset quality)
{
    if (quality < 0 || quality >= QUALITY_COUNT)
        return QUALITY_BALANCED;
    return (gint) quality;
}

const FormatSpec *media_format_spec(TargetFormat target)
{
    if (target < 0 || target >= TARGET_COUNT)
        return NULL;
    return &format_specs[target];
}

const gchar *media_target_id(TargetFormat target)
{
    switch (target) {
    case TARGET_GIF_IMAGE:
        return "gif-image";
    case TARGET_GIF_VIDEO:
        return "gif-video";
    default:
        break;
    }

    const FormatSpec *spec = media_format_spec(target);
    return spec ? spec->extension : NULL;
}

TargetFormat media_target_from_id(const gchar *id)
{
    if (!id || !*id)
        return TARGET_NONE;

    for (gint i = 0; i < TARGET_COUNT; i++) {
        const FormatSpec *spec = &format_specs[i];
        const gchar *target_id = media_target_id((TargetFormat) i);
        if (g_strcmp0(id, target_id) == 0)
            return (TargetFormat) i;
        if (i == TARGET_JPEG && g_ascii_strcasecmp(id, "jpeg") == 0)
            return TARGET_JPEG;
        if (i == TARGET_TIFF && g_ascii_strcasecmp(id, "tif") == 0)
            return TARGET_TIFF;
        if (g_ascii_strcasecmp(id, spec->extension) == 0)
            return (TargetFormat) i;
    }
    return TARGET_NONE;
}

TargetFormat media_target_for_extension(const gchar *extension, MediaKind kind)
{
    if (!extension)
        return TARGET_NONE;

    gchar *ext = g_ascii_strdown(extension, -1);
    if (*ext == '.')
        memmove(ext, ext + 1, strlen(ext));

    if (g_strcmp0(ext, "jpeg") == 0 || g_strcmp0(ext, "jpe") == 0 ||
        g_strcmp0(ext, "jfif") == 0) {
        g_free(ext);
        return media_target_allowed(TARGET_JPEG, kind) ? TARGET_JPEG : TARGET_NONE;
    }
    if (g_strcmp0(ext, "tif") == 0) {
        g_free(ext);
        return media_target_allowed(TARGET_TIFF, kind) ? TARGET_TIFF : TARGET_NONE;
    }
    if (g_strcmp0(ext, "gif") == 0) {
        g_free(ext);
        if (kind == MEDIA_IMAGE)
            return TARGET_GIF_IMAGE;
        if (kind == MEDIA_VIDEO || kind == MEDIA_GIF)
            return TARGET_GIF_VIDEO;
        return TARGET_GIF_IMAGE;
    }

    for (gint i = 0; i < TARGET_COUNT; i++) {
        if (g_ascii_strcasecmp(ext, format_specs[i].extension) == 0 &&
            media_target_allowed((TargetFormat) i, kind)) {
            g_free(ext);
            return (TargetFormat) i;
        }
    }

    g_free(ext);
    return TARGET_NONE;
}

TargetFormat media_default_target(MediaKind kind, const gchar *input_path)
{
    gchar *base = input_path ? g_path_get_basename(input_path) : NULL;
    const gchar *dot = base ? strrchr(base, '.') : NULL;
    gchar *ext = dot ? g_ascii_strdown(dot + 1, -1) : NULL;
    TargetFormat target = TARGET_NONE;

    switch (kind) {
    case MEDIA_IMAGE:
        if (ext && (g_strcmp0(ext, "jpg") == 0 ||
                    g_strcmp0(ext, "jpeg") == 0 ||
                    g_strcmp0(ext, "jpe") == 0 ||
                    g_strcmp0(ext, "jfif") == 0))
            target = TARGET_PNG;
        else if (ext && g_strcmp0(ext, "png") == 0)
            target = TARGET_JPEG;
        else
            target = TARGET_PNG;
        break;
    case MEDIA_VIDEO:
        target = (ext && g_strcmp0(ext, "mp4") == 0) ? TARGET_MKV : TARGET_MP4;
        break;
    case MEDIA_GIF:
        target = TARGET_MP4;
        break;
    default:
        target = TARGET_NONE;
        break;
    }

    g_free(ext);
    g_free(base);
    return target;
}

gboolean media_target_allowed(TargetFormat target, MediaKind kind)
{
    const FormatSpec *spec = media_format_spec(target);
    if (!spec)
        return FALSE;

    switch (kind) {
    case MEDIA_IMAGE:
        return spec->for_image;
    case MEDIA_VIDEO:
        return spec->for_video;
    case MEDIA_GIF:
        /* An animated GIF can become either a still image or a video. */
        return spec->for_image || spec->for_video;
    default:
        return FALSE;
    }
}

gboolean media_quality_supported(TargetFormat target)
{
    switch (target) {
    case TARGET_JPEG:
    case TARGET_WEBP:
    case TARGET_MP4:
    case TARGET_MKV:
    case TARGET_WEBM:
    case TARGET_AVI:
    case TARGET_MOV:
    case TARGET_GIF_VIDEO:
        return TRUE;
    default:
        return FALSE;
    }
}

const gchar *media_quality_name(QualityPreset quality)
{
    static const gchar *const names[QUALITY_COUNT] = {
        "Высокое качество", "Баланс", "Малый размер"
    };
    return names[quality_index(quality)];
}

QualityPreset media_quality_from_id(const gchar *id)
{
    if (g_strcmp0(id, "high") == 0)
        return QUALITY_HIGH;
    if (g_strcmp0(id, "small") == 0)
        return QUALITY_SMALL;
    return QUALITY_BALANCED;
}

const gchar *media_kind_name(MediaKind kind)
{
    switch (kind) {
    case MEDIA_IMAGE:
        return "Изображение";
    case MEDIA_VIDEO:
        return "Видео";
    case MEDIA_GIF:
        return "GIF";
    default:
        return "Неизвестно";
    }
}

const gchar *const *media_supported_image_inputs(void)
{
    return hyuga_image_inputs;
}

const gchar *const *media_supported_video_inputs(void)
{
    return hyuga_video_inputs;
}

static gboolean extension_in_list(const gchar *extension,
                                  const gchar *const *list)
{
    for (gint i = 0; list[i] != NULL; i++) {
        if (g_ascii_strcasecmp(extension, list[i]) == 0)
            return TRUE;
    }
    return FALSE;
}

static gchar *path_extension(const gchar *path)
{
    if (!path)
        return g_strdup("");

    const gchar *base = strrchr(path, G_DIR_SEPARATOR);
    base = base ? base + 1 : path;
    const gchar *dot = strrchr(base, '.');
    if (!dot || dot == base || dot[1] == '\0')
        return g_strdup("");

    return g_ascii_strdown(dot + 1, -1);
}

MediaKind media_detect_kind(const gchar *path)
{
    if (!path || !*path)
        return MEDIA_NONE;

    gchar *ext = path_extension(path);
    MediaKind kind = MEDIA_NONE;

    if (g_strcmp0(ext, "gif") == 0)
        kind = MEDIA_GIF;
    else if (extension_in_list(ext, hyuga_image_inputs))
        kind = MEDIA_IMAGE;
    else if (extension_in_list(ext, hyuga_video_inputs))
        kind = MEDIA_VIDEO;
    g_free(ext);

    if (kind != MEDIA_NONE)
        return kind;

    /* Unknown extension: ask GIO, then FFprobe, before giving up. */
    gchar *content_type = g_content_type_guess(path, NULL, 0, NULL);
    gchar *mime_type = content_type ? g_content_type_get_mime_type(content_type) : NULL;

    if (mime_type && g_str_has_prefix(mime_type, "image/"))
        kind = MEDIA_IMAGE;
    else if (mime_type && g_str_has_prefix(mime_type, "video/"))
        kind = MEDIA_VIDEO;

    g_free(mime_type);
    g_free(content_type);

    if (kind != MEDIA_NONE)
        return kind;

    MediaInfo info = { 0 };
    if (media_probe(path, &info)) {
        if (info.codec && g_ascii_strcasecmp(info.codec, "gif") == 0)
            kind = MEDIA_GIF;
        else if (info.duration > 0.0)
            kind = MEDIA_VIDEO;
        else if (info.width > 0 && info.height > 0)
            kind = MEDIA_IMAGE;
    }
    media_info_clear(&info);

    return kind;
}

gboolean media_probe(const gchar *path, MediaInfo *info)
{
    g_return_val_if_fail(info != NULL, FALSE);
    memset(info, 0, sizeof(*info));

    if (!path || !*path || !g_find_program_in_path("ffprobe"))
        return FALSE;

    const gchar *argv[] = {
        "ffprobe", "-v", "error",
        "-select_streams", "v:0",
        "-show_entries", "stream=width,height,codec_name:format=duration",
        "-of", "default=noprint_wrappers=1",
        path, NULL
    };

    GError *error = NULL;
    GSubprocess *process = g_subprocess_newv(
        argv,
        G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE,
        &error);

    if (!process) {
        g_clear_error(&error);
        return FALSE;
    }

    gchar *stdout_buf = NULL;
    gchar *stderr_buf = NULL;
    gboolean communicated = g_subprocess_communicate_utf8(
        process, NULL, NULL, &stdout_buf, &stderr_buf, &error);

    gboolean successful = communicated && g_subprocess_get_successful(process);
    g_clear_error(&error);
    g_clear_object(&process);

    if (!successful || !stdout_buf) {
        g_free(stdout_buf);
        g_free(stderr_buf);
        return FALSE;
    }

    gchar **lines = g_strsplit(stdout_buf, "\n", -1);
    for (gint i = 0; lines[i] != NULL; i++) {
        gchar *line = g_strstrip(lines[i]);
        if (g_str_has_prefix(line, "width=")) {
            info->width = (gint) g_ascii_strtoll(line + 6, NULL, 10);
        } else if (g_str_has_prefix(line, "height=")) {
            info->height = (gint) g_ascii_strtoll(line + 7, NULL, 10);
        } else if (g_str_has_prefix(line, "duration=")) {
            const gchar *value = line + 9;
            if (g_strcmp0(value, "N/A") != 0)
                info->duration = g_ascii_strtod(value, NULL);
        } else if (g_str_has_prefix(line, "codec_name=")) {
            g_free(info->codec);
            info->codec = g_strdup(line + 11);
        }
    }
    g_strfreev(lines);

    g_free(stdout_buf);
    g_free(stderr_buf);

    info->ok = info->width > 0 && info->height > 0;
    return info->ok;
}

void media_info_clear(MediaInfo *info)
{
    if (!info)
        return;
    g_free(info->codec);
    memset(info, 0, sizeof(*info));
}

static gchar *format_duration(gdouble seconds)
{
    gint total = (gint) (seconds + 0.5);
    gint hours = total / 3600;
    gint minutes = (total % 3600) / 60;
    gint secs = total % 60;

    if (hours > 0)
        return g_strdup_printf("%d:%02d:%02d", hours, minutes, secs);
    return g_strdup_printf("%02d:%02d", minutes, secs);
}

gchar *media_info_label(const gchar *path, MediaKind kind, const MediaInfo *info)
{
    GString *text = g_string_new(media_kind_name(kind));
    gchar *ext = path_extension(path);

    if (ext[0] != '\0' && !(kind == MEDIA_GIF && g_strcmp0(ext, "gif") == 0)) {
        gchar *upper = g_ascii_strup(ext, -1);
        g_string_append_printf(text, " · %s", upper);
        g_free(upper);
    }
    g_free(ext);

    if (info && info->ok)
        g_string_append_printf(text, " · %d×%d", info->width, info->height);

    if (info && info->duration > 0.0) {
        gchar *duration = format_duration(info->duration);
        g_string_append_printf(text, " · %s", duration);
        g_free(duration);
    }

    GStatBuf stat_buf;
    if (path && g_stat(path, &stat_buf) == 0 && stat_buf.st_size > 0) {
        gchar *size = g_format_size((guint64) stat_buf.st_size);
        g_string_append_printf(text, " · %s", size);
        g_free(size);
    }

    return g_string_free(text, FALSE);
}

gchar *media_stem(const gchar *path)
{
    if (!path || !*path)
        return g_strdup("output");

    gchar *base = g_path_get_basename(path);
    gchar *dot = strrchr(base, '.');
    if (dot && dot != base)
        *dot = '\0';

    if (base[0] == '\0') {
        g_free(base);
        return g_strdup("output");
    }
    return base;
}

gchar *media_unique_output_path(const gchar *folder,
                                const gchar *stem,
                                const gchar *extension)
{
    const gchar *safe_stem = (stem && *stem) ? stem : "output";
    const gchar *safe_ext = (extension && *extension) ? extension : "out";
    if (*safe_ext == '.')
        safe_ext++;

    gchar *filename = g_strdup_printf("%s.%s", safe_stem, safe_ext);
    gchar *candidate = (folder && *folder)
        ? g_build_filename(folder, filename, NULL)
        : g_strdup(filename);
    g_free(filename);

    if (!g_file_test(candidate, G_FILE_TEST_EXISTS))
        return candidate;

    for (gint i = 1; i < 1000; i++) {
        g_free(candidate);
        if (i == 1) {
            filename = g_strdup_printf("%s-converted.%s", safe_stem, safe_ext);
        } else {
            filename = g_strdup_printf("%s-converted-%d.%s",
                                       safe_stem, i, safe_ext);
        }
        candidate = (folder && *folder)
            ? g_build_filename(folder, filename, NULL)
            : g_strdup(filename);
        g_free(filename);
        if (!g_file_test(candidate, G_FILE_TEST_EXISTS))
            return candidate;
    }

    return candidate;
}

static void argv_add(GPtrArray *argv, const gchar *value)
{
    g_ptr_array_add(argv, (gpointer) value);
}

GPtrArray *media_build_ffmpeg_argv(const gchar *input,
                                   const gchar *output,
                                   TargetFormat target,
                                   QualityPreset quality,
                                   gboolean with_progress)
{
    if (!input || !output || !media_format_spec(target))
        return NULL;

    gint q = quality_index(quality);
    GPtrArray *argv = g_ptr_array_sized_new(48);

    argv_add(argv, "ffmpeg");
    argv_add(argv, "-hide_banner");
    argv_add(argv, "-nostdin");
    argv_add(argv, "-y");
    argv_add(argv, "-loglevel");
    argv_add(argv, "error");
    argv_add(argv, "-i");
    argv_add(argv, input);

    if (with_progress) {
        argv_add(argv, "-progress");
        argv_add(argv, "pipe:2");
        argv_add(argv, "-nostats");
    }

    switch (target) {
    case TARGET_PNG:
        argv_add(argv, "-frames:v");
        argv_add(argv, "1");
        argv_add(argv, "-an");
        argv_add(argv, "-c:v");
        argv_add(argv, "png");
        argv_add(argv, "-compression_level");
        argv_add(argv, "7");
        break;

    case TARGET_JPEG:
        argv_add(argv, "-frames:v");
        argv_add(argv, "1");
        argv_add(argv, "-an");
        argv_add(argv, "-c:v");
        argv_add(argv, "mjpeg");
        argv_add(argv, "-q:v");
        argv_add(argv, jpeg_qscale[q]);
        /* 4:4:4 also keeps odd-sized images valid and preserves detail. */
        argv_add(argv, "-pix_fmt");
        argv_add(argv, "yuvj444p");
        break;

    case TARGET_WEBP:
        argv_add(argv, "-frames:v");
        argv_add(argv, "1");
        argv_add(argv, "-an");
        argv_add(argv, "-c:v");
        argv_add(argv, "libwebp");
        argv_add(argv, "-quality");
        argv_add(argv, webp_quality[q]);
        argv_add(argv, "-preset");
        argv_add(argv, "picture");
        break;

    case TARGET_BMP:
        argv_add(argv, "-frames:v");
        argv_add(argv, "1");
        argv_add(argv, "-an");
        argv_add(argv, "-c:v");
        argv_add(argv, "bmp");
        break;

    case TARGET_TIFF:
        argv_add(argv, "-frames:v");
        argv_add(argv, "1");
        argv_add(argv, "-an");
        argv_add(argv, "-c:v");
        argv_add(argv, "tiff");
        argv_add(argv, "-compression_algo");
        argv_add(argv, "deflate");
        break;

    case TARGET_GIF_IMAGE:
        argv_add(argv, "-frames:v");
        argv_add(argv, "1");
        argv_add(argv, "-an");
        argv_add(argv, "-c:v");
        argv_add(argv, "gif");
        break;

    case TARGET_MP4:
    case TARGET_MKV:
    case TARGET_MOV:
        argv_add(argv, "-map");
        argv_add(argv, "0:v:0");
        argv_add(argv, "-map");
        argv_add(argv, "0:a:0?");
        argv_add(argv, "-vf");
        argv_add(argv, "scale=trunc(iw/2)*2:trunc(ih/2)*2");
        argv_add(argv, "-c:v");
        argv_add(argv, "libx264");
        argv_add(argv, "-preset");
        argv_add(argv, x264_presets[q]);
        argv_add(argv, "-crf");
        argv_add(argv, x264_crf[q]);
        argv_add(argv, "-pix_fmt");
        argv_add(argv, "yuv420p");
        argv_add(argv, "-c:a");
        argv_add(argv, "aac");
        argv_add(argv, "-b:a");
        argv_add(argv, aac_bitrate[q]);
        if (target == TARGET_MP4 || target == TARGET_MOV) {
            argv_add(argv, "-movflags");
            argv_add(argv, "+faststart");
        }
        break;

    case TARGET_WEBM:
        argv_add(argv, "-map");
        argv_add(argv, "0:v:0");
        argv_add(argv, "-map");
        argv_add(argv, "0:a:0?");
        argv_add(argv, "-vf");
        argv_add(argv, "scale=trunc(iw/2)*2:trunc(ih/2)*2");
        argv_add(argv, "-c:v");
        argv_add(argv, "libvpx-vp9");
        argv_add(argv, "-crf");
        argv_add(argv, vp9_crf[q]);
        argv_add(argv, "-b:v");
        argv_add(argv, "0");
        argv_add(argv, "-row-mt");
        argv_add(argv, "1");
        argv_add(argv, "-cpu-used");
        argv_add(argv, vp9_cpu_used[q]);
        argv_add(argv, "-pix_fmt");
        argv_add(argv, "yuv420p");
        argv_add(argv, "-c:a");
        argv_add(argv, "libopus");
        argv_add(argv, "-b:a");
        argv_add(argv, opus_bitrate[q]);
        break;

    case TARGET_AVI:
        argv_add(argv, "-map");
        argv_add(argv, "0:v:0");
        argv_add(argv, "-map");
        argv_add(argv, "0:a:0?");
        argv_add(argv, "-vf");
        argv_add(argv, "scale=trunc(iw/2)*2:trunc(ih/2)*2");
        argv_add(argv, "-c:v");
        argv_add(argv, "mpeg4");
        argv_add(argv, "-vtag");
        argv_add(argv, "XVID");
        argv_add(argv, "-qscale:v");
        argv_add(argv, mpeg4_qscale[q]);
        argv_add(argv, "-pix_fmt");
        argv_add(argv, "yuv420p");
        argv_add(argv, "-c:a");
        argv_add(argv, "libmp3lame");
        argv_add(argv, "-b:a");
        argv_add(argv, mp3_bitrate[q]);
        break;

    case TARGET_GIF_VIDEO:
        argv_add(argv, "-map");
        argv_add(argv, "0:v:0");
        argv_add(argv, "-vf");
        argv_add(argv, gif_filters[q]);
        argv_add(argv, "-c:v");
        argv_add(argv, "gif");
        argv_add(argv, "-loop");
        argv_add(argv, "0");
        argv_add(argv, "-an");
        break;

    default:
        g_ptr_array_free(argv, TRUE);
        return NULL;
    }

    argv_add(argv, output);
    argv_add(argv, NULL);

    return argv;
}

gchar *media_argv_to_string(GPtrArray *argv)
{
    if (!argv)
        return g_strdup("");

    GString *text = g_string_new(NULL);
    for (guint i = 0; i < argv->len; i++) {
        const gchar *argument = g_ptr_array_index(argv, i);
        if (!argument)
            break;
        if (i > 0)
            g_string_append_c(text, ' ');
        gchar *quoted = g_shell_quote(argument);
        g_string_append(text, quoted);
        g_free(quoted);
    }
    return g_string_free(text, FALSE);
}
