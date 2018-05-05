/*
 * week-view.c
 * This file is part of focal, a calendar application for Linux
 * Copyright 2018 Oliver Giles and focal contributors.
 *
 * Focal is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * Focal is distributed without any explicit or implied warranty.
 * You should have received a copy of the GNU General Public License
 * version 3 with focal. If not, see <http://www.gnu.org/licenses/>.
 */
#include <gtk/gtk.h>
#include <libical/ical.h>
#include <stdlib.h>
#include <string.h>

#include "week-view.h"

struct _EventWidget {
	CalendarEvent ev;
	// associated calendar
	Calendar* cal;
	// cached time values for faster drawing
	int minutes_from, minutes_to;
	// list pointer
	struct _EventWidget* next;
};
typedef struct _EventWidget EventWidget;

struct _WeekView {
	GtkDrawingArea drawing_area;
	int x, y, width, height;
	double scroll_pos;
	GtkAdjustment* adj;
	EventWidget* events_week[7];
	int current_week;
	int current_year;
	icaltimezone* current_tz;
	icaltime_span current_view;
	struct {
		int dow;
		int minutes;
	} now;
};

enum {
	SIGNAL_EVENT_SELECTED,
	LAST_SIGNAL
};

static gint week_view_signals[LAST_SIGNAL] = {0};

enum {
	PROP_0,
	PROP_HADJUSTMENT,
	PROP_VADJUSTMENT,
	PROP_HSCROLL_POLICY,
	PROP_VSCROLL_POLICY,
};

#define HEADER_HEIGHT 50.5

// Implemented from GtkScrollable, causes the scroll bar to start below the header
static gboolean get_border(GtkScrollable* scrollable, GtkBorder* border)
{
	border->top = HEADER_HEIGHT;
	return TRUE;
}

static void week_view_scrollable_init(GtkScrollableInterface* iface)
{
	iface->get_border = get_border;
}

G_DEFINE_TYPE_WITH_CODE(WeekView, week_view, GTK_TYPE_DRAWING_AREA, G_IMPLEMENT_INTERFACE(GTK_TYPE_SCROLLABLE, week_view_scrollable_init))

#define SIDEBAR_WIDTH 50.5
#define HALFHOUR_HEIGHT 30.0

static void week_view_draw(WeekView* wv, cairo_t* cr)
{
	const int num_days = 7;
	const double dashes[] = {1.0};

	double col = 0.5;
	cairo_set_source_rgb(cr, col, col, col);
	cairo_set_line_width(cr, 1.0);

	const int first_visible_halfhour = wv->scroll_pos / HALFHOUR_HEIGHT + 1;

	for (int hh = first_visible_halfhour;; ++hh) {
		double y = wv->y + HEADER_HEIGHT + hh * HALFHOUR_HEIGHT - wv->scroll_pos;
		if (y > wv->y + wv->height)
			break;
		cairo_move_to(cr, wv->x, y);
		cairo_rel_line_to(cr, wv->width, 0);
		if (hh % 2 == 0) {
			// draw hour labels
			char hourlabel[8];
			cairo_move_to(cr, wv->x + 5, y + 10);
			sprintf(hourlabel, "%d:00", hh / 2);
			cairo_show_text(cr, hourlabel);
			cairo_set_dash(cr, NULL, 0, 0);
		} else
			cairo_set_dash(cr, dashes, 1, 0);
		cairo_stroke(cr);
	}

	// draw vertical lines for days
	const int day_width = (double) (wv->width - SIDEBAR_WIDTH) / num_days;
	cairo_set_dash(cr, NULL, 0, 0);
	time_t t = time(NULL);
	struct tm* firstday = localtime(&t);
	while (firstday->tm_wday != 0) {
		t -= 60 * 60 * 24;
		firstday = localtime(&t);
	}
	for (int d = 0; d < num_days; ++d) {
		double x = wv->x + SIDEBAR_WIDTH + d * day_width;

		char daylabel[16];
		strftime(daylabel, 16, "%e %a", firstday);
		t += 60 * 60 * 24;
		firstday = localtime(&t);
		cairo_move_to(cr, x + 5, wv->y + HEADER_HEIGHT - 5);
		cairo_show_text(cr, daylabel);

		cairo_move_to(cr, x, wv->y + HEADER_HEIGHT);
		cairo_rel_line_to(cr, 0, wv->height);
		cairo_stroke(cr);
	}
	// top bar
	cairo_move_to(cr, wv->x, wv->y + HEADER_HEIGHT);
	cairo_rel_line_to(cr, wv->width, 0);
	cairo_stroke(cr);

	// draw events
	for (int d = 0; d < num_days; ++d) {
		for (EventWidget* tmp = wv->events_week[d]; tmp; tmp = tmp->next) {
			double yminutescale = HALFHOUR_HEIGHT / 30.0; //(cal->height - header_height) / (60.0 * num_hours_displayed);
			//double y = cal->y + header_height + hh * halfhour_height - cal->scroll_top;
			double yfrom = tmp->minutes_from * yminutescale + wv->y + HEADER_HEIGHT - wv->scroll_pos;
			double yto = tmp->minutes_to * yminutescale + wv->y + HEADER_HEIGHT - wv->scroll_pos;
			double x = wv->x + SIDEBAR_WIDTH + d * day_width;
			//printf("from %d to %d\n", icalcomponent_get_dtstart(event).hour, icalcomponent_get_dtend(event).hour);
			//cairo_set_source_rgba(cr, 0.3, 0.3, 0.8, 0.8);
			GdkRGBA* color = calendar_get_color(tmp->cal);
			cairo_set_source_rgba(cr, color->red, color->green, color->blue, color->alpha);
			cairo_rectangle(cr, x + 1, yfrom + 1, day_width - 2, yto - yfrom - 2);
			cairo_fill(cr);

			cairo_set_source_rgb(cr, 1, 1, 1);
			cairo_move_to(cr, x + 5, yfrom + 15);
			cairo_show_text(cr, icalcomponent_get_summary(tmp->ev.v));
		}
	}

	double nowY = wv->y + HEADER_HEIGHT + wv->now.minutes * HALFHOUR_HEIGHT / 30 - wv->scroll_pos;
	cairo_set_source_rgb(cr, 1, 0, 0);
	cairo_set_dash(cr, NULL, 0, 0);
	cairo_move_to(cr, wv->x + SIDEBAR_WIDTH + wv->now.dow * day_width, nowY);
	cairo_rel_line_to(cr, day_width, 0);
	cairo_stroke(cr);
}

static gboolean on_draw_event(GtkWidget* widget, cairo_t* cr, gpointer user_data)
{
	WeekView* wv = FOCAL_WEEK_VIEW(widget);
	week_view_draw(wv, cr);
	return FALSE;
}

static gboolean on_press_event(GtkWidget* widget, GdkEventButton* event, gpointer data)
{
	WeekView* wv = FOCAL_WEEK_VIEW(widget);
	if (event->button == GDK_BUTTON_PRIMARY) {
		// look for collisions
		if (event->x < SIDEBAR_WIDTH || event->y < HEADER_HEIGHT)
			return TRUE;

		int dow = 7 * (event->x - SIDEBAR_WIDTH) / (wv->width - SIDEBAR_WIDTH);
		int minutesAt = (event->y - HEADER_HEIGHT + wv->scroll_pos) * 30 / HALFHOUR_HEIGHT;

		EventWidget* tmp;
		for (tmp = wv->events_week[dow]; tmp; tmp = tmp->next) {
			if (tmp->minutes_from < minutesAt && minutesAt < tmp->minutes_to) {
				break;
			}
		}

		GdkRectangle rect;
		if (tmp) {
			rect.width = (wv->width - SIDEBAR_WIDTH) / 7;
			rect.x = dow * rect.width + SIDEBAR_WIDTH;
			rect.y = HEADER_HEIGHT + (tmp->minutes_from - wv->scroll_pos) * HALFHOUR_HEIGHT / 30;
			rect.height = (tmp->minutes_to - tmp->minutes_from) * HALFHOUR_HEIGHT / 30;
			g_signal_emit(wv, week_view_signals[SIGNAL_EVENT_SELECTED], 0, tmp->cal, &tmp->ev, &rect);
		} else
			g_signal_emit(wv, week_view_signals[SIGNAL_EVENT_SELECTED], 0, NULL, NULL);
	}
	return TRUE;
}

static void adjustment_changed(GtkAdjustment* adjustment, WeekView* wv)
{
	wv->scroll_pos = gtk_adjustment_get_value(adjustment);
	gtk_widget_queue_draw(GTK_WIDGET(wv));
}

static void set_vadjustment(WeekView* wv, GtkAdjustment* adjustment)
{
	if (!adjustment)
		return;

	// this function should only be called once with a real adjustment
	g_assert_null(wv->adj);

	wv->adj = g_object_ref_sink(adjustment);
	g_signal_connect(adjustment, "value-changed", G_CALLBACK(adjustment_changed), wv);
}

static void set_property(GObject* object, guint prop_id, const GValue* value, GParamSpec* pspec)
{
	if (prop_id == PROP_HADJUSTMENT) {
		// ignored, horizontal scrolling not supported
	} else if (prop_id == PROP_VADJUSTMENT) {
		set_vadjustment(FOCAL_WEEK_VIEW(object), g_value_get_object(value));
	} else {
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
	}
}

static void get_property(GObject* object, guint prop_id, GValue* value, GParamSpec* pspec)
{
	if (prop_id == PROP_HADJUSTMENT) {
		// set to NULL, horizontal scrolling not supported
		g_value_set_object(value, NULL);
	} else if (prop_id == PROP_VADJUSTMENT) {
		g_value_set_object(value, FOCAL_WEEK_VIEW(object)->adj);
	} else if (prop_id == PROP_HSCROLL_POLICY || prop_id == PROP_VSCROLL_POLICY) {
		g_value_set_enum(value, GTK_SCROLL_MINIMUM);
	} else {
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
	}
}

static void week_view_class_init(WeekViewClass* klass)
{
	GObjectClass* goc = (GObjectClass*) klass;

	((GObjectClass*) goc)->set_property = set_property;
	((GObjectClass*) goc)->get_property = get_property;
	g_object_class_override_property((GObjectClass*) goc, PROP_HADJUSTMENT, "hadjustment");
	g_object_class_override_property((GObjectClass*) goc, PROP_VADJUSTMENT, "vadjustment");
	g_object_class_override_property((GObjectClass*) goc, PROP_HSCROLL_POLICY, "hscroll-policy");
	g_object_class_override_property((GObjectClass*) goc, PROP_VSCROLL_POLICY, "vscroll-policy");

	week_view_signals[SIGNAL_EVENT_SELECTED] = g_signal_new("event-selected", G_TYPE_FROM_CLASS((GObjectClass*) goc), G_SIGNAL_RUN_LAST | G_SIGNAL_ACTION, 0, NULL, NULL, NULL, G_TYPE_NONE, 3, G_TYPE_POINTER, G_TYPE_POINTER, G_TYPE_POINTER);
}

static void on_size_allocate(GtkWidget* widget, GdkRectangle* allocation, gpointer user_data)
{
	WeekView* wv = FOCAL_WEEK_VIEW(widget);
	g_assert_nonnull(wv->adj);

	wv->width = allocation->width;
	wv->height = allocation->height;

	gtk_adjustment_configure(wv->adj,
							 wv->scroll_pos,
							 0,
							 24 * 2 * HALFHOUR_HEIGHT + HEADER_HEIGHT,
							 0.1 * wv->height,
							 0.9 * wv->height,
							 wv->height);
}

static void week_view_init(WeekView* wv)
{
	wv->scroll_pos = 410;

	gtk_widget_set_events((GtkWidget*) wv, GDK_SCROLL_MASK | GDK_SMOOTH_SCROLL_MASK | GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK);

	g_signal_connect(G_OBJECT(wv), "size-allocate", G_CALLBACK(on_size_allocate), NULL);
	g_signal_connect(G_OBJECT(wv), "draw", G_CALLBACK(on_draw_event), NULL);
	g_signal_connect(G_OBJECT(wv), "button-press-event", G_CALLBACK(on_press_event), NULL);
}

static void update_current_time(WeekView* wv)
{
	struct tm ld;
	time_t now = time(NULL);
	localtime_r(&now, &ld);
	wv->now.minutes = ld.tm_hour * 60 + ld.tm_min;
	wv->now.dow = ld.tm_wday;
}

static gboolean timer_update_current_time(gpointer user_data)
{
	update_current_time(FOCAL_WEEK_VIEW(user_data));
	gtk_widget_queue_draw(GTK_WIDGET(user_data));
	return TRUE;
}

GtkWidget* week_view_new()
{
	WeekView* cw = g_object_new(FOCAL_TYPE_WEEK_VIEW, NULL);

	char* zoneinfo_link = realpath("/etc/localtime", NULL);
	cw->current_tz = icaltimezone_get_builtin_timezone(zoneinfo_link + strlen("/usr/share/zoneinfo/"));
	free(zoneinfo_link);

	icaltimetype today = icaltime_today();
	cw->current_view = icaltime_span_new(icaltime_from_day_of_year(icaltime_day_of_year(today) - icaltime_day_of_week(today), today.year),
										 icaltime_from_day_of_year(icaltime_day_of_year(today) - icaltime_day_of_week(today) + 7, today.year), 0);
	cw->current_week = icaltime_week_number(today);
	cw->current_year = today.year;

	update_current_time(cw);
	g_timeout_add_seconds(120, &timer_update_current_time, cw);
	return (GtkWidget*) cw;
}

static void add_event_from_calendar(gpointer user_data, Calendar* cal, CalendarEvent ce)
{
	WeekView* cw = FOCAL_WEEK_VIEW(user_data);
	icaltimetype dtstart = icalcomponent_get_dtstart(ce.v);
	icaltimetype dtend = icalcomponent_get_dtend(ce.v);
	const icaltimezone* tz = icaltime_get_timezone(dtstart);
	// convert to local time
	icaltimezone_convert_time(&dtstart, (icaltimezone*) tz, cw->current_tz);
	icaltimezone_convert_time(&dtend, (icaltimezone*) tz, cw->current_tz);

	struct icaldurationtype duration = icalcomponent_get_duration(ce.v);

	icalproperty* rrule = icalcomponent_get_first_property(ce.v, ICAL_RRULE_PROPERTY);
	if (rrule) {
		// recurring event
		struct icalrecurrencetype recur = icalproperty_get_rrule(rrule);
		icalrecur_iterator* ritr = icalrecur_iterator_new(recur, dtstart);
		icaltimetype next;
		while (next = icalrecur_iterator_next(ritr), !icaltime_is_null_time(next)) {
			// crude faster filter
			// TODO what if the week overlaps a year boundary?
			if (next.year < cw->current_year)
				continue;
			else if (next.year > cw->current_year)
				break;

			// exact check
			icaltime_span span = icaltime_span_new(next, icaltime_add(next, duration), 0);
			if (icaltime_span_overlaps(&span, &cw->current_view)) {
				int dow = icaltime_day_of_week(next) - 1;
				EventWidget* w = (EventWidget*) malloc(sizeof(EventWidget));
				w->ev = ce;
				w->cal = cal;
				w->minutes_from = next.hour * 60 + next.minute;
				w->minutes_to = (next.hour + duration.hours) * 60 + next.minute + duration.minutes;
				w->next = cw->events_week[dow];
				cw->events_week[dow] = w;
			}
		}
	} else {
		// non-recurring event
		if (dtstart.year == cw->current_year) {
			icaltime_span span = icaltime_span_new(dtstart, icaltime_add(dtstart, duration), 0);
			if (icaltime_span_overlaps(&span, &cw->current_view)) {
				int dow = icaltime_day_of_week(dtstart) - 1;
				EventWidget* w = (EventWidget*) malloc(sizeof(EventWidget));
				w->ev = ce;
				w->cal = cal;
				w->minutes_from = dtstart.hour * 60 + dtstart.minute;
				w->minutes_to = dtend.hour * 60 + dtend.minute;
				w->next = cw->events_week[dow];
				cw->events_week[dow] = w;
			}
		}
	}
}

void week_view_add_event(WeekView* wv, Calendar* cal, CalendarEvent vevent)
{
	add_event_from_calendar(wv, cal, vevent);
	gtk_widget_queue_draw((GtkWidget*) wv);
}

void week_view_remove_event(WeekView* wv, CalendarEvent ce)
{
	icaltimetype dtstart = icalcomponent_get_dtstart(ce.v);
	const icaltimezone* tz = icaltime_get_timezone(dtstart);
	// convert to local time
	icaltimezone_convert_time(&dtstart, (icaltimezone*) tz, wv->current_tz);
	int dow = icaltime_day_of_week(dtstart) - 1;
	for (EventWidget** ew = &wv->events_week[dow]; *ew; ew = &(*ew)->next) {
		if (memcmp(&(*ew)->ev, &ce, sizeof(CalendarEvent)) == 0) {
			EventWidget* next = (*ew)->next;
			free(*ew);
			*ew = next;
			break;
		}
	}
	gtk_widget_queue_draw((GtkWidget*) wv);
}

void week_view_add_calendar(WeekView* wv, Calendar* cal)
{
	calendar_each_event(cal, add_event_from_calendar, wv);
	gtk_widget_queue_draw((GtkWidget*) wv);
}

int week_view_get_current_week(WeekView* wv)
{
	return wv->current_week;
}
