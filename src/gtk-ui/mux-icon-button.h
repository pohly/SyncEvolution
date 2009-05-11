/*
 * Copyright (C) 2009 Intel Corporation
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) version 3.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301  USA
 */

#ifndef _MUX_ICON_BUTTON
#define _MUX_ICON_BUTTON

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define MUX_TYPE_ICON_BUTTON mux_icon_button_get_type()

#define MUX_ICON_BUTTON(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST ((obj), MUX_TYPE_ICON_BUTTON, MuxIconButton))

#define MUX_ICON_BUTTON_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST ((klass), MUX_TYPE_ICON_BUTTON, MuxIconButtonClass))

#define MUX_IS_ICON_BUTTON(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE ((obj), MUX_TYPE_ICON_BUTTON))

#define MUX_IS_ICON_BUTTON_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE ((klass), MUX_TYPE_ICON_BUTTON))

#define MUX_ICON_BUTTON_GET_CLASS(obj) \
  (G_TYPE_INSTANCE_GET_CLASS ((obj), MUX_TYPE_ICON_BUTTON, MuxIconButtonClass))

typedef struct {
    GtkButton parent;
    GdkPixbuf *pixbufs[5];
} MuxIconButton;

typedef struct {
    GtkButtonClass parent_class;
} MuxIconButtonClass;

GType mux_icon_button_get_type (void);

GtkWidget* mux_icon_button_new (GdkPixbuf *normal_pixbuf);

void mux_icon_button_set_pixbuf (MuxIconButton *button, GtkStateType state, GdkPixbuf *pixbuf);

GdkPixbuf* mux_icon_button_get_pixbuf (MuxIconButton *button, GtkStateType state);

G_END_DECLS

#endif
