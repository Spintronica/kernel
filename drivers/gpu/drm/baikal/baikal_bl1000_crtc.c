// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2023 Baikal Electronics, JSC
 *
 * Author: Pavel Parkhomenko <Pavel.Parkhomenko@baikalelectronics.ru>
 *
 */

/**
 * baikal_vdu_l1000_crtc.c
 * Implementation of the CRTC functions for Baikal Electronics BE-L1000 VDU driver
 */

#include <drm/drm_vblank.h>
#include <linux/clk.h>

#include "baikal_bl1000_dp.h"
#include "baikal_bl1000_drm.h"
#include "baikal_bl1000_vdu.h"

static int baikal_enable_vblank(struct drm_crtc *crtc)
{
	struct baikal_vdu_private *priv = crtc_to_baikal_vdu(crtc);

	baikal_vdu_write(priv, INT_MASK, ~(INT_VFP_START | INT_ANY_ERROR));
	return 0;
}

static void baikal_disable_vblank(struct drm_crtc *crtc)
{
	struct baikal_vdu_private *priv = crtc_to_baikal_vdu(crtc);

	baikal_vdu_write(priv, INT_CTRL, ~0);
	baikal_vdu_write(priv, INT_MASK, ~INT_ANY_ERROR);
}

static u32 baikal_get_vblank_counter(struct drm_crtc *crtc)
{
	struct baikal_vdu_private *priv = crtc_to_baikal_vdu(crtc);

	return atomic_read(&priv->vblank_counter);
}

irqreturn_t baikal_vdu_l1000_irq(int irq, void *data)
{
	struct baikal_vdu_private *priv = data;
	struct baikal_vdu_crossbar *crossbar;
	struct baikal_dp *dp;
	irqreturn_t status = IRQ_NONE;
	u32 raw_stat;
	u32 irq_stat;
	u32 reg;

	if (!priv || !priv->drm)
		return status;
	crossbar = drm_to_baikal_vdu_crossbar(priv->drm);
	dp = crossbar->dp;

	//priv->counters[0]++;
	irq_stat = readl(priv->regs + INT_STAT);
	raw_stat = readl(priv->regs + INT_CTRL);

	if (irq_stat & INT_VFP_START) {
		priv->counters[11]++;
		atomic_inc(&priv->vblank_counter);
		drm_crtc_handle_vblank(&priv->crtc);
		status = IRQ_HANDLED;
	}

	if (irq_stat & INT_ANY_ERROR) {
		if (raw_stat & INT_AXI_RESP)
			priv->errors[0]++;
		if (raw_stat & INT_WINDOW_EMPTY_0)
			priv->errors[1]++;
		if (raw_stat & INT_WINDOW_EMPTY_1)
			priv->errors[2]++;
		if (raw_stat & INT_WINDOW_EMPTY_2)
			priv->errors[3]++;
		if (raw_stat & INT_CURSOR_EMPTY)
			priv->errors[4]++;
		status = IRQ_HANDLED;
	}

	reg = baikal_dp_read(dp->dp_base, BAIKAL_DP_INTERRUPT_STATE);
	if (reg & BAIKAL_DP_INTERRUPT_STATE_VS0_FIFO_OVERFLOW)
		priv->errors[5]++;
	if (reg & BAIKAL_DP_INTERRUPT_STATE_VS0_FIFO_ERROR)
		priv->errors[6]++;
	if (reg & BAIKAL_DP_INTERRUPT_STATE_VS1_FIFO_OVERFLOW)
		priv->errors[7]++;
	if (reg & BAIKAL_DP_INTERRUPT_STATE_VS1_FIFO_ERROR)
		priv->errors[8]++;

	reg = baikal_dp_read(dp->dp_base, BAIKAL_DP_SRC0_USER_FIFO_STATUS);
	if (reg & BAIKAL_DP_SRC0_USER_FIFO_STATUS_ERROR)
		priv->errors[9]++;
	if (reg & BAIKAL_DP_SRC0_USER_FIFO_STATUS_OVERFLOW)
		priv->errors[10]++;

	reg = baikal_dp_read(dp->dp_base, BAIKAL_DP_SRC0_USER_FRAMING_STATUS);
	if (reg & BAIKAL_DP_SRC0_USER_FRAMING_STATUS_VBI_TIMING_ERROR)
		priv->errors[11]++;
	if (reg & BAIKAL_DP_SRC0_USER_FRAMING_STATUS_DATA_UNDERFLOW)
		priv->errors[12]++;

	//priv->counters[3] |= raw_stat;
	//priv->counters[4] |= irq_stat;

	/* Clear all interrupts */
	writel(raw_stat, priv->regs + INT_CTRL);

	return status;
}

