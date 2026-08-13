/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2023 Baikal Electronics, JSC
 *
 * Author: Pavel Parkhomenko <Pavel.Parkhomenko@baikalelectronics.ru>
 *
 */

#include <linux/firmware/baikal/baikal-smc.h>
#include <linux/platform_device.h>

#include <drm/drm_debugfs.h>
#include <drm/drm_drv.h>
#include <drm/drm_fbdev_dma.h>
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

	dp0->enable_vblank = false;
	dp0->ops = crossbar->ops;
	dp0->drm = &crossbar->drm;
	baikal_vdu_set_name(dp0, CRTC_DP0, "dp0");
	dp1->enable_vblank = false;
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
	baikal_vdu_write(priv, INT_MASK, ~(INT_VS_START | INT_ANY_ERROR));
}

static void baikal_vdu_l1000_post_allocate_resources(struct baikal_vdu_private *priv)
{
	/* TODO */
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

const struct baikal_vdu_ops baikal_vdu_l1000_ops = {
	.irq = baikal_vdu_l1000_irq,
	.probe = baikal_vdu_l1000_probe,
	//.switch_on = baikal_vdu_l1000_switch_on,
	//.switch_off = baikal_vdu_l1000_switch_off,
	.irq_on = baikal_vdu_l1000_irq_on,
	//.irq_off = baikal_vdu_l1000_irq_off,
	.post_allocate_resources = baikal_vdu_l1000_post_allocate_resources,
	.crtc_create = baikal_vdu_l1000_crtc_create,
	.reg_defs = baikal_vdu_l1000_reg_defs,
	.reg_defs_size = ARRAY_SIZE(baikal_vdu_l1000_reg_defs),
};
