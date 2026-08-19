// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2025 Baikal Electronics, JSC
 *
 * Author: Pavel Parkhomenko <Pavel.Parkhomenko@baikalelectronics.ru>
 *
 */

#include <linux/clk.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/of_graph.h>
#include <linux/platform_device.h>
#include <linux/version.h>

#include <drm/drm_aperture.h>
#include <drm/drm_drv.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_fb_helper.h>
#include <drm/drm_of.h>
#include <drm/drm_vblank.h>

#include "baikal_bl1000_drm.h"

#define DRIVER_NAME	"pl111"
#define DRIVER_DESC	"Baikal VDU DRM driver"
#define DRIVER_DATE	"20251201"

int dp0_off = 0;
int dp1_off = 0;
int no_edid = 0;
int hw_cursor = 1;
int max_pix_clock = 600000;
int max_width = 3840;
int max_height = 2160;

extern const struct baikal_vdu_ops baikal_vdu_l1000_ops;

static struct drm_driver vdu_drm_driver;

struct drm_mode_config_funcs mode_config_funcs = {
	.fb_create = drm_gem_fb_create,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

inline void baikal_vdu_write(struct baikal_vdu_private *priv,
				 unsigned int reg, u32 value)
{
	writel(value, priv->regs + reg);
}

inline u32 baikal_vdu_read(struct baikal_vdu_private *priv,
			       unsigned int reg)
{
	u32 value = readl(priv->regs + reg);
	return value;
}

int baikal_vdu_remove_efifb(struct drm_device *dev)
{
	int err;
	err = drm_aperture_remove_framebuffers(&vdu_drm_driver);
	if (err)
		dev_warn(dev->dev, "failed to remove firmware framebuffer\n");
	return err;
}

void baikal_vdu_set_name(struct baikal_vdu_private *priv, int index, const char *name)
{
	char *c;
	int len = sizeof(priv->name) / sizeof(priv->name[0]) - 1;
	strncpy(priv->name, name, len);
	for (c = priv->name; c < priv->name + len && *c; c++) {
		*c = toupper(*c);
	}
	sprintf(priv->irq_name, "%s_irq", name);
	sprintf(priv->pclk_name, "%s_pclk", name);
	sprintf(priv->regs_name, "%s_regs", name);
	priv->debugfs_list[0].name = priv->regs_name;
	priv->debugfs_list[0].show = baikal_bl1000_vdu_debugfs_regs;
	priv->debugfs_list[0].driver_features = 0;
	priv->index = index;
}

static int baikal_vdu_modeset_init(struct baikal_vdu_private *priv)
{
	struct drm_device *dev = priv->drm;
	int ret = 0;

	if (priv == NULL)
		return -EINVAL;

	ret = baikal_bl1000_primary_plane_init(priv);
	if (ret != 0) {
		dev_err(dev->dev, "%s: failed to init primary plane\n", priv->name);
		return ret;
	}

	if (hw_cursor) {
		ret = baikal_bl1000_cursor_plane_init(priv);
		if (ret != 0) {
			dev_err(dev->dev, "%s: failed to init cursor plane\n", priv->name);
			return ret;
		}
	}

	ret = priv->ops->crtc_create(priv);
	if (ret) {
		dev_err(dev->dev, "%s: failed to create CRTC\n", priv->name);
		return ret;
	}

	return ret;
}

static int baikal_vdu_allocate_resources(struct platform_device *pdev,
		struct baikal_vdu_private *priv)
{
	struct device *dev = &pdev->dev;
	struct resource *mem;
	int ret;

	if (!(mem = platform_get_resource_byname(pdev, IORESOURCE_MEM, priv->regs_name))) {
		dev_err(dev, "%s %s: no MMIO resource specified\n", __func__, priv->name);
		return -EINVAL;
	}

	priv->regs = devm_ioremap_resource(dev, mem);
	if (IS_ERR(priv->regs)) {
		dev_err(dev, "%s %s: MMIO allocation failed\n", __func__, priv->name);
		return PTR_ERR(priv->regs);
	}

	if (priv->off) {
		priv->ops->switch_off(priv);
		return -EPERM;
	} else {
		ret = baikal_vdu_modeset_init(priv);
		if (ret) {
			dev_err(dev, "%s %s: failed to init modeset\n", __func__, priv->name);
			if (ret == -ENODEV) {
				priv->ops->switch_off(priv);
			}
			return ret;
		} else {
			if (priv->ops->post_allocate_resources) {
				priv->ops->post_allocate_resources(priv);
			}
			return 0;
		}
	}
}

static int baikal_vdu_allocate_irq(struct platform_device *pdev,
	       struct baikal_vdu_private *priv)
{
	struct device *dev = &pdev->dev;
	int ret;

	if (!priv->ops->irq)
		return 0;
	if (!(priv->irq = platform_get_irq_byname(pdev, priv->irq_name))) {
		dev_err(dev, "%s %s: no IRQ resource specified\n", __func__, priv->name);
		return -EINVAL;
	}