static void baikal_vdu_crtc_helper_mode_set_nofb(struct drm_crtc *crtc)
{
	struct baikal_vdu_private *priv = crtc_to_baikal_vdu(crtc);
	const struct drm_display_mode *mode = &crtc->state->adjusted_mode;
	unsigned long rate;
	unsigned int val;

	drm_mode_debug_printmodeline(mode);

	struct clk *clk_parent = clk_get_parent(priv->clk);
	rate = clk_round_rate(clk_parent, mode->crtc_clock * 1000 * 2);
	clk_set_rate(clk_parent, rate);
	clk_set_rate(priv->clk, rate / 2);

	baikal_vdu_write(priv, TE_CTRL_0,
		(TE_CTRL_0_HFP(mode->hsync_start - mode->hdisplay) & TE_CTRL_0_HFP_MASK) |
		(TE_CTRL_0_HBP(mode->htotal - mode->hsync_end) & TE_CTRL_0_HBP_MASK));
	baikal_vdu_write(priv, TE_CTRL_1,
		(TE_CTRL_1_HS(mode->hsync_end - mode->hsync_start) & TE_CTRL_1_HS_MASK) |
		(TE_CTRL_1_HA(mode->hdisplay) & TE_CTRL_1_HA_MASK));
	baikal_vdu_write(priv, TE_CTRL_2,
		(TE_CTRL_2_VFP(mode->vsync_start - mode->vdisplay) & TE_CTRL_2_VFP_MASK) |
		(TE_CTRL_2_VBP(mode->vtotal - mode->vsync_end) & TE_CTRL_2_VBP_MASK));
	baikal_vdu_write(priv, TE_CTRL_3,
		(TE_CTRL_3_VS(mode->vsync_end - mode->vsync_start) & TE_CTRL_3_VS_MASK) |
		(TE_CTRL_3_VA(mode->vdisplay) & TE_CTRL_3_VA_MASK));

	if (mode->flags & DRM_MODE_FLAG_NVSYNC)
		val = POLAR_CTRL_VS_INV;
	else
		val = 0;
	if (mode->flags & DRM_MODE_FLAG_NHSYNC)
		val |= POLAR_CTRL_HS_INV;
	baikal_vdu_write(priv, POLAR_CTRL, val);
}

static enum drm_mode_status baikal_vdu_l1000_mode_valid(struct drm_crtc *crtc,
	                const struct drm_display_mode *mode)
{
	struct baikal_vdu_private *priv = crtc_to_baikal_vdu(crtc);

	if ((mode->hdisplay <= priv->max_width &&
			mode->vdisplay <= priv->max_height &&
			mode->clock <= priv->max_pix_clock) ||
			(mode->hdisplay == 3200 && mode->vdisplay == 1800))
		return MODE_OK;
	else
		return MODE_BAD;
}

static void baikal_vdu_l1000_crtc_helper_atomic_enable(struct drm_crtc *crtc,
						       struct drm_atomic_state *state)
{
	struct baikal_vdu_private *priv = crtc_to_baikal_vdu(crtc);
	struct clk *clk_parent = clk_get_parent(priv->clk);

	DRM_DEV_DEBUG_DRIVER(crtc->dev->dev, "enabling pixel clock\n");
	clk_prepare_enable(clk_parent);
	clk_prepare_enable(priv->clk);

	/* Enable and Power Up */
	baikal_vdu_write(priv, TE_EN, 1);

	// TODO enable IRQs if needed
	drm_crtc_vblank_on(crtc);
}

static void baikal_vdu_l1000_crtc_helper_atomic_disable(struct drm_crtc *crtc,
							struct drm_atomic_state *state)
{
	struct baikal_vdu_private *priv = crtc_to_baikal_vdu(crtc);
	struct clk *clk_parent = clk_get_parent(priv->clk);

	// TODO disable IRQs if needed
	drm_crtc_vblank_off(crtc);

	/* Disable and Power Down */
	baikal_vdu_write(priv, TE_EN, 0);

	/* Disable clock */
	DRM_DEV_DEBUG_DRIVER(crtc->dev->dev, "disabling pixel clock\n");
	clk_disable_unprepare(clk_parent);
	clk_disable_unprepare(priv->clk);
}

static void baikal_vdu_l1000_crtc_helper_atomic_flush(struct drm_crtc *crtc,
						      struct drm_atomic_state *state)
{
	struct drm_crtc_state *new_crtc_state = drm_atomic_get_new_crtc_state(state, crtc);
	struct drm_pending_vblank_event *event;
	unsigned long flags;

	event = new_crtc_state->event;

	if (event) {
		new_crtc_state->event = NULL;

		spin_lock_irqsave(&crtc->dev->event_lock, flags);
		if (new_crtc_state->active && drm_crtc_vblank_get(crtc) == 0) {
			drm_crtc_arm_vblank_event(crtc, event);
		} else {
			drm_crtc_send_vblank_event(crtc, event);
		}
		spin_unlock_irqrestore(&crtc->dev->event_lock, flags);
	}
}

// TODO common for M1000 and L1000, consider refactoring
const struct drm_crtc_funcs baikal_vdu_l1000_crtc_funcs = {
	.reset = drm_atomic_helper_crtc_reset,
	.destroy = drm_crtc_cleanup,
	.set_config = drm_atomic_helper_set_config,
	.page_flip = drm_atomic_helper_page_flip,
	.atomic_duplicate_state = drm_atomic_helper_crtc_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_crtc_destroy_state,
	.get_vblank_counter = baikal_get_vblank_counter,
	.enable_vblank = baikal_enable_vblank,
	.disable_vblank = baikal_disable_vblank,
};

const struct drm_crtc_helper_funcs baikal_vdu_l1000_crtc_helper_funcs = {
	.mode_set_nofb = baikal_vdu_crtc_helper_mode_set_nofb,
	.mode_valid = baikal_vdu_l1000_mode_valid,
	.atomic_enable = baikal_vdu_l1000_crtc_helper_atomic_enable,
	.atomic_flush = baikal_vdu_l1000_crtc_helper_atomic_flush,
	.atomic_disable = baikal_vdu_l1000_crtc_helper_atomic_disable,
};

int baikal_vdu_l1000_crtc_create(struct baikal_vdu_private *priv)
{
	struct drm_device *dev = priv->drm;
	struct drm_crtc *crtc = &priv->crtc;
	struct clk *clk_parent = clk_get_parent(priv->clk);

	drm_crtc_init_with_planes(dev, crtc,
				  &priv->primary,
				  hw_cursor ? &priv->cursor : NULL,
				  &baikal_vdu_l1000_crtc_funcs, "primary");
	drm_crtc_helper_add(crtc, &baikal_vdu_l1000_crtc_helper_funcs);

	DRM_DEV_DEBUG_DRIVER(crtc->dev->dev, "enabling pixel clock\n");
	clk_prepare_enable(clk_parent);
	clk_prepare_enable(priv->clk);

	return 0;
}
