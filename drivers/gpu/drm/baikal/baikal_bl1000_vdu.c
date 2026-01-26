/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2023 Baikal Electronics, JSC
 *
 * Author: Pavel Parkhomenko <Pavel.Parkhomenko@baikalelectronics.ru>
 *
 */

#include <linux/of_device.h>
#include <linux/of_graph.h>
#include <linux/platform_device.h>

#include <drm/drm_atomic_helper.h>
#include <drm/drm_debugfs.h>
#include <drm/drm_drv.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_fbdev_dma.h>
#include <drm/drm_fb_dma_helper.h>
#include <drm/drm_fb_helper.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_vblank.h>

#include "baikal_bl1000_dp.h"
#include "baikal_bl1000_drm.h"
#include "baikal_bl1000_vdu.h"

static int baikal_vdu_l1000_probe(struct platform_device *pdev, struct drm_device *drm, struct baikal_vdu_crossbar *crossbar)
{
	struct device *dev = &pdev->dev;
	struct baikal_vdu_private *dp0 = &crossbar->vdu[0];
	struct baikal_vdu_private *dp1 = &crossbar->vdu[1];
	struct drm_mode_config *mode_config;
	int ret;

	dp0->ops = crossbar->ops;
	dp0->drm = &crossbar->drm;
	baikal_vdu_set_name(dp0, CRTC_DP0, "dp0");
	dp1->ops = crossbar->ops;
	dp1->drm = &crossbar->drm;
	baikal_vdu_set_name(dp1, CRTC_DP1, "dp1");

	baikal_vdu_remove_efifb(drm);

	ret = drmm_mode_config_init(drm);
	if (ret)
		return ret;
	mode_config = &drm->mode_config;
	mode_config->funcs = &mode_config_funcs;
	mode_config->max_width = BAIKAL_VDU_L1000_XRES_MAX;
	mode_config->max_height = BAIKAL_VDU_L1000_YRES_MAX;

	dp0->off = dp0_off;
	dp0->ready = baikal_vdu_resources_init(pdev, dp0);
	dp1->off = dp1_off;
	dp1->ready = baikal_vdu_resources_init(pdev, dp1);

	dp0->ready = dp0->ready & !dp0->off;
	dp1->ready = dp1->ready & !dp1->off;

	dev_info(dev, "%s output %s\n", dp0->name, dp0->ready ? "enabled" : "disabled");
	dev_info(dev, "%s output %s\n", dp1->name, dp1->ready ? "enabled" : "disabled");

	if (dp0->ready || dp1->ready) {
		baikal_dp_probe(pdev);
		/* TODO refactor for 2 VDUs */
		ret = drm_vblank_init(drm, 1);
		if (ret != 0) {
			dev_err(dev, "Failed to init vblank\n");
			goto out_drm;
		}
		/* TODO refactor for 2 VDUs */
		crossbar->ops->irq_on(dp0);
		drm_mode_config_reset(drm);
		drm_kms_helper_poll_init(drm);
		ret = drm_dev_register(drm, 0);
		if (ret) {
			dev_err(dev, "failed to register DRM device\n");
			goto out_config;
		}
		drm_fbdev_dma_setup(drm, 32);
#if defined(CONFIG_DEBUG_FS)
		if (dp0->ready) {
			drm_debugfs_create_files(dp0->debugfs_list,
					ARRAY_SIZE(dp0->debugfs_list),
					drm->primary->debugfs_root, drm->primary);
		}
		if (dp1->ready) {
			drm_debugfs_create_files(dp1->debugfs_list,
					ARRAY_SIZE(dp1->debugfs_list),
					drm->primary->debugfs_root, drm->primary);
		}
#endif
		return 0;
	} else {
		dev_err(dev, "no active outputs configured\n");
		ret = -ENODEV;
	}
out_config:
	drm_mode_config_cleanup(drm);
out_drm:
	dev_err(dev, "failed to probe: %d\n", ret);
	return ret;
}

/*static void baikal_vdu_l1000_switch_off(struct baikal_vdu_private *priv)
{
	// TODO
}

static void baikal_vdu_l1000_switch_on(struct baikal_vdu_private *priv)
{
	// TODO
}

static void baikal_vdu_l1000_irq_off(struct baikal_vdu_private *priv)
{
	// TODO
}*/

static void baikal_vdu_l1000_irq_on(struct baikal_vdu_private *priv)
{
	baikal_vdu_write(priv, INT_CTRL, ~0);
	baikal_vdu_write(priv, INT_MASK, ~INT_VFP_START);
}

static void baikal_vdu_l1000_post_allocate_resources(struct baikal_vdu_private *priv)
{
	/* TODO */
}

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

static const struct baikal_vdu_reg_defs baikal_vdu_l1000_reg_defs[] = {
	REGDEF(VDU_CONF),
	REGDEF(INT_CTRL),
	REGDEF(INT_MASK),
	REGDEF(INT_STAT),
	REGDEF(PWM_CTRL_0),
	REGDEF(PWM_CTRL_1),
	REGDEF(POLAR_CTRL),
	REGDEF(TE_EN),
	REGDEF(TE_CTRL_0),
	REGDEF(TE_CTRL_1),
	REGDEF(TE_CTRL_2),
	REGDEF(TE_CTRL_3),
	REGDEF(PIPE_DMA_ADDR_0(0)),
	REGDEF(PIPE_DMA_ADDR_1(0)),
	REGDEF(PIPE_DMA_CTRL(0)),
	REGDEF(PIPE_AXI_PARAM(0)),
	REGDEF(PIPE_WINDOW_X(0)),
	REGDEF(PIPE_WINDOW_Y(0)),
	REGDEF(PIPE_WINDOW_EN(0)),
	REGDEF(PIPE_PU_CTRL(0)),
};

static const struct drm_plane_helper_funcs baikal_vdu_l1000_primary_plane_helper_funcs = {
	.atomic_update = baikal_vdu_l1000_primary_plane_atomic_update,
};

const struct baikal_vdu_ops baikal_vdu_l1000_ops = {
	.irq = baikal_vdu_l1000_irq,
	.probe = baikal_vdu_l1000_probe,
	//.switch_on = baikal_vdu_l1000_switch_on,
	//.switch_off = baikal_vdu_l1000_switch_off,
	.irq_on = baikal_vdu_l1000_irq_on,
	//.irq_off = baikal_vdu_l1000_irq_off,
	.post_allocate_resources = baikal_vdu_l1000_post_allocate_resources,
	.crtc_create = baikal_vdu_l1000_crtc_create,
	.primary_plane_helper_funcs = &baikal_vdu_l1000_primary_plane_helper_funcs,
	.reg_defs = baikal_vdu_l1000_reg_defs,
	.reg_defs_size = ARRAY_SIZE(baikal_vdu_l1000_reg_defs),
};
