/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8; coding: utf-8 -*- /
 *
 * This file is part of GtkSourceView
 *
 * Copyright 2019 - Christian Hergert <chergert@redhat.com>
 *
 * GtkSourceView is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * GtkSourceView is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include <gtk/gtk.h>

#include "gtksourcegutterlines.h"

G_BEGIN_DECLS

G_GNUC_INTERNAL
GtkSourceGutterLines *_gtk_source_gutter_lines_new             (GtkTextView          *text_view,
                                                                const GtkTextIter    *begin,
                                                                const GtkTextIter    *end,
                                                                gboolean              needs_wrap_first,
                                                                gboolean              needs_wrap_last);
G_GNUC_INTERNAL
guint                 _gtk_source_gutter_lines_get_cursor_line (GtkSourceGutterLines *lines);

G_END_DECLS