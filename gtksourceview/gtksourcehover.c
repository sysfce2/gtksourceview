/*
 * This file is part of GtkSourceView
 *
 * Copyright 2021 Christian Hergert <chergert@redhat.com>
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
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"

#include "gtksourceassistant-private.h"
#include "gtksourcehover-private.h"
#include "gtksourcehoverassistant-private.h"
#include "gtksourcehovercontext.h"
#include "gtksourcehoverprovider.h"
#include "gtksourceiter-private.h"
#include "gtksourcesignalgroup-private.h"
#include "gtksourceview-private.h"

#define DISMISS_DELAY_MSEC 10
#define GRACE_X 20
#define GRACE_Y 20
#define MOTION_SETTLE_TIMEOUT_MSEC 250

struct _GtkSourceHover
{
	GObject             parent_instance;

	GtkSourceView      *view;
	GtkSourceAssistant *assistant;

	GPtrArray          *providers;

	gdouble             motion_x;
	gdouble             motion_y;

	guint               state;

	guint               delay_display_source;
	guint               dismiss_source;
};

enum {
	HOVER_STATE_INITIAL,
	HOVER_STATE_DISPLAY,
	HOVER_STATE_IN_POPOVER,
};

G_DEFINE_TYPE (GtkSourceHover, gtk_source_hover, G_TYPE_OBJECT)

static gboolean
gtk_source_hover_dismiss_cb (gpointer data)
{
	GtkSourceHover *self = data;

	g_assert (GTK_SOURCE_IS_HOVER (self));

	g_print ("Hover dismiss\n");

	self->dismiss_source = 0;

	switch (self->state) {
	case HOVER_STATE_DISPLAY:
		g_assert (GTK_SOURCE_IS_HOVER_ASSISTANT (self->assistant));

		gtk_widget_hide (GTK_WIDGET (self->assistant));

		g_assert (self->state == HOVER_STATE_INITIAL);
		g_assert (self->assistant == NULL);

	break;

	case HOVER_STATE_INITIAL:
	case HOVER_STATE_IN_POPOVER:
	default:
		g_clear_handle_id (&self->delay_display_source, g_source_remove);
	break;
	}

	return G_SOURCE_REMOVE;
}

static void
gtk_source_hover_queue_dismiss (GtkSourceHover *self)
{
	g_assert (GTK_SOURCE_IS_HOVER (self));

	g_clear_handle_id (&self->dismiss_source, g_source_remove);
	self->dismiss_source = g_timeout_add (DISMISS_DELAY_MSEC,
					      gtk_source_hover_dismiss_cb,
					      self);
}

static void
on_assistant_motion_cb (GtkSourceHover           *self,
                        double                    x,
                        double                    y,
                        GtkEventControllerMotion *controller)
{
	GtkAllocation alloc;
	GdkSurface *surface;
	GtkRoot *toplevel;
	double abs_x, abs_y;
	double popup_x, popup_y;

	g_assert (GTK_SOURCE_IS_HOVER (self));
	g_assert (GTK_IS_EVENT_CONTROLLER_MOTION (controller));

	if (self->assistant == NULL || self->view == NULL)
	{
		return;
	}

	toplevel = gtk_widget_get_root (GTK_WIDGET (self->view));
	surface = gtk_native_get_surface (GTK_NATIVE (self->assistant));

	/* To translate, because we are crossing surfaces, we need to
	 * take the position of the popup relative to the surface of
	 * the parent view.
	 */
	popup_x = gdk_popup_get_position_x (GDK_POPUP (surface));
	popup_y = gdk_popup_get_position_y (GDK_POPUP (surface));
	gtk_widget_get_allocation (GTK_WIDGET (self->assistant), &alloc);
	gtk_widget_translate_coordinates (GTK_WIDGET (self->view),
	                                  GTK_WIDGET (toplevel),
	                                  x, y, &abs_x, &abs_y);

	if (abs_x < (popup_x - GRACE_X) ||
	    abs_x > (popup_x + alloc.width + GRACE_X) ||
	    abs_y < (popup_y - GRACE_Y) ||
	    abs_y > (popup_y + alloc.height + GRACE_Y))
	{
		gtk_event_controller_reset (GTK_EVENT_CONTROLLER (controller));
		gtk_widget_hide (GTK_WIDGET (self->assistant));

		g_assert (self->assistant == NULL);
		g_assert (self->state == HOVER_STATE_INITIAL);
	}

	g_clear_handle_id (&self->dismiss_source, g_source_remove);
	g_clear_handle_id (&self->delay_display_source, g_source_remove);
}

static gboolean
on_key_pressed_cb (GtkSourceHover        *self,
                   guint                  keyval,
                   guint                  keycode,
                   GdkModifierType        state,
                   GtkEventControllerKey *controller)
{
	g_assert (GTK_SOURCE_IS_HOVER (self));
	g_assert (GTK_IS_EVENT_CONTROLLER_KEY (controller));

	return GDK_EVENT_PROPAGATE;
}

static void
on_motion_enter_cb (GtkSourceHover           *self,
                    double                    x,
                    double                    y,
                    GtkEventControllerMotion *controller)
{
	g_assert (GTK_SOURCE_IS_HOVER (self));
	g_assert (GTK_SOURCE_IS_VIEW (self->view));
	g_assert (GTK_IS_EVENT_CONTROLLER_MOTION (controller));

}

static void
on_motion_leave_cb (GtkSourceHover          *self,
                    GtkEventControllerMotion *controller)
{
	g_assert (GTK_SOURCE_IS_HOVER (self));
	g_assert (GTK_IS_EVENT_CONTROLLER_MOTION (controller));

}

static gboolean
gtk_source_hover_get_bounds (GtkSourceHover *self,
                             GtkTextIter    *begin,
                             GtkTextIter    *end,
                             GtkTextIter    *location)
{
	GtkTextIter iter;
	int x, y;

	g_assert (GTK_SOURCE_IS_HOVER (self));
	g_assert (!self->view || GTK_SOURCE_IS_VIEW (self->view));
	g_assert (begin != NULL);
	g_assert (end != NULL);
	g_assert (location != NULL);

	memset (begin, 0, sizeof *begin);
	memset (end, 0, sizeof *end);
	memset (location, 0, sizeof *location);

	if (self->view == NULL)
	{
		return FALSE;
	}

	g_assert (GTK_SOURCE_IS_VIEW (self->view));

	gtk_text_view_window_to_buffer_coords (GTK_TEXT_VIEW (self->view),
	                                       GTK_TEXT_WINDOW_WIDGET,
	                                       self->motion_x,
	                                       self->motion_y,
	                                       &x, &y);

	if (!gtk_text_view_get_iter_at_location (GTK_TEXT_VIEW (self->view), &iter, x, y))
	{
		return FALSE;
	}

	*location = iter;

	if (!_gtk_source_iter_inside_word (&iter))
	{
		*begin = iter;
		gtk_text_iter_set_line_offset (begin, 0);

		*end = iter;
		gtk_text_iter_forward_to_line_end (end);

		return TRUE;
	}

	if (!_gtk_source_iter_starts_full_word (&iter))
	{
		_gtk_source_iter_backward_full_word_start (&iter);
	}

	*begin = iter;
	*end = iter;

	_gtk_source_iter_forward_full_word_end (end);

	return TRUE;
}

static void
on_assistant_closed_cb (GtkSourceHover          *self,
                        GtkSourceHoverAssistant *assistant)
{
	g_assert (GTK_SOURCE_IS_HOVER (self));
	g_assert (GTK_SOURCE_IS_HOVER_ASSISTANT (assistant));

	self->state = HOVER_STATE_INITIAL;

	g_clear_pointer (&self->assistant, _gtk_source_assistant_destroy);

	g_clear_handle_id (&self->dismiss_source, g_source_remove);
	g_clear_handle_id (&self->delay_display_source, g_source_remove);

	g_assert (self->assistant == NULL);
	g_assert (self->state == HOVER_STATE_INITIAL);
	g_assert (self->dismiss_source == 0);
	g_assert (self->delay_display_source == 0);
}

static gboolean
on_assistant_motion_enter_cb (GtkSourceHover           *self,
                              double                    x,
                              double                    y,
                              GtkEventControllerMotion *motion)
{
	g_assert (GTK_SOURCE_IS_HOVER (self));
	g_assert (GTK_IS_EVENT_CONTROLLER_MOTION (motion));

	g_print ("Motion enter\n");

	/* Possible with DnD dragging? */
	if (self->state != HOVER_STATE_DISPLAY)
	{
		return GDK_EVENT_PROPAGATE;
	}

	self->state = HOVER_STATE_IN_POPOVER;
	g_clear_handle_id (&self->dismiss_source, g_source_remove);

	return GDK_EVENT_PROPAGATE;
}

static gboolean
on_assistant_motion_leave_cb (GtkSourceHover           *self,
                              GtkEventControllerMotion *motion)
{
	g_assert (GTK_SOURCE_IS_HOVER (self));
	g_assert (GTK_IS_EVENT_CONTROLLER_MOTION (motion));

	if (self->state == HOVER_STATE_IN_POPOVER)
	{
		self->state = HOVER_STATE_DISPLAY;
	}

	gtk_source_hover_queue_dismiss (self);

	return GDK_EVENT_PROPAGATE;
}

