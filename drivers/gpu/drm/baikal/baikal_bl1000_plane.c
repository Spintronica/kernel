// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2019-2023 Baikal Electronics, JSC
 *
 * Author: Pavel Parkhomenko <Pavel.Parkhomenko@baikalelectronics.ru>
 *
 */

#include <drm/drm_atomic_helper.h>
#include <drm/drm_fb_dma_helper.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_plane_helper.h>

#include "baikal_bl1000_drm.h"
#include "baikal_bl1000_vdu.h"

static const struct drm_plane_funcs baikal_vdu_plane_funcs = {
	.update_plane = drm_atomic_helper_update_plane,
	.disable_plane = drm_atomic_helper_disable_plane,
	.reset = drm_atomic_helper_plane_reset,
	.destroy = drm_plane_cleanup,
	.atomic_duplicate_state = drm_atomic_helper_plane_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_plane_destroy_state,
};

static const struct baikal_vdu_drm_format baikal_vdu_pu_ctrl_table[] = {
	{pipe_pu_ctrl_reg(1, 0, 0, 4), DRM_FORMAT_RGB565},
	{pipe_pu_ctrl_reg(1, 0, 1, 4), DRM_FORMAT_BGR565},
	{pipe_pu_ctrl_reg(1, 0, 1, 2), DRM_FORMAT_XBGR1555},
	{pipe_pu_ctrl_reg(1, 1, 1, 2), DRM_FORMAT_ABGR1555},
	{pipe_pu_ctrl_reg(1, 0, 0, 2), DRM_FORMAT_XRGB1555},
	{pipe_pu_ctrl_reg(1, 1, 0, 2), DRM_FORMAT_ARGB1555},
	{pipe_pu_ctrl_reg(2, 0, 1, 0), DRM_FORMAT_XBGR8888},
	{pipe_pu_ctrl_reg(2, 1, 1, 0), DRM_FORMAT_ABGR8888},
	{pipe_pu_ctrl_reg(2, 0, 0, 0), DRM_FORMAT_XRGB8888},
	{pipe_pu_ctrl_reg(2, 1, 0, 0), DRM_FORMAT_ARGB8888},
};

static void baikal_vdu_l1000_set_pxl_fmt(struct baikal_vdu_private *priv,
		struct drm_framebuffer *fb)
{
	uint32_t pixel_format = fb->format->format;
	u32 format = DRM_FORMAT_INVALID;
	int i;
	u32 reg = 0;

	for (i = 0; i < ARRAY_SIZE(baikal_vdu_pu_ctrl_table); i++) {
		if (baikal_vdu_pu_ctrl_table[i].format == pixel_format) {
			format = baikal_vdu_pu_ctrl_table[i].format;
			reg = baikal_vdu_pu_ctrl_table[i].reg;
			break;
		}
	}

	if (WARN_ON(format == DRM_FORMAT_INVALID))
		return;

	baikal_vdu_write(priv, PIPE_PU_CTRL(0), reg);
}

static void baikal_vdu_l1000_primary_plane_atomic_update(struct drm_plane *plane,
					      struct drm_atomic_state *old_state)
{
	struct baikal_vdu_private *priv;
	struct drm_gem_dma_object *gem;
	struct drm_plane_state *state = plane->state;
	struct drm_crtc *crtc = state->crtc;
	struct drm_framebuffer *fb = state->fb;
	u16 x_start, x_end, y_start, y_end;
	/*u8 cpp = fb->format->cpp[0];*/

	if (!fb)
		return;

	priv = crtc_to_baikal_vdu(crtc);
	x_start = fb->offsets[0] % fb->pitches[0] - (state->src_x >> 16);
	x_end = x_start + fb->width - 1;
	y_start = fb->offsets[0] / fb->pitches[0] - (state->src_y >> 16);
	y_end = y_start + fb->height - 1;
	baikal_vdu_write(priv, PIPE_WINDOW_X(0), pipe_window_reg(x_start, x_end));
	baikal_vdu_write(priv, PIPE_WINDOW_Y(0), pipe_window_reg(y_start, y_end));
	gem = drm_fb_dma_get_gem_obj(fb, 0);
	baikal_vdu_write(priv, PIPE_DMA_ADDR_0(0), gem->dma_addr + fb->offsets[0]);
	baikal_vdu_write(priv, PIPE_DMA_CTRL(0),
			((PIPE_DMA_CTRL_WORDS(16) & PIPE_DMA_CTRL_WORDS_MASK) | \
			 (PIPE_DMA_CTRL_OUTST(8) & PIPE_DMA_CTRL_OUTST_MASK)));
	baikal_vdu_l1000_set_pxl_fmt(priv, fb);
	baikal_vdu_write(priv, PIPE_WINDOW_EN(0), 1);
}

static const struct drm_plane_helper_funcs baikal_vdu_l1000_primary_plane_helper_funcs = {
	.atomic_update = baikal_vdu_l1000_primary_plane_atomic_update,
};

int baikal_bl1000_primary_plane_init(struct baikal_vdu_private *priv)
{
	struct drm_device *drm = priv->drm;
	struct drm_plane *plane = &priv->primary;
	static const u32 formats[] = {
		DRM_FORMAT_ABGR8888,
		DRM_FORMAT_XBGR8888,
		DRM_FORMAT_ARGB8888,
		DRM_FORMAT_XRGB8888,
		DRM_FORMAT_BGR565,
		DRM_FORMAT_RGB565,
		DRM_FORMAT_ABGR1555,
		DRM_FORMAT_XBGR1555,
		DRM_FORMAT_ARGB1555,
		DRM_FORMAT_XRGB1555,
	};
	int ret;

	ret = drm_universal_plane_init(drm, plane, 0,
				       &baikal_vdu_plane_funcs,
				       formats,
				       ARRAY_SIZE(formats),
				       NULL,
				       DRM_PLANE_TYPE_PRIMARY,
				       NULL);
	if (ret)
		return ret;

	drm_plane_helper_add(plane, &baikal_vdu_l1000_primary_plane_helper_funcs);

	return 0;
}