	/* turn off interrupts before requesting the irq */
	if (priv->ops->irq_off)
		priv->ops->irq_off(priv);
	ret = request_threaded_irq(priv->irq,
			NULL,
			priv->ops->irq,
			IRQF_ONESHOT,
			dev->driver->name,
			priv);
	if (ret != 0)
		dev_err(dev, "%s %s: IRQ %d allocation failed\n", __func__, priv->name, priv->irq);
	return ret;
}

static int baikal_vdu_allocate_clk(struct baikal_vdu_private *priv)
{
	priv->clk = devm_clk_get_enabled(priv->drm->dev, priv->pclk_name);
	if (IS_ERR(priv->clk)) {
		dev_err(priv->drm->dev, "%s: unable to get %s, err %ld\n", priv->name, priv->pclk_name, PTR_ERR(priv->clk));
		return PTR_ERR(priv->clk);
	} else
		return clk_prepare_enable(priv->clk);
}

int baikal_vdu_resources_init(struct platform_device *pdev, struct baikal_vdu_private *priv)
{
	int ret;

	priv->max_pix_clock = max_pix_clock;
	priv->max_width = max_width;
	priv->max_height = max_height;

	ret = baikal_vdu_allocate_resources(pdev, priv);
	if (ret)
		return 0;
	ret = baikal_vdu_allocate_irq(pdev, priv);
	if (ret)
		return 0;
	ret = baikal_vdu_allocate_clk(priv);
	if (ret) {
		priv->ops->irq_off(priv);
		free_irq(priv->irq, priv->drm->dev);
		return 0;
	} else {
		return 1;
	}
}

static int baikal_dumb_create(struct drm_file *file, struct drm_device *dev,
			     struct drm_mode_create_dumb *args)
{
	/*
	 * We need 64bytes aligned stride, and PAGE aligned size
	 */
	args->pitch = ALIGN(DIV_ROUND_UP(args->width * args->bpp, 8), SZ_64);
	args->size = PAGE_ALIGN(args->pitch * args->height);

	return drm_gem_dma_dumb_create_internal(file, dev, args);
}

DEFINE_DRM_GEM_DMA_FOPS(baikal_drm_fops);

static struct drm_driver vdu_drm_driver = {
	.driver_features = DRIVER_GEM |	DRIVER_MODESET | DRIVER_ATOMIC,
	DRM_GEM_DMA_DRIVER_OPS_WITH_DUMB_CREATE(baikal_dumb_create),
	.fops = &baikal_drm_fops,
	.name = DRIVER_NAME,
	.desc = DRIVER_DESC,
	.date = DRIVER_DATE,
	.major = 3,
	.minor = 0,
	.patchlevel = 0,
};

static int baikal_vdu_drm_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct baikal_vdu_crossbar *crossbar;
	const struct baikal_vdu_ops *ops;
	struct drm_device *drm;

	if (dma_set_mask_and_coherent(dev, DMA_BIT_MASK(64)))
		return -EIO;

	crossbar = devm_drm_dev_alloc(dev, &vdu_drm_driver,
                  struct baikal_vdu_crossbar, drm);
	if (IS_ERR(crossbar))
		return PTR_ERR(crossbar);
	ops = &baikal_vdu_l1000_ops;
	crossbar->ops = ops;
	drm = &crossbar->drm;
	platform_set_drvdata(pdev, drm);
	return ops->probe(pdev, drm, crossbar);
}

static void baikal_vdu_drm_remove(struct platform_device *pdev)
{
	struct drm_device *drm = platform_get_drvdata(pdev);
	struct baikal_vdu_crossbar *crossbar = drm_to_baikal_vdu_crossbar(drm);

	drm_dev_unregister(drm);
	drm_mode_config_cleanup(drm);
	if (crossbar->vdu[0].irq)
		free_irq(crossbar->vdu[0].irq, drm->dev);
	if (crossbar->vdu[1].irq)
		free_irq(crossbar->vdu[1].irq, drm->dev);
}

int baikal_bl1000_vdu_debugfs_regs(struct seq_file *m, void *unused)
{
	struct drm_info_node *node = (struct drm_info_node *)m->private;
	struct drm_device *dev = node->minor->dev;
	struct baikal_vdu_crossbar *crossbar = drm_to_baikal_vdu_crossbar(dev);
	char *filename = m->file->f_path.dentry->d_iname;
	struct baikal_vdu_private *priv = NULL;
	int i;

	if (!strcmp(crossbar->vdu[0].regs_name, filename))
		priv = &crossbar->vdu[0];
	if (!strcmp(crossbar->vdu[1].regs_name, filename))
		priv = &crossbar->vdu[1];

	if (!priv || !priv->regs)
		return 0;

	for (i = 0; i < priv->ops->reg_defs_size; i++) {
		seq_printf(m, "%s (0x%04x): 0x%08x\n",
			   priv->ops->reg_defs[i].name, priv->ops->reg_defs[i].reg,
			   readl(priv->regs + priv->ops->reg_defs[i].reg));
	}
	for (i = 0; i < ARRAY_SIZE(priv->errors); i++) {
		seq_printf(m, "ERROR[%d]: 0x%08x\n", i, priv->errors[i]);
	}
	for (i = 0; i < ARRAY_SIZE(priv->counters); i++) {
		seq_printf(m, "COUNTER[%d]: 0x%08x\n", i, priv->counters[i]);
	}

	return 0;
}

static const struct of_device_id baikal_vdu_of_match[] = {
	{ .compatible = "baikal,bl1000-vdu" },
	{ },
};

MODULE_DEVICE_TABLE(of, baikal_vdu_of_match);

static struct platform_driver baikal_bl1000_vdu_driver = {
	.probe  = baikal_vdu_drm_probe,
	.remove = baikal_vdu_drm_remove,
	.driver = {
		.name   = DRIVER_NAME,
		.of_match_table = baikal_vdu_of_match,
	},
};

module_param(dp0_off, int, 0644);
module_param(dp1_off, int, 0644);
module_param(no_edid, int, 0644);
module_param(hw_cursor, int, 0644);
module_param(max_pix_clock, int, 0644);
module_param(max_width, int, 0644);
module_param(max_height, int, 0644);

module_platform_driver(baikal_bl1000_vdu_driver);

MODULE_AUTHOR("Pavel Parkhomenko <Pavel.Parkhomenko@baikalelectronics.ru>");
MODULE_DESCRIPTION("Baikal Electronics BE-L1000 Video Display Unit (VDU) DRM Driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:" DRIVER_NAME);
