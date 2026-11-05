/*
 * input_formats.h — extensions accepted by the FFmpeg backend.
 *
 * Copyright (C) 2026 Telona Hyuga
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef HYUGA_INPUT_FORMATS_H
#define HYUGA_INPUT_FORMATS_H

#include <glib.h>

static const gchar *const hyuga_image_inputs[] = {
    "png", "apng", "jpg", "jpeg", "jpe", "jfif", "webp",
    "bmp", "tif", "tiff", "gif", "tga", "ppm", "pgm",
    "pbm", "pnm", NULL
};

static const gchar *const hyuga_video_inputs[] = {
    "mp4", "m4v", "mkv", "webm", "avi", "mov", "mpg",
    "mpeg", "m2v", "ts", "mts", "m2ts", "flv", "wmv",
    "3gp", "3g2", "ogv", "gif", "vob", "asf", "f4v", NULL
};

#endif /* HYUGA_INPUT_FORMATS_H */
