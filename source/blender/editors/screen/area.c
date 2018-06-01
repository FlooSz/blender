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
 * The Original Code is Copyright (C) 2008 Blender Foundation.
 * All rights reserved.
 *
 *
 * Contributor(s): Blender Foundation
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file blender/editors/screen/area.c
 *  \ingroup edscr
 */


#include <string.h>
#include <stdio.h>

#include "MEM_guardedalloc.h"

#include "DNA_userdef_types.h"

#include "BLI_blenlib.h"
#include "BLI_math.h"
#include "BLI_utildefines.h"
#include "BLI_linklist_stack.h"

#include "BKE_context.h"
#include "BKE_global.h"
#include "BKE_screen.h"
#include "BKE_workspace.h"

#include "RNA_access.h"
#include "RNA_types.h"

#include "WM_api.h"
#include "WM_types.h"
#include "WM_message.h"
#include "WM_toolsystem.h"

#include "ED_screen.h"
#include "ED_screen_types.h"
#include "ED_space_api.h"

#include "GPU_immediate.h"
#include "GPU_immediate_util.h"
#include "GPU_matrix.h"
#include "GPU_draw.h"

#include "BLF_api.h"

#include "IMB_imbuf.h"
#include "IMB_imbuf_types.h"
#include "IMB_metadata.h"

#include "UI_interface.h"
#include "UI_interface_icons.h"
#include "UI_resources.h"
#include "UI_view2d.h"

#include "screen_intern.h"

enum RegionEmbossSide {
	REGION_EMBOSS_LEFT   = (1 << 0),
	REGION_EMBOSS_TOP    = (1 << 1),
	REGION_EMBOSS_BOTTOM = (1 << 2),
	REGION_EMBOSS_RIGHT  = (1 << 3),
	REGION_EMBOSS_ALL    = REGION_EMBOSS_LEFT | REGION_EMBOSS_TOP | REGION_EMBOSS_RIGHT | REGION_EMBOSS_BOTTOM,
};

/* general area and region code */

static void region_draw_emboss(const ARegion *ar, const rcti *scirct, int sides)
{
	rcti rect;

	/* translate scissor rect to region space */
	rect.xmin = scirct->xmin - ar->winrct.xmin;
	rect.ymin = scirct->ymin - ar->winrct.ymin;
	rect.xmax = scirct->xmax - ar->winrct.xmin;
	rect.ymax = scirct->ymax - ar->winrct.ymin;

	/* set transp line */
	glEnable(GL_BLEND);
	glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

	float color[4] = {0.0f, 0.0f, 0.0f, 0.25f};
	UI_GetThemeColor3fv(TH_EDITOR_OUTLINE, color);

	Gwn_VertFormat *format = immVertexFormat();
	unsigned int pos = GWN_vertformat_attr_add(format, "pos", GWN_COMP_F32, 2, GWN_FETCH_FLOAT);
	immBindBuiltinProgram(GPU_SHADER_2D_UNIFORM_COLOR);
	immUniformColor4fv(color);

	immBeginAtMost(GWN_PRIM_LINES, 8);

	/* right */
	if (sides & REGION_EMBOSS_RIGHT) {
		immVertex2f(pos, rect.xmax, rect.ymax);
		immVertex2f(pos, rect.xmax, rect.ymin);
	}

	/* bottom */
	if (sides & REGION_EMBOSS_BOTTOM) {
		immVertex2f(pos, rect.xmax, rect.ymin);
		immVertex2f(pos, rect.xmin, rect.ymin);
	}

	/* left */
	if (sides & REGION_EMBOSS_LEFT) {
		immVertex2f(pos, rect.xmin, rect.ymin);
		immVertex2f(pos, rect.xmin, rect.ymax);
	}

	/* top */
	if (sides & REGION_EMBOSS_TOP) {
		immVertex2f(pos, rect.xmin, rect.ymax);
		immVertex2f(pos, rect.xmax, rect.ymax);
	}

	immEnd();
	immUnbindProgram();

	glDisable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}

void ED_region_pixelspace(ARegion *ar)
{
	wmOrtho2_region_pixelspace(ar);
	gpuLoadIdentity();
}

/* only exported for WM */
void ED_region_do_listen(bScreen *sc, ScrArea *sa, ARegion *ar, wmNotifier *note, const Scene *scene)
{
	/* generic notes first */
	switch (note->category) {
		case NC_WM:
			if (note->data == ND_FILEREAD)
				ED_region_tag_redraw(ar);
			break;
		case NC_WINDOW:
			ED_region_tag_redraw(ar);
			break;
	}

	if (ar->type && ar->type->listener)
		ar->type->listener(sc, sa, ar, note, scene);
}

/* only exported for WM */
void ED_area_do_listen(bScreen *sc, ScrArea *sa, wmNotifier *note, Scene *scene, WorkSpace *workspace)
{
	/* no generic notes? */
	if (sa->type && sa->type->listener) {
		sa->type->listener(sc, sa, note, scene, workspace);
	}
}

/* only exported for WM */
void ED_area_do_refresh(bContext *C, ScrArea *sa)
{
	/* no generic notes? */
	if (sa->type && sa->type->refresh) {
		sa->type->refresh(C, sa);
	}
	sa->do_refresh = false;
}

/**
 * Action zones are only updated if the mouse is inside of them, but in some cases (currently only fullscreen icon)
 * it might be needed to update their properties and redraw if the mouse isn't inside.
 */
void ED_area_azones_update(ScrArea *sa, const int mouse_xy[2])
{
	AZone *az;
	bool changed = false;

	for (az = sa->actionzones.first; az; az = az->next) {
		if (az->type == AZONE_FULLSCREEN) {
			/* only if mouse is not hovering the azone */
			if (BLI_rcti_isect_pt_v(&az->rect, mouse_xy) == false) {
				az->alpha = 0.0f;
				changed = true;

				/* can break since currently only this is handled here */
				break;
			}
		}
		else if (az->type == AZONE_REGION_SCROLL) {
			/* only if mouse is not hovering the azone */
			if (BLI_rcti_isect_pt_v(&az->rect, mouse_xy) == false) {
				View2D *v2d = &az->ar->v2d;

				if (az->direction == AZ_SCROLL_VERT) {
					az->alpha = v2d->alpha_vert = 0;
					changed = true;
				}
				else if (az->direction == AZ_SCROLL_HOR) {
					az->alpha = v2d->alpha_hor = 0;
					changed = true;
				}
				else {
					BLI_assert(0);
				}
			}
		}
	}

	if (changed) {
		sa->flag &= ~AREA_FLAG_ACTIONZONES_UPDATE;
		ED_area_tag_redraw_no_rebuild(sa);
	}
}

/**
 * \brief Corner widget use for quitting fullscreen.
 */
static void area_draw_azone_fullscreen(short x1, short y1, short x2, short y2, float alpha)
{
	int x = x2 - ((float) x2 - x1) * 0.5f / UI_DPI_FAC;
	int y = y2 - ((float) y2 - y1) * 0.5f / UI_DPI_FAC;

	/* adjust the icon distance from the corner */
	x += 36.0f / UI_DPI_FAC;
	y += 36.0f / UI_DPI_FAC;

	/* draws from the left bottom corner of the icon */
	x -= UI_DPI_ICON_SIZE;
	y -= UI_DPI_ICON_SIZE;

	alpha = min_ff(alpha, 0.75f);

	UI_icon_draw_aspect(x, y, ICON_FULLSCREEN_EXIT, 0.7f / UI_DPI_FAC, alpha);

	/* debug drawing :
	 * The click_rect is the same as defined in fullscreen_click_rcti_init
	 * Keep them both in sync */

	if (G.debug_value == 1) {
		rcti click_rect;
		float icon_size = UI_DPI_ICON_SIZE + 7 * UI_DPI_FAC;

		BLI_rcti_init(&click_rect, x, x + icon_size, y, y + icon_size);

		Gwn_VertFormat *format = immVertexFormat();
		unsigned int pos = GWN_vertformat_attr_add(format, "pos", GWN_COMP_F32, 2, GWN_FETCH_FLOAT);

		immBindBuiltinProgram(GPU_SHADER_2D_UNIFORM_COLOR);

		immUniformColor4f(1.0f, 0.0f, 0.0f, alpha);
		imm_draw_box_wire_2d(pos, click_rect.xmin, click_rect.ymin, click_rect.xmax, click_rect.ymax);

		immUniformColor4f(0.0f, 1.0f, 1.0f, alpha);
		immBegin(GWN_PRIM_LINES, 4);
		immVertex2f(pos, click_rect.xmin, click_rect.ymin);
		immVertex2f(pos, click_rect.xmax, click_rect.ymax);
		immVertex2f(pos, click_rect.xmin, click_rect.ymax);
		immVertex2f(pos, click_rect.xmax, click_rect.ymin);
		immEnd();

		immUnbindProgram();
	}
}

/**
 * \brief Corner widgets use for dragging and splitting the view.
 */
static void area_draw_azone(short UNUSED(x1), short UNUSED(y1), short UNUSED(x2), short UNUSED(y2))
{
	/* No drawing needed since all corners are action zone, and visually distinguishable. */
}

static void draw_azone_plus(float x1, float y1, float x2, float y2)
{
	float width = 0.1f * U.widget_unit;
	float pad = 0.2f * U.widget_unit;

	Gwn_VertFormat *format = immVertexFormat();
	unsigned int pos = GWN_vertformat_attr_add(format, "pos", GWN_COMP_F32, 2, GWN_FETCH_FLOAT);

	glEnable(GL_BLEND);
	immBindBuiltinProgram(GPU_SHADER_2D_UNIFORM_COLOR);
	immUniformColor4f(0.8f, 0.8f, 0.8f, 0.4f);

	immRectf(pos, (x1 + x2 - width) * 0.5f, y1 + pad, (x1 + x2 + width) * 0.5f, y2 - pad);
	immRectf(pos, x1 + pad, (y1 + y2 - width) * 0.5f, (x1 + x2 - width) * 0.5f, (y1 + y2 + width) * 0.5f);
	immRectf(pos, (x1 + x2 + width) * 0.5f, (y1 + y2 - width) * 0.5f, x2 - pad, (y1 + y2 + width) * 0.5f);

	immUnbindProgram();
	glDisable(GL_BLEND);
}

static void region_draw_azone_tab_plus(AZone *az)
{
	glEnable(GL_BLEND);
	
	/* add code to draw region hidden as 'too small' */
	switch (az->edge) {
		case AE_TOP_TO_BOTTOMRIGHT:
			UI_draw_roundbox_corner_set(UI_CNR_TOP_LEFT | UI_CNR_TOP_RIGHT);
			break;
		case AE_BOTTOM_TO_TOPLEFT:
			UI_draw_roundbox_corner_set(UI_CNR_BOTTOM_RIGHT | UI_CNR_BOTTOM_LEFT);
			break;
		case AE_LEFT_TO_TOPRIGHT:
			UI_draw_roundbox_corner_set(UI_CNR_TOP_LEFT | UI_CNR_BOTTOM_LEFT);
			break;
		case AE_RIGHT_TO_TOPLEFT:
			UI_draw_roundbox_corner_set(UI_CNR_TOP_RIGHT | UI_CNR_BOTTOM_RIGHT);
			break;
	}

	float color[4] = {0.05f, 0.05f, 0.05f, 0.4f};
	UI_draw_roundbox_aa(true, (float)az->x1, (float)az->y1, (float)az->x2, (float)az->y2, 4.0f, color);

	draw_azone_plus((float)az->x1, (float)az->y1, (float)az->x2, (float)az->y2);
}

static void area_azone_tag_update(ScrArea *sa)
{
	sa->flag |= AREA_FLAG_ACTIONZONES_UPDATE;
}

static void region_draw_azones(ScrArea *sa, ARegion *ar)
{
	AZone *az;

	if (!sa)
		return;

	glLineWidth(1.0f);
	glEnable(GL_BLEND);
	glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

	gpuPushMatrix();
	gpuTranslate2f(-ar->winrct.xmin, -ar->winrct.ymin);
	
	for (az = sa->actionzones.first; az; az = az->next) {
		/* test if action zone is over this region */
		rcti azrct;
		BLI_rcti_init(&azrct, az->x1, az->x2, az->y1, az->y2);

		if (BLI_rcti_isect(&ar->drawrct, &azrct, NULL)) {
			if (az->type == AZONE_AREA) {
				area_draw_azone(az->x1, az->y1, az->x2, az->y2);
			}
			else if (az->type == AZONE_REGION) {
				if (az->ar) {
					/* only display tab or icons when the region is hidden */
					if (az->ar->flag & (RGN_FLAG_HIDDEN | RGN_FLAG_TOO_SMALL)) {
						region_draw_azone_tab_plus(az);
					}
				}
			}
			else if (az->type == AZONE_FULLSCREEN) {
				area_draw_azone_fullscreen(az->x1, az->y1, az->x2, az->y2, az->alpha);

				if (az->alpha != 0.0f) {
					area_azone_tag_update(sa);
				}
			}
			else if (az->type == AZONE_REGION_SCROLL) {
				if (az->alpha != 0.0f) {
					area_azone_tag_update(sa);
				}
				/* Don't draw this azone. */
			}
		}
	}

	gpuPopMatrix();

	glDisable(GL_BLEND);
}

