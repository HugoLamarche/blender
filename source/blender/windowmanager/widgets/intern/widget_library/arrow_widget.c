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
 * Contributor(s): Blender Foundation
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file blender/windowmanager/widgets/intern/widget_library/arrow_widget.c
 *  \ingroup wm
 *
 * \name Arrow Widget
 *
 * 3D Widget
 *
 * \brief Simple arrow widget which is dragged into a certain direction.
 * The arrow head can have varying shapes, e.g. cone, box, etc.
 */

#include "BIF_gl.h"

#include "BKE_context.h"

#include "BLI_math.h"

#include "DNA_view3d_types.h"

#include "ED_view3d.h"
#include "ED_screen.h"

#include "GPU_select.h"

#include "MEM_guardedalloc.h"

#include "RNA_access.h"

#include "WM_types.h"
#include "WM_api.h"

/* own includes */
#include "wm_widget_wmapi.h"
#include "wm_widget_intern.h"
#include "widget_geometry.h"
#include "widget_library_intern.h"


/* to use custom arrows exported to arrow_widget.c */
//#define WIDGET_USE_CUSTOM_ARROWS

#ifdef WIDGET_USE_CUSTOM_ARROWS
WidgetDrawInfo arrow_head_draw_info = {0};
#endif
WidgetDrawInfo cube_draw_info = {0};

/* ArrowWidget->flag */
enum {
	ARROW_UP_VECTOR_SET    = (1 << 0),
	ARROW_CUSTOM_RANGE_SET = (1 << 1),
};

typedef struct ArrowWidget {
	wmWidget widget;

	WidgetCommonData data;

	int style;
	int flag;

	float len;          /* arrow line length */
	float direction[3];
	float up[3];
	float aspect[2];    /* cone style only */
} ArrowWidget;


/* -------------------------------------------------------------------- */

static void widget_arrow_get_final_pos(wmWidget *widget, float r_pos[3])
{
	ArrowWidget *arrow = (ArrowWidget *)widget;

	mul_v3_v3fl(r_pos, arrow->direction, arrow->data.offset);
	add_v3_v3(r_pos, arrow->widget.origin);
}

static void arrow_draw_geom(const ArrowWidget *arrow, const bool select)
{
	if (arrow->style & WIDGET_ARROW_STYLE_CROSS) {
		glPushAttrib(GL_ENABLE_BIT);
		glDisable(GL_LIGHTING);
		glBegin(GL_LINES);
		glVertex2f(-1.0, 0.f);
		glVertex2f(1.0, 0.f);
		glVertex2f(0.f, -1.0);
		glVertex2f(0.f, 1.0);
		glEnd();

		glPopAttrib();
	}
	else if (arrow->style & WIDGET_ARROW_STYLE_CONE) {
		const float unitx = arrow->aspect[0];
		const float unity = arrow->aspect[1];
		const float vec[4][3] = {
			{-unitx, -unity, 0},
			{ unitx, -unity, 0},
			{ unitx,  unity, 0},
			{-unitx,  unity, 0},
		};

		glLineWidth(arrow->widget.line_width);
		glEnableClientState(GL_VERTEX_ARRAY);
		glVertexPointer(3, GL_FLOAT, 0, vec);
		glDrawArrays(GL_LINE_LOOP, 0, ARRAY_SIZE(vec));
		glDisableClientState(GL_VERTEX_ARRAY);
		glLineWidth(1.0);
	}
	else {
#ifdef WIDGET_USE_CUSTOM_ARROWS
		widget_draw_intern(&arrow_head_draw_info, select);
#else
		const float vec[2][3] = {
			{0.0f, 0.0f, 0.0f},
			{0.0f, 0.0f, arrow->len},
		};

		glLineWidth(arrow->widget.line_width);
		glEnableClientState(GL_VERTEX_ARRAY);
		glVertexPointer(3, GL_FLOAT, 0, vec);
		glDrawArrays(GL_LINE_STRIP, 0, ARRAY_SIZE(vec));
		glDisableClientState(GL_VERTEX_ARRAY);
		glLineWidth(1.0);


		/* *** draw arrow head *** */

		glPushMatrix();

		if (arrow->style & WIDGET_ARROW_STYLE_BOX) {
			const float size = 0.05f;

			/* translate to line end with some extra offset so box starts exactly where line ends */
			glTranslatef(0.0f, 0.0f, arrow->len + size);
			/* scale down to box size */
			glScalef(size, size, size);

			/* draw cube */
			widget_draw_intern(&cube_draw_info, select);
		}
		else {
			const float len = 0.25f;
			const float width = 0.06f;
			const bool use_lighting = select == false && ((U.widget_flag & V3D_SHADED_WIDGETS) != 0);

			/* translate to line end */
			glTranslatef(0.0f, 0.0f, arrow->len);

			if (use_lighting) {
				glShadeModel(GL_SMOOTH);
			}

			GLUquadricObj *qobj = gluNewQuadric();
			gluQuadricDrawStyle(qobj, GLU_FILL);
			gluQuadricOrientation(qobj, GLU_INSIDE);
			gluDisk(qobj, 0.0, width, 8, 1);
			gluQuadricOrientation(qobj, GLU_OUTSIDE);
			gluCylinder(qobj, width, 0.0, len, 8, 1);
			gluDeleteQuadric(qobj);

			if (use_lighting) {
				glShadeModel(GL_FLAT);
			}
		}

		glPopMatrix();
#endif
	}
}