static gboolean
gtk_source_hover_motion_timeout_cb (gpointer data)
{
	GtkSourceHover *self = data;
	GdkRectangle rect;
	GdkRectangle begin_rect;
	GdkRectangle end_rect;
	GdkRectangle location_rect;
	GtkTextIter begin;
	GtkTextIter end;
	GtkTextIter location;

	g_assert (GTK_SOURCE_IS_HOVER (self));

	g_print ("Motion timeout\n");

	self->delay_display_source = 0;

	if (self->view == NULL ||
	    self->state != HOVER_STATE_INITIAL ||
	    !gtk_source_hover_get_bounds (self, &begin, &end, &location))
	{
		return G_SOURCE_REMOVE;
	}

	g_assert (GTK_SOURCE_IS_VIEW (self->view));

	if (self->assistant == NULL)
	{
		GtkEventController *motion;

		self->assistant = _gtk_source_hover_assistant_new ();

		gtk_popover_set_position (GTK_POPOVER (self->assistant), GTK_POS_TOP);
		gtk_popover_set_autohide (GTK_POPOVER (self->assistant), TRUE);

		g_signal_connect_object (self->assistant,
		                         "closed",
		                         G_CALLBACK (on_assistant_closed_cb),
		                         self,
		                         G_CONNECT_SWAPPED);

		motion = gtk_event_controller_motion_new ();
		gtk_event_controller_set_propagation_phase (motion, GTK_PHASE_CAPTURE);
		g_signal_connect_object (motion,
		                         "enter",
		                         G_CALLBACK (on_assistant_motion_enter_cb),
		                         self,
		                         G_CONNECT_SWAPPED);
		g_signal_connect_object (motion,
		                         "motion",
		                         G_CALLBACK (on_assistant_motion_cb),
		                         self,
		                         G_CONNECT_SWAPPED);
		g_signal_connect_object (motion,
		                         "leave",
		                         G_CALLBACK (on_assistant_motion_leave_cb),
		                         self,
		                         G_CONNECT_SWAPPED);
		gtk_widget_add_controller (GTK_WIDGET (self->assistant), motion);

		_gtk_source_view_add_assistant (self->view, self->assistant);
	}

	self->state = HOVER_STATE_DISPLAY;

	gtk_text_view_get_iter_location (GTK_TEXT_VIEW (self->view), &begin, &begin_rect);
	gtk_text_view_get_iter_location (GTK_TEXT_VIEW (self->view), &end, &end_rect);
	gtk_text_view_get_iter_location (GTK_TEXT_VIEW (self->view), &location, &location_rect);
	gdk_rectangle_union (&begin_rect, &end_rect, &rect);

	gtk_text_view_buffer_to_window_coords (GTK_TEXT_VIEW (self->view),
					       GTK_TEXT_WINDOW_WIDGET,
					       rect.x, rect.y, &rect.x, &rect.y);

	_gtk_source_hover_assistant_set_hovered_at (GTK_SOURCE_HOVER_ASSISTANT (self->assistant),
	                                            &location_rect);

	if (gtk_text_iter_equal (&begin, &end) &&
	    gtk_text_iter_starts_line (&begin))
	{
		rect.width = 1;
		gtk_popover_set_position (GTK_POPOVER (self->assistant), GTK_POS_RIGHT);
	}
	else
	{
		gtk_popover_set_position (GTK_POPOVER (self->assistant), GTK_POS_TOP);
	}

	gtk_widget_show (GTK_WIDGET (self->assistant));

	return G_SOURCE_REMOVE;
}

static void
on_motion_cb (GtkSourceHover           *self,
              double                    x,
              double                    y,
              GtkEventControllerMotion *controller)
{
	g_assert (GTK_SOURCE_IS_HOVER (self));
	g_assert (GTK_IS_EVENT_CONTROLLER_MOTION (controller));

	self->motion_x = x;
	self->motion_y = y;

	g_clear_handle_id (&self->dismiss_source, g_source_remove);
	g_clear_handle_id (&self->delay_display_source, g_source_remove);

	g_print ("Motion cb\n");

	self->delay_display_source = g_timeout_add (MOTION_SETTLE_TIMEOUT_MSEC,
	                                            gtk_source_hover_motion_timeout_cb,
	                                            self);
}