/* Follow wmMsgNotifyFn spec */
void ED_region_do_msg_notify_tag_redraw(
        bContext *UNUSED(C), wmMsgSubscribeKey *UNUSED(msg_key), wmMsgSubscribeValue *msg_val)
{
	ARegion *ar = msg_val->owner;
	ED_region_tag_redraw(ar);

	/* This avoids _many_ situations where header/properties control display settings.
	 * the common case is space properties in the header */
	if (ELEM(ar->regiontype, RGN_TYPE_HEADER, RGN_TYPE_UI)) {
		while (ar && ar->prev) {
			ar = ar->prev;
		}
		for (; ar; ar = ar->next) {
			if (ELEM(ar->regiontype, RGN_TYPE_WINDOW, RGN_TYPE_CHANNELS)) {
				ED_region_tag_redraw(ar);
			}
		}
	}
}
/* Follow wmMsgNotifyFn spec */
void ED_area_do_msg_notify_tag_refresh(
        bContext *UNUSED(C), wmMsgSubscribeKey *UNUSED(msg_key), wmMsgSubscribeValue *msg_val)
{
	ScrArea *sa = msg_val->user_data;
	ED_area_tag_refresh(sa);
}

/* only exported for WM */
void ED_region_do_layout(bContext *C, ARegion *ar)
{
	/* This is optional, only needed for dynamically sized regions. */
	ScrArea *sa = CTX_wm_area(C);
	ARegionType *at = ar->type;

	if (!at->layout) {
		return;
	}

	if (at->do_lock) {
		return;
	}

	ar->do_draw |= RGN_DRAWING;

	UI_SetTheme(sa ? sa->spacetype : 0, at->regionid);
	at->layout(C, ar);
}

/* only exported for WM */
void ED_region_do_draw(bContext *C, ARegion *ar)
{
	wmWindow *win = CTX_wm_window(C);
	ScrArea *sa = CTX_wm_area(C);
	ARegionType *at = ar->type;

	/* see BKE_spacedata_draw_locks() */
	if (at->do_lock)
		return;

	ar->do_draw |= RGN_DRAWING;
	
	/* Set viewport, scissor, ortho and ar->drawrct. */
	wmPartialViewport(&ar->drawrct, &ar->winrct, &ar->drawrct);

	wmOrtho2_region_pixelspace(ar);
	
	UI_SetTheme(sa ? sa->spacetype : 0, at->regionid);
	
	/* optional header info instead? */
	if (ar->headerstr) {
		UI_ThemeClearColor(TH_HEADER);
		glClear(GL_COLOR_BUFFER_BIT);
		
		UI_FontThemeColor(BLF_default(), TH_TEXT);
		BLF_draw_default(UI_UNIT_X, 0.4f * UI_UNIT_Y, 0.0f, ar->headerstr, BLF_DRAW_STR_DUMMY_MAX);
	}
	else if (at->draw) {
		at->draw(C, ar);
	}

	/* XXX test: add convention to end regions always in pixel space, for drawing of borders/gestures etc */
	ED_region_pixelspace(ar);

	ED_region_draw_cb_draw(C, ar, REGION_DRAW_POST_PIXEL);

	region_draw_azones(sa, ar);

	/* for debugging unneeded area redraws and partial redraw */
#if 0
	glEnable(GL_BLEND);
	Gwn_VertFormat *format = immVertexFormat();
	unsigned int pos = GWN_vertformat_attr_add(format, "pos", GWN_COMP_F32, 2, GWN_FETCH_FLOAT);
	immBindBuiltinProgram(GPU_SHADER_2D_UNIFORM_COLOR);
	immUniformColor4f(drand48(), drand48(), drand48(), 0.1f);
	immRectf(pos, ar->drawrct.xmin - ar->winrct.xmin, ar->drawrct.ymin - ar->winrct.ymin,
	        ar->drawrct.xmax - ar->winrct.xmin, ar->drawrct.ymax - ar->winrct.ymin);
	immUnbindProgram();
	glDisable(GL_BLEND);
#endif

	memset(&ar->drawrct, 0, sizeof(ar->drawrct));
	
	UI_blocklist_free_inactive(C, &ar->uiblocks);

	if (sa) {
		const bScreen *screen = WM_window_get_active_screen(win);

		/* Only draw region emboss for top-bar and quad-view. */
		if ((screen->state != SCREENFULL) && ED_area_is_global(sa)) {
			region_draw_emboss(ar, &ar->winrct, (REGION_EMBOSS_LEFT | REGION_EMBOSS_RIGHT));
		}
		else if ((ar->regiontype == RGN_TYPE_WINDOW) && (ar->alignment == RGN_ALIGN_QSPLIT)) {
			region_draw_emboss(ar, &ar->winrct, REGION_EMBOSS_ALL);
		}
	}

	/* We may want to detach message-subscriptions from drawing. */
	{
		WorkSpace *workspace = CTX_wm_workspace(C);
		wmWindowManager *wm = CTX_wm_manager(C);
		bScreen *screen = WM_window_get_active_screen(win);
		Scene *scene = CTX_data_scene(C);
		struct wmMsgBus *mbus = wm->message_bus;
		WM_msgbus_clear_by_owner(mbus, ar);

		/* Cheat, always subscribe to this space type properties.
		 *
		 * This covers most cases and avoids copy-paste similar code for each space type.
		 */
		if (ELEM(ar->regiontype, RGN_TYPE_WINDOW, RGN_TYPE_CHANNELS, RGN_TYPE_UI, RGN_TYPE_TOOLS)) {
			SpaceLink *sl = sa->spacedata.first;

			PointerRNA ptr;
			RNA_pointer_create(&screen->id, &RNA_Space, sl, &ptr);

			wmMsgSubscribeValue msg_sub_value_region_tag_redraw = {
				.owner = ar,
				.user_data = ar,
				.notify = ED_region_do_msg_notify_tag_redraw,
			};
			/* All properties for this space type. */
			WM_msg_subscribe_rna(mbus, &ptr, NULL, &msg_sub_value_region_tag_redraw, __func__);
		}

		ED_region_message_subscribe(C, workspace, scene, screen, sa, ar, mbus);
	}
}

/* **********************************
 * maybe silly, but let's try for now
 * to keep these tags protected
 * ********************************** */

void ED_region_tag_redraw(ARegion *ar)
{
	/* don't tag redraw while drawing, it shouldn't happen normally
	 * but python scripts can cause this to happen indirectly */
	if (ar && !(ar->do_draw & RGN_DRAWING)) {
		/* zero region means full region redraw */
		ar->do_draw &= ~(RGN_DRAW_PARTIAL | RGN_DRAW_NO_REBUILD);
		ar->do_draw |= RGN_DRAW;
		memset(&ar->drawrct, 0, sizeof(ar->drawrct));
	}
}

void ED_region_tag_redraw_overlay(ARegion *ar)
{
	if (ar)
		ar->do_draw_overlay = RGN_DRAW;
}

void ED_region_tag_redraw_no_rebuild(ARegion *ar)
{
	if (ar && !(ar->do_draw & (RGN_DRAWING | RGN_DRAW))) {
		ar->do_draw &= ~RGN_DRAW_PARTIAL;
		ar->do_draw |= RGN_DRAW_NO_REBUILD;
		memset(&ar->drawrct, 0, sizeof(ar->drawrct));
	}
}

void ED_region_tag_refresh_ui(ARegion *ar)
{
	if (ar) {
		ar->do_draw |= RGN_DRAW_REFRESH_UI;
	}
}

void ED_region_tag_redraw_partial(ARegion *ar, const rcti *rct)
{
	if (ar && !(ar->do_draw & RGN_DRAWING)) {
		if (!(ar->do_draw & (RGN_DRAW | RGN_DRAW_NO_REBUILD | RGN_DRAW_PARTIAL))) {
			/* no redraw set yet, set partial region */
			ar->do_draw |= RGN_DRAW_PARTIAL;
			ar->drawrct = *rct;
		}
		else if (ar->drawrct.xmin != ar->drawrct.xmax) {
			BLI_assert((ar->do_draw & RGN_DRAW_PARTIAL) != 0);
			/* partial redraw already set, expand region */
			BLI_rcti_union(&ar->drawrct, rct);
		}
		else {
			BLI_assert((ar->do_draw & (RGN_DRAW | RGN_DRAW_NO_REBUILD)) != 0);
			/* Else, full redraw is already requested, nothing to do here. */
		}
	}
}

void ED_area_tag_redraw(ScrArea *sa)
{
	ARegion *ar;
	
	if (sa)
		for (ar = sa->regionbase.first; ar; ar = ar->next)
			ED_region_tag_redraw(ar);
}

void ED_area_tag_redraw_no_rebuild(ScrArea *sa)
{
	ARegion *ar;

	if (sa)
		for (ar = sa->regionbase.first; ar; ar = ar->next)
			ED_region_tag_redraw_no_rebuild(ar);
}

void ED_area_tag_redraw_regiontype(ScrArea *sa, int regiontype)
{
	ARegion *ar;
	
	if (sa) {
		for (ar = sa->regionbase.first; ar; ar = ar->next) {
			if (ar->regiontype == regiontype) {
				ED_region_tag_redraw(ar);
			}
		}
	}
}

void ED_area_tag_refresh(ScrArea *sa)
{
	if (sa)
		sa->do_refresh = true;
}

/* *************************************************************** */

/* use NULL to disable it */
void ED_area_headerprint(ScrArea *sa, const char *str)
{
	ARegion *ar;

	/* happens when running transform operators in backround mode */
	if (sa == NULL)
		return;

	for (ar = sa->regionbase.first; ar; ar = ar->next) {
		if (ar->regiontype == RGN_TYPE_HEADER) {
			if (str) {
				if (ar->headerstr == NULL)
					ar->headerstr = MEM_mallocN(UI_MAX_DRAW_STR, "headerprint");
				BLI_strncpy(ar->headerstr, str, UI_MAX_DRAW_STR);
			}
			else if (ar->headerstr) {
				MEM_freeN(ar->headerstr);
				ar->headerstr = NULL;
			}
			ED_region_tag_redraw(ar);
		}
	}
}

/* ************************************************************ */


static void area_azone_initialize(wmWindow *win, const bScreen *screen, ScrArea *sa)
{
	AZone *az;
	
	/* reinitalize entirely, regions and fullscreen add azones too */
	BLI_freelistN(&sa->actionzones);

	if (screen->state != SCREENNORMAL) {
		return;
	}

	if (U.app_flag & USER_APP_LOCK_UI_LAYOUT) {
		return;
	}

	if (ED_area_is_global(sa)) {
		return;
	}

	float coords[4][4] = {
	    /* Bottom-left. */
	    {sa->totrct.xmin,
	     sa->totrct.ymin,
	     sa->totrct.xmin + (AZONESPOT - 1),
	     sa->totrct.ymin + (AZONESPOT - 1)},
	    /* Bottom-right. */
	    {sa->totrct.xmax,
	     sa->totrct.ymin,
	     sa->totrct.xmax - (AZONESPOT - 1),
	     sa->totrct.ymin + (AZONESPOT - 1)},
	    /* Top-left. */
	    {sa->totrct.xmin,
	     sa->totrct.ymax,
	     sa->totrct.xmin + (AZONESPOT - 1),
	     sa->totrct.ymax - (AZONESPOT - 1)},
	    /* Top-right. */
	    {sa->totrct.xmax,
	     sa->totrct.ymax,
	     sa->totrct.xmax - (AZONESPOT - 1),
	     sa->totrct.ymax - (AZONESPOT - 1)}};

	for (int i = 0; i < 4; i++) {
		/* can't click on bottom corners on OS X, already used for resizing */
#ifdef __APPLE__
		if (!WM_window_is_fullscreen(win) &&
		    ((coords[i][0] == 0 && coords[i][1] == 0) ||
		     (coords[i][0] == WM_window_pixels_x(win) && coords[i][1] == 0)))
		{
			continue;
		}
#else
		(void)win;
#endif

		/* set area action zones */
		az = (AZone *)MEM_callocN(sizeof(AZone), "actionzone");
		BLI_addtail(&(sa->actionzones), az);
		az->type = AZONE_AREA;
		az->x1 = coords[i][0];
		az->y1 = coords[i][1];
		az->x2 = coords[i][2];
		az->y2 = coords[i][3];
		BLI_rcti_init(&az->rect, az->x1, az->x2, az->y1, az->y2);
	}
}