static void arrow_draw_intern(ArrowWidget *arrow, const bool select, const bool highlight)
{
	const float up[3] = {0.0f, 0.0f, 1.0f};
	float rot[3][3];
	float mat[4][4];
	float final_pos[3];

	widget_arrow_get_final_pos(&arrow->widget, final_pos);

	if (arrow->flag & ARROW_UP_VECTOR_SET) {
		copy_v3_v3(rot[2], arrow->direction);
		copy_v3_v3(rot[1], arrow->up);
		cross_v3_v3v3(rot[0], arrow->up, arrow->direction);
	}
	else {
		rotation_between_vecs_to_mat3(rot, up, arrow->direction);
	}
	copy_m4_m3(mat, rot);
	copy_v3_v3(mat[3], final_pos);
	mul_mat3_m4_fl(mat, arrow->widget.scale);

	glPushMatrix();
	glMultMatrixf(mat);

	if (highlight && !(arrow->widget.flag & WM_WIDGET_DRAW_HOVER)) {
		glColor4fv(arrow->widget.col_hi);
	}
	else {
		glColor4fv(arrow->widget.col);
	}

	glEnable(GL_BLEND);
	glTranslate3fv(arrow->widget.offset);
	arrow_draw_geom(arrow, select);
	glDisable(GL_BLEND);

	glPopMatrix();

	if (arrow->widget.interaction_data) {
		WidgetInteraction *inter = arrow->widget.interaction_data;

		copy_m4_m3(mat, rot);
		copy_v3_v3(mat[3], inter->init_origin);
		mul_mat3_m4_fl(mat, inter->init_scale);

		glPushMatrix();
		glMultMatrixf(mat);

		glEnable(GL_BLEND);
		glColor4f(0.5f, 0.5f, 0.5f, 0.5f);
		glTranslate3fv(arrow->widget.offset);
		arrow_draw_geom(arrow, select);
		glDisable(GL_BLEND);

		glPopMatrix();
	}
}

static void widget_arrow_render_3d_intersect(const bContext *UNUSED(C), wmWidget *widget, int selectionbase)
{
	GPU_select_load_id(selectionbase);
	arrow_draw_intern((ArrowWidget *)widget, true, false);
}

static void widget_arrow_draw(const bContext *UNUSED(C), wmWidget *widget)
{
	arrow_draw_intern((ArrowWidget *)widget, false, (widget->flag & WM_WIDGET_HIGHLIGHT) != 0);
}

/**
 * Calculate arrow offset independent from prop min value,
 * meaning the range will not be offset by min value first.
 */
#define USE_ABS_HANDLE_RANGE

static int widget_arrow_handler(bContext *C, const wmEvent *event, wmWidget *widget, const int flag)
{
	ArrowWidget *arrow = (ArrowWidget *)widget;
	WidgetInteraction *inter = widget->interaction_data;
	ARegion *ar = CTX_wm_region(C);
	RegionView3D *rv3d = ar->regiondata;

	float orig_origin[4];
	float viewvec[3], tangent[3], plane[3];
	float offset[4];
	float m_diff[2];
	float dir_2d[2], dir2d_final[2];
	float facdir = 1.0f;
	bool use_vertical = false;


	copy_v3_v3(orig_origin, inter->init_origin);
	orig_origin[3] = 1.0f;
	add_v3_v3v3(offset, orig_origin, arrow->direction);
	offset[3] = 1.0f;

	/* calculate view vector */
	if (rv3d->is_persp) {
		sub_v3_v3v3(viewvec, orig_origin, rv3d->viewinv[3]);
	}
	else {
		copy_v3_v3(viewvec, rv3d->viewinv[2]);
	}
	normalize_v3(viewvec);

	/* first determine if view vector is really close to the direction. If it is, we use
	 * vertical movement to determine offset, just like transform system does */
	if (RAD2DEG(acos(dot_v3v3(viewvec, arrow->direction))) > 5.0f) {
		/* multiply to projection space */
		mul_m4_v4(rv3d->persmat, orig_origin);
		mul_v4_fl(orig_origin, 1.0f / orig_origin[3]);
		mul_m4_v4(rv3d->persmat, offset);
		mul_v4_fl(offset, 1.0f / offset[3]);

		sub_v2_v2v2(dir_2d, offset, orig_origin);
		dir_2d[0] *= ar->winx;
		dir_2d[1] *= ar->winy;
		normalize_v2(dir_2d);
	}
	else {
		dir_2d[0] = 0.0f;
		dir_2d[1] = 1.0f;
		use_vertical = true;
	}

	/* find mouse difference */
	m_diff[0] = event->mval[0] - inter->init_mval[0];
	m_diff[1] = event->mval[1] - inter->init_mval[1];

	/* project the displacement on the screen space arrow direction */
	project_v2_v2v2(dir2d_final, m_diff, dir_2d);

	float zfac = ED_view3d_calc_zfac(rv3d, orig_origin, NULL);
	ED_view3d_win_to_delta(ar, dir2d_final, offset, zfac);

	add_v3_v3v3(orig_origin, offset, inter->init_origin);

	/* calculate view vector for the new position */
	if (rv3d->is_persp) {
		sub_v3_v3v3(viewvec, orig_origin, rv3d->viewinv[3]);
	}
	else {
		copy_v3_v3(viewvec, rv3d->viewinv[2]);
	}

	normalize_v3(viewvec);
	if (!use_vertical) {
		float fac;
		/* now find a plane parallel to the view vector so we can intersect with the arrow direction */
		cross_v3_v3v3(tangent, viewvec, offset);
		cross_v3_v3v3(plane, tangent, viewvec);
		fac = dot_v3v3(plane, offset) / dot_v3v3(arrow->direction, plane);

		facdir = (fac < 0.0) ? -1.0 : 1.0;
		mul_v3_v3fl(offset, arrow->direction, fac);
	}
	else {
		facdir = (m_diff[1] < 0.0) ? -1.0 : 1.0;
	}


	WidgetCommonData *data = &arrow->data;
	const float ofs_new = facdir * len_v3(offset);
	const int slot = ARROW_SLOT_OFFSET_WORLD_SPACE;

	/* set the property for the operator and call its modal function */
	if (widget->props[slot]) {
		const bool constrained = arrow->style & WIDGET_ARROW_STYLE_CONSTRAINED;
		const bool inverted = arrow->style & WIDGET_ARROW_STYLE_INVERTED;
		const bool use_precision = flag & WM_WIDGET_TWEAK_PRECISE;
		float value = widget_value_from_offset(data, inter, ofs_new, constrained, inverted, use_precision);

		widget_property_value_set(C, widget, slot, value);
		/* get clamped value */
		value = widget_property_value_get(widget, slot);

		data->offset = widget_offset_from_value(data, value, constrained, inverted);
	}
	else {
		data->offset = ofs_new;
	}

	/* tag the region for redraw */
	ED_region_tag_redraw(ar);
	WM_event_add_mousemove(C);

	return OPERATOR_PASS_THROUGH;
}


static int widget_arrow_invoke(bContext *UNUSED(C), const wmEvent *event, wmWidget *widget)
{
	ArrowWidget *arrow = (ArrowWidget *)widget;
	WidgetInteraction *inter = MEM_callocN(sizeof(WidgetInteraction), __func__);
	PointerRNA ptr = widget->ptr[ARROW_SLOT_OFFSET_WORLD_SPACE];
	PropertyRNA *prop = widget->props[ARROW_SLOT_OFFSET_WORLD_SPACE];

	if (prop) {
		inter->init_value = RNA_property_float_get(&ptr, prop);
	}

	inter->init_offset = arrow->data.offset;

	inter->init_mval[0] = event->mval[0];
	inter->init_mval[1] = event->mval[1];

	inter->init_scale = widget->scale;

	widget_arrow_get_final_pos(widget, inter->init_origin);

	widget->interaction_data = inter;

	return OPERATOR_RUNNING_MODAL;
}

static void widget_arrow_bind_to_prop(wmWidget *widget, const int slot)
{
	ArrowWidget *arrow = (ArrowWidget *)widget;
	widget_property_bind(
	            widget, &arrow->data, slot,
	            arrow->style & WIDGET_ARROW_STYLE_CONSTRAINED,
	            arrow->style & WIDGET_ARROW_STYLE_INVERTED);
}

static void widget_arrow_exit(bContext *C, wmWidget *widget, const bool cancel)
{
	if (!cancel)
		return;

	widget_property_value_reset(C, widget, (WidgetInteraction *)widget->interaction_data, ARROW_SLOT_OFFSET_WORLD_SPACE);
}


/* -------------------------------------------------------------------- */
/** \name Arrow Widget API
 *
 * \{ */