static int baikal_vdu_l1000_cursor_plane_atomic_check(struct drm_plane *plane,
						      struct drm_atomic_state *state)
{
	struct drm_plane_state *new_plane_state =
				drm_atomic_get_new_plane_state(state, plane);
	struct drm_crtc_state *new_crtc_state;
	struct drm_crtc *crtc = new_plane_state->crtc;
	int ret;

	if (!crtc)
		return 0;

	if (WARN_ON(!new_plane_state->fb))
		return -EINVAL;

	new_crtc_state = drm_atomic_get_new_crtc_state(state, crtc);
	if (WARN_ON(!new_crtc_state))
		return -EINVAL;

	if (new_plane_state->crtc_w > 64 || new_plane_state->crtc_h > 64) {
		drm_dbg_atomic(plane->dev, "cursor size %dx%d exceeds maximum 64x64\n",
			       new_plane_state->crtc_w,
			       new_plane_state->crtc_h);
		return -EINVAL;
	}

	if (new_plane_state->crtc_w != new_plane_state->src_w >> 16 ||
			new_plane_state->crtc_h != new_plane_state->src_h >> 16) {
		drm_dbg_atomic(plane->dev, "cursor scaling not allowed\n");
		return -EINVAL;
	}

	ret = drm_atomic_helper_check_plane_state(new_plane_state,
						  new_crtc_state,
						  DRM_PLANE_NO_SCALING,
						  DRM_PLANE_NO_SCALING,
						  true,
						  true);
        return ret;
}

static void baikal_vdu_l1000_cursor_plane_atomic_update(struct drm_plane *plane,
							struct drm_atomic_state *old_state)
{
	struct baikal_vdu_private *priv;
	struct drm_gem_dma_object *gem;
	struct drm_plane_state *state = plane->state;
	struct drm_crtc *crtc = state->crtc;
	struct drm_framebuffer *fb = state->fb;
	u32 reg;

	if (!fb)
		return;

	priv = crtc_to_baikal_vdu(crtc);

	baikal_vdu_write(priv, CURSOR_CTRL_3,
			((CURSOR_CTRL_3_WORDS(16) & CURSOR_CTRL_3_WORDS_MASK) | \
			 (CURSOR_CTRL_3_OUTST(8) & CURSOR_CTRL_3_OUTST_MASK)));
	baikal_vdu_write(priv, CURSOR_CTRL_4,
			CURSOR_CTRL_4_X_START(state->crtc_x) | CURSOR_CTRL_4_Y_START(state->crtc_y));

	reg = baikal_vdu_read(priv, CURSOR_CTRL_0);
	reg &= CURSOR_CTRL_0_SIZE_MASK;
	reg |= CURSOR_CTRL_0_ALPHA;

	switch (state->crtc_w) {
	case 64:
		reg |= CURSOR_CTRL_0_SIZE_64x64;
		break;
	case 48:
		reg |= CURSOR_CTRL_0_SIZE_48x48;
		break;
	case 32:
		reg |= CURSOR_CTRL_0_SIZE_32x32;
		break;
	case 24:
		reg |= CURSOR_CTRL_0_SIZE_24x24;
		break;
	}

	gem = drm_fb_dma_get_gem_obj(fb, 0);
	baikal_vdu_write(priv, CURSOR_CTRL_1, gem->dma_addr + fb->offsets[0]);

	reg |= CURSOR_CTRL_0_ENABLE;
	baikal_vdu_write(priv, CURSOR_CTRL_0, reg);
}

static void baikal_vdu_l1000_cursor_plane_atomic_disable(struct drm_plane *plane,
							 struct drm_atomic_state *old_state)
{
	struct baikal_vdu_private *priv;
	struct drm_plane_state *state = plane->state;
	struct drm_crtc *crtc = state->crtc;
	u32 reg;

	if (!crtc)
		return;

	priv = crtc_to_baikal_vdu(crtc);
	reg = baikal_vdu_read(priv, CURSOR_CTRL_0);
	reg &= ~CURSOR_CTRL_0_ENABLE;
	baikal_vdu_write(priv, CURSOR_CTRL_0, reg);
}

static const struct drm_plane_helper_funcs baikal_vdu_l1000_cursor_plane_helper_funcs = {
	.atomic_update = baikal_vdu_l1000_cursor_plane_atomic_update,
	.atomic_check = baikal_vdu_l1000_cursor_plane_atomic_check,
	.atomic_disable = baikal_vdu_l1000_cursor_plane_atomic_disable,
};

int baikal_bl1000_cursor_plane_init(struct baikal_vdu_private *priv)
{
	struct drm_device *drm = priv->drm;
	struct drm_plane *cursor = &priv->cursor;
	static const u32 formats[] = {
		DRM_FORMAT_ARGB8888,
	};
	int ret;

	ret = drm_universal_plane_init(drm, cursor, drm_crtc_mask(&priv->crtc),
				       &baikal_vdu_plane_funcs,
				       formats,
				       ARRAY_SIZE(formats),
				       NULL,
				       DRM_PLANE_TYPE_CURSOR,
				       "cursor-plane-%d",
				       drm_crtc_index(&priv->crtc));
	if (ret)
		return ret;

	drm_plane_helper_add(cursor, &baikal_vdu_l1000_cursor_plane_helper_funcs);

	return 0;
}