static void fullscreen_azone_initialize(ScrArea *sa, ARegion *ar)
{
	AZone *az;

	if (ED_area_is_global(sa) || (ar->regiontype != RGN_TYPE_WINDOW))
		return;

	az = (AZone *)MEM_callocN(sizeof(AZone), "fullscreen action zone");
	BLI_addtail(&(sa->actionzones), az);
	az->type = AZONE_FULLSCREEN;
	az->ar = ar;
	az->alpha = 0.0f;

	az->x1 = ar->winrct.xmax - (AZONEFADEOUT - 1);
	az->y1 = ar->winrct.ymax - (AZONEFADEOUT - 1);
	az->x2 = ar->winrct.xmax;
	az->y2 = ar->winrct.ymax;
	BLI_rcti_init(&az->rect, az->x1, az->x2, az->y1, az->y2);
}

#define AZONEPAD_EDGE   (0.1f * U.widget_unit)
#define AZONEPAD_ICON   (0.45f * U.widget_unit)
static void region_azone_edge(AZone *az, ARegion *ar)
{
	switch (az->edge) {
		case AE_TOP_TO_BOTTOMRIGHT:
			az->x1 = ar->winrct.xmin;
			az->y1 = ar->winrct.ymax - AZONEPAD_EDGE;
			az->x2 = ar->winrct.xmax;
			az->y2 = ar->winrct.ymax + AZONEPAD_EDGE;
			break;
		case AE_BOTTOM_TO_TOPLEFT:
			az->x1 = ar->winrct.xmin;
			az->y1 = ar->winrct.ymin + AZONEPAD_EDGE;
			az->x2 = ar->winrct.xmax;
			az->y2 = ar->winrct.ymin - AZONEPAD_EDGE;
			break;
		case AE_LEFT_TO_TOPRIGHT:
			az->x1 = ar->winrct.xmin - AZONEPAD_EDGE;
			az->y1 = ar->winrct.ymin;
			az->x2 = ar->winrct.xmin + AZONEPAD_EDGE;
			az->y2 = ar->winrct.ymax;
			break;
		case AE_RIGHT_TO_TOPLEFT:
			az->x1 = ar->winrct.xmax + AZONEPAD_EDGE;
			az->y1 = ar->winrct.ymin;
			az->x2 = ar->winrct.xmax - AZONEPAD_EDGE;
			az->y2 = ar->winrct.ymax;
			break;
	}

	BLI_rcti_init(&az->rect, az->x1, az->x2, az->y1, az->y2);
}

#define AZONEPAD_TAB_PLUSW  (0.7f * U.widget_unit)
#define AZONEPAD_TAB_PLUSH  (0.7f * U.widget_unit)

/* region already made zero sized, in shape of edge */
static void region_azone_tab_plus(ScrArea *sa, AZone *az, ARegion *ar)
{
	AZone *azt;
	int tot = 0, add;
	
	for (azt = sa->actionzones.first; azt; azt = azt->next) {
		if (azt->edge == az->edge) tot++;
	}
	
	switch (az->edge) {
		case AE_TOP_TO_BOTTOMRIGHT:
			add = (ar->winrct.ymax == sa->totrct.ymin) ? 1 : 0;
			az->x1 = ar->winrct.xmax - 2.5f * AZONEPAD_TAB_PLUSW;
			az->y1 = ar->winrct.ymax - add;
			az->x2 = ar->winrct.xmax - 1.5f * AZONEPAD_TAB_PLUSW;
			az->y2 = ar->winrct.ymax - add + AZONEPAD_TAB_PLUSH;
			break;
		case AE_BOTTOM_TO_TOPLEFT:
			az->x1 = ar->winrct.xmax - 2.5f * AZONEPAD_TAB_PLUSW;
			az->y1 = ar->winrct.ymin - AZONEPAD_TAB_PLUSH;
			az->x2 = ar->winrct.xmax - 1.5f * AZONEPAD_TAB_PLUSW;
			az->y2 = ar->winrct.ymin;
			break;
		case AE_LEFT_TO_TOPRIGHT:
			az->x1 = ar->winrct.xmin - AZONEPAD_TAB_PLUSH;
			az->y1 = ar->winrct.ymax - 2.5f * AZONEPAD_TAB_PLUSW;
			az->x2 = ar->winrct.xmin;
			az->y2 = ar->winrct.ymax - 1.5f * AZONEPAD_TAB_PLUSW;
			break;
		case AE_RIGHT_TO_TOPLEFT:
			az->x1 = ar->winrct.xmax - 1;
			az->y1 = ar->winrct.ymax - 2.5f * AZONEPAD_TAB_PLUSW;
			az->x2 = ar->winrct.xmax - 1 + AZONEPAD_TAB_PLUSH;
			az->y2 = ar->winrct.ymax - 1.5f * AZONEPAD_TAB_PLUSW;
			break;
	}
	/* rect needed for mouse pointer test */
	BLI_rcti_init(&az->rect, az->x1, az->x2, az->y1, az->y2);
}

static void region_azone_edge_initialize(ScrArea *sa, ARegion *ar, AZEdge edge, const bool is_fullscreen)
{
	AZone *az = NULL;
	const bool is_hidden = (ar->flag & (RGN_FLAG_HIDDEN | RGN_FLAG_TOO_SMALL));

	if (is_hidden && is_fullscreen) {
		return;
	}

	az = (AZone *)MEM_callocN(sizeof(AZone), "actionzone");
	BLI_addtail(&(sa->actionzones), az);
	az->type = AZONE_REGION;
	az->ar = ar;
	az->edge = edge;

	if (is_hidden) {
		region_azone_tab_plus(sa, az, ar);
	}
	else if (!is_hidden && (ar->regiontype != RGN_TYPE_HEADER)) {
		region_azone_edge(az, ar);
	}
}

static void region_azone_scrollbar_initialize(ScrArea *sa, ARegion *ar, AZScrollDirection direction)
{
	rcti scroller_vert = (direction == AZ_SCROLL_VERT) ? ar->v2d.vert : ar->v2d.hor;
	AZone *az = MEM_callocN(sizeof(*az), __func__);

	BLI_addtail(&sa->actionzones, az);
	az->type = AZONE_REGION_SCROLL;
	az->ar = ar;
	az->direction = direction;

	if (direction == AZ_SCROLL_VERT) {
		az->ar->v2d.alpha_vert = 0;
	}
	else if (direction == AZ_SCROLL_HOR) {
		az->ar->v2d.alpha_hor = 0;
	}

	BLI_rcti_translate(&scroller_vert, ar->winrct.xmin, ar->winrct.ymin);
	az->x1 = scroller_vert.xmin - AZONEFADEIN;
	az->y1 = scroller_vert.ymin - AZONEFADEIN;
	az->x2 = scroller_vert.xmax + AZONEFADEIN;
	az->y2 = scroller_vert.ymax + AZONEFADEIN;

	BLI_rcti_init(&az->rect, az->x1, az->x2, az->y1, az->y2);
}

static void region_azones_scrollbars_initialize(ScrArea *sa, ARegion *ar)
{
	const View2D *v2d = &ar->v2d;

	if ((v2d->scroll & V2D_SCROLL_VERTICAL)   && ((v2d->scroll & V2D_SCROLL_SCALE_VERTICAL)   == 0)) {
		region_azone_scrollbar_initialize(sa, ar, AZ_SCROLL_VERT);
	}
	if ((v2d->scroll & V2D_SCROLL_HORIZONTAL) && ((v2d->scroll & V2D_SCROLL_SCALE_HORIZONTAL) == 0)) {
		region_azone_scrollbar_initialize(sa, ar, AZ_SCROLL_HOR);
	}
}


/* *************************************************************** */

static void region_azones_add(const bScreen *screen, ScrArea *sa, ARegion *ar, const int alignment)
{
	const bool is_fullscreen = screen->state == SCREENFULL;

	/* edge code (t b l r) is along which area edge azone will be drawn */

	if (ar->regiontype == RGN_TYPE_HEADER && ar->winy + 6 > sa->winy) {
		/* The logic for this is: when the header takes up the full area,
		 * disallow hiding it to view the main window.
		 *
		 * Without this, you can drag down the file selectors header and hide it
		 * by accident very easily (highly annoying!), the value 6 is arbitrary
		 * but accounts for small common rounding problems when scaling the UI,
		 * must be minimum '4' */
	}
	else if (alignment == RGN_ALIGN_TOP)
		region_azone_edge_initialize(sa, ar, AE_BOTTOM_TO_TOPLEFT, is_fullscreen);
	else if (alignment == RGN_ALIGN_BOTTOM)
		region_azone_edge_initialize(sa, ar, AE_TOP_TO_BOTTOMRIGHT, is_fullscreen);
	else if (alignment == RGN_ALIGN_RIGHT)
		region_azone_edge_initialize(sa, ar, AE_LEFT_TO_TOPRIGHT, is_fullscreen);
	else if (alignment == RGN_ALIGN_LEFT)
		region_azone_edge_initialize(sa, ar, AE_RIGHT_TO_TOPLEFT, is_fullscreen);

	if (is_fullscreen) {
		fullscreen_azone_initialize(sa, ar);
	}

	region_azones_scrollbars_initialize(sa, ar);
}

/* dir is direction to check, not the splitting edge direction! */
static int rct_fits(const rcti *rect, char dir, int size)
{
	if (dir == 'h') {
		return BLI_rcti_size_x(rect) + 1 - size;
	}
	else {  /* 'v' */
		return BLI_rcti_size_y(rect) + 1 - size;
	}
}

/* *************************************************************** */

/* ar should be overlapping */
/* function checks if some overlapping region was defined before - on same place */
static void region_overlap_fix(ScrArea *sa, ARegion *ar)
{
	ARegion *ar1;
	const int align = ar->alignment & ~RGN_SPLIT_PREV;
	int align1 = 0;

	/* find overlapping previous region on same place */
	for (ar1 = ar->prev; ar1; ar1 = ar1->prev) {
		if (ar1->flag & (RGN_FLAG_HIDDEN)) {
			continue;
		}

		if (ar1->overlap && ((ar1->alignment & RGN_SPLIT_PREV) == 0)) {
			align1 = ar1->alignment;
			if (BLI_rcti_isect(&ar1->winrct, &ar->winrct, NULL)) {
				if (align1 != align) {
					/* Left overlapping right or vice-versa, forbid this! */
					ar->flag |= RGN_FLAG_TOO_SMALL;
					return;
				}
				/* Else, we have our previous region on same side. */
				break;
			}
		}
	}

	/* translate or close */
	if (ar1) {
		if (align1 == RGN_ALIGN_LEFT) {
			if (ar->winrct.xmax + ar1->winx > sa->winx - U.widget_unit) {
				ar->flag |= RGN_FLAG_TOO_SMALL;
				return;
			}
			else {
				BLI_rcti_translate(&ar->winrct, ar1->winx, 0);
			}
		}
		else if (align1 == RGN_ALIGN_RIGHT) {
			if (ar->winrct.xmin - ar1->winx < U.widget_unit) {
				ar->flag |= RGN_FLAG_TOO_SMALL;
				return;
			}
			else {
				BLI_rcti_translate(&ar->winrct, -ar1->winx, 0);
			}
		}
	}

	/* At this point, 'ar' is in its final position and still open.
	 * Make a final check it does not overlap any previous 'other side' region. */
	for (ar1 = ar->prev; ar1; ar1 = ar1->prev) {
		if (ar1->flag & (RGN_FLAG_HIDDEN)) {
			continue;
		}

		if (ar1->overlap && (ar1->alignment & RGN_SPLIT_PREV) == 0) {
			if ((ar1->alignment != align) && BLI_rcti_isect(&ar1->winrct, &ar->winrct, NULL)) {
				/* Left overlapping right or vice-versa, forbid this! */
				ar->flag |= RGN_FLAG_TOO_SMALL;
				return;
			}
		}
	}
}

/* overlapping regions only in the following restricted cases */
bool ED_region_is_overlap(int spacetype, int regiontype)
{
	if (U.uiflag2 & USER_REGION_OVERLAP) {
		if (ELEM(spacetype, SPACE_VIEW3D, SPACE_SEQ, SPACE_IMAGE)) {
			if (ELEM(regiontype, RGN_TYPE_TOOLS, RGN_TYPE_UI, RGN_TYPE_TOOL_PROPS))
				return 1;

			if (ELEM(spacetype, SPACE_VIEW3D, SPACE_IMAGE)) {
				if (regiontype == RGN_TYPE_HEADER)
					return 1;
			}
			else if (spacetype == SPACE_SEQ) {
				if (regiontype == RGN_TYPE_PREVIEW)
					return 1;
			}
		}
	}

	return 0;
}