static gboolean
on_scroll_cb (GtkSourceHover           *self,
              double                    dx,
              double                    dy,
              GtkEventControllerScroll *controller)
{
	g_assert (GTK_SOURCE_IS_HOVER (self));
	g_assert (GTK_IS_EVENT_CONTROLLER_SCROLL (controller));

	g_clear_pointer (&self->assistant, _gtk_source_assistant_destroy);

	return GDK_EVENT_PROPAGATE;
}

static void
gtk_source_hover_dispose (GObject *object)
{
	GtkSourceHover *self = (GtkSourceHover *)object;

	g_clear_pointer (&self->assistant, _gtk_source_assistant_destroy);

	g_clear_weak_pointer (&self->view);

	g_clear_handle_id (&self->delay_display_source, g_source_remove);
	g_clear_handle_id (&self->dismiss_source, g_source_remove);

	if (self->providers->len > 0)
	{
		g_ptr_array_remove_range (self->providers, 0, self->providers->len);
	}

	G_OBJECT_CLASS (gtk_source_hover_parent_class)->dispose (object);
}

static void
gtk_source_hover_finalize (GObject *object)
{
	GtkSourceHover *self = (GtkSourceHover *)object;

	g_clear_pointer (&self->providers, g_ptr_array_unref);

	g_assert (self->delay_display_source == 0);
	g_assert (self->dismiss_source == 0);

	G_OBJECT_CLASS (gtk_source_hover_parent_class)->finalize (object);
}

static void
gtk_source_hover_class_init (GtkSourceHoverClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);

	object_class->dispose = gtk_source_hover_dispose;
	object_class->finalize = gtk_source_hover_finalize;
}

static void
gtk_source_hover_init (GtkSourceHover *self)
{
	self->providers = g_ptr_array_new_with_free_func (g_object_unref);
}

GtkSourceHover *
_gtk_source_hover_new (GtkSourceView *view)
{
	GtkSourceHover *self;
	GtkEventController *key;
	GtkEventController *motion;
	GtkEventController *scroll;

	g_return_val_if_fail (GTK_SOURCE_IS_VIEW (view), NULL);

	self = g_object_new (GTK_SOURCE_TYPE_HOVER, NULL);
	g_set_weak_pointer (&self->view, view);

	key = gtk_event_controller_key_new ();
	g_signal_connect_object (key,
	                         "key-pressed",
	                         G_CALLBACK (on_key_pressed_cb),
	                         self,
	                         G_CONNECT_SWAPPED);
	gtk_widget_add_controller (GTK_WIDGET (view), key);

	motion = gtk_event_controller_motion_new ();
	gtk_event_controller_set_propagation_phase (motion, GTK_PHASE_CAPTURE);
	g_signal_connect_object (motion,
	                         "enter",
	                         G_CALLBACK (on_motion_enter_cb),
	                         self,
	                         G_CONNECT_SWAPPED);
	g_signal_connect_object (motion,
	                         "leave",
	                         G_CALLBACK (on_motion_leave_cb),
	                         self,
	                         G_CONNECT_SWAPPED);
	g_signal_connect_object (motion,
	                         "motion",
	                         G_CALLBACK (on_motion_cb),
	                         self,
	                         G_CONNECT_SWAPPED);
	gtk_widget_add_controller (GTK_WIDGET (view), motion);

	scroll = gtk_event_controller_scroll_new (GTK_EVENT_CONTROLLER_SCROLL_BOTH_AXES);
	g_signal_connect_object (scroll,
	                         "scroll",
	                         G_CALLBACK (on_scroll_cb),
	                         self,
	                         G_CONNECT_SWAPPED);
	gtk_widget_add_controller (GTK_WIDGET (view), scroll);

	return self;
}

void
gtk_source_hover_add_provider (GtkSourceHover         *self,
                               GtkSourceHoverProvider *provider)
{
	g_return_if_fail (GTK_SOURCE_IS_HOVER (self));
	g_return_if_fail (GTK_SOURCE_IS_HOVER_PROVIDER (provider));

	for (guint i = 0; i < self->providers->len; i++)
	{
		if (g_ptr_array_index (self->providers, i) == (gpointer)provider)
		{
			return;
		}
	}

	g_ptr_array_add (self->providers, g_object_ref (provider));
}

void
gtk_source_hover_remove_provider (GtkSourceHover         *self,
                                  GtkSourceHoverProvider *provider)
{
	g_return_if_fail (GTK_SOURCE_IS_HOVER (self));
	g_return_if_fail (GTK_SOURCE_IS_HOVER_PROVIDER (provider));

	for (guint i = 0; i < self->providers->len; i++)
	{
		if (g_ptr_array_index (self->providers, i) == (gpointer)provider)
		{
			g_ptr_array_remove_index (self->providers, i);
			return;
		}
	}
}