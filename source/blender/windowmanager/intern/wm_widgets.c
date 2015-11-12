/*
 * ***** BEGIN GPL LICENSE BLOCK *****
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version. 
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * The Original Code is Copyright (C) 2014 Blender Foundation.
 * All rights reserved.
 *
 * Contributor(s): Blender Foundation, 2008
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file blender/windowmanager/intern/wm_widgets.c
 *  \ingroup wm
 *
 * Window management, widget API.
 */

#include <stdlib.h>
#include <string.h>

#include "DNA_screen_types.h"
#include "DNA_scene_types.h"
#include "DNA_windowmanager_types.h"
#include "DNA_view3d_types.h"
#include "DNA_userdef_types.h"

#include "MEM_guardedalloc.h"

#include "BLI_listbase.h"
#include "BLI_string.h"
#include "BLI_math.h"
#include "BLI_path_util.h"

#include "BKE_context.h"
#include "BKE_idprop.h"
#include "BKE_global.h"
#include "BKE_main.h"
#include "BKE_report.h"
#include "BKE_scene.h"
#include "BKE_screen.h"

#include "ED_view3d.h"
#include "ED_screen.h"
#include "ED_util.h"

#include "WM_api.h"
#include "WM_types.h"
#include "wm.h"
#include "wm_window.h"
#include "wm_event_system.h"
#include "wm_event_types.h"
#include "wm_draw.h"

#include "GL/glew.h"
#include "GPU_select.h"

#include "RNA_access.h"
#include "RNA_define.h"
#include "BPY_extern.h"

/**
 * This is a container for all widget types that can be instantiated in a region.
 * (similar to dropboxes).
 *
 * \note There is only ever one of these for every (area, region) combination.
 */
typedef struct wmWidgetMapType {
	wmWidgetMapType *next, *prev;
	char idname[64];
	short spaceid, regionid;
	/**
	 * Check if widgetmap does 3D drawing
	 * (uses a different kind of interaction),
	 * - 3d: use glSelect buffer.
	 * - 2d: use simple cursor position intersection test. */
	bool is_3d;
	/* types of widgetgroups for this widgetmap type */
	ListBase widgetgrouptypes;
} wmWidgetMapType;


/* store all widgetboxmaps here. Anyone who wants to register a widget for a certain
 * area type can query the widgetbox to do so */
static ListBase widgetmaptypes = {NULL, NULL};


wmWidgetGroupType *WM_widgetgrouptype_new(
        int (*poll)(const bContext *C, wmWidgetGroupType *),
        void (*create)(const bContext *, wmWidgetGroup *),
        wmKeyMap *(*keymap_init)(wmKeyConfig *, const char *),
        const Main *bmain, const char *mapidname, const char *name,
        const short spaceid, const short regionid, const bool is_3d)
{
	wmWidgetMapType *wmaptype = WM_widgetmaptype_find(mapidname, spaceid, regionid, is_3d, false);
	wmWidgetGroupType *wgrouptype;
	bScreen *sc;

	if (!wmaptype) {
		fprintf(stderr, "widgetgrouptype creation: widgetmap type does not exist");
		return NULL;
	}

	wgrouptype = MEM_callocN(sizeof(wmWidgetGroupType), "widgetgroup");

	wgrouptype->poll = poll;
	wgrouptype->create = create;
	wgrouptype->keymap_init = keymap_init;
	wgrouptype->spaceid = spaceid;
	wgrouptype->regionid = regionid;
	wgrouptype->is_3d = is_3d;
	BLI_strncpy(wgrouptype->name, name, MAX_NAME);
	BLI_strncpy(wgrouptype->mapidname, mapidname, MAX_NAME);

	/* add the type for future created areas of the same type  */
	BLI_addtail(&wmaptype->widgetgrouptypes, wgrouptype);

	/* Main is missing on startup when we create new areas.
	 * So this is only called for widgets initialized on runtime */
	if (!bmain)
		return wgrouptype;


	/* init keymap - on startup there's an extra call to init keymaps for 'permanent' widget-groups */
	wm_widgetgrouptype_keymap_init(wgrouptype, ((wmWindowManager *)bmain->wm.first)->defaultconf);

	/* now create a widget for all existing areas */
	for (sc = bmain->screen.first; sc; sc = sc->id.next) {
		ScrArea *sa;

		for (sa = sc->areabase.first; sa; sa = sa->next) {
			SpaceLink *sl;

			for (sl = sa->spacedata.first; sl; sl = sl->next) {
				ARegion *ar;
				ListBase *lb = (sl == sa->spacedata.first) ? &sa->regionbase : &sl->regionbase;

				for (ar = lb->first; ar; ar = ar->next) {
					wmWidgetMap *wmap;

					for (wmap = ar->widgetmaps.first; wmap; wmap = wmap->next) {
						if (wmap->type == wmaptype) {
							wmWidgetGroup *wgroup = MEM_callocN(sizeof(wmWidgetGroup), "widgetgroup");

							wgroup->type = wgrouptype;

							/* just add here, drawing will occur on next update */
							BLI_addtail(&wmap->widgetgroups, wgroup);
							wm_widgetmap_set_highlighted_widget(wmap, NULL, NULL, 0);
							ED_region_tag_redraw(ar);
						}
					}
				}
			}
		}
	}

	return wgrouptype;
}

wmWidget *WM_widget_new(void (*draw)(const bContext *C, wmWidget *customdata),
                        void (*render_3d_intersection)(const bContext *C, wmWidget *customdata, int selectionbase),
                        int  (*intersect)(bContext *C, const wmEvent *event, wmWidget *widget),
                        int  (*handler)(bContext *C, const wmEvent *event, wmWidget *widget))
{
	wmWidget *widget;

	widget = MEM_callocN(sizeof(wmWidget), "widget");

	widget->draw = draw;
	widget->handler = handler;
	widget->intersect = intersect;
	widget->render_3d_intersection = render_3d_intersection;

	return widget;
}


static void wm_widget_delete(ListBase *widgetlist, wmWidget *widget)
{
	if (widget->opptr.data) {
		WM_operator_properties_free(&widget->opptr);
	}

	MEM_freeN(widget->props);
	MEM_freeN(widget->ptr);

	BLI_freelinkN(widgetlist, widget);
}


static void widget_calculate_scale(wmWidget *widget, const bContext *C)
{
	const RegionView3D *rv3d = CTX_wm_region_view3d(C);
	float scale = 1.0f;

	if (rv3d && (U.tw_flag & V3D_3D_WIDGETS) == 0 && (widget->flag & WM_WIDGET_SCALE_3D)) {
		if (widget->get_final_position) {
			float position[3];

			widget->get_final_position(widget, position);
			scale = ED_view3d_pixel_size(rv3d, position) * (float)U.tw_size;
		}
		else {
			scale = ED_view3d_pixel_size(rv3d, widget->origin) * (float)U.tw_size;
		}
	}

	widget->scale = scale * widget->user_scale;
}

/**
 * Initialize keymaps for all existing widget-groups
 */
void wm_widgets_keymap(wmKeyConfig *keyconf)
{
	wmWidgetMapType *wmaptype;
	wmWidgetGroupType *wgrouptype;

	for (wmaptype = widgetmaptypes.first; wmaptype; wmaptype = wmaptype->next) {
		for (wgrouptype = wmaptype->widgetgrouptypes.first; wgrouptype; wgrouptype = wgrouptype->next) {
			wm_widgetgrouptype_keymap_init(wgrouptype, keyconf);
		}
	}
}

BLI_INLINE bool widgets_compare(const wmWidget *a, const wmWidget *b)
{
	return STREQ(a->idname, b->idname);
}

void WM_widgets_update(const bContext *C, wmWidgetMap *wmap)
{
	wmWidget *widget;

	if (!wmap)
		return;

	widget = wmap->active_widget;

	if (widget) {
		widget_calculate_scale(widget, C);
	}
	else if (wmap->widgetgroups.first) {
		wmWidgetGroup *wgroup;

		for (wgroup = wmap->widgetgroups.first; wgroup; wgroup = wgroup->next) {
			if (!wgroup->type->poll || wgroup->type->poll(C, wgroup->type)) {
				wmWidget *highlighted = NULL;

				/* first delete and recreate the widgets */
				for (widget = wgroup->widgets.first; widget;) {
					wmWidget *widget_next = widget->next;

					if (widget == wmap->selected_widget) {
						/* skip */
					}
					/* do not delete the highlighted widget, instead keep it to compare with the new one */
					else if (widget->flag & WM_WIDGET_HIGHLIGHT) {
						highlighted = widget;
						BLI_remlink(&wgroup->widgets, widget);
						widget->next = widget->prev = NULL;
					}
					else {
						wm_widget_delete(&wgroup->widgets, widget);
					}
					widget = widget_next;
				}

				if (wgroup->type->create) {
					wgroup->type->create(C, wgroup);
				}

				if (highlighted) {
					for (widget = wgroup->widgets.first; widget; widget = widget->next) {
						if (widgets_compare(widget, highlighted)) {
							widget->flag |= WM_WIDGET_HIGHLIGHT;
							wmap->highlighted_widget = widget;
							widget->highlighted_part = highlighted->highlighted_part;
							wm_widget_delete(&wgroup->widgets, highlighted);
							highlighted = NULL;
							break;
						}
					}
				}

				/* if we don't find a highlighted widget, delete the old one here */
				if (highlighted) {
					MEM_freeN(highlighted);
					highlighted = NULL;
					wmap->highlighted_widget = NULL;
				}

				for (widget = wgroup->widgets.first; widget; widget = widget->next) {
					widget_calculate_scale(widget, C);
				}
			}
		}
	}
}

BLI_INLINE bool widgetgroup_poll_check(const bContext *C, const wmWidgetGroup *wgroup)
{
	return (!wgroup->type->poll || wgroup->type->poll(C, wgroup->type));
}

void WM_widgets_draw(const bContext *C, const wmWidgetMap *wmap, const bool in_scene)
{
	wmWidget *widget;
	bool use_lighting;

	if (!wmap)
		return;

	use_lighting = (U.tw_flag & V3D_SHADED_WIDGETS) != 0;

	if (use_lighting) {
		const float lightpos[4] = {0.0, 0.0, 1.0, 0.0};
		const float diffuse[4] = {1.0, 1.0, 1.0, 0.0};

		glPushAttrib(GL_LIGHTING_BIT | GL_ENABLE_BIT);
		glEnable(GL_LIGHTING);
		glEnable(GL_LIGHT0);
		glEnable(GL_COLOR_MATERIAL);
		glColorMaterial(GL_FRONT_AND_BACK, GL_DIFFUSE);
		glPushMatrix();
		glLoadIdentity();
		glLightfv(GL_LIGHT0, GL_POSITION, lightpos);
		glLightfv(GL_LIGHT0, GL_DIFFUSE, diffuse);
		glPopMatrix();
	}

	widget = wmap->active_widget;

	if (widget && in_scene == ((widget->flag & WM_WIDGET_SCENE_DEPTH) != 0)) {
		if (widget->flag & WM_WIDGET_DRAW_ACTIVE) {
			/* notice that we don't update the widgetgroup, widget is now on
			 * its own, it should have all relevant data to update itself */
			widget->draw(C, widget);
		}
	}
	else if (wmap->widgetgroups.first) {
		wmWidgetGroup *wgroup;

		for (wgroup = wmap->widgetgroups.first; wgroup; wgroup = wgroup->next) {
			if (widgetgroup_poll_check(C, wgroup)) {
				for (widget = wgroup->widgets.first; widget; widget = widget->next) {
					if ((widget->flag & WM_WIDGET_HIDDEN) == 0 &&
					    (!(widget->flag & WM_WIDGET_DRAW_HOVER) || (widget->flag & WM_WIDGET_HIGHLIGHT)) &&
					    ((widget->flag & WM_WIDGET_SCENE_DEPTH) != 0) == in_scene)
					{
						widget->draw(C, widget);
					}
				}
			}
		}
	}

	/* draw selected widgets last */
	if ((widget = wmap->selected_widget) && in_scene == ((widget->flag & WM_WIDGET_SCENE_DEPTH) != 0)) {
		if (widgetgroup_poll_check(C, widget->wgroup)) {
			/* notice that we don't update the widgetgroup, widget is now on
			 * its own, it should have all relevant data to update itself */
			widget->draw(C, widget);
		}
	}

	if (use_lighting)
		glPopAttrib();
}

void WM_event_add_area_widgetmap_handlers(ARegion *ar)
{
	wmWidgetMap *wmap;
	wmEventHandler *handler;

	for (wmap = ar->widgetmaps.first; wmap; wmap = wmap->next) {
		handler = MEM_callocN(sizeof(wmEventHandler), "widget handler");

		handler->widgetmap = wmap;
		BLI_addtail(&ar->handlers, handler);
	}
}

void WM_modal_handler_attach_widgetgroup(
        bContext *C, wmEventHandler *handler, wmWidgetGroupType *wgrouptype, wmOperator *op)
{
	/* maybe overly careful, but widgetgrouptype could come from a failed creation */
	if (!wgrouptype) {
		return;
	}

	/* now instantiate the widgetmap */
	wgrouptype->op = op;

	if (handler->op_region && handler->op_region->widgetmaps.first) {
		wmWidgetMap *wmap;
		for (wmap = handler->op_region->widgetmaps.first; wmap; wmap = wmap->next) {
			wmWidgetMapType *wmaptype = wmap->type;

			if (wmaptype->spaceid == wgrouptype->spaceid && wmaptype->regionid == wgrouptype->regionid) {
				handler->widgetmap = wmap;
			}
		}
	}
	
	WM_event_add_mousemove(C);
}

static void widget_unique_idname_set(wmWidgetGroup *wgroup, wmWidget *widget, const char *rawname)
{
	if (wgroup->type->idname[0]) {
		BLI_snprintf(widget->idname, sizeof(widget->idname), "%s_%s", wgroup->type->idname, rawname);
	}
	else {
		BLI_strncpy(widget->idname, rawname, sizeof(widget->idname));
	}

	/* ensure name is unique, append '.001', '.002', etc if not */
	BLI_uniquename(&wgroup->widgets, widget, "Widget", '.', offsetof(wmWidget, idname), sizeof(widget->idname));
}

/**
 * Search for an active widget in region \a ar
 */
static wmWidget *widget_find_active_in_region(const ARegion *ar, wmWidgetMap **r_wmap)
{
	for (*r_wmap = ar->widgetmaps.first; *r_wmap; *r_wmap = (*r_wmap)->next) {
		if ((*r_wmap)->active_widget) {
			return (*r_wmap)->active_widget;
		}
	}

	*r_wmap = NULL;
	return NULL;
}

/**
 * Register \a widget
 *
 * \param name  name used to create a unique idname for \a widget in \a wgroup
 */
bool wm_widget_register(wmWidgetGroup *wgroup, wmWidget *widget, const char *name)
{
	const float col_default[4] = {1.0f, 1.0f, 1.0f, 1.0f};

	widget_unique_idname_set(wgroup, widget, name);

	widget->user_scale = 1.0f;
	widget->line_width = 1.0f;

	/* defaults */
	copy_v4_v4(widget->col, col_default);
	copy_v4_v4(widget->col_hi, col_default);

	/* create at least one property for interaction */
	if (widget->max_prop == 0) {
		widget->max_prop = 1;
	}

	widget->props = MEM_callocN(sizeof(PropertyRNA *) * widget->max_prop, "widget->props");
	widget->ptr = MEM_callocN(sizeof(PointerRNA) * widget->max_prop, "widget->ptr");

	widget->wgroup = wgroup;

	BLI_addtail(&wgroup->widgets, widget);
	return true;
}

void WM_widget_set_property(wmWidget *widget, const int slot, PointerRNA *ptr, const char *propname)
{
	if (slot < 0 || slot >= widget->max_prop) {
		fprintf(stderr, "invalid index %d when binding property for widget type %s\n", slot, widget->idname);
		return;
	}

	/* if widget evokes an operator we cannot use it for property manipulation */
	widget->opname = NULL;
	widget->ptr[slot] = *ptr;
	widget->props[slot] = RNA_struct_find_property(ptr, propname);

	if (widget->bind_to_prop)
		widget->bind_to_prop(widget, slot);
}

PointerRNA *WM_widget_set_operator(wmWidget *widget, const char *opname)
{
	wmOperatorType *ot = WM_operatortype_find(opname, 0);

	if (ot) {
		widget->opname = opname;

		WM_operator_properties_create_ptr(&widget->opptr, ot);

		return &widget->opptr;
	}
	else {
		fprintf(stderr, "Error binding operator to widget: operator %s not found!\n", opname);
	}

	return NULL;
}

/**
 * \brief Set widget select callback
 *
 * Callback is called when widget gets selected/deselected
 */
void WM_widget_set_func_select(wmWidget *widget, void (*select)(bContext *, wmWidget *, const int action))
{
	widget->flag |= WM_WIDGET_SELECTABLE;
	widget->select = select;
}

void WM_widget_set_origin(wmWidget *widget, const float origin[3])
{
	copy_v3_v3(widget->origin, origin);
}

void WM_widget_set_offset(wmWidget *widget, const float offset[3])
{
	copy_v3_v3(widget->offset, offset);
}

void WM_widget_set_flag(wmWidget *widget, const int flag, const bool enable)
{
	if (enable) {
		widget->flag |= flag;
	}
	else {
		widget->flag &= ~flag;
	}
}

void WM_widget_set_scale(wmWidget *widget, const float scale)
{
	widget->user_scale = scale;
}

void WM_widget_set_line_width(wmWidget *widget, const float line_width)
{
	widget->line_width = line_width;
}

/**
 * Set widget rgba colors
 *
 * \param col  Normal state color
 * \param col_hi  Highlighted state color
 */
void WM_widget_set_colors(wmWidget *widget, const float col[4], const float col_hi[4])
{
	copy_v4_v4(widget->col, col);
	copy_v4_v4(widget->col_hi, col_hi);
}


/** \name Widget operators
 *
 * Basic operators for widget interaction with user configurable keymaps.
 *
 * \{ */

static int widget_set_active_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
	ARegion *ar = CTX_wm_region(C);
	wmWidgetMap *wmap;
	const bool deactivate = RNA_boolean_get(op->ptr, "deactivate");

	for (wmap = ar->widgetmaps.first; wmap; wmap = wmap->next) {
		if (deactivate) {
			wm_widgetmap_set_active_widget(wmap, C, event, NULL);

			/* ugly hack - send widget release event */
			((wmEvent *)event)->type = EVT_WIDGET_RELEASED;
		}
		else {
			wmWidget *widget = wmap->highlighted_widget;
			if (widget) {
				wm_widgetmap_set_active_widget(wmap, C, event, widget);
				break;
			}
			else {
				BLI_assert(0);
				return (OPERATOR_CANCELLED | OPERATOR_PASS_THROUGH);
			}
		}
	}

	return OPERATOR_FINISHED;
}

void WIDGETGROUP_OT_widget_set_active(wmOperatorType *ot)
{
	/* identifiers */
	ot->name = "Widget Activate";
	ot->description = "Activate the currently highlighted widget";
	ot->idname = "WIDGETGROUP_OT_widget_set_active";

	/* api callbacks */
	ot->invoke = widget_set_active_invoke;

	ot->flag = OPTYPE_UNDO;

	RNA_def_boolean(ot->srna, "deactivate", 0, "Deactivate", "Deactivate currently active widget");
}

static int widget_set_select_invoke(bContext *C, wmOperator *UNUSED(op), const wmEvent *UNUSED(event))
{
	ARegion *ar = CTX_wm_region(C);
	wmWidgetMap *wmap;

	for (wmap = ar->widgetmaps.first; wmap; wmap = wmap->next) {
		wmWidget *widget = wmap->highlighted_widget;
		if (widget) {
			if (widget->flag & WM_WIDGET_SELECTABLE) {
				wm_widgetmap_set_selected_widget(C, wmap, widget);
			}
			break;
		}
		else {
			BLI_assert(0);
			return (OPERATOR_CANCELLED | OPERATOR_PASS_THROUGH);
		}
	}

	return OPERATOR_FINISHED;
}

void WIDGETGROUP_OT_widget_set_select(wmOperatorType *ot)
{
	/* identifiers */
	ot->name = "Widget Select";
	ot->description = "Select the currently highlighted widget";
	ot->idname = "WIDGETGROUP_OT_widget_set_select";

	/* api callbacks */
	ot->invoke = widget_set_select_invoke;

	ot->flag = OPTYPE_UNDO;

	/* TODO - more fancy selections are not implemented yet */
//	RNA_def_boolean(ot->srna, "deactivate", 0, "Deactivate", "Deactivate currently active widget");
}

static int widget_tweak_invoke(bContext *C, wmOperator *UNUSED(op), const wmEvent *event)
{
	ARegion *ar = CTX_wm_region(C);
	wmWidgetMap *wmap;
	wmWidget *widget = widget_find_active_in_region(ar, &wmap);

	if (!widget) {
		BLI_assert(0);
		return (OPERATOR_CANCELLED | OPERATOR_PASS_THROUGH);
	}

	/* handle widget */
	widget->handler(C, event, widget);

	/* ugly hack - send widget update event */
	((wmEvent *)event)->type = EVT_WIDGET_UPDATE;

	return OPERATOR_FINISHED;
}

void WIDGETGROUP_OT_widget_tweak(wmOperatorType *ot)
{
	/* identifiers */
	ot->name = "Widget Tweak";
	ot->description = "Tweak the active widget";
	ot->idname = "WIDGETGROUP_OT_widget_tweak";

	/* api callbacks */
	ot->invoke = widget_tweak_invoke;
}

static int widget_cancel_invoke(bContext *C, wmOperator *UNUSED(op), const wmEvent *event)
{
	ARegion *ar = CTX_wm_region(C);
	wmWidgetMap *wmap;
	wmWidget *widget = widget_find_active_in_region(ar, &wmap);

	if (!widget) {
		/* don't assert(0) here, this might be called if modal handler
		 * which has widget attached uses same shortcut as widget-cancel */
		return (OPERATOR_CANCELLED | OPERATOR_PASS_THROUGH);
	}

	if (widget->cancel) {
		widget->cancel(C, widget);
	}
	wm_widgetmap_set_active_widget(wmap, C, event, NULL);

	/* ugly hack - send widget release event */
	((wmEvent *)event)->type = EVT_WIDGET_RELEASED;

	return OPERATOR_FINISHED;
}

void WIDGETGROUP_OT_widget_tweak_cancel(wmOperatorType *ot)
{
	/* identifiers */
	ot->name = "Widget Tweak Cancel";
	ot->description = "Cancel tweaking of active widget";
	ot->idname = "WIDGETGROUP_OT_widget_tweak_cancel";

	/* api callbacks */
	ot->invoke = widget_cancel_invoke;
}

/** \} */ // Widget operators


wmWidgetMapType *WM_widgetmaptype_find(
        const char *idname, const int spaceid, const int regionid, const bool is_3d, const bool create)
{
	wmWidgetMapType *wmaptype;

	for (wmaptype = widgetmaptypes.first; wmaptype; wmaptype = wmaptype->next) {
		if (wmaptype->spaceid == spaceid &&
		    wmaptype->regionid == regionid &&
		    wmaptype->is_3d == is_3d &&
		    STREQ(wmaptype->idname, idname))
		{
			return wmaptype;
		}
	}

	if (!create) return NULL;

	wmaptype = MEM_callocN(sizeof(wmWidgetMapType), "widgettype list");
	wmaptype->spaceid = spaceid;
	wmaptype->regionid = regionid;
	wmaptype->is_3d = is_3d;
	BLI_strncpy(wmaptype->idname, idname, 64);
	BLI_addhead(&widgetmaptypes, wmaptype);

	return wmaptype;
}

void WM_widgetmaptypes_free(void)
{
	wmWidgetMapType *wmaptype;

	for (wmaptype = widgetmaptypes.first; wmaptype; wmaptype = wmaptype->next) {
		BLI_freelistN(&wmaptype->widgetgrouptypes);
	}
	BLI_freelistN(&widgetmaptypes);

	fix_linking_widget_lib();
}

bool wm_widgetmap_is_3d(const wmWidgetMap *wmap)
{
	return wmap->type->is_3d;
}

static void widget_find_active_3D_loop(const bContext *C, ListBase *visible_widgets)
{
	int selectionbase = 0;
	LinkData *link;
	wmWidget *widget;

	for (link = visible_widgets->first; link; link = link->next) {
		widget = link->data;
		/* pass the selection id shifted by 8 bits. Last 8 bits are used for selected widget part id */
		widget->render_3d_intersection(C, widget, selectionbase << 8);

		selectionbase++;
	}
}

static int wm_widget_find_highlighted_3D_intern(
        ListBase *visible_widgets, const bContext *C, const wmEvent *event, const float hotspot)
{
	ScrArea *sa = CTX_wm_area(C);
	ARegion *ar = CTX_wm_region(C);
	View3D *v3d = sa->spacedata.first;
	RegionView3D *rv3d = ar->regiondata;
	rctf rect, selrect;
	GLuint buffer[64];      // max 4 items per select, so large enuf
	short hits;
	const bool do_passes = GPU_select_query_check_active();

	extern void view3d_winmatrix_set(ARegion *ar, View3D *v3d, rctf *rect);


	rect.xmin = event->mval[0] - hotspot;
	rect.xmax = event->mval[0] + hotspot;
	rect.ymin = event->mval[1] - hotspot;
	rect.ymax = event->mval[1] + hotspot;

	selrect = rect;

	view3d_winmatrix_set(ar, v3d, &rect);
	mul_m4_m4m4(rv3d->persmat, rv3d->winmat, rv3d->viewmat);

	if (do_passes)
		GPU_select_begin(buffer, 64, &selrect, GPU_SELECT_NEAREST_FIRST_PASS, 0);
	else
		GPU_select_begin(buffer, 64, &selrect, GPU_SELECT_ALL, 0);
	/* do the drawing */
	widget_find_active_3D_loop(C, visible_widgets);

	hits = GPU_select_end();

	if (do_passes) {
		GPU_select_begin(buffer, 64, &selrect, GPU_SELECT_NEAREST_SECOND_PASS, hits);
		widget_find_active_3D_loop(C, visible_widgets);
		GPU_select_end();
	}

	view3d_winmatrix_set(ar, v3d, NULL);
	mul_m4_m4m4(rv3d->persmat, rv3d->winmat, rv3d->viewmat);

	if (hits == 1) {
		return buffer[3];
	}
	/* find the widget the value belongs to */
	else if (hits > 1) {
		GLuint val, dep, mindep = 0, minval = -1;
		int a;

		/* we compare the hits in buffer, but value centers highest */
		/* we also store the rotation hits separate (because of arcs) and return hits on other widgets if there are */

		for (a = 0; a < hits; a++) {
			dep = buffer[4 * a + 1];
			val = buffer[4 * a + 3];

			if (minval == -1 || dep < mindep) {
				mindep = dep;
				minval = val;
			}
		}

		return minval;
	}

	return -1;
}

static void wm_prepare_visible_widgets_3D(wmWidgetMap *wmap, ListBase *visible_widgets, bContext *C)
{
	wmWidget *widget;
	wmWidgetGroup *wgroup;

	for (wgroup = wmap->widgetgroups.first; wgroup; wgroup = wgroup->next) {
		if (!wgroup->type->poll || wgroup->type->poll(C, wgroup->type)) {
			for (widget = wgroup->widgets.first; widget; widget = widget->next) {
				if (widget->render_3d_intersection && (widget->flag & WM_WIDGET_HIDDEN) == 0) {
					BLI_addhead(visible_widgets, BLI_genericNodeN(widget));
				}
			}
		}
	}
}

wmWidget *wm_widget_find_highlighted_3D(wmWidgetMap *wmap, bContext *C, const wmEvent *event, unsigned char *part)
{
	wmWidget *result = NULL;
	ListBase visible_widgets = {0};
	const float hotspot = 14.0f;
	int ret;

	wm_prepare_visible_widgets_3D(wmap, &visible_widgets, C);

	*part = 0;
	/* set up view matrices */
	view3d_operator_needs_opengl(C);

	ret = wm_widget_find_highlighted_3D_intern(&visible_widgets, C, event, 0.5f * hotspot);

	if (ret != -1) {
		LinkData *link;
		int retsec;
		retsec = wm_widget_find_highlighted_3D_intern(&visible_widgets, C, event, 0.2f * hotspot);

		if (retsec != -1)
			ret = retsec;

		link = BLI_findlink(&visible_widgets, ret >> 8);
		*part = ret & 255;
		result = link->data;
	}

	BLI_freelistN(&visible_widgets);

	return result;
}

wmWidget *wm_widget_find_highlighted(wmWidgetMap *wmap, bContext *C, const wmEvent *event, unsigned char *part)
{
	wmWidget *widget;
	wmWidgetGroup *wgroup;

	for (wgroup = wmap->widgetgroups.first; wgroup; wgroup = wgroup->next) {
		if (!wgroup->type->poll || wgroup->type->poll(C, wgroup->type)) {
			for (widget = wgroup->widgets.first; widget; widget = widget->next) {
				if (widget->intersect) {
					if ((*part = widget->intersect(C, event, widget)))
						return widget;
				}
			}
		}
	}
	
	return NULL;
}

bool WM_widgetmap_cursor_set(const wmWidgetMap *wmap, wmWindow *win)
{
	for (; wmap; wmap = wmap->next) {
		wmWidget *widget = wmap->highlighted_widget;
		if (widget && widget->get_cursor) {
			WM_cursor_set(win, widget->get_cursor(widget));
			return true;
		}
	}

	return false;
}

void wm_widgetmap_set_highlighted_widget(wmWidgetMap *wmap, bContext *C, wmWidget *widget, unsigned char part)
{
	if ((widget != wmap->highlighted_widget) || (widget && part != widget->highlighted_part)) {
		if (wmap->highlighted_widget) {
			wmap->highlighted_widget->flag &= ~WM_WIDGET_HIGHLIGHT;
			wmap->highlighted_widget->highlighted_part = 0;
		}

		wmap->highlighted_widget = widget;

		if (widget) {
			widget->flag |= WM_WIDGET_HIGHLIGHT;
			widget->highlighted_part = part;
			wmap->activegroup = widget->wgroup;

			if (C && widget->get_cursor) {
				wmWindow *win = CTX_wm_window(C);
				WM_cursor_set(win, widget->get_cursor(widget));
			}
		}
		else {
			wmap->activegroup = NULL;
			if (C) {
				wmWindow *win = CTX_wm_window(C);
				WM_cursor_set(win, CURSOR_STD);
			}
		}

		/* tag the region for redraw */
		if (C) {
			ARegion *ar = CTX_wm_region(C);
			ED_region_tag_redraw(ar);
		}
	}
}

wmWidget *wm_widgetmap_get_highlighted_widget(wmWidgetMap *wmap)
{
	return wmap->highlighted_widget;
}

void wm_widgetmap_set_active_widget(
        wmWidgetMap *wmap, bContext *C,
        const wmEvent *event, wmWidget *widget)
{
	if (widget) {
		if (widget->opname) {
			wmOperatorType *ot;

			ot = WM_operatortype_find(widget->opname, 0);

			if (ot) {
				/* first activate the widget itself */
				if (widget->invoke && widget->handler) {
					widget->flag |= WM_WIDGET_ACTIVE;
					widget->invoke(C, event, widget);
				}
				wmap->active_widget = widget;

				WM_operator_name_call_ptr(C, ot, WM_OP_INVOKE_DEFAULT, &widget->opptr);

				/* we failed to hook the widget to the operator handler or operator was cancelled, return */
				if (!wmap->active_widget) {
					widget->flag &= ~WM_WIDGET_ACTIVE;
					/* first activate the widget itself */
					if (widget->interaction_data) {
						MEM_freeN(widget->interaction_data);
						widget->interaction_data = NULL;
					}
				}
				return;
			}
			else {
				printf("Widget error: operator not found");
				wmap->active_widget = NULL;
				return;
			}
		}
		else {
			if (widget->invoke && widget->handler) {
				widget->flag |= WM_WIDGET_ACTIVE;
				widget->invoke(C, event, widget);
				wmap->active_widget = widget;
			}
		}
	}
	else {
		widget = wmap->active_widget;

		/* deactivate, widget but first take care of some stuff */
		if (widget) {
			widget->flag &= ~WM_WIDGET_ACTIVE;
			/* first activate the widget itself */
			if (widget->interaction_data) {
				MEM_freeN(widget->interaction_data);
				widget->interaction_data = NULL;
			}
		}
		wmap->active_widget = NULL;

		if (C) {
			ARegion *ar = CTX_wm_region(C);
			ED_region_tag_redraw(ar);
			WM_event_add_mousemove(C);
		}
	}
}

wmWidget *wm_widgetmap_get_selected_widget(wmWidgetMap *wmap)
{
	return wmap->selected_widget;
}

void wm_widgetmap_set_selected_widget(bContext *C, wmWidgetMap *wmap, wmWidget *widget)
{
	const int action = SEL_SELECT; /* TODO currently SEL_SELECT only */

	if (widget) {
		wmap->selected_widget = widget;
		widget->flag |= WM_WIDGET_SELECTED;
		if (widget->select) {
			widget->select(C, widget, action);
		}
		wm_widgetmap_set_highlighted_widget(wmap, C, NULL, wmap->highlighted_widget->highlighted_part);
	}
	else {
		widget = wmap->selected_widget;
		if (widget) {
			wmap->selected_widget = NULL;
			widget->flag &= ~WM_WIDGET_SELECTED;
		}
	}

	/* tag the region for redraw */
	if (C) {
		ARegion *ar = CTX_wm_region(C);
		ED_region_tag_redraw(ar);
	}
}

/**
 * Select/Deselect all selectable widgets in \a wmap
 *
 * \return if selection has changed.
 *
 * TODO currently only one selected widget supported
 */
bool WM_widgetmap_select_all(bContext *C, wmWidgetMap *wmap, const int action)
{
	bool changed = false;

	switch (action) {
		case SEL_SELECT:
			printf("Selecting multiple widgets is not supported yet!\n");
			break;
		case SEL_DESELECT:
			if (wmap->selected_widget) {
				changed = true;
			}
			wm_widgetmap_set_selected_widget(C, wmap, NULL);
			break;
		default:
			BLI_assert(0);
	}

	return changed;
}

void wm_widgetmap_handler_context(bContext *C, wmEventHandler *handler)
{
	bScreen *screen = CTX_wm_screen(C);

	if (screen) {
		if (handler->op_area == NULL) {
			/* do nothing in this context */
		}
		else {
			ScrArea *sa;

			for (sa = screen->areabase.first; sa; sa = sa->next)
				if (sa == handler->op_area)
					break;
			if (sa == NULL) {
				/* when changing screen layouts with running modal handlers (like render display), this
				 * is not an error to print */
				if (handler->widgetmap == NULL)
					printf("internal error: modal widgetmap handler has invalid area\n");
			}
			else {
				ARegion *ar;
				CTX_wm_area_set(C, sa);
				for (ar = sa->regionbase.first; ar; ar = ar->next)
					if (ar == handler->op_region)
						break;
				/* XXX no warning print here, after full-area and back regions are remade */
				if (ar)
					CTX_wm_region_set(C, ar);
			}
		}
	}
}

void wm_widget_handler_modal_update(bContext *C, wmEvent *event, wmEventHandler *handler)
{
	wmWidgetMap *wmap;

	/* happens on render */
	if (!handler->op_region)
		return;

	for (wmap = handler->op_region->widgetmaps.first; wmap; wmap = wmap->next) {
		wmWidget *widget = wm_widgetmap_get_active_widget(wmap);
		ScrArea *area = CTX_wm_area(C);
		ARegion *region = CTX_wm_region(C);

		if (!widget)
			continue;

		wm_widgetmap_handler_context(C, handler);

		/* regular update for running operator */
		if (handler->op) {
			if (widget && widget->handler) {
				widget->handler(C, event, widget);
			}
		}
		/* operator not running anymore */
		else {
			wm_widgetmap_set_active_widget(wmap, C, event, NULL);
		}

		/* restore the area */
		CTX_wm_area_set(C, area);
		CTX_wm_region_set(C, region);
	}
}

wmWidget *wm_widgetmap_get_active_widget(wmWidgetMap *wmap)
{
	return wmap->active_widget;
}


wmWidgetMap *WM_widgetmap_from_type(const char *idname, const int spaceid, const int regionid, const bool is_3d)
{
	wmWidgetMapType *wmaptype = WM_widgetmaptype_find(idname, spaceid, regionid, is_3d, true);
	wmWidgetGroupType *wgrouptype = wmaptype->widgetgrouptypes.first;
	wmWidgetMap *wmap;

	wmap = MEM_callocN(sizeof(wmWidgetMap), "WidgetMap");
	wmap->type = wmaptype;

	/* create all widgetgroups for this widgetmap. We may create an empty one
	 * too in anticipation of widgets from operators etc */
	for (; wgrouptype; wgrouptype = wgrouptype->next) {
		wmWidgetGroup *wgroup = MEM_callocN(sizeof(wmWidgetGroup), "widgetgroup");
		wgroup->type = wgrouptype;
		BLI_addtail(&wmap->widgetgroups, wgroup);
	}

	return wmap;
}

void WM_widgetmap_delete(wmWidgetMap *wmap)
{
	wmWidgetGroup *wgroup;

	if (!wmap)
		return;

	for (wgroup = wmap->widgetgroups.first; wgroup; wgroup = wgroup->next) {
		wmWidget *widget;

		for (widget = wgroup->widgets.first; widget;) {
			wmWidget *widget_next = widget->next;
			wm_widget_delete(&wgroup->widgets, widget);
			widget = widget_next;
		}
	}
	BLI_freelistN(&wmap->widgetgroups);

	MEM_freeN(wmap);
}

static void wm_widgetgroup_free(bContext *C, wmWidgetMap *wmap, wmWidgetGroup *wgroup)
{
	wmWidget *widget;

	for (widget = wgroup->widgets.first; widget;) {
		wmWidget *widget_next = widget->next;
		if (widget->flag & WM_WIDGET_HIGHLIGHT) {
			wm_widgetmap_set_highlighted_widget(wmap, C, NULL, 0);
		}
		if (widget->flag & WM_WIDGET_ACTIVE) {
			wm_widgetmap_set_active_widget(wmap, C, NULL, NULL);
		}
		wm_widget_delete(&wgroup->widgets, widget);
		widget = widget_next;
	}

#ifdef WITH_PYTHON
	if (wgroup->py_instance) {
		/* do this first in case there are any __del__ functions or
		 * similar that use properties */
		BPY_DECREF_RNA_INVALIDATE(wgroup->py_instance);
	}
#endif

	if (wgroup->reports && (wgroup->reports->flag & RPT_FREE)) {
		BKE_reports_clear(wgroup->reports);
		MEM_freeN(wgroup->reports);
	}

	BLI_remlink(&wmap->widgetgroups, wgroup);
	MEM_freeN(wgroup);
}

/**
 * Common default keymap for widget groups
 */
wmKeyMap *WM_widgetgroup_keymap_common(wmKeyConfig *config, const char *wgroupname)
{
	wmKeyMap *km = WM_keymap_find(config, wgroupname, 0, 0);
	wmKeyMapItem *kmi;

	kmi = WM_keymap_add_item(km, "WIDGETGROUP_OT_widget_set_active", ACTIONMOUSE, KM_PRESS, 0, 0);
	RNA_boolean_set(kmi->ptr, "deactivate", false);
	kmi = WM_keymap_add_item(km, "WIDGETGROUP_OT_widget_set_active", ACTIONMOUSE, KM_RELEASE, 0, 0);
	RNA_boolean_set(kmi->ptr, "deactivate", true);

	WM_keymap_add_item(km, "WIDGETGROUP_OT_widget_tweak", MOUSEMOVE, KM_ANY, KM_ANY, 0);

	WM_keymap_add_item(km, "WIDGETGROUP_OT_widget_tweak_cancel", RIGHTMOUSE, KM_PRESS, 0, 0);
	WM_keymap_add_item(km, "WIDGETGROUP_OT_widget_tweak_cancel", ESCKEY, KM_PRESS, 0, 0);

	WM_keymap_add_item(km, "WIDGETGROUP_OT_widget_set_select", SELECTMOUSE, KM_PRESS, 0, 0);

	return km;
}

void wm_widgetgrouptype_keymap_init(wmWidgetGroupType *wgrouptype, wmKeyConfig *keyconf)
{
	wgrouptype->keymap = wgrouptype->keymap_init(keyconf, wgrouptype->name);
}

void WM_widgetgrouptype_unregister(bContext *C, Main *bmain, wmWidgetGroupType *wgrouptype)
{
	bScreen *sc;
	wmWidgetMapType *wmaptype;

	for (sc = bmain->screen.first; sc; sc = sc->id.next) {
		ScrArea *sa;
		for (sa = sc->areabase.first; sa; sa = sa->next) {
			SpaceLink *sl;

			for (sl = sa->spacedata.first; sl; sl = sl->next) {
				ARegion *ar;
				ListBase *lb = (sl == sa->spacedata.first) ? &sa->regionbase : &sl->regionbase;

				for (ar = lb->first; ar; ar = ar->next) {
					wmWidgetMap *wmap;
					for (wmap = ar->widgetmaps.first; wmap; wmap = wmap->next) {
						wmWidgetGroup *wgroup, *wgroup_tmp;
						for (wgroup = wmap->widgetgroups.first; wgroup; wgroup = wgroup_tmp) {
							wgroup_tmp = wgroup->next;
							if (wgroup->type == wgrouptype) {
								wm_widgetgroup_free(C, wmap, wgroup);
								ED_region_tag_redraw(ar);
							}
						}
					}
				}
			}
		}
	}

	wmaptype = WM_widgetmaptype_find(wgrouptype->mapidname, wgrouptype->spaceid, wgrouptype->regionid, wgrouptype->is_3d, false);

	BLI_remlink(&wmaptype->widgetgrouptypes, wgrouptype);
	wgrouptype->prev = wgrouptype->next = NULL;

	MEM_freeN(wgrouptype);
}