static void region_rect_recursive(wmWindow *win, ScrArea *sa, ARegion *ar, rcti *remainder, rcti *overlap_remainder, int quad)
{
	rcti *remainder_prev = remainder;
	int prefsizex, prefsizey;
	int alignment;
	
	if (ar == NULL)
		return;
	
	/* no returns in function, winrct gets set in the end again */
	BLI_rcti_init(&ar->winrct, 0, 0, 0, 0);
	
	/* for test; allow split of previously defined region */
	if (ar->alignment & RGN_SPLIT_PREV)
		if (ar->prev)
			remainder = &ar->prev->winrct;
	
	alignment = ar->alignment & ~RGN_SPLIT_PREV;
	
	/* set here, assuming userpref switching forces to call this again */
	ar->overlap = ED_region_is_overlap(sa->spacetype, ar->regiontype);

	/* clear state flags first */
	ar->flag &= ~RGN_FLAG_TOO_SMALL;
	/* user errors */
	if (ar->next == NULL && alignment != RGN_ALIGN_QSPLIT)
		alignment = RGN_ALIGN_NONE;

	/* prefsize, taking into account DPI */
	prefsizex = UI_DPI_FAC * ((ar->sizex > 1) ? ar->sizex + 0.5f : ar->type->prefsizex);

	if (ar->regiontype == RGN_TYPE_HEADER) {
		prefsizey = ED_area_headersize();
	}
	else if (ED_area_is_global(sa)) {
		prefsizey = ED_region_global_size_y();
	}
	else if (ar->regiontype == RGN_TYPE_UI && sa->spacetype == SPACE_FILE) {
		prefsizey = UI_UNIT_Y * 2 + (UI_UNIT_Y / 2);
	}
	else {
		prefsizey = UI_DPI_FAC * (ar->sizey > 1 ? ar->sizey + 0.5f : ar->type->prefsizey);
	}


	if (ar->flag & RGN_FLAG_HIDDEN) {
		/* hidden is user flag */
	}
	else if (alignment == RGN_ALIGN_FLOAT) {
		/* XXX floating area region, not handled yet here */
	}
	else if (rct_fits(remainder, 'v', 1) < 0 || rct_fits(remainder, 'h', 1) < 0) {
		/* remainder is too small for any usage */
		ar->flag |= RGN_FLAG_TOO_SMALL;
	}
	else if (alignment == RGN_ALIGN_NONE) {
		/* typically last region */
		ar->winrct = *remainder;
		BLI_rcti_init(remainder, 0, 0, 0, 0);
	}
	else if (alignment == RGN_ALIGN_TOP || alignment == RGN_ALIGN_BOTTOM) {
		rcti *winrct = (ar->overlap) ? overlap_remainder : remainder;
		
		if (rct_fits(winrct, 'v', prefsizey) < 0) {
			ar->flag |= RGN_FLAG_TOO_SMALL;
		}
		else {
			int fac = rct_fits(winrct, 'v', prefsizey);

			if (fac < 0)
				prefsizey += fac;
			
			ar->winrct = *winrct;
			
			if (alignment == RGN_ALIGN_TOP) {
				ar->winrct.ymin = ar->winrct.ymax - prefsizey + 1;
				winrct->ymax = ar->winrct.ymin - 1;
			}
			else {
				ar->winrct.ymax = ar->winrct.ymin + prefsizey - 1;
				winrct->ymin = ar->winrct.ymax + 1;
			}
		}
	}
	else if (ELEM(alignment, RGN_ALIGN_LEFT, RGN_ALIGN_RIGHT)) {
		rcti *winrct = (ar->overlap) ? overlap_remainder : remainder;
		
		if (rct_fits(winrct, 'h', prefsizex) < 0) {
			ar->flag |= RGN_FLAG_TOO_SMALL;
		}
		else {
			int fac = rct_fits(winrct, 'h', prefsizex);
			
			if (fac < 0)
				prefsizex += fac;
			
			ar->winrct = *winrct;
			
			if (alignment == RGN_ALIGN_RIGHT) {
				ar->winrct.xmin = ar->winrct.xmax - prefsizex + 1;
				winrct->xmax = ar->winrct.xmin - 1;
			}
			else {
				ar->winrct.xmax = ar->winrct.xmin + prefsizex - 1;
				winrct->xmin = ar->winrct.xmax + 1;
			}
		}
	}
	else if (alignment == RGN_ALIGN_VSPLIT || alignment == RGN_ALIGN_HSPLIT) {
		/* percentage subdiv*/
		ar->winrct = *remainder;
		
		if (alignment == RGN_ALIGN_HSPLIT) {
			if (rct_fits(remainder, 'h', prefsizex) > 4) {
				ar->winrct.xmax = BLI_rcti_cent_x(remainder);
				remainder->xmin = ar->winrct.xmax + 1;
			}
			else {
				BLI_rcti_init(remainder, 0, 0, 0, 0);
			}
		}
		else {
			if (rct_fits(remainder, 'v', prefsizey) > 4) {
				ar->winrct.ymax = BLI_rcti_cent_y(remainder);
				remainder->ymin = ar->winrct.ymax + 1;
			}
			else {
				BLI_rcti_init(remainder, 0, 0, 0, 0);
			}
		}
	}
	else if (alignment == RGN_ALIGN_QSPLIT) {
		ar->winrct = *remainder;
		
		/* test if there's still 4 regions left */
		if (quad == 0) {
			ARegion *artest = ar->next;
			int count = 1;
			
			while (artest) {
				artest->alignment = RGN_ALIGN_QSPLIT;
				artest = artest->next;
				count++;
			}
			
			if (count != 4) {
				/* let's stop adding regions */
				BLI_rcti_init(remainder, 0, 0, 0, 0);
				if (G.debug & G_DEBUG)
					printf("region quadsplit failed\n");
			}
			else {
				quad = 1;
			}
		}
		if (quad) {
			if (quad == 1) { /* left bottom */
				ar->winrct.xmax = BLI_rcti_cent_x(remainder);
				ar->winrct.ymax = BLI_rcti_cent_y(remainder);
			}
			else if (quad == 2) { /* left top */
				ar->winrct.xmax = BLI_rcti_cent_x(remainder);
				ar->winrct.ymin = BLI_rcti_cent_y(remainder) + 1;
			}
			else if (quad == 3) { /* right bottom */
				ar->winrct.xmin = BLI_rcti_cent_x(remainder) + 1;
				ar->winrct.ymax = BLI_rcti_cent_y(remainder);
			}
			else {  /* right top */
				ar->winrct.xmin = BLI_rcti_cent_x(remainder) + 1;
				ar->winrct.ymin = BLI_rcti_cent_y(remainder) + 1;
				BLI_rcti_init(remainder, 0, 0, 0, 0);
			}

			quad++;
		}
	}
	
	/* for speedup */
	ar->winx = BLI_rcti_size_x(&ar->winrct) + 1;
	ar->winy = BLI_rcti_size_y(&ar->winrct) + 1;
	
	/* if region opened normally, we store this for hide/reveal usage */
	/* prevent rounding errors for UI_DPI_FAC mult and divide */
	if (ar->winx > 1) ar->sizex = (ar->winx + 0.5f) /  UI_DPI_FAC;
	if (ar->winy > 1) ar->sizey = (ar->winy + 0.5f) /  UI_DPI_FAC;
		
	/* exception for multiple overlapping regions on same spot */
	if (ar->overlap) {
		region_overlap_fix(sa, ar);
	}

	/* set winrect for azones */
	if (ar->flag & (RGN_FLAG_HIDDEN | RGN_FLAG_TOO_SMALL)) {
		ar->winrct = (ar->overlap) ? *overlap_remainder : *remainder;
		
		switch (alignment) {
			case RGN_ALIGN_TOP:
				ar->winrct.ymin = ar->winrct.ymax;
				break;
			case RGN_ALIGN_BOTTOM:
				ar->winrct.ymax = ar->winrct.ymin;
				break;
			case RGN_ALIGN_RIGHT:
				ar->winrct.xmin = ar->winrct.xmax;
				break;
			case RGN_ALIGN_LEFT:
			default:
				/* prevent winrct to be valid */
				ar->winrct.xmax = ar->winrct.xmin;
				break;
		}
	}

	/* restore prev-split exception */
	if (ar->alignment & RGN_SPLIT_PREV) {
		if (ar->prev) {
			remainder = remainder_prev;
			ar->prev->winx = BLI_rcti_size_x(&ar->prev->winrct) + 1;
			ar->prev->winy = BLI_rcti_size_y(&ar->prev->winrct) + 1;
		}
	}

	/* After non-overlapping region, all following overlapping regions
	 * fit within the remaining space again. */
	if (!ar->overlap) {
		*overlap_remainder = *remainder;
	}

	region_rect_recursive(win, sa, ar->next, remainder, overlap_remainder, quad);
}

static void area_calc_totrct(ScrArea *sa, const rcti *window_rect)
{
	short px = (short)U.pixelsize;

	sa->totrct.xmin = sa->v1->vec.x;
	sa->totrct.xmax = sa->v4->vec.x;
	sa->totrct.ymin = sa->v1->vec.y;
	sa->totrct.ymax = sa->v2->vec.y;

	/* scale down totrct by 1 pixel on all sides not matching window borders */
	if (sa->totrct.xmin > window_rect->xmin) {
		sa->totrct.xmin += px;
	}
	if (sa->totrct.xmax < (window_rect->xmax - 1)) {
		sa->totrct.xmax -= px;
	}
	if (sa->totrct.ymin > window_rect->ymin) {
		sa->totrct.ymin += px;
	}
	if (sa->totrct.ymax < (window_rect->ymax - 1)) {
		sa->totrct.ymax -= px;
	}
	/* Although the following asserts are correct they lead to a very unstable Blender.
	 * And the asserts would fail even in 2.7x (they were added in 2.8x as part of the top-bar commit).
	 * For more details see T54864. */
#if 0
	BLI_assert(sa->totrct.xmin >= 0);
	BLI_assert(sa->totrct.xmax >= 0);
	BLI_assert(sa->totrct.ymin >= 0);
	BLI_assert(sa->totrct.ymax >= 0);
#endif

	/* for speedup */
	sa->winx = BLI_rcti_size_x(&sa->totrct) + 1;
	sa->winy = BLI_rcti_size_y(&sa->totrct) + 1;
}


/* used for area initialize below */
static void region_subwindow(ARegion *ar)
{
	bool hidden = (ar->flag & (RGN_FLAG_HIDDEN | RGN_FLAG_TOO_SMALL)) != 0;

	if ((ar->alignment & RGN_SPLIT_PREV) && ar->prev)
		hidden = hidden || (ar->prev->flag & (RGN_FLAG_HIDDEN | RGN_FLAG_TOO_SMALL));

	ar->visible = !hidden;
}

static void ed_default_handlers(wmWindowManager *wm, ScrArea *sa, ListBase *handlers, int flag)
{
	/* note, add-handler checks if it already exists */

	/* XXX it would be good to have boundbox checks for some of these... */
	if (flag & ED_KEYMAP_UI) {
		wmKeyMap *keymap = WM_keymap_find(wm->defaultconf, "User Interface", 0, 0);
		WM_event_add_keymap_handler(handlers, keymap);

		/* user interface widgets */
		UI_region_handlers_add(handlers);
	}
	if (flag & ED_KEYMAP_VIEW2D) {
		/* 2d-viewport handling+manipulation */
		wmKeyMap *keymap = WM_keymap_find(wm->defaultconf, "View2D", 0, 0);
		WM_event_add_keymap_handler(handlers, keymap);
	}
	if (flag & ED_KEYMAP_MARKERS) {
		/* time-markers */
		wmKeyMap *keymap = WM_keymap_find(wm->defaultconf, "Markers", 0, 0);
		
		/* use a boundbox restricted map */
		ARegion *ar;
		/* same local check for all areas */
		static rcti rect = {0, 10000, 0, -1};
		rect.ymax = UI_MARKER_MARGIN_Y;
		ar = BKE_area_find_region_type(sa, RGN_TYPE_WINDOW);
		if (ar) {
			WM_event_add_keymap_handler_bb(handlers, keymap, &rect, &ar->winrct);
		}
	}
	if (flag & ED_KEYMAP_ANIMATION) {
		/* frame changing and timeline operators (for time spaces) */
		wmKeyMap *keymap = WM_keymap_find(wm->defaultconf, "Animation", 0, 0);
		WM_event_add_keymap_handler(handlers, keymap);
	}
	if (flag & ED_KEYMAP_FRAMES) {
		/* frame changing/jumping (for all spaces) */
		wmKeyMap *keymap = WM_keymap_find(wm->defaultconf, "Frames", 0, 0);
		WM_event_add_keymap_handler(handlers, keymap);
	}
	if (flag & ED_KEYMAP_GPENCIL) {
		/* grease pencil */
		/* NOTE: This is now 2 keymaps - One for basic functionality, 
		 *       and one that only applies when "Edit Mode" is enabled 
		 *       for strokes.
		 *
		 *       For now, it's easier to just include both, 
		 *       since you hardly want one without the other.
		 */
		wmKeyMap *keymap_general = WM_keymap_find(wm->defaultconf, "Grease Pencil", 0, 0);
		wmKeyMap *keymap_edit = WM_keymap_find(wm->defaultconf, "Grease Pencil Stroke Edit Mode", 0, 0);
		
		WM_event_add_keymap_handler(handlers, keymap_general);
		WM_event_add_keymap_handler(handlers, keymap_edit);
	}
	if (flag & ED_KEYMAP_HEADER) {
		/* standard keymap for headers regions */
		wmKeyMap *keymap = WM_keymap_find(wm->defaultconf, "Header", 0, 0);
		WM_event_add_keymap_handler(handlers, keymap);
	}
}

void ED_area_update_region_sizes(wmWindowManager *wm, wmWindow *win, ScrArea *area)
{
	rcti rect, overlap_rect;
	rcti window_rect;

	if (!(area->flag & AREA_FLAG_REGION_SIZE_UPDATE)) {
		return;
	}

	WM_window_rect_calc(win, &window_rect);
	area_calc_totrct(area, &window_rect);

	/* region rect sizes */
	rect = area->totrct;
	overlap_rect = rect;
	region_rect_recursive(win, area, area->regionbase.first, &rect, &overlap_rect, 0);

	for (ARegion *ar = area->regionbase.first; ar; ar = ar->next) {
		region_subwindow(ar);

		/* region size may have changed, init does necessary adjustments */
		if (ar->type->init) {
			ar->type->init(wm, ar);
		}
	}

	area->flag &= ~AREA_FLAG_REGION_SIZE_UPDATE;
}

/* called in screen_refresh, or screens_init, also area size changes */
void ED_area_initialize(wmWindowManager *wm, wmWindow *win, ScrArea *sa)
{
	WorkSpace *workspace = WM_window_get_active_workspace(win);
	const bScreen *screen = BKE_workspace_active_screen_get(win->workspace_hook);
	Scene *scene = WM_window_get_active_scene(win);
	ARegion *ar;
	rcti rect, overlap_rect;
	rcti window_rect;

	if (ED_area_is_global(sa) && (sa->global->flag & GLOBAL_AREA_IS_HIDDEN)) {
		return;
	}
	WM_window_rect_calc(win, &window_rect);

	/* set typedefinitions */
	sa->type = BKE_spacetype_from_id(sa->spacetype);
	
	if (sa->type == NULL) {
		sa->spacetype = SPACE_VIEW3D;
		sa->type = BKE_spacetype_from_id(sa->spacetype);
	}

	for (ar = sa->regionbase.first; ar; ar = ar->next)
		ar->type = BKE_regiontype_from_id(sa->type, ar->regiontype);

	/* area sizes */
	area_calc_totrct(sa, &window_rect);

	/* region rect sizes */
	rect = sa->totrct;
	overlap_rect = rect;
	region_rect_recursive(win, sa, sa->regionbase.first, &rect, &overlap_rect, 0);
	sa->flag &= ~AREA_FLAG_REGION_SIZE_UPDATE;
	
	/* default area handlers */
	ed_default_handlers(wm, sa, &sa->handlers, sa->type->keymapflag);
	/* checks spacedata, adds own handlers */
	if (sa->type->init)
		sa->type->init(wm, sa);

	/* clear all azones, add the area triange widgets */
	area_azone_initialize(win, screen, sa);

	/* region windows, default and own handlers */
	for (ar = sa->regionbase.first; ar; ar = ar->next) {
		region_subwindow(ar);
		
		if (ar->visible) {
			/* default region handlers */
			ed_default_handlers(wm, sa, &ar->handlers, ar->type->keymapflag);
			/* own handlers */
			if (ar->type->init) {
				ar->type->init(wm, ar);
			}
		}
		else {
			/* prevent uiblocks to run */
			UI_blocklist_free(NULL, &ar->uiblocks);
		}

		/* Some AZones use View2D data which is only updated in region init, so call that first! */
		region_azones_add(screen, sa, ar, ar->alignment & ~RGN_SPLIT_PREV);
	}

	WM_toolsystem_refresh_screen_area(workspace, scene, sa);
}

static void region_update_rect(ARegion *ar)
{
	ar->winx = BLI_rcti_size_x(&ar->winrct) + 1;
	ar->winy = BLI_rcti_size_y(&ar->winrct) + 1;

	/* v2d mask is used to subtract scrollbars from a 2d view. Needs initialize here. */
	BLI_rcti_init(&ar->v2d.mask, 0, ar->winx - 1, 0, ar->winy -1);
}

/**
 * Call to move a popup window (keep OpenGL context free!)
 */
void ED_region_update_rect(bContext *UNUSED(C), ARegion *ar)
{
	region_update_rect(ar);
}

/* externally called for floating regions like menus */
void ED_region_init(bContext *UNUSED(C), ARegion *ar)
{
	/* refresh can be called before window opened */
	region_subwindow(ar);

	region_update_rect(ar);
}

void ED_region_cursor_set(wmWindow *win, ScrArea *sa, ARegion *ar)
{
	if (ar && sa && ar->type && ar->type->cursor) {
		ar->type->cursor(win, sa, ar);
	}
	else {
		if (WM_cursor_set_from_tool(win, sa, ar)) {
			return;
		}
		WM_cursor_set(win, CURSOR_STD);
	}
}

/* for use after changing visiblity of regions */
void ED_region_visibility_change_update(bContext *C, ARegion *ar)
{
	ScrArea *sa = CTX_wm_area(C);
	
	if (ar->flag & RGN_FLAG_HIDDEN)
		WM_event_remove_handlers(C, &ar->handlers);
	
	ED_area_initialize(CTX_wm_manager(C), CTX_wm_window(C), sa);
	ED_area_tag_redraw(sa);
}

/* for quick toggle, can skip fades */
void region_toggle_hidden(bContext *C, ARegion *ar, const bool do_fade)
{
	ScrArea *sa = CTX_wm_area(C);
	
	ar->flag ^= RGN_FLAG_HIDDEN;
	
	if (do_fade && ar->overlap) {
		/* starts a timer, and in end calls the stuff below itself (region_sblend_invoke()) */
		region_blend_start(C, sa, ar);
	}
	else {
		ED_region_visibility_change_update(C, ar);
	}
}

/* exported to all editors, uses fading default */
void ED_region_toggle_hidden(bContext *C, ARegion *ar)
{
	region_toggle_hidden(C, ar, true);
}

/**
 * we swap spaces for fullscreen to keep all allocated data area vertices were set
 */
void ED_area_data_copy(ScrArea *sa_dst, ScrArea *sa_src, const bool do_free)
{
	SpaceType *st;
	ARegion *ar;
	const char spacetype = sa_dst->spacetype;
	const short flag_copy = HEADER_NO_PULLDOWN;
	
	sa_dst->spacetype = sa_src->spacetype;
	sa_dst->type = sa_src->type;

	sa_dst->flag = (sa_dst->flag & ~flag_copy) | (sa_src->flag & flag_copy);

	/* area */
	if (do_free) {
		BKE_spacedata_freelist(&sa_dst->spacedata);
	}
	BKE_spacedata_copylist(&sa_dst->spacedata, &sa_src->spacedata);

	/* Note; SPACE_EMPTY is possible on new screens */

	/* regions */
	if (do_free) {
		st = BKE_spacetype_from_id(spacetype);
		for (ar = sa_dst->regionbase.first; ar; ar = ar->next)
			BKE_area_region_free(st, ar);
		BLI_freelistN(&sa_dst->regionbase);
	}
	st = BKE_spacetype_from_id(sa_src->spacetype);
	for (ar = sa_src->regionbase.first; ar; ar = ar->next) {
		ARegion *newar = BKE_area_region_copy(st, ar);
		BLI_addtail(&sa_dst->regionbase, newar);
	}
}

void ED_area_data_swap(ScrArea *sa_dst, ScrArea *sa_src)
{
	SWAP(char, sa_dst->spacetype, sa_src->spacetype);
	SWAP(SpaceType *, sa_dst->type, sa_src->type);


	SWAP(ListBase, sa_dst->spacedata, sa_src->spacedata);
	SWAP(ListBase, sa_dst->regionbase, sa_src->regionbase);
}

/* *********** Space switching code *********** */

void ED_area_swapspace(bContext *C, ScrArea *sa1, ScrArea *sa2)
{
	ScrArea *tmp = MEM_callocN(sizeof(ScrArea), "addscrarea");

	ED_area_exit(C, sa1);
	ED_area_exit(C, sa2);

	ED_area_data_copy(tmp, sa1, false);
	ED_area_data_copy(sa1, sa2, true);
	ED_area_data_copy(sa2, tmp, true);
	ED_area_initialize(CTX_wm_manager(C), CTX_wm_window(C), sa1);
	ED_area_initialize(CTX_wm_manager(C), CTX_wm_window(C), sa2);

	BKE_screen_area_free(tmp);
	MEM_freeN(tmp);

	/* tell WM to refresh, cursor types etc */
	WM_event_add_mousemove(C);
	
	ED_area_tag_redraw(sa1);
	ED_area_tag_refresh(sa1);
	ED_area_tag_redraw(sa2);
	ED_area_tag_refresh(sa2);
}

/**
 * \param skip_ar_exit  Skip calling area exit callback. Set for opening temp spaces.
 */
void ED_area_newspace(bContext *C, ScrArea *sa, int type, const bool skip_ar_exit)
{
	wmWindow *win = CTX_wm_window(C);

	if (sa->spacetype != type) {
		SpaceType *st;
		SpaceLink *slold;
		SpaceLink *sl;
		/* store sa->type->exit callback */
		void *sa_exit = sa->type ? sa->type->exit : NULL;
		int header_alignment = ED_area_header_alignment(sa);

		/* in some cases (opening temp space) we don't want to
		 * call area exit callback, so we temporarily unset it */
		if (skip_ar_exit && sa->type) {
			sa->type->exit = NULL;
		}

		ED_area_exit(C, sa);

		/* restore old area exit callback */
		if (skip_ar_exit && sa->type) {
			sa->type->exit = sa_exit;
		}

		st = BKE_spacetype_from_id(type);
		slold = sa->spacedata.first;

		sa->spacetype = type;
		sa->type = st;

		/* If st->new may be called, don't use context until then. The
		 * sa->type->context() callback has changed but data may be invalid
		 * (e.g. with properties editor) until space-data is properly created */

		/* check previously stored space */
		for (sl = sa->spacedata.first; sl; sl = sl->next)
			if (sl->spacetype == type)
				break;
		
		/* old spacedata... happened during work on 2.50, remove */
		if (sl && BLI_listbase_is_empty(&sl->regionbase)) {
			st->free(sl);
			BLI_freelinkN(&sa->spacedata, sl);
			if (slold == sl) {
				slold = NULL;
			}
			sl = NULL;
		}

		if (sl) {
			/* swap regions */
			slold->regionbase = sa->regionbase;
			sa->regionbase = sl->regionbase;
			BLI_listbase_clear(&sl->regionbase);
			
			/* put in front of list */
			BLI_remlink(&sa->spacedata, sl);
			BLI_addhead(&sa->spacedata, sl);


			/* Sync header alignment. */
			for (ARegion *ar = sa->regionbase.first; ar; ar = ar->next) {
				if (ar->regiontype == RGN_TYPE_HEADER) {
					ar->alignment = header_alignment;
					break;
				}
			}
		}
		else {
			/* new space */
			if (st) {
				/* Don't get scene from context here which may depend on space-data. */
				Scene *scene = WM_window_get_active_scene(win);
				sl = st->new(sa, scene);
				BLI_addhead(&sa->spacedata, sl);
				
				/* swap regions */
				if (slold)
					slold->regionbase = sa->regionbase;
				sa->regionbase = sl->regionbase;
				BLI_listbase_clear(&sl->regionbase);
			}
		}
		
		ED_area_initialize(CTX_wm_manager(C), win, sa);
		
		/* tell WM to refresh, cursor types etc */
		WM_event_add_mousemove(C);
				
		/* send space change notifier */
		WM_event_add_notifier(C, NC_SPACE | ND_SPACE_CHANGED, sa);
		
		ED_area_tag_refresh(sa);
	}
	
	/* also redraw when re-used */
	ED_area_tag_redraw(sa);
}

void ED_area_prevspace(bContext *C, ScrArea *sa)
{
	SpaceLink *sl = sa->spacedata.first;

	if (sl && sl->next) {
		ED_area_newspace(C, sa, sl->next->spacetype, false);

		/* keep old spacedata but move it to end, so calling
		 * ED_area_prevspace once more won't open it again */
		BLI_remlink(&sa->spacedata, sl);
		BLI_addtail(&sa->spacedata, sl);
	}
	else {
		/* no change */
		return;
	}
	sa->flag &= ~AREA_FLAG_STACKED_FULLSCREEN;

	ED_area_tag_redraw(sa);

	/* send space change notifier */
	WM_event_add_notifier(C, NC_SPACE | ND_SPACE_CHANGED, sa);
}

/* returns offset for next button in header */
int ED_area_header_switchbutton(const bContext *C, uiBlock *block, int yco)
{
	ScrArea *sa = CTX_wm_area(C);
	bScreen *scr = CTX_wm_screen(C);
	PointerRNA areaptr;
	int xco = 0.4 * U.widget_unit;

	RNA_pointer_create(&(scr->id), &RNA_Area, sa, &areaptr);

	uiDefButR(block, UI_BTYPE_MENU, 0, "", xco, yco, 1.6 * U.widget_unit, U.widget_unit,
	          &areaptr, "ui_type", 0, 0.0f, 0.0f, 0.0f, 0.0f, "");

	return xco + 1.7 * U.widget_unit;
}

/************************ standard UI regions ************************/

static ThemeColorID region_background_color_id(const bContext *C, const ARegion *region)
{
	ScrArea *area = CTX_wm_area(C);

	switch (region->regiontype) {
		case RGN_TYPE_HEADER:
			if (ED_screen_area_active(C) || ED_area_is_global(area)) {
				return TH_HEADER;
			}
			else {
				return TH_HEADERDESEL;
			}
		case RGN_TYPE_PREVIEW:
			return TH_PREVIEW_BACK;
		default:
			return TH_BACK;
	}
}

static void region_clear_color(const bContext *C, const ARegion *ar, ThemeColorID colorid)
{
	if (ar->overlap) {
		/* view should be in pixelspace */
		UI_view2d_view_restore(C);

		float back[4];
		UI_GetThemeColor4fv(colorid, back);
		glClearColor(back[3] * back[0], back[3] * back[1], back[3] * back[2], back[3]);
		glClear(GL_COLOR_BUFFER_BIT);
	}
	else {
		UI_ThemeClearColor(colorid);
		glClear(GL_COLOR_BUFFER_BIT);
	}
}

BLI_INLINE bool streq_array_any(const char *s, const char *arr[])
{
	for (uint i = 0; arr[i]; i++) {
		if (STREQ(arr[i], s)) {
			return true;
		}
	}
	return false;
}

/**
 * \param contexts: A NULL terminated array of context strings to match against.
 * Matching against any of these strings will draw the panel.
 * Can be NULL to skip context checks.
 */
void ED_region_panels(const bContext *C, ARegion *ar, const char *contexts[], int contextnr, const bool vertical)
{
	const WorkSpace *workspace = CTX_wm_workspace(C);
	ScrArea *sa = CTX_wm_area(C);
	uiStyle *style = UI_style_get_dpi();
	uiBlock *block;
	PanelType *pt;
	Panel *panel;
	View2D *v2d = &ar->v2d;
	View2DScrollers *scrollers;
	int x, y, xco, yco, w, em, triangle;
	bool is_context_new = 0;
	int scroll;

	/* XXX, should use some better check? */
	bool use_category_tabs = (ELEM(ar->regiontype, RGN_TYPE_TOOLS, RGN_TYPE_UI, RGN_TYPE_WINDOW));
	/* offset panels for small vertical tab area */
	const char *category = NULL;
	const int category_tabs_width = UI_PANEL_CATEGORY_MARGIN_WIDTH;
	int margin_x = 0;

	BLI_SMALLSTACK_DECLARE(pt_stack, PanelType *);

	if (contextnr != -1)
		is_context_new = UI_view2d_tab_set(v2d, contextnr);
	
	/* before setting the view */
	if (vertical) {
		/* only allow scrolling in vertical direction */
		v2d->keepofs |= V2D_LOCKOFS_X | V2D_KEEPOFS_Y;
		v2d->keepofs &= ~(V2D_LOCKOFS_Y | V2D_KEEPOFS_X);
		v2d->scroll &= ~(V2D_SCROLL_BOTTOM);
		v2d->scroll |= (V2D_SCROLL_RIGHT);
	}
	else {
		/* for now, allow scrolling in both directions (since layouts are optimized for vertical,
		 * they often don't fit in horizontal layout)
		 */
		v2d->keepofs &= ~(V2D_LOCKOFS_X | V2D_LOCKOFS_Y | V2D_KEEPOFS_X | V2D_KEEPOFS_Y);
		v2d->scroll |= (V2D_SCROLL_BOTTOM);
		v2d->scroll &= ~(V2D_SCROLL_RIGHT);
	}

	scroll = v2d->scroll;


	/* collect panels to draw */
	for (pt = ar->type->paneltypes.last; pt; pt = pt->prev) {
		/* verify context */
		if (contexts && pt->context[0] && !streq_array_any(pt->context, contexts)) {
			continue;
		}

		/* If we're tagged, only use compatible. */
		if (pt->owner_id[0] && BKE_workspace_owner_id_check(workspace, pt->owner_id) == false) {
			continue;
		}

		/* draw panel */
		if (pt->draw && (!pt->poll || pt->poll(C, pt))) {
			BLI_SMALLSTACK_PUSH(pt_stack, pt);
		}
	}


	/* collect categories */
	if (use_category_tabs) {
		UI_panel_category_clear_all(ar);

		/* gather unique categories */
		BLI_SMALLSTACK_ITER_BEGIN(pt_stack, pt)
		{
			if (pt->category[0]) {
				if (!UI_panel_category_find(ar, pt->category)) {
					UI_panel_category_add(ar, pt->category);
				}
			}
		}
		BLI_SMALLSTACK_ITER_END;

		if (!UI_panel_category_is_visible(ar)) {
			use_category_tabs = false;
		}
		else {
			category = UI_panel_category_active_get(ar, true);
			margin_x = category_tabs_width;
		}
	}


	if (vertical) {
		w = BLI_rctf_size_x(&v2d->cur);
		em = (ar->type->prefsizex) ? 10 : 20; /* works out to 10*UI_UNIT_X or 20*UI_UNIT_X */
	}
	else {
		w = UI_PANEL_WIDTH;
		em = (ar->type->prefsizex) ? 10 : 20;
	}

	w -= margin_x;

	/* create panels */
	UI_panels_begin(C, ar);

	/* set view2d view matrix  - UI_block_begin() stores it */
	UI_view2d_view_ortho(v2d);

	BLI_SMALLSTACK_ITER_BEGIN(pt_stack, pt)
	{
		bool open;

		panel = UI_panel_find_by_type(ar, pt);

		if (use_category_tabs && pt->category[0] && !STREQ(category, pt->category)) {
			if ((panel == NULL) || ((panel->flag & PNL_PIN) == 0)) {
				continue;
			}
		}

		/* draw panel */
		block = UI_block_begin(C, ar, pt->idname, UI_EMBOSS);
		panel = UI_panel_begin(sa, ar, block, pt, panel, &open);

		/* bad fixed values */
		triangle = (int)(UI_UNIT_Y * 1.1f);

		if (pt->draw_header && !(pt->flag & PNL_NO_HEADER) && (open || vertical)) {
			/* for enabled buttons */
			panel->layout = UI_block_layout(
			        block, UI_LAYOUT_HORIZONTAL, UI_LAYOUT_HEADER,
			        triangle, (UI_UNIT_Y * 1.1f) + style->panelspace, UI_UNIT_Y, 1, 0, style);

			pt->draw_header(C, panel);

			UI_block_layout_resolve(block, &xco, &yco);
			panel->labelofs = xco - triangle;
			panel->layout = NULL;
		}
		else {
			panel->labelofs = 0;
		}

		if (open) {
			short panelContext;

			/* panel context can either be toolbar region or normal panels region */
			if (ar->regiontype == RGN_TYPE_TOOLS)
				panelContext = UI_LAYOUT_TOOLBAR;
			else
				panelContext = UI_LAYOUT_PANEL;

			panel->layout = UI_block_layout(
			        block, UI_LAYOUT_VERTICAL, panelContext,
			        style->panelspace, 0, w - 2 * style->panelspace, em, 0, style);

			pt->draw(C, panel);

			UI_block_layout_resolve(block, &xco, &yco);
			panel->layout = NULL;

			yco -= 2 * style->panelspace;
			UI_panel_end(block, w, -yco);
		}
		else {
			yco = 0;
			UI_panel_end(block, w, 0);
		}

		UI_block_end(C, block);
	}
	BLI_SMALLSTACK_ITER_END;

	/* align panels and return size */
	UI_panels_end(C, ar, &x, &y);

	/* before setting the view */
	if (vertical) {
		/* we always keep the scroll offset - so the total view gets increased with the scrolled away part */
		if (v2d->cur.ymax < -FLT_EPSILON) {
			/* Clamp to lower view boundary */
			if (v2d->tot.ymin < -v2d->winy) {
				y = min_ii(y, 0);
			}
			else {
				y = min_ii(y, v2d->cur.ymin);
			}
		}

		y = -y;
	}
	else {
		/* don't jump back when panels close or hide */
		if (!is_context_new) {
			if (v2d->tot.xmax > v2d->winx) {
				x = max_ii(x, 0);
			}
			else {
				x = max_ii(x, v2d->cur.xmax);
			}
		}

		y = -y;
	}

	/* this also changes the 'cur' */
	UI_view2d_totRect_set(v2d, x, y);

	if (scroll != v2d->scroll) {
		/* Note: this code scales fine, but because of rounding differences, positions of elements
		 * flip +1 or -1 pixel compared to redoing the entire layout again.
		 * Leaving in commented code for future tests */
#if 0
		UI_panels_scale(ar, BLI_rctf_size_x(&v2d->cur));
		break;
#endif
	}

	region_clear_color(C, ar, (ar->type->regionid == RGN_TYPE_PREVIEW) ? TH_PREVIEW_BACK : TH_BACK);
	
	/* reset line width for drawing tabs */
	glLineWidth(1.0f);

	/* set the view */
	UI_view2d_view_ortho(v2d);

	/* draw panels */
	UI_panels_draw(C, ar);

	/* restore view matrix */
	UI_view2d_view_restore(C);
	
	if (use_category_tabs) {
		UI_panel_category_draw_all(ar, category);
	}

	/* scrollers */
	scrollers = UI_view2d_scrollers_calc(C, v2d, V2D_ARG_DUMMY, V2D_ARG_DUMMY, V2D_ARG_DUMMY, V2D_ARG_DUMMY);
	UI_view2d_scrollers_draw(C, v2d, scrollers);
	UI_view2d_scrollers_free(scrollers);
}

void ED_region_panels_init(wmWindowManager *wm, ARegion *ar)
{
	wmKeyMap *keymap;

	UI_view2d_region_reinit(&ar->v2d, V2D_COMMONVIEW_PANELS_UI, ar->winx, ar->winy);

	keymap = WM_keymap_find(wm->defaultconf, "View2D Buttons List", 0, 0);
	WM_event_add_keymap_handler(&ar->handlers, keymap);
}

void ED_region_header_layout(const bContext *C, ARegion *ar)
{
	uiStyle *style = UI_style_get_dpi();
	uiBlock *block;
	uiLayout *layout;
	HeaderType *ht;
	Header header = {NULL};
	int maxco, xco, yco;
	int headery = ED_area_headersize();
	const int start_ofs = 0.4f * UI_UNIT_X;
	bool region_layout_based = ar->flag & RGN_FLAG_DYNAMIC_SIZE;

	/* set view2d view matrix for scrolling (without scrollers) */
	UI_view2d_view_ortho(&ar->v2d);

	xco = maxco = start_ofs;
	yco = headery + (ar->winy - headery) / 2 - floor(0.2f * UI_UNIT_Y);

	/* XXX workaround for 1 px alignment issue. Not sure what causes it... Would prefer a proper fix - Julian */
	if (CTX_wm_area(C)->spacetype == SPACE_TOPBAR) {
		xco += 1;
		yco += 1;
	}

	/* draw all headers types */
	for (ht = ar->type->headertypes.first; ht; ht = ht->next) {
		block = UI_block_begin(C, ar, ht->idname, UI_EMBOSS);
		layout = UI_block_layout(block, UI_LAYOUT_HORIZONTAL, UI_LAYOUT_HEADER, xco, yco, UI_UNIT_Y, 1, 0, style);

		if (ht->draw) {
			header.type = ht;
			header.layout = layout;
			ht->draw(C, &header);
			
			/* for view2d */
			xco = uiLayoutGetWidth(layout);
			if (xco > maxco)
				maxco = xco;
		}

		UI_block_layout_resolve(block, &xco, &yco);
		
		/* for view2d */
		if (xco > maxco)
			maxco = xco;

		int new_sizex = (maxco + start_ofs) / UI_DPI_FAC;

		if (region_layout_based && (ar->sizex != new_sizex)) {
			/* region size is layout based and needs to be updated */
			ScrArea *sa = CTX_wm_area(C);

			ar->sizex = new_sizex;
			sa->flag |= AREA_FLAG_REGION_SIZE_UPDATE;
		}

		UI_block_end(C, block);
	}

	/* always as last  */
	UI_view2d_totRect_set(&ar->v2d, maxco + (region_layout_based ? 0 : UI_UNIT_X + 80), headery);

	/* restore view matrix */
	UI_view2d_view_restore(C);
}

void ED_region_header_draw(const bContext *C, ARegion *ar)
{
	UI_view2d_view_ortho(&ar->v2d);

	/* clear */
	region_clear_color(C, ar, region_background_color_id(C, ar));

	/* View2D matrix might have changed due to dynamic sized regions. */
	UI_blocklist_update_window_matrix(C, &ar->uiblocks);

	/* draw blocks */
	UI_blocklist_draw(C, &ar->uiblocks);

	/* restore view matrix */
	UI_view2d_view_restore(C);
}

void ED_region_header(const bContext *C, ARegion *ar)
{
	/* TODO: remove? */
	ED_region_header_layout(C, ar);
	ED_region_header_draw(C, ar);
}

void ED_region_header_init(ARegion *ar)
{
	UI_view2d_region_reinit(&ar->v2d, V2D_COMMONVIEW_HEADER, ar->winx, ar->winy);
}

/* UI_UNIT_Y is defined as U variable now, depending dpi */
int ED_area_headersize(void)
{
	return (int)(HEADERY * UI_DPI_FAC);
}


int ED_area_header_alignment(const ScrArea *area)
{
	for (ARegion *ar = area->regionbase.first; ar; ar = ar->next) {
		if (ar->regiontype == RGN_TYPE_HEADER) {
			return ar->alignment;
		}
	}

	return RGN_ALIGN_TOP;
}

/**
 * \return the final height of a global \a area, accounting for DPI.
 */
int ED_area_global_size_y(const ScrArea *area)
{
	BLI_assert(ED_area_is_global(area));
	return round_fl_to_int(area->global->cur_fixed_height * UI_DPI_FAC);
}

bool ED_area_is_global(const ScrArea *area)
{
	return area->global != NULL;
}

ScrArea *ED_screen_areas_iter_first(const wmWindow *win, const bScreen *screen)
{
	ScrArea *global_area = win->global_areas.areabase.first;

	if (!global_area) {
		return screen->areabase.first;
	}
	else if ((global_area->global->flag & GLOBAL_AREA_IS_HIDDEN) == 0) {
		return global_area;
	}
	/* Find next visible area. */
	return ED_screen_areas_iter_next(screen, global_area);
}
ScrArea *ED_screen_areas_iter_next(const bScreen *screen, const ScrArea *area)
{
	if (area->global) {
		for (ScrArea *area_iter = area->next; area_iter; area_iter = area_iter->next) {
			if ((area_iter->global->flag & GLOBAL_AREA_IS_HIDDEN) == 0) {
				return area_iter;
			}
		}
		/* No visible next global area found, start iterating over layout areas. */
		return screen->areabase.first;
	}

	return area->next;
}

/**
 * For now we just assume all global areas are made up out of horizontal bars
 * with the same size. A fixed size could be stored in ARegion instead if needed.
 *
 * \return the DPI aware height of a single bar/region in global areas.
 */
int ED_region_global_size_y(void)
{
	return ED_area_headersize(); /* same size as header */
}

void ED_region_info_draw_multiline(ARegion *ar, const char *text_array[], float fill_color[4], const bool full_redraw)
{
	const int header_height = UI_UNIT_Y;
	uiStyle *style = UI_style_get_dpi();
	int fontid = style->widget.uifont_id;
	GLint scissor[4];
	rcti rect;
	int num_lines = 0;

	/* background box */
	ED_region_visible_rect(ar, &rect);

	/* Box fill entire width or just around text. */
	if (!full_redraw) {
		const char **text = &text_array[0];
		while (*text) {
			rect.xmax = min_ii(rect.xmax, rect.xmin + BLF_width(fontid, *text, BLF_DRAW_STR_DUMMY_MAX) + 1.2f * U.widget_unit);
			text++;
			num_lines++;
		}
	}
	/* Just count the line number. */
	else {
		const char **text = &text_array[0];
		while (*text) {
			text++;
			num_lines++;
		}
	}

	rect.ymin = rect.ymax - header_height * num_lines;

	/* setup scissor */
	glGetIntegerv(GL_SCISSOR_BOX, scissor);
	glScissor(rect.xmin, rect.ymin,
	          BLI_rcti_size_x(&rect) + 1, BLI_rcti_size_y(&rect) + 1);

	glEnable(GL_BLEND);
	glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
	Gwn_VertFormat *format = immVertexFormat();
	unsigned int pos = GWN_vertformat_attr_add(format, "pos", GWN_COMP_I32, 2, GWN_FETCH_INT_TO_FLOAT);
	immBindBuiltinProgram(GPU_SHADER_2D_UNIFORM_COLOR);
	immUniformColor4fv(fill_color);
	immRecti(pos, rect.xmin, rect.ymin, rect.xmax + 1, rect.ymax + 1);
	immUnbindProgram();
	glDisable(GL_BLEND);

	/* text */
	UI_FontThemeColor(fontid, TH_TEXT_HI);
	BLF_clipping(fontid, rect.xmin, rect.ymin, rect.xmax, rect.ymax);
	BLF_enable(fontid, BLF_CLIPPING);
	int offset = num_lines - 1;
	{
		const char **text = &text_array[0];
		while (*text) {
			BLF_position(fontid, rect.xmin + 0.6f * U.widget_unit, rect.ymin + 0.3f * U.widget_unit + offset * header_height, 0.0f);
			BLF_draw(fontid, *text, BLF_DRAW_STR_DUMMY_MAX);
			text++;
			offset--;
		}
	}

	BLF_disable(fontid, BLF_CLIPPING);

	/* restore scissor as it was before */
	glScissor(scissor[0], scissor[1], scissor[2], scissor[3]);
}

void ED_region_info_draw(ARegion *ar, const char *text, float fill_color[4], const bool full_redraw)
{
	ED_region_info_draw_multiline(ar, (const char *[2]){text, NULL}, fill_color, full_redraw);
}

#define MAX_METADATA_STR    1024

static const char *meta_data_list[] =
{
	"File",
	"Strip",
	"Date",
	"RenderTime",
	"Note",
	"Marker",
	"Time",
	"Frame",
	"Camera",
	"Scene"
};

BLI_INLINE bool metadata_is_valid(ImBuf *ibuf, char *r_str, short index, int offset)
{
	return (IMB_metadata_get_field(ibuf->metadata, meta_data_list[index], r_str + offset, MAX_METADATA_STR - offset) && r_str[0]);
}

static void metadata_draw_imbuf(ImBuf *ibuf, const rctf *rect, int fontid, const bool is_top)
{
	char temp_str[MAX_METADATA_STR];
	int line_width;
	int ofs_y = 0;
	short i;
	int len;
	const float height = BLF_height_max(fontid);
	const float margin = height / 8;
	const float vertical_offset = (height + margin);

	/* values taking margins into account */
	const float descender = BLF_descender(fontid);
	const float xmin = (rect->xmin + margin);
	const float xmax = (rect->xmax - margin);
	const float ymin = (rect->ymin + margin) - descender;
	const float ymax = (rect->ymax - margin) - descender;

	if (is_top) {
		for (i = 0; i < 4; i++) {
			/* first line */
			if (i == 0) {
				bool do_newline = false;
				len = BLI_snprintf_rlen(temp_str, MAX_METADATA_STR, "%s: ", meta_data_list[0]);
				if (metadata_is_valid(ibuf, temp_str, 0, len)) {
					BLF_position(fontid, xmin, ymax - vertical_offset, 0.0f);
					BLF_draw(fontid, temp_str, BLF_DRAW_STR_DUMMY_MAX);
					do_newline = true;
				}

				len = BLI_snprintf_rlen(temp_str, MAX_METADATA_STR, "%s: ", meta_data_list[1]);
				if (metadata_is_valid(ibuf, temp_str, 1, len)) {
					line_width = BLF_width(fontid, temp_str, BLF_DRAW_STR_DUMMY_MAX);
					BLF_position(fontid, xmax - line_width, ymax - vertical_offset, 0.0f);
					BLF_draw(fontid, temp_str, BLF_DRAW_STR_DUMMY_MAX);
					do_newline = true;
				}

				if (do_newline)
					ofs_y += vertical_offset;
			} /* Strip */
			else if (i == 1 || i == 2) {
				len = BLI_snprintf_rlen(temp_str, MAX_METADATA_STR, "%s: ", meta_data_list[i + 1]);
				if (metadata_is_valid(ibuf, temp_str, i + 1, len)) {
					BLF_position(fontid, xmin, ymax - vertical_offset - ofs_y, 0.0f);
					BLF_draw(fontid, temp_str, BLF_DRAW_STR_DUMMY_MAX);
					ofs_y += vertical_offset;
				}
			} /* Note (wrapped) */
			else if (i == 3) {
				len = BLI_snprintf_rlen(temp_str, MAX_METADATA_STR, "%s: ", meta_data_list[i + 1]);
				if (metadata_is_valid(ibuf, temp_str, i + 1, len)) {
					struct ResultBLF info;
					BLF_enable(fontid, BLF_WORD_WRAP);
					BLF_wordwrap(fontid, ibuf->x - (margin * 2));
					BLF_position(fontid, xmin, ymax - vertical_offset - ofs_y, 0.0f);
					BLF_draw_ex(fontid, temp_str, BLF_DRAW_STR_DUMMY_MAX, &info);
					BLF_wordwrap(fontid, 0);
					BLF_disable(fontid, BLF_WORD_WRAP);
					ofs_y += vertical_offset * info.lines;
				}
			}
			else {
				len = BLI_snprintf_rlen(temp_str, MAX_METADATA_STR, "%s: ", meta_data_list[i + 1]);
				if (metadata_is_valid(ibuf, temp_str, i + 1, len)) {
					line_width = BLF_width(fontid, temp_str, BLF_DRAW_STR_DUMMY_MAX);
					BLF_position(fontid, xmax  - line_width, ymax - vertical_offset - ofs_y, 0.0f);
					BLF_draw(fontid, temp_str, BLF_DRAW_STR_DUMMY_MAX);
					ofs_y += vertical_offset;
				}
			}
		}
	}
	else {
		int ofs_x = 0;
		for (i = 5; i < 10; i++) {
			len = BLI_snprintf_rlen(temp_str, MAX_METADATA_STR, "%s: ", meta_data_list[i]);
			if (metadata_is_valid(ibuf, temp_str, i, len)) {
				BLF_position(fontid, xmin + ofs_x, ymin, 0.0f);
				BLF_draw(fontid, temp_str, BLF_DRAW_STR_DUMMY_MAX);
	
				ofs_x += BLF_width(fontid, temp_str, BLF_DRAW_STR_DUMMY_MAX) + UI_UNIT_X;
			}
		}
	}
}

static float metadata_box_height_get(ImBuf *ibuf, int fontid, const bool is_top)
{
	const float height = BLF_height_max(fontid);
	const float margin = (height / 8);
	char str[MAX_METADATA_STR] = "";
	short i, count = 0;

	if (is_top) {
		if (metadata_is_valid(ibuf, str, 0, 0) || metadata_is_valid(ibuf, str, 1, 0)) {
			count++;
		}
		for (i = 2; i < 5; i++) {
			if (metadata_is_valid(ibuf, str, i, 0)) {
				if (i == 4) {
					struct {
						struct ResultBLF info;
						rctf rect;
					} wrap;

					BLF_enable(fontid, BLF_WORD_WRAP);
					BLF_wordwrap(fontid, ibuf->x - (margin * 2));
					BLF_boundbox_ex(fontid, str, sizeof(str), &wrap.rect, &wrap.info);
					BLF_wordwrap(fontid, 0);
					BLF_disable(fontid, BLF_WORD_WRAP);

					count += wrap.info.lines;
				}
				else {
					count++;
				}
			}
		}
	}
	else {
		for (i = 5; i < 10; i++) {
			if (metadata_is_valid(ibuf, str, i, 0)) {
				count = 1;
			}
		}
	}

	if (count) {
		return (height + margin) * count;
	}

	return 0;
}

#undef MAX_METADATA_STR

