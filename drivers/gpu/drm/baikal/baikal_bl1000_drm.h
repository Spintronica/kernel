/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2019-2023 Baikal Electronics, JSC
 *
 * Author: Pavel Parkhomenko <Pavel.Parkhomenko@baikalelectronics.ru>
 *
 */

#ifndef __BAIKAL_VDU_DRM_H__
#define __BAIKAL_VDU_DRM_H__

#include <linux/backlight.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_debugfs.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_simple_kms_helper.h>

#include "baikal_bl1000_dp.h"

#define REGDEF(reg) { reg, #reg }

#define connector_to_baikal_vdu(x) \
	container_of(x, struct baikal_vdu_private, connector)
#define crtc_to_baikal_vdu(x) \
	container_of(x, struct baikal_vdu_private, crtc)
#define drm_to_baikal_vdu_crossbar(x) \
	container_of(x, struct baikal_vdu_crossbar, drm)

#define VDU_NAME_LEN 5

#define MAX_VDUS	3

#define VDU_M1000	0
#define VDU_L1000	1

struct baikal_vdu_ops;

struct baikal_vdu_reg_defs {
	u32 reg;
	const char *name;
};

struct baikal_vdu_private {
	struct drm_device *drm;
	struct drm_crtc crtc;
	struct drm_encoder encoder;
	struct drm_connector connector;
	struct drm_plane primary;
	struct clk *clk;
	void *regs;
	int irq;
	int index;
	char name[VDU_NAME_LEN];
	char irq_name[VDU_NAME_LEN + 4];
	char pclk_name[VDU_NAME_LEN + 5];
	char regs_name[VDU_NAME_LEN + 5];
	spinlock_t lock;
	u32 counters[20];
	u32 errors[13];
	atomic_t vblank_counter;
	int num_lanes;
	int data_mapping;
	struct drm_info_list debugfs_list[1];
	const struct baikal_vdu_ops *ops;
	int off;
	int ready;

	/* Backlight */
	struct gpio_desc *enable_gpio;
	struct backlight_device *bl_dev;
	int min_brightness;
	int brightness_step;
	bool brightness_on;
};

struct baikal_vdu_crossbar {
	struct drm_device drm;
	struct baikal_dp *dp;
	const struct baikal_vdu_ops *ops;
	struct baikal_vdu_private vdu[MAX_VDUS];
};

struct baikal_vdu_ops {
	irqreturn_t (*irq) (int irq, void *data);
	int (*probe) (struct platform_device *pdev, struct drm_device *drm, struct baikal_vdu_crossbar *crossbar);
	void (*switch_off) (struct baikal_vdu_private *priv);
	void (*switch_on) (struct baikal_vdu_private *priv);
	void (*irq_off) (struct baikal_vdu_private *priv);
	void (*irq_on) (struct baikal_vdu_private *priv);
	void (*post_allocate_resources) (struct baikal_vdu_private *priv);
	int (*crtc_create) (struct baikal_vdu_private *priv);
	const struct drm_plane_helper_funcs *primary_plane_helper_funcs;
	const struct baikal_vdu_reg_defs *reg_defs;
	int reg_defs_size;
};

/* Generic functions */
inline void baikal_vdu_write(struct baikal_vdu_private *priv,
				 unsigned int reg, u32 value);
inline u32 baikal_vdu_read(struct baikal_vdu_private *priv,
			       unsigned int reg);
void baikal_vdu_set_name(struct baikal_vdu_private *priv, int index, const char *name);
int baikal_vdu_resources_init(struct platform_device *pdev, struct baikal_vdu_private *priv);
int baikal_vdu_remove_efifb(struct drm_device *dev);

/* CRTC Functions */
irqreturn_t baikal_vdu_l1000_irq(int irq, void *data);
int baikal_vdu_l1000_crtc_create(struct baikal_vdu_private *priv);
void baikal_vdu_crtc_helper_atomic_flush(struct drm_crtc *crtc,
					   struct drm_atomic_state *old_state);
int baikal_bl1000_primary_plane_init(struct baikal_vdu_private *priv);

/* Backlight Functions */
int baikal_vdu_backlight_init(struct drm_device *drm);

/* Debugfs functions */
void baikal_vdu_dp0_debugfs_init(struct drm_minor *minor);
void baikal_vdu_dp1_debugfs_init(struct drm_minor *minor);
int baikal_bl1000_vdu_debugfs_regs(struct seq_file *m, void *unused);

/* Structures and variables */
extern struct drm_mode_config_funcs mode_config_funcs;
extern int dp0_off;
extern int dp1_off;
extern int no_edid;

#endif /* __BAIKAL_VDU_DRM_H__ */
