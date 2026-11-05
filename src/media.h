/*
 * media.h — format catalog, probing and FFmpeg command construction.
 *
 * Copyright (C) 2026 Telona Hyuga
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef HYUGA_MEDIA_H
#define HYUGA_MEDIA_H

#include <glib.h>

G_BEGIN_DECLS

typedef enum {
    MEDIA_NONE = 0,
    MEDIA_IMAGE,
    MEDIA_VIDEO,
    /* GIF is both a still image and an animation, so it gets its own kind. */
    MEDIA_GIF
} MediaKind;

typedef enum {
    TARGET_NONE = -1,
    TARGET_PNG = 0,
    TARGET_JPEG,
    TARGET_WEBP,
    TARGET_BMP,
    TARGET_TIFF,
    TARGET_GIF_IMAGE,
    TARGET_MP4,
    TARGET_MKV,
    TARGET_WEBM,
    TARGET_AVI,
    TARGET_MOV,
    TARGET_GIF_VIDEO,
    TARGET_COUNT
} TargetFormat;

typedef enum {
    QUALITY_HIGH = 0,
    QUALITY_BALANCED,
    QUALITY_SMALL,
    QUALITY_COUNT
} QualityPreset;

typedef struct {
    TargetFormat id;
    const gchar *extension;
    const gchar *label;
    const gchar *description;
    gboolean for_image;
    gboolean for_video;
} FormatSpec;

typedef struct {
    gboolean ok;
    gint width;
    gint height;
    gdouble duration;
    gchar *codec;
} MediaInfo;

const FormatSpec *media_format_spec(TargetFormat target);
TargetFormat media_target_from_id(const gchar *id);
const gchar *media_target_id(TargetFormat target);
TargetFormat media_target_for_extension(const gchar *extension, MediaKind kind);
TargetFormat media_default_target(MediaKind kind, const gchar *input_path);
gboolean media_target_allowed(TargetFormat target, MediaKind kind);
gboolean media_quality_supported(TargetFormat target);
const gchar *media_quality_name(QualityPreset quality);
QualityPreset media_quality_from_id(const gchar *id);

const gchar *media_kind_name(MediaKind kind);
MediaKind media_detect_kind(const gchar *path);
const gchar *const *media_supported_image_inputs(void);
const gchar *const *media_supported_video_inputs(void);

gboolean media_probe(const gchar *path, MediaInfo *info);
void media_info_clear(MediaInfo *info);
gchar *media_info_label(const gchar *path, MediaKind kind, const MediaInfo *info);

gchar *media_stem(const gchar *path);
gchar *media_unique_output_path(const gchar *folder,
                                const gchar *stem,
                                const gchar *extension);

GPtrArray *media_build_ffmpeg_argv(const gchar *input,
                                   const gchar *output,
                                   TargetFormat target,
                                   QualityPreset quality,
                                   gboolean with_progress);
gchar *media_argv_to_string(GPtrArray *argv);

G_END_DECLS

#endif /* HYUGA_MEDIA_H */