void ED_region_image_metadata_draw(int x, int y, ImBuf *ibuf, const rctf *frame, float zoomx, float zoomy)
{
	float box_y;
	rctf rect;
	uiStyle *style = UI_style_get_dpi();

	if (!ibuf->metadata)
		return;

	/* find window pixel coordinates of origin */
	gpuPushMatrix();

	/* offset and zoom using ogl */
	gpuTranslate2f(x, y);
	gpuScale2f(zoomx, zoomy);

	BLF_size(blf_mono_font, style->widgetlabel.points * 1.5f * U.pixelsize, U.dpi);

	/* *** upper box*** */

	/* get needed box height */
	box_y = metadata_box_height_get(ibuf, blf_mono_font, true);

	if (box_y) {
		/* set up rect */
		BLI_rctf_init(&rect, frame->xmin, frame->xmax, frame->ymax, frame->ymax + box_y);
		/* draw top box */
		Gwn_VertFormat *format = immVertexFormat();
		unsigned int pos = GWN_vertformat_attr_add(format, "pos", GWN_COMP_F32, 2, GWN_FETCH_FLOAT);
		immBindBuiltinProgram(GPU_SHADER_2D_UNIFORM_COLOR);
		immUniformThemeColor(TH_METADATA_BG);
		immRectf(pos, rect.xmin, rect.ymin, rect.xmax, rect.ymax);
		immUnbindProgram();

		BLF_clipping(blf_mono_font, rect.xmin, rect.ymin, rect.xmax, rect.ymax);
		BLF_enable(blf_mono_font, BLF_CLIPPING);

		UI_FontThemeColor(blf_mono_font, TH_METADATA_TEXT);
		metadata_draw_imbuf(ibuf, &rect, blf_mono_font, true);

		BLF_disable(blf_mono_font, BLF_CLIPPING);
	}


	/* *** lower box*** */

	box_y = metadata_box_height_get(ibuf, blf_mono_font, false);

	if (box_y) {
		/* set up box rect */
		BLI_rctf_init(&rect, frame->xmin, frame->xmax, frame->ymin - box_y, frame->ymin);
		/* draw top box */
		Gwn_VertFormat *format = immVertexFormat();
		unsigned int pos = GWN_vertformat_attr_add(format, "pos", GWN_COMP_F32, 2, GWN_FETCH_FLOAT);
		immBindBuiltinProgram(GPU_SHADER_2D_UNIFORM_COLOR);
		immUniformThemeColor(TH_METADATA_BG);
		immRectf(pos, rect.xmin, rect.ymin, rect.xmax, rect.ymax);
		immUnbindProgram();

		BLF_clipping(blf_mono_font, rect.xmin, rect.ymin, rect.xmax, rect.ymax);
		BLF_enable(blf_mono_font, BLF_CLIPPING);

		UI_FontThemeColor(blf_mono_font, TH_METADATA_TEXT);
		metadata_draw_imbuf(ibuf, &rect, blf_mono_font, false);

		BLF_disable(blf_mono_font, BLF_CLIPPING);
	}

	gpuPopMatrix();
}

void ED_region_grid_draw(ARegion *ar, float zoomx, float zoomy)
{
	float gridsize, gridstep = 1.0f / 32.0f;
	float fac, blendfac;
	int x1, y1, x2, y2;

	/* the image is located inside (0, 0), (1, 1) as set by view2d */
	UI_view2d_view_to_region(&ar->v2d, 0.0f, 0.0f, &x1, &y1);
	UI_view2d_view_to_region(&ar->v2d, 1.0f, 1.0f, &x2, &y2);

	Gwn_VertFormat *format = immVertexFormat();
	unsigned int pos = GWN_vertformat_attr_add(format, "pos", GWN_COMP_F32, 2, GWN_FETCH_FLOAT);
	
	immBindBuiltinProgram(GPU_SHADER_2D_UNIFORM_COLOR);
	immUniformThemeColorShade(TH_BACK, 20);
	immRectf(pos, x1, y1, x2, y2);
	immUnbindProgram();

	/* gridsize adapted to zoom level */
	gridsize = 0.5f * (zoomx + zoomy);
	if (gridsize <= 0.0f)
		return;

	if (gridsize < 1.0f) {
		while (gridsize < 1.0f) {
			gridsize *= 4.0f;
			gridstep *= 4.0f;
		}
	}
	else {
		while (gridsize >= 4.0f) {
			gridsize /= 4.0f;
			gridstep /= 4.0f;
		}
	}

	blendfac = 0.25f * gridsize - floorf(0.25f * gridsize);
	CLAMP(blendfac, 0.0f, 1.0f);

	int count_fine = 1.0f / gridstep;
	int count_large = 1.0f / (4.0f * gridstep);

	if (count_fine > 0) {
		GWN_vertformat_clear(format);
		pos = GWN_vertformat_attr_add(format, "pos", GWN_COMP_F32, 2, GWN_FETCH_FLOAT);
		unsigned color = GWN_vertformat_attr_add(format, "color", GWN_COMP_F32, 3, GWN_FETCH_FLOAT);
		
		immBindBuiltinProgram(GPU_SHADER_2D_FLAT_COLOR);
		immBegin(GWN_PRIM_LINES, 4 * count_fine + 4 * count_large);
		
		float theme_color[3];
		UI_GetThemeColorShade3fv(TH_BACK, (int)(20.0f * (1.0f - blendfac)), theme_color);
		fac = 0.0f;
		
		/* the fine resolution level */
		for (int i = 0; i < count_fine; i++) {
			immAttrib3fv(color, theme_color);
			immVertex2f(pos, x1, y1 * (1.0f - fac) + y2 * fac);
			immAttrib3fv(color, theme_color);
			immVertex2f(pos, x2, y1 * (1.0f - fac) + y2 * fac);
			immAttrib3fv(color, theme_color);
			immVertex2f(pos, x1 * (1.0f - fac) + x2 * fac, y1);
			immAttrib3fv(color, theme_color);
			immVertex2f(pos, x1 * (1.0f - fac) + x2 * fac, y2);
			fac += gridstep;
		}

		if (count_large > 0) {
			UI_GetThemeColor3fv(TH_BACK, theme_color);
			fac = 0.0f;
			
			/* the large resolution level */
			for (int i = 0; i < count_large; i++) {
				immAttrib3fv(color, theme_color);
				immVertex2f(pos, x1, y1 * (1.0f - fac) + y2 * fac);
				immAttrib3fv(color, theme_color);
				immVertex2f(pos, x2, y1 * (1.0f - fac) + y2 * fac);
				immAttrib3fv(color, theme_color);
				immVertex2f(pos, x1 * (1.0f - fac) + x2 * fac, y1);
				immAttrib3fv(color, theme_color);
				immVertex2f(pos, x1 * (1.0f - fac) + x2 * fac, y2);
				fac += 4.0f * gridstep;
			}
		}

		immEnd();
		immUnbindProgram();
	}
}

/* If the area has overlapping regions, it returns visible rect for Region *ar */
/* rect gets returned in local region coordinates */
void ED_region_visible_rect(ARegion *ar, rcti *rect)
{
	ARegion *arn = ar;
	
	/* allow function to be called without area */
	while (arn->prev)
		arn = arn->prev;
	
	*rect = ar->winrct;
	
	/* check if a region overlaps with the current one */
	for (; arn; arn = arn->next) {
		if (ar != arn && arn->overlap) {
			if (BLI_rcti_isect(rect, &arn->winrct, NULL)) {
				if (ELEM(arn->alignment, RGN_ALIGN_LEFT, RGN_ALIGN_RIGHT)) {
					/* Overlap left, also check 1 pixel offset (2 regions on one side). */
					if (ABS(rect->xmin - arn->winrct.xmin) < 2) {
						rect->xmin = arn->winrct.xmax;
					}

					/* Overlap right. */
					if (ABS(rect->xmax - arn->winrct.xmax) < 2) {
						rect->xmax = arn->winrct.xmin;
					}
				}
				else if (ELEM(arn->alignment, RGN_ALIGN_TOP, RGN_ALIGN_BOTTOM)) {
					/* Same logic as above for vertical regions. */
					if (ABS(rect->ymin - arn->winrct.ymin) < 2) {
						rect->ymin = arn->winrct.ymax;
					}
					if (ABS(rect->ymax - arn->winrct.ymax) < 2) {
						rect->ymax = arn->winrct.ymin;
					}
				}
				else {
					BLI_assert(!"Region overlap with unknown alignment");
				}
			}
		}
	}
	BLI_rcti_translate(rect, -ar->winrct.xmin, -ar->winrct.ymin);
}

/* Cache display helpers */

void ED_region_cache_draw_background(const ARegion *ar)
{
	unsigned int pos = GWN_vertformat_attr_add(immVertexFormat(), "pos", GWN_COMP_I32, 2, GWN_FETCH_INT_TO_FLOAT);
	immBindBuiltinProgram(GPU_SHADER_2D_UNIFORM_COLOR);
	immUniformColor4ub(128, 128, 255, 64);
	immRecti(pos, 0, 0, ar->winx, 8 * UI_DPI_FAC);
	immUnbindProgram();
}

void ED_region_cache_draw_curfra_label(const int framenr, const float x, const float y)
{
	uiStyle *style = UI_style_get();
	int fontid = style->widget.uifont_id;
	char numstr[32];
	float font_dims[2] = {0.0f, 0.0f};

	/* frame number */
	BLF_size(fontid, 11.0f * U.pixelsize, U.dpi);
	BLI_snprintf(numstr, sizeof(numstr), "%d", framenr);

	BLF_width_and_height(fontid, numstr, sizeof(numstr), &font_dims[0], &font_dims[1]);

	unsigned int pos = GWN_vertformat_attr_add(immVertexFormat(), "pos", GWN_COMP_I32, 2, GWN_FETCH_INT_TO_FLOAT);
	immBindBuiltinProgram(GPU_SHADER_2D_UNIFORM_COLOR);
	immUniformThemeColor(TH_CFRAME);
	immRecti(pos, x, y, x + font_dims[0] + 6.0f, y + font_dims[1] + 4.0f);
	immUnbindProgram();

	UI_FontThemeColor(fontid, TH_TEXT);
	BLF_position(fontid, x + 2.0f, y + 2.0f, 0.0f);
	BLF_draw(fontid, numstr, sizeof(numstr));
}

void ED_region_cache_draw_cached_segments(const ARegion *ar, const int num_segments, const int *points, const int sfra, const int efra)
{
	if (num_segments) {
		unsigned int pos = GWN_vertformat_attr_add(immVertexFormat(), "pos", GWN_COMP_I32, 2, GWN_FETCH_INT_TO_FLOAT);
		immBindBuiltinProgram(GPU_SHADER_2D_UNIFORM_COLOR);
		immUniformColor4ub(128, 128, 255, 128);

		for (int a = 0; a < num_segments; a++) {
			float x1 = (float)(points[a * 2] - sfra) / (efra - sfra + 1) * ar->winx;
			float x2 = (float)(points[a * 2 + 1] - sfra + 1) / (efra - sfra + 1) * ar->winx;

			immRecti(pos, x1, 0, x2, 8 * UI_DPI_FAC);
			/* TODO(merwin): use primitive restart to draw multiple rects more efficiently */
		}

		immUnbindProgram();
	}
}

/**
 * Generate subscriptions for this region.
 */
void ED_region_message_subscribe(
        bContext *C,
        struct WorkSpace *workspace, struct Scene *scene,
        struct bScreen *screen, struct ScrArea *sa, struct ARegion *ar,
        struct wmMsgBus *mbus)
{
	if (ar->manipulator_map != NULL) {
		WM_manipulatormap_message_subscribe(C, ar->manipulator_map, ar, mbus);
	}

	if (BLI_listbase_is_empty(&ar->uiblocks)) {
		UI_region_message_subscribe(ar, mbus);
	}

	if (ar->type->message_subscribe != NULL) {
		ar->type->message_subscribe(C, workspace, scene, screen, sa, ar, mbus);
	}
}

int ED_region_snap_size_test(const ARegion *ar)
{
	/* Use a larger value because toggling scrollbars can jump in size. */
	const int snap_match_threshold = 16;
	if (ar->type->snap_size != NULL) {
		return ((((ar->sizex - ar->type->snap_size(ar, ar->sizex, 0)) <= snap_match_threshold) << 0) |
		        (((ar->sizey - ar->type->snap_size(ar, ar->sizey, 1)) <= snap_match_threshold) << 1));
	}
	return 0;
}

bool ED_region_snap_size_apply(ARegion *ar, int snap_flag)
{
	bool changed = false;
	if (ar->type->snap_size != NULL) {
		if (snap_flag & (1 << 0)) {
			short snap_size = ar->type->snap_size(ar, ar->sizex, 0);
			if (snap_size != ar->sizex) {
				ar->sizex = snap_size;
				changed = true;
			}
		}
		if (snap_flag & (1 << 1)) {
			short snap_size = ar->type->snap_size(ar, ar->sizey, 1);
			if (snap_size != ar->sizey) {
				ar->sizey = snap_size;
				changed = true;
			}
		}
	}
	return changed;
}