wmWidget *WIDGET_arrow_new(wmWidgetGroup *wgroup, const char *name, const int style)
{
	int real_style = style;

#ifdef WIDGET_USE_CUSTOM_ARROWS
	if (!arrow_head_draw_info.init) {
		arrow_head_draw_info.nverts = _WIDGET_nverts_arrow,
		arrow_head_draw_info.ntris = _WIDGET_ntris_arrow,
		arrow_head_draw_info.verts = _WIDGET_verts_arrow,
		arrow_head_draw_info.normals = _WIDGET_normals_arrow,
		arrow_head_draw_info.indices = _WIDGET_indices_arrow,
		arrow_head_draw_info.init = true;
	}
#endif
	if (!cube_draw_info.init) {
		cube_draw_info.nverts = _WIDGET_nverts_cube,
		cube_draw_info.ntris = _WIDGET_ntris_cube,
		cube_draw_info.verts = _WIDGET_verts_cube,
		cube_draw_info.normals = _WIDGET_normals_cube,
		cube_draw_info.indices = _WIDGET_indices_cube,
		cube_draw_info.init = true;
	}

	/* inverted only makes sense in a constrained arrow */
	if (real_style & WIDGET_ARROW_STYLE_INVERTED) {
		real_style |= WIDGET_ARROW_STYLE_CONSTRAINED;
	}


	ArrowWidget *arrow = MEM_callocN(sizeof(ArrowWidget), name);
	const float dir_default[3] = {0.0f, 0.0f, 1.0f};

	arrow->widget.draw = widget_arrow_draw;
	arrow->widget.get_final_position = widget_arrow_get_final_pos;
	arrow->widget.intersect = NULL;
	arrow->widget.handler = widget_arrow_handler;
	arrow->widget.invoke = widget_arrow_invoke;
	arrow->widget.render_3d_intersection = widget_arrow_render_3d_intersect;
	arrow->widget.bind_to_prop = widget_arrow_bind_to_prop;
	arrow->widget.exit = widget_arrow_exit;
	arrow->widget.flag |= (WM_WIDGET_SCALE_3D | WM_WIDGET_DRAW_ACTIVE);

	arrow->style = real_style;
	arrow->len = 1.0f;
	arrow->data.range_fac = 1.0f;
	copy_v3_v3(arrow->direction, dir_default);

	wm_widget_register(wgroup, &arrow->widget, name);

	return (wmWidget *)arrow;
}

/**
 * Define direction the arrow will point towards
 */
void WIDGET_arrow_set_direction(wmWidget *widget, const float direction[3])
{
	ArrowWidget *arrow = (ArrowWidget *)widget;

	copy_v3_v3(arrow->direction, direction);
	normalize_v3(arrow->direction);
}

/**
 * Define up-direction of the arrow widget
 */
void WIDGET_arrow_set_up_vector(wmWidget *widget, const float direction[3])
{
	ArrowWidget *arrow = (ArrowWidget *)widget;

	if (direction) {
		copy_v3_v3(arrow->up, direction);
		normalize_v3(arrow->up);
		arrow->flag |= ARROW_UP_VECTOR_SET;
	}
	else {
		arrow->flag &= ~ARROW_UP_VECTOR_SET;
	}
}

/**
 * Define a custom arrow line length
 */
void WIDGET_arrow_set_line_len(wmWidget *widget, const float len)
{
	ArrowWidget *arrow = (ArrowWidget *)widget;
	arrow->len = len;
}

/**
 * Define a custom property UI range
 *
 * \note Needs to be called before WM_widget_set_property!
 */
void WIDGET_arrow_set_ui_range(wmWidget *widget, const float min, const float max)
{
	ArrowWidget *arrow = (ArrowWidget *)widget;

	BLI_assert(min < max);
	BLI_assert(!(arrow->widget.props[0] && "Make sure this function is called before WM_widget_set_property"));

	arrow->data.range = max - min;
	arrow->data.min = min;
	arrow->data.flag |= WIDGET_CUSTOM_RANGE_SET;
}

/**
 * Define a custom factor for arrow min/max distance
 *
 * \note Needs to be called before WM_widget_set_property!
 */
void WIDGET_arrow_set_range_fac(wmWidget *widget, const float range_fac)
{
	ArrowWidget *arrow = (ArrowWidget *)widget;

	BLI_assert(!(arrow->widget.props[0] && "Make sure this function is called before WM_widget_set_property"));

	arrow->data.range_fac = range_fac;
}

/**
 * Define xy-aspect for arrow cone
 */
void WIDGET_arrow_cone_set_aspect(wmWidget *widget, const float aspect[2])
{
	ArrowWidget *arrow = (ArrowWidget *)widget;

	copy_v2_v2(arrow->aspect, aspect);
}

/** \} */ /* Arrow Widget API */


/* -------------------------------------------------------------------- */

void fix_linking_widget_arrow(void)
{
	(void)0;
}