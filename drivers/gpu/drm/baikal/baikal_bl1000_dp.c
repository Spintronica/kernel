// SPDX-License-Identifier: GPL-2.0
/*
 * Baikal Electronics DisplayPort TX Driver
 *
 * Copyright (C) 2025 Baikal Electronics JSC
 *
 * Author: Pavel Parkhomenko <Pavel.Parkhomenko@baikalelectronics.ru>
 *
 */

#include <clocksource/arm_arch_timer.h>
#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/gpio/consumer.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/phy/phy.h>
#include <linux/phy/phy-dp.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <uapi/linux/videodev2.h>

#include <video/videomode.h>

#include <drm/drm_atomic_helper.h>
#include <drm/drm_connector.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_crtc.h>
#include <drm/drm_crtc_helper.h>
#include <drm/display/drm_dp.h>
#include <drm/display/drm_dp_helper.h>
#include <drm/display/drm_dp_mst_helper.h>
#include <drm/display/drm_hdmi_helper.h>

#include <drm/drm_edid.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_of.h>
#include <drm/drm_print.h>
#include <drm/drm_probe_helper.h>

#include <linux/hdmi.h>

#include <linux/debugfs.h>

#include "baikal_bl1000_dp.h"

#define AUX_RETRY_INTERVAL 500

#define AUX_READ_BIT	0x1

static int int_sig_state;

static ssize_t baikal_dp_aux_transfer(struct drm_dp_aux *aux, struct drm_dp_aux_msg *msg);

static int baikal_dp_train_loop(struct baikal_dp *dp);
static void baikal_dp_encoder_mode_set_transfer_unit(struct baikal_dp *dp,
						     struct drm_display_mode *mode);
static void baikal_dp_encoder_mode_set_stream(struct baikal_dp *dp,
					    struct drm_display_mode *mode);
static void baikal_dp_hpd_pulse_work_func(struct work_struct *work);
static int baikal_dp_txconnected(struct baikal_dp *dp);

static inline struct baikal_dp *encoder_to_dp(struct drm_encoder *encoder)
{
	return container_of(encoder, struct baikal_dp, encoder);
}

static inline struct baikal_dp *connector_to_dp(struct drm_connector *connector)
{
	return container_of(connector, struct baikal_dp, connector);
}

static inline void baikal_dp_write(void __iomem *base, int offset, u32 val)
{
	writel(val, base + (offset << BAIKAL_DP_REG_ADDR_OFFSET));
}

inline u32 baikal_dp_read(void __iomem *base, int offset)
{
	return readl(base + (offset << BAIKAL_DP_REG_ADDR_OFFSET));
}

static void baikal_dp_set(void __iomem *base, int offset, u32 set)
{
	baikal_dp_write(base, offset, baikal_dp_read(base, offset) | set);
}

static void baikal_dp_update_bpp(struct baikal_dp *dp)
{
	struct baikal_dp_config *config = &dp->config;

	config->bpp = dp->config.bpc * dp->config.num_colors;
}

static void baikal_dp_set_color(struct baikal_dp *dp, u32 drm_fourcc)
{
	struct baikal_dp_config *config = &dp->config;

	config->misc0 &= ~BAIKAL_DP_SRC0_FORMAT_MASK;

	switch (drm_fourcc) {
	case DRM_FORMAT_XBGR8888:
		fallthrough;
	case DRM_FORMAT_XRGB8888:
		fallthrough;
	case DRM_FORMAT_BGR888:
		fallthrough;
	case DRM_FORMAT_RGB888:
		fallthrough;
	case DRM_FORMAT_XBGR2101010:
		config->misc0 |= BAIKAL_DP_SRC0_RGB_MASK;
		config->num_colors = 3;
		config->fmt = 0x0;
		break;
	case DRM_FORMAT_VUY888:
		config->misc0 |= BAIKAL_DP_SRC0_YCRCB444_MASK;
		config->num_colors = 3;
		config->fmt = 0x1;
		break;
	case DRM_FORMAT_YUYV:
		fallthrough;
	case DRM_FORMAT_UYVY:
		fallthrough;
	case DRM_FORMAT_NV16:
		config->misc0 |= BAIKAL_DP_SRC0_YCRCB422_MASK;
		config->num_colors = 2;
		config->fmt = 0x2;
		break;
	default:
		dev_dbg(dp->dev, "Warning: Unknown drm_fourcc format :%d\n",
			drm_fourcc);
		config->misc0 |= BAIKAL_DP_SRC0_RGB_MASK;
	}
	baikal_dp_update_bpp(dp);
}

static int baikal_dp_set_linkrate(struct baikal_dp *dp, u8 bw_code)
{
	struct phy_configure_opts_dp *phy_cfg = &dp->phy_opts.dp;
	int ret;
	u8 lane_count = dp->mode.lane_cnt;

	if (!baikal_dp_txconnected(dp)) {
		dev_info(dp->dev, "display is not connected");
		return connector_status_disconnected;
	}

	if (dp->mode.bw_code != bw_code) {
		dp->mode.bw_code = bw_code;

		/* configure video phy controller to new link rate */
		phy_cfg->set_rate = 1;
		phy_cfg->set_lanes = 0;
		phy_cfg->set_voltages = 0;
		phy_cfg->link_rate = bw_code * 270;
		phy_cfg->lanes = lane_count;
		phy_configure(dp->phy[0], &dp->phy_opts);
		dev_err(dp->dev, "PHY SET RATE");
	}

	/* write new link rate to the DisplayPort TX core */
	baikal_dp_write(dp->dp_base, BAIKAL_DP_LINK_BW_SET, bw_code);
	/* write new link rate to the RX device */
	ret = drm_dp_dpcd_writeb(&dp->aux, DP_LINK_BW_SET, bw_code);
	if (ret < 0) {
		dev_err(dp->dev, "failed to set DP bandwidth\n");
		return ret;
	}

	return 0;
}

static int baikal_dp_set_lanecount(struct baikal_dp *dp, u8 lane_cnt)
{
	int ret;
	u8 data;
	struct phy_configure_opts_dp *phy_cfg = &dp->phy_opts.dp;

	if (dp->mode.lane_cnt != lane_cnt) {
		dp->mode.lane_cnt = lane_cnt;

		data = baikal_dp_read(dp->dp_base, BAIKAL_DP_TRANSMITTER_ENABLE);
		baikal_dp_write(dp->dp_base, BAIKAL_DP_TRANSMITTER_ENABLE, 0);
		phy_cfg->set_rate = 0;
		phy_cfg->set_lanes = 1;
		phy_cfg->set_voltages = 0;
		phy_cfg->lanes = lane_cnt;
		phy_configure(dp->phy[0], &dp->phy_opts);
		if (data)
			baikal_dp_write(dp->dp_base, BAIKAL_DP_TRANSMITTER_ENABLE, 1);
	}

	baikal_dp_write(dp->dp_base, BAIKAL_DP_LANE_COUNT_SET, lane_cnt);

	ret = drm_dp_dpcd_readb(&dp->aux, DP_LANE_COUNT_SET, &data);
	if (ret < 0) {
		dev_err(dp->dev, "DPCD read retry fails");
		return ret;
	}

	data &= ~DP_LANE_COUNT_MASK;
	data |= dp->mode.lane_cnt;
	ret = drm_dp_dpcd_writeb(&dp->aux, DP_LANE_COUNT_SET, data);
	if (ret < 0) {
		dev_err(dp->dev, "failed to set lane count\n");
		return ret;
	}

	return 0;
}

static int baikal_dp_check_link_status(struct baikal_dp *dp)
{
	int ret;
	u8 link_status[DP_LINK_STATUS_SIZE];
	u8 retry = 0;

	memset(link_status, 0, sizeof(link_status));

	if (!baikal_dp_txconnected(dp)) {
		dev_dbg(dp->dev, "display is not connected");
		return connector_status_disconnected;
	}

	for (retry = 0; retry < 5; retry++) {
		ret = drm_dp_dpcd_read_link_status(&dp->aux, link_status);
		if (ret < 0)
			return ret;

		if (drm_dp_clock_recovery_ok(link_status, dp->mode.lane_cnt) ||
		    drm_dp_channel_eq_ok(link_status, dp->mode.lane_cnt))
			return 0;
	}

	return -EINVAL;
}

static int baikal_dp_init_aux(struct baikal_dp *dp)
{
	unsigned long rate;
	u32 reg;

	rate = clk_get_rate(dp->axi_lite_clk);
	if (rate < BAIKAL_DP_AUX_CLK_MHZ) {
		dev_err(dp->dev, "aclk should be higher than 1MHz\n");
		return -EINVAL;
	}

	reg |= rate / BAIKAL_DP_AUX_CLK_MHZ;
	baikal_dp_write(dp->dp_base, BAIKAL_DP_AUX_CLK_DIVIDER, reg);

	baikal_dp_write(dp->dp_base, BAIKAL_DP_TRANSMITTER_ENABLE, 1);

	/* TODO this is for EDP only, refactor ASAP */
	baikal_dp_write(dp->dp_base, BAIKAL_EDP_CAPABILITY_CONFIG, BAIKAL_EDP_ENABLE_REDUCED_AUX_SYNC);

	return 0;
}

static void baikal_dp_update_misc(struct baikal_dp *dp)
{
	struct baikal_dp_config *config = &dp->config;

	if (!dp->colorimetry_through_vsc) {
		baikal_dp_write(dp->dp_base, BAIKAL_DP_SRC0_STREAM_MISC0,
			      config->misc0 | BAIKAL_DP_SRC0_STREAM_MISC0_MASK);
		baikal_dp_write(dp->dp_base, BAIKAL_DP_SRC0_STREAM_MISC1, 0x0);
	} else {
		baikal_dp_set(dp->dp_base, BAIKAL_DP_SRC0_STREAM_MISC1,
			    BAIKAL_DP_SRC0_STREAM_MISC1_VSC_COLORIMETRY);
	}
}

static void baikal_dp_set_sync_mode(struct baikal_dp *dp, bool mode)
{
	struct baikal_dp_config *config = &dp->config;

	if (mode)
		config->misc0 |= BAIKAL_DP_SRC0_STREAM_MISC0_MASK;
	else
		config->misc0 &= ~BAIKAL_DP_SRC0_STREAM_MISC0_MASK;
}

static u32 baikal_dp_set_bpc(struct baikal_dp *dp, u8 bpc)
{
	struct baikal_dp_config *config = &dp->config;
	unsigned int ret = 0;

	if (dp->connector.display_info.bpc &&
	    dp->connector.display_info.bpc != bpc) {
		dev_err(dp->dev, "requested bpc (%u) != display info (%u)\n",
			bpc, dp->connector.display_info.bpc);
		bpc = dp->connector.display_info.bpc;
	}

	config->misc0 &= ~BAIKAL_DP_SRC0_BPC_MASK;
	switch (bpc) {
	case 6:
		config->misc0 |= BAIKAL_DP_SRC0_BPC6_MASK;
		break;
	case 8:
		config->misc0 |= BAIKAL_DP_SRC0_BPC8_MASK;
		break;
	case 10:
		config->misc0 |= BAIKAL_DP_SRC0_BPC10_MASK;
		break;
	case 12:
		config->misc0 |= BAIKAL_DP_SRC0_BPC12_MASK;
		break;
	case 16:
		config->misc0 |= BAIKAL_DP_SRC0_BPC16_MASK;
		break;
	default:
		dev_err(dp->dev, "Not supported bpc (%u). fall back to 8bpc\n",
			bpc);
		config->misc0 |= BAIKAL_DP_SRC0_BPC8_MASK;
		ret = 8;
	}
	config->bpc = bpc;
	baikal_dp_update_bpp(dp);

	return ret;
}

static void baikal_dp_encoder_mode_set_stream(struct baikal_dp *dp,
					    struct drm_display_mode *mode)
{
	void __iomem *dp_base = dp->dp_base;
	u32 reg, wpl;
	u8 lane_cnt = dp->mode.lane_cnt;

	udelay(10000);

	baikal_dp_write(dp_base, BAIKAL_DP_SRC0_STREAM_HTOTAL, mode->htotal);
	baikal_dp_write(dp_base, BAIKAL_DP_SRC0_STREAM_VTOTAL, mode->vtotal);

	baikal_dp_write(dp_base, BAIKAL_DP_SRC0_STREAM_POL,
		      (!!(mode->flags & DRM_MODE_FLAG_PVSYNC) <<
		      BAIKAL_DP_SRC0_STREAM_POLVSYNC_SHIFT) |
		      (!!(mode->flags & DRM_MODE_FLAG_PHSYNC) <<
		      BAIKAL_DP_SRC0_STREAM_POLHSYNC_SHIFT));

	baikal_dp_write(dp_base, BAIKAL_DP_SRC0_STREAM_HSWIDTH,
		      mode->hsync_end - mode->hsync_start);
	baikal_dp_write(dp_base, BAIKAL_DP_SRC0_STREAM_VSWIDTH,
		      mode->vsync_end - mode->vsync_start);
	baikal_dp_write(dp_base, BAIKAL_DP_SRC0_STREAM_HRES, mode->hdisplay);
	baikal_dp_write(dp_base, BAIKAL_DP_SRC0_STREAM_VRES, mode->vdisplay);

	baikal_dp_write(dp_base, BAIKAL_DP_SRC0_STREAM_HSTART,
		      mode->htotal - mode->hsync_start);
	baikal_dp_write(dp_base, BAIKAL_DP_SRC0_STREAM_VSTART,
		      mode->vtotal - mode->vsync_start);
	baikal_dp_update_misc(dp);

	baikal_dp_write(dp_base, BAIKAL_DP_SRC0_M_VID, mode->clock);
	reg = drm_dp_bw_code_to_link_rate(dp->mode.bw_code);
	baikal_dp_write(dp_base, BAIKAL_DP_SRC0_N_VID, reg);

	/* In synchronous mode, set the dividers */
	if (dp->config.misc0 & BAIKAL_DP_SRC0_STREAM_MISC0_MASK) {
		reg = drm_dp_bw_code_to_link_rate(dp->mode.bw_code);
		baikal_dp_write(dp_base, BAIKAL_DP_SRC0_N_VID, reg);
		baikal_dp_write(dp_base, BAIKAL_DP_SRC0_M_VID, mode->clock);
	}

	wpl = (mode->hdisplay * dp->config.bpp + 7) / 8;
	reg = (wpl + lane_cnt - 1) / lane_cnt;
	baikal_dp_write(dp_base, BAIKAL_DP_SRC0_USER_DATA_COUNT, reg);
}

static void baikal_dp_mainlink_en(struct baikal_dp *dp, u8 enable)
{
	baikal_dp_write(dp->dp_base, BAIKAL_DP_SRC0_STREAM_ENABLE, enable);
}

static int baikal_dp_power_cycle(struct baikal_dp *dp)
{
	int ret = 0, i;

	for (i = 0; i < 3; i++) {
		drm_dp_dpcd_writeb(&dp->aux, DP_SET_POWER, DP_SET_POWER_D3);
		usleep_range(300, 500);
		ret = drm_dp_dpcd_writeb(&dp->aux, DP_SET_POWER, DP_SET_POWER_D0);
		if (ret == 1)
			break;
		usleep_range(300, 500);
		ret = drm_dp_dpcd_writeb(&dp->aux, DP_SET_POWER, DP_SET_POWER_D0);
		if (ret == 1)
			break;
		usleep_range(300, 500);
		ret = drm_dp_dpcd_writeb(&dp->aux, DP_SET_POWER, DP_SET_POWER_D0);
		if (ret == 1)
			break;
		usleep_range(3000, 4000);
	}

	if (ret < 0) {
		dev_err(dp->dev, "DP aux failed\n");
		return ret;
	}

	return 0;
}

/*static struct baikal_dp_connector *baikal_dp_connector_alloc(void)
{
	return kzalloc(sizeof(struct baikal_dp_connector), GFP_KERNEL);
}

static void baikal_dp_connector_free(struct baikal_dp_connector *connector)
{
	kfree(connector);
}*/

static void baikal_dp_start(struct baikal_dp *dp)
{
	struct baikal_dp_mode *mode = &dp->mode;
	int link_rate = dp->link_config.link_rate;
	int ret = 0;
	u32 val, intr_mask;
	u8 data;
	bool enhanced;

	mode->bw_code = drm_dp_link_rate_to_bw_code(link_rate);
	mode->lane_cnt = dp->link_config.lane_count;
	dp->link_config.cr_done_oldstate = dp->link_config.max_lanes;

	baikal_dp_power_cycle(dp);
	baikal_dp_write(dp->dp_base, BAIKAL_DP_TRANSMITTER_ENABLE, 0);

	/*
	 * Give a bit of time for DP IP after monitor came up and starting
	 * link training
	 */
	msleep(100);
	baikal_dp_write(dp->dp_base, BAIKAL_DP_TRANSMITTER_ENABLE, 1);

	ret = baikal_dp_set_linkrate(dp, mode->bw_code);
	if (ret < 0) {
		dev_err(dp->dev, "failed to set link rate\n");
		return;
	}

	ret = baikal_dp_set_lanecount(dp, mode->lane_cnt);
	if (ret < 0) {
		dev_err(dp->dev, "failed to set lane count\n");
		return;
	}

	/* Disable main link during training. */
	val = baikal_dp_read(dp->dp_base, BAIKAL_DP_SRC0_STREAM_ENABLE);
	if (val)
		baikal_dp_mainlink_en(dp, 0x0);

	/* Disable HPD pulse interrupts during link training */
	intr_mask = baikal_dp_read(dp->dp_base, BAIKAL_DP_INTERRUPT_MASK);
	baikal_dp_write(dp->dp_base, BAIKAL_DP_INTERRUPT_MASK,
		      intr_mask | BAIKAL_DP_INTERRUPT_HPDPULSE_MASK);

	/* Set MISC0 and SST_SOURCE here */
	/* TODO refactor */
	//printk("ENCODER MISC0=0x%x", baikal_dp_read(dp->dp_base, BAIKAL_DP_SRC0_STREAM_MISC0));
	//baikal_dp_write(dp->dp_base, BAIKAL_DP_SRC0_STREAM_MISC0, 0x1);
	/* TODO refactor */
	//baikal_dp_write(dp->dp_base, BAIKAL_DP_SST_SOURCE_SELECT, 0x0);

	// Disable clock spreading for RX device
	drm_dp_dpcd_readb(&dp->aux, DP_DOWNSPREAD_CTRL, &data);
	data &= ~DP_SPREAD_AMP_0_5;
	drm_dp_dpcd_writeb(&dp->aux, DP_DOWNSPREAD_CTRL, data);

	// Enhanced framing model
	drm_dp_dpcd_readb(&dp->aux, DP_LANE_COUNT_SET, &data);
	enhanced = drm_dp_enhanced_frame_cap(dp->dpcd);
	if (enhanced) {
		data |= DP_LANE_COUNT_ENHANCED_FRAME_EN;
	}

	ret = drm_dp_dpcd_writeb(&dp->aux, DP_LANE_COUNT_SET, data);
	if (ret < 0) {
		dev_err(dp->dev, "failed to set lane count\n");
		return;
	}

	// set channel encoding
	ret = drm_dp_dpcd_writeb(&dp->aux, DP_MAIN_LINK_CHANNEL_CODING_SET,
				 DP_SET_ANSI_8B10B);
	if (ret < 0) {
		dev_err(dp->dev, "failed to set ANSI 8B/10B encoding\n");
		return;
	}

	/* TODO isn't it redundant? */
	memset(dp->train_set, 0, BAIKAL_DP_MAX_LANES);

	ret = baikal_dp_train_loop(dp);
	if (ret < 0) {
		dev_err(dp->dev, "DP link training Failed\n");
		return;
	}

	/* TODO sort out */
	//baikal_dp_write(dp->dp_base, BAIKAL_DP_SRC0_USER_CONTROL, 0);

	baikal_dp_write(dp->dp_base, BAIKAL_DP_SRC0_USER_SYNC_POLARITY, 0xf);
	//baikal_dp_write(dp->dp_base, BAIKAL_DP_SRC0_COLORIMETRY_OVERRIDE, 0x0);

	if (val)
		baikal_dp_mainlink_en(dp, 0x1);

	/* Enable HPD interrupts after link training */
	baikal_dp_write(dp->dp_base, BAIKAL_DP_INTERRUPT_MASK, intr_mask);

	dev_dbg(dp->dev, "DP link training done\n");

	// reset transmitter
	baikal_dp_write(dp->dp_base, BAIKAL_DP_SOFT_RESET,
			      BAIKAL_DP_SOFT_RESET_LINK_RESET |
			      BAIKAL_DP_SOFT_RESET_VIDEO_RESET);
	/* SECTION A BEGIN */
	baikal_dp_mainlink_en(dp, 0x1);

	// check link status
	ret = baikal_dp_check_link_status(dp);
	if (ret < 0) {
		dev_err(dp->dev, "Link is DOWN after main link enabled!\n");
		return;
        }

	/* TODO VSC packet handling */

	/* Enable all interrupts */
	baikal_dp_write(dp->dp_base, BAIKAL_DP_INTERRUPT_MASK, 0);
}

static void baikal_dp_stop(struct baikal_dp *dp)
{
	struct phy_configure_opts_dp *phy_cfg = &dp->phy_opts.dp;

	baikal_dp_write(dp->dp_base, BAIKAL_DP_SRC0_STREAM_ENABLE, 0);

	/* set Vs and Pe to 0, 0 on cable disconnect */
	phy_cfg->pre[0] = 0;
	phy_cfg->voltage[0] = 0;
	phy_cfg->set_voltages = 1;

	phy_configure(dp->phy[0], &dp->phy_opts);
}

static int baikal_dp_txconnected(struct baikal_dp *dp)
{
	if (baikal_dp_read(dp->dp_base, BAIKAL_DP_HPD_INPUT_STATE) &
	    BAIKAL_DP_HPD_INPUT_STATE_HPD)
		return true;
	return false;
}

static enum drm_connector_status
baikal_dp_connector_detect(struct drm_connector *connector, bool force)
{
	struct baikal_dp *dp = connector_to_dp(connector);
	struct baikal_dp_link_config *link_config = &dp->link_config;
	struct baikal_dp_mode *mode = &dp->mode;
	int ret;
	u8 dpcd_ext[DP_RECEIVER_CAP_SIZE];
	u8 max_link_rate, ext_cap_rd = 0, data;

	if (!baikal_dp_txconnected(dp)) {
		dev_dbg(dp->dev, "Display is not connected");
		goto disconnected;
	}

	/* Reading the Ext capability for compliance */
	ret = drm_dp_dpcd_read(&dp->aux, DP_DP13_DPCD_REV, dpcd_ext,
			       sizeof(dpcd_ext));
	if (ret < 0) {
		dev_dbg(dp->dev, "DPCD read first try fails");
		ret = drm_dp_dpcd_read(&dp->aux, DP_DP13_DPCD_REV, dpcd_ext,
				       sizeof(dpcd_ext));
		if (ret < 0) {
			dev_info(dp->dev, "DPCD EXT read fails");
			goto disconnected;
		}
	}

	if ((dp->dpcd[6] & 0x1) == 0x1) {
		ret = drm_dp_dpcd_read(&dp->aux, DP_DOWNSTREAM_PORT_0,
				       dpcd_ext, sizeof(dpcd_ext));
	}

	ret = drm_dp_dpcd_read(&dp->aux, DP_DPCD_REV, dp->dpcd,
			       sizeof(dp->dpcd));
	if (ret < 0) {
		dev_dbg(dp->dev, "DPCD read first try fails");
		ret = drm_dp_dpcd_read(&dp->aux, DP_DPCD_REV, dp->dpcd,
				       sizeof(dp->dpcd));
		if (ret < 0) {
			dev_info(dp->dev, "DPCD read fails");
			goto disconnected;
		}
	}

	/* set MaxLinkRate to TX rate, if sink provides a non-standard value */
	if (dp->dpcd[DP_MAX_LINK_RATE] != DP_LINK_BW_8_1 &&
	    dp->dpcd[DP_MAX_LINK_RATE] != DP_LINK_BW_5_4 &&
	    dp->dpcd[DP_MAX_LINK_RATE] != DP_LINK_BW_2_7 &&
	    dp->dpcd[DP_MAX_LINK_RATE] != DP_LINK_BW_1_62) {
		dp->dpcd[DP_MAX_LINK_RATE] = DP_LINK_BW_8_1;
	}

	if (dp->dpcd[DP_TRAINING_AUX_RD_INTERVAL] &
	    DP_EXTENDED_RECEIVER_CAP_FIELD_PRESENT) {
		ret = drm_dp_dpcd_read(&dp->aux, DP_DP13_MAX_LINK_RATE,
				       &max_link_rate, 1);
		if (ret < 0) {
			dev_dbg(dp->dev, "DPCD read failed");
			goto disconnected;
		}

		if (max_link_rate == DP_LINK_BW_8_1)
			dp->dpcd[DP_MAX_LINK_RATE] = DP_LINK_BW_8_1;

		/* compliance: UCD400 required reading these extended registers */
		ret = drm_dp_dpcd_read(&dp->aux, DP_DP13_MAX_LINK_RATE,
				       &ext_cap_rd, 1);
		ret = drm_dp_dpcd_read(&dp->aux, DP_SINK_COUNT_ESI,
				       &ext_cap_rd, 1);
		ret = drm_dp_dpcd_read(&dp->aux,
				       DP_DEVICE_SERVICE_IRQ_VECTOR_ESI0,
				       &ext_cap_rd, 1);
		ret = drm_dp_dpcd_read(&dp->aux, DP_LANE0_1_STATUS_ESI,
				       &ext_cap_rd, 1);
		ret = drm_dp_dpcd_read(&dp->aux, DP_LANE2_3_STATUS_ESI,
				       &ext_cap_rd, 1);
		ret = drm_dp_dpcd_read(&dp->aux,
				       DP_LANE_ALIGN_STATUS_UPDATED_ESI,
				       &ext_cap_rd, 1);
		ret = drm_dp_dpcd_read(&dp->aux, DP_SINK_STATUS_ESI,
				       &ext_cap_rd, 1);
		if (ret < 0)
			dev_dbg(dp->dev, "DPCD read fails");
	}

	link_config->max_rate = min_t(int,
				      drm_dp_max_link_rate(dp->dpcd),
				      dp->config.max_link_rate);
	link_config->max_lanes = min_t(u8,
				       drm_dp_max_lane_count(dp->dpcd),
				       dp->config.max_lanes);
	link_config->link_rate = link_config->max_rate;
	link_config->lane_count = link_config->max_lanes;
	mode->lane_cnt = link_config->max_lanes;
	mode->bw_code = drm_dp_link_rate_to_bw_code(link_config->link_rate);

	ret = drm_dp_dpcd_read(&dp->aux, DP_DPRX_FEATURE_ENUMERATION_LIST,
				&data, 1);
	if (ret < 0) {
		dev_dbg(dp->dev, "DPCD read failed");
		goto disconnected;
	}
	dp->status = connector_status_connected;

	/*if (data & DP_VSC_SDP_EXT_FOR_COLORIMETRY_SUPPORTED)
		dp->colorimetry_through_vsc = true;
	else*/
	dp->colorimetry_through_vsc = false;

	return connector_status_connected;
disconnected:
	dp->status = connector_status_disconnected;
	if (dp->enabled) {
		//baikal_dp_stop(dp);
	}

	return connector_status_disconnected;
}

static int baikal_dp_connector_get_modes(struct drm_connector *connector)
{
	struct baikal_dp *dp = connector_to_dp(connector);
	struct edid *edid;
	int ret;

	edid = drm_get_edid(connector, &dp->aux.ddc);
	if (!edid) {
		drm_connector_update_edid_property(connector, NULL);
		dp->have_edid = false;
		return 0;
	}

	drm_connector_update_edid_property(connector, edid);
	ret = drm_add_edid_modes(connector, edid);
	dp->have_edid = true;
	kfree(edid);

	return ret;
}

static int baikal_dp_connector_get_modes_noedid(struct drm_connector *connector)
{
	struct drm_display_mode *drm_mode;
	struct videomode videomode;
	int count = drm_add_modes_noedid(connector, 3840, 2160);

	drm_mode = drm_mode_create(connector->dev);
	if (!drm_mode)
		return count;

	memset(&videomode, 0, sizeof(videomode));

	videomode.hactive = 3840;
	videomode.vactive = 2160;
	videomode.hfront_porch = 176;
	videomode.hsync_len = 88;
	videomode.hback_porch = 296;
	videomode.vfront_porch = 8;
	videomode.vsync_len = 10;
	videomode.vback_porch = 72;
	videomode.pixelclock = 594000000;

	videomode.flags = DISPLAY_FLAGS_HSYNC_LOW | DISPLAY_FLAGS_VSYNC_LOW;

	drm_mode->type = DRM_MODE_TYPE_PREFERRED | DRM_MODE_TYPE_DRIVER;

	drm_display_mode_from_videomode(&videomode, drm_mode);
	drm_mode_probed_add(connector, drm_mode);

	return count + 1;
}

static struct drm_encoder *
baikal_dp_connector_atomic_best_encoder(struct drm_connector *connector,
				      struct drm_atomic_state *state)
{
	struct baikal_dp *dp = connector_to_dp(connector);

	return &dp->encoder;
}

static inline int baikal_dp_max_rate(int link_rate, u8 lane_num, u8 bpp)
{
	return link_rate * lane_num * 8 / bpp;
}

static int baikal_dp_connector_mode_valid(struct drm_connector *connector,
					struct drm_display_mode *mode)
{
	struct baikal_dp *dp = connector_to_dp(connector);
	u8 max_lanes = dp->link_config.max_lanes;
	u8 bpp = dp->config.bpp;
	int max_rate = dp->link_config.max_rate;
	int rate;

	/*if (mode->clock > BAIKAL_DP_MAX_FREQ) {
		dev_info(dp->dev, "filtered the mode, %s,for high pixel rate\n",
			 mode->name);
		drm_mode_debug_printmodeline(mode);
		return MODE_CLOCK_HIGH;
	}*/

	/* check with link rate and lane count */
	rate = baikal_dp_max_rate(max_rate, max_lanes, bpp);
	if (mode->clock > rate) {
		dev_dbg(dp->dev, "filtered the mode, %s,for high pixel rate\n",
			mode->name);
		drm_mode_debug_printmodeline(mode);

		return MODE_CLOCK_HIGH;
	}

	return MODE_OK;
}

static void baikal_dp_connector_destroy(struct drm_connector *connector)
{
	drm_connector_unregister(connector);
	drm_connector_cleanup(connector);
}

static int
baikal_dp_connector_atomic_set_property(struct drm_connector *connector,
				      struct drm_connector_state *state,
				      struct drm_property *property,
				      uint64_t val)
{
	struct baikal_dp *dp = connector_to_dp(connector);
	unsigned int bpc;

	if (property == dp->sync_prop) {
		baikal_dp_set_sync_mode(dp, val);
	} else if (property == dp->bpc_prop) {
		bpc = baikal_dp_set_bpc(dp, val);
		if (bpc) {
			drm_object_property_set_value(&connector->base,
						      property, bpc);
			return -EINVAL;
		}
	} else {
		return -EINVAL;
	}

	return 0;
}

static int
baikal_dp_connector_atomic_get_property(struct drm_connector *connector,
				      const struct drm_connector_state *state,
					struct drm_property *property,
					uint64_t *val)
{
	struct baikal_dp *dp = connector_to_dp(connector);

	if (property == dp->sync_prop)
		*val = (dp->config.misc0 & BAIKAL_DP_SRC0_STREAM_MISC0_MASK);
	else if (property == dp->bpc_prop)
		*val = dp->config.bpc;
	else
		return -EINVAL;

	return 0;
}

static const struct drm_connector_funcs baikal_dp_connector_funcs = {
	.detect			= baikal_dp_connector_detect,
	.fill_modes		= drm_helper_probe_single_connector_modes,
	.destroy		= baikal_dp_connector_destroy,
	.atomic_duplicate_state	= drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state	= drm_atomic_helper_connector_destroy_state,
	.reset			= drm_atomic_helper_connector_reset,
	.atomic_set_property	= baikal_dp_connector_atomic_set_property,
	.atomic_get_property	= baikal_dp_connector_atomic_get_property,
};

static struct drm_connector_helper_funcs baikal_dp_connector_helper_funcs = {
	.get_modes		= baikal_dp_connector_get_modes,
	.atomic_best_encoder	= baikal_dp_connector_atomic_best_encoder,
	.mode_valid		= baikal_dp_connector_mode_valid,
};

static struct drm_connector_helper_funcs baikal_dp_connector_helper_funcs_noedid = {
	.get_modes		= baikal_dp_connector_get_modes_noedid,
	.atomic_best_encoder	= baikal_dp_connector_atomic_best_encoder,
	.mode_valid		= baikal_dp_connector_mode_valid,
};

static void baikal_dp_encoder_enable(struct drm_encoder *encoder)
{
	struct baikal_dp *dp = encoder_to_dp(encoder);

	//pm_runtime_get_sync(dp->dev);

	if (!dp->enabled) {
		dp->enabled = true;

		baikal_dp_start(dp);
		baikal_dp_write(dp->dp_base, BAIKAL_DP_INPUT_SOURCE_ENABLE, 1);
	}
}

static void baikal_dp_encoder_disable(struct drm_encoder *encoder)
{
	struct baikal_dp *dp = encoder_to_dp(encoder);

	if (dp->enabled) {
		dp->enabled = false;
		cancel_delayed_work(&dp->hpd_work);
		cancel_delayed_work(&dp->hpd_pulse_work);

		baikal_dp_write(dp->dp_base, BAIKAL_DP_INPUT_SOURCE_ENABLE, 0);
		baikal_dp_stop(dp);
	}

	//pm_runtime_put_sync(dp->dev);
}

static void baikal_dp_encoder_mode_set_transfer_unit(struct baikal_dp *dp,
						     struct drm_display_mode *mode)
{
	/* Use the max transfer unit size (default) */
	uint32_t tu = BAIKAL_DP_DEFAULT_TRANSFER_UNITSIZE;
	uint64_t vid_mbytes;
	uint32_t avg_bytes_per_tu, avg_bytes_per_tu_x16;
	uint32_t init_wait;
	uint32_t coeff, fifo_accumulation_delay;

	vid_mbytes = mode->clock / 1000 * (dp->config.bpp / 8);
	avg_bytes_per_tu_x16 = vid_mbytes * tu / dp->mode.lane_cnt * 16 / (dp->mode.bw_code * 27);
	avg_bytes_per_tu = avg_bytes_per_tu_x16 / 16;

	baikal_dp_write(dp->dp_base, BAIKAL_DP_SRC0_TRANSFER_UNIT_CONFIG,
		     tu |
		     (avg_bytes_per_tu << BAIKAL_DP_TU_CONFIG_SYMBOLS_PER_TU_SHIFT) |
		     ((avg_bytes_per_tu_x16 % 16) << BAIKAL_DP_TU_CONFIG_FRAC_SYMBOLS_PER_TU_SHIFT));

	/* Configure the initial wait cycle based on transfer unit size */
	if (tu < (avg_bytes_per_tu / 1000))
		init_wait = 0;
	else if ((avg_bytes_per_tu / 1000) <= 4)
		init_wait = tu;
	else
		init_wait = tu / 2; /* TODO sort out */
	coeff = avg_bytes_per_tu * 1000 / tu;
	if (coeff > 920)
		fifo_accumulation_delay = 4;
	else if (coeff > 740)
		fifo_accumulation_delay = 5;
	else if (coeff > 250)
		fifo_accumulation_delay = 6;
	else if (coeff > 70)
		fifo_accumulation_delay = 7;
	else
		fifo_accumulation_delay = 8;

	init_wait = 0x20; /* TODO sort out */
	fifo_accumulation_delay = 0x4; /* TODO sort out */
	baikal_dp_write(dp->dp_base, BAIKAL_DP_SRC0_DATA_CONTROL, (init_wait << BAIKAL_DP_SRC0_DATA_CONTROL_ACC_DELAY_SHIFT) | fifo_accumulation_delay);
}

static void
baikal_dp_encoder_atomic_mode_set(struct drm_encoder *encoder,
				struct drm_crtc_state *crtc_state,
				struct drm_connector_state *connector_state)
{
	struct baikal_dp *dp = encoder_to_dp(encoder);
	/*struct drm_display_mode *mode = &crtc_state->mode;*/
	struct drm_display_mode *adjusted_mode = &crtc_state->adjusted_mode;
	u32 drm_fourcc;
	/*u32 clock;
	int rate, max_rate = dp->link_config.max_rate;
	u8 max_lanes = dp->link_config.max_lanes;
	u8 bpp = dp->config.bpp;*/

	dp->adjusted_mode = adjusted_mode;

	/*
	 * This assumes that there is no conversion  between framebuffer
	 * and DP Tx
	 */
	drm_fourcc = encoder->crtc->primary->state->fb->format->format;

	/* SECTION B BEGIN */
	baikal_dp_set_color(dp, drm_fourcc);

	baikal_dp_encoder_mode_set_stream(dp, dp->adjusted_mode);
	baikal_dp_encoder_mode_set_transfer_unit(dp, dp->adjusted_mode);
	/* SECTION B END */
}

static const struct drm_encoder_funcs baikal_dp_encoder_funcs = {
	.destroy = drm_encoder_cleanup,
};

static const struct drm_encoder_helper_funcs baikal_dp_encoder_helper_funcs = {
	.enable			= baikal_dp_encoder_enable,
	.disable		= baikal_dp_encoder_disable,
	.atomic_mode_set	= baikal_dp_encoder_atomic_mode_set,
};

static void baikal_dp_hpd_work_func(struct work_struct *work)
{
	struct baikal_dp *dp;

	dp = container_of(work, struct baikal_dp, hpd_work.work);

	bool handled = true, ret = true;
	int rc;
	u8 esi[8] = {};

	while (handled) {
		u8 ack[8] = {};

		rc = drm_dp_dpcd_read(&dp->aux, DP_SINK_COUNT_ESI, esi, 8);
		if (rc != 8) {
			ret = false;
			break;
		}

		drm_dp_mst_hpd_irq_handle_event(&dp->mst_mgr, esi, ack, &handled);

		if (!handled)
			break;

		rc = drm_dp_dpcd_writeb(&dp->aux, DP_SINK_COUNT_ESI + 1, ack[1]);

		if (rc != 1) {
			ret = false;
			break;
		}

		drm_dp_mst_hpd_irq_send_new_request(&dp->mst_mgr);
	}

	if (!ret)
		dev_err(dp->dev, "MST IRQ failed %d %d\n", handled, rc);
	else
		dev_err(dp->dev, "MST IRQ OK %d %d\n", handled, rc);

	/*(if (dp->drm)
		drm_helper_hpd_irq_event(dp->drm);
	*/
}

static struct drm_prop_enum_list baikal_dp_bpc_enum[] = {
	{ 6, "6BPC" },
	{ 8, "8BPC" },
	{ 10, "10BPC" },
	{ 12, "12BPC" },
};

static void baikal_dp_hpd_pulse_work_func(struct work_struct *work)
{
	struct baikal_dp *dp;
	int ret;
	u8 link_status[DP_LINK_STATUS_SIZE];
	u8 bw_set, lane_set;

	dp = container_of(work, struct baikal_dp, hpd_pulse_work.work);

	if (!dp->enabled)
		return;

	if (!baikal_dp_txconnected(dp)) {
		dev_err(dp->dev, "incorrect HPD pulse received\n");
		return;
	}

	/* Read Link information from downstream device */
	ret = drm_dp_dpcd_read_link_status(&dp->aux, link_status);
	if (ret < 0)
		return;
	ret |= drm_dp_dpcd_read(&dp->aux, DP_LINK_BW_SET, &bw_set, 1);
	ret |= drm_dp_dpcd_read(&dp->aux, DP_LANE_COUNT_SET, &lane_set, 1);
	if (ret < 0)
		return;

	if (bw_set != DP_LINK_BW_8_1 &&
	    bw_set != DP_LINK_BW_5_4 &&
	    bw_set != DP_LINK_BW_2_7 &&
	    bw_set != DP_LINK_BW_1_62)
		goto retrain_link;

	lane_set &= DP_LANE_COUNT_MASK;
	if (lane_set != 1 && lane_set != 2 && lane_set != 4)
		goto retrain_link;

	/* Verify the link status */
	ret = drm_dp_channel_eq_ok(link_status, lane_set);
	if (!ret)
		goto retrain_link;

	return;

retrain_link:
	baikal_dp_stop(dp);
	baikal_dp_start(dp);
}

static irqreturn_t baikal_dp_irq_handler(int irq, void *data)
{
	struct baikal_dp *dp = (struct baikal_dp *)data;
	u32 intrstatus;

	intrstatus = baikal_dp_read(dp->dp_base, BAIKAL_DP_INTERRUPT_CAUSE);
	int_sig_state = intrstatus;
	baikal_dp_write(dp->dp_base, BAIKAL_DP_INTERRUPT_MASK, 0x7fff);

	if (!intrstatus)
		return IRQ_NONE;

	if (intrstatus & BAIKAL_DP_INTERRUPT_REPLY_RCVD_MASK) {
		dp->counters[3]++;
	}

	if (intrstatus & BAIKAL_DP_INTERRUPT_HPDEVENT_MASK) {
		dp->counters[2]++;
		/* dev_dbg_ratelimited(dp->dev, "hpdevent detected\n"); */
		/* schedule_delayed_work(&dp->hpd_work, 0); */
	}

	if (intrstatus & BAIKAL_DP_INTERRUPT_HPDPULSE_MASK) {
		/* schedule_delayed_work(&dp->hpd_pulse_work, 0); */
	}

	baikal_dp_write(dp->dp_base, BAIKAL_DP_INTERRUPT_MASK, 0x7fe0);

	return IRQ_HANDLED;
}

static int baikal_dp_connector_create(struct baikal_dp *dp, struct drm_device *drm)
{
	struct drm_encoder *encoder = &dp->encoder;
	struct drm_connector *connector = &dp->connector;
	unsigned int ret;

	baikal_dp_mainlink_en(dp, 0x0); /* TODO sort out */

	encoder->possible_crtcs = 1;
	drm_encoder_init(drm, encoder, &baikal_dp_encoder_funcs,
			 DRM_MODE_ENCODER_TMDS, NULL);
	drm_encoder_helper_add(encoder, &baikal_dp_encoder_helper_funcs);

	connector->polled = DRM_CONNECTOR_POLL_HPD;
	ret = drm_connector_init(encoder->dev, connector,
				 &baikal_dp_connector_funcs,
				 DRM_MODE_CONNECTOR_DisplayPort);
	if (ret) {
		dev_err(dp->dev, "failed to initialize the drm connector");
		goto error_encoder;
	}

	if (!no_edid)
		drm_connector_helper_add(connector, &baikal_dp_connector_helper_funcs);
	else
		drm_connector_helper_add(connector, &baikal_dp_connector_helper_funcs_noedid);
	drm_connector_register(connector);

	drm_connector_attach_encoder(connector, encoder);
	connector->dpms = DRM_MODE_DPMS_OFF;

	dp->drm = drm;
	dp->sync_prop = drm_property_create_bool(drm, 0, "sync");
	dp->bpc_prop = drm_property_create_enum(drm, 0, "bpc",
						baikal_dp_bpc_enum,
						ARRAY_SIZE(baikal_dp_bpc_enum));
	dp->config.misc0 &= ~BAIKAL_DP_SRC0_STREAM_MISC0_MASK;
	drm_object_attach_property(&connector->base, dp->sync_prop, false);
	ret = baikal_dp_set_bpc(dp, 8);
	drm_object_attach_property(&connector->base, dp->bpc_prop,
				   ret ? ret : 8);
	baikal_dp_update_bpp(dp);

	drm_object_attach_property(&connector->base,
				   connector->dev->mode_config.hdr_output_metadata_property, 0);

	baikal_dp_write(dp->dp_base, BAIKAL_DP_TRANSMITTER_ENABLE, 0);
	udelay(1);
	baikal_dp_init_aux(dp);
	baikal_dp_write(dp->dp_base, BAIKAL_DP_TRANSMITTER_ENABLE, 1);

	INIT_DELAYED_WORK(&dp->hpd_work, baikal_dp_hpd_work_func);
	INIT_DELAYED_WORK(&dp->hpd_pulse_work, baikal_dp_hpd_pulse_work_func);

	return 0;

/*error_prop:
	drm_property_destroy(dp->drm, dp->bpc_prop);
	drm_property_destroy(dp->drm, dp->sync_prop);
	baikal_dp_connector_destroy(&dp->connector);*/
error_encoder:
	drm_encoder_cleanup(&dp->encoder);

	return ret;
}

static int baikal_dp_parse_of(struct baikal_dp *dp)
{
	struct baikal_dp_config *config = &dp->config;

	/* TODO implement */

	config->max_lanes = 4;
	config->max_link_rate = BAIKAL_DP_HIGH_BIT_RATE_2;
	baikal_dp_set_color(dp, DRM_FORMAT_RGB888);
	config->misc0 |= BAIKAL_DP_SRC0_STREAM_MISC0_MASK;
	config->misc0 |= BAIKAL_DP_SRC0_BPC16_MASK;
	//config->misc0 |= BAIKAL_DP_SRC0_BPC8_MASK;

	return 0;
}

static void baikal_dp_debugfs_init(struct baikal_dp *dp);

int baikal_dp_probe(struct platform_device *pdev)
{
	struct drm_device *drm = platform_get_drvdata(pdev);
	struct baikal_vdu_crossbar *crossbar = drm_to_baikal_vdu_crossbar(drm);
	struct baikal_dp *dp;
	struct resource *res;
	int irq, ret;

	dp = devm_kzalloc(&pdev->dev, sizeof(*dp), GFP_KERNEL);
	if (!dp)
		return -ENOMEM;

	dp->drm = drm;

	dp->dpms = DRM_MODE_DPMS_OFF;
	dp->status = connector_status_disconnected;
	dp->dev = &pdev->dev;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "dp_base");
	dp->dp_base = devm_ioremap_resource(dp->dev, res);
	if (IS_ERR(dp->dp_base)) {
		dev_err(&pdev->dev, "couldn't map DisplayPort registers\n");
		return -ENODEV;
	}

	dp->reset_gpio = devm_gpiod_get_optional(dp->dev, "reset", GPIOD_ASIS);
	if (IS_ERR(dp->reset_gpio)) {
		dev_warn(dp->dev, "failed to get RESET GPIO\n");
		dp->reset_gpio = NULL;
	}

	ret = baikal_dp_parse_of(dp);
	if (ret < 0)
		return ret;

	dp->phy[0] = devm_phy_get(dp->dev, "dp_phy");
	if (IS_ERR(dp->phy[0]))
		return dev_err_probe(dp->dev, ret, "failed to get phy\n");

	ret = phy_init(dp->phy[0]);
	if (ret)
		goto error_phy;

	dp->axi_lite_clk = devm_clk_get(&pdev->dev, "s_axi_aclk");
	if (IS_ERR(dp->axi_lite_clk))
		return PTR_ERR(dp->axi_lite_clk);

	crossbar->dp = dp;
	dp->crossbar = crossbar;

	baikal_dp_debugfs_init(dp);

	dp->tx_link_config.vs_level = 0;
	dp->tx_link_config.pe_level = 0;

	dp->aux.name = "Xlnx_DP_AUX";
	dp->aux.dev = dp->dev;
	dp->aux.drm_dev = dp->drm;
	dp->aux.transfer = baikal_dp_aux_transfer;
	ret = drm_dp_aux_register(&dp->aux);
	if (ret < 0) {
		dev_err(dp->dev, "failed to initialize DP aux\n");
		goto error;
	}

	ret = baikal_dp_connector_create(dp, drm);
	if (ret < 0) {
		dev_err(dp->dev, "failed to initialize DP connector\n");
		goto error;
	}

	baikal_dp_write(dp->dp_base, BAIKAL_DP_INTERRUPT_MASK, 0x7fe1);

	irq = platform_get_irq_byname(pdev, "dptx_irq");
	if (irq < 0) {
		ret = irq;
		goto error;
	}

	ret = devm_request_threaded_irq(dp->dev, irq, NULL,
					baikal_dp_irq_handler, IRQF_ONESHOT,
					dev_name(dp->dev), dp);

	if (ret < 0)
		goto error;

	return 0;

error:
	drm_dp_aux_unregister(&dp->aux);
error_phy:
	dev_dbg(&pdev->dev, "baikal_dp_probe() error_phy:\n");

	return ret;
}

/*static void baikal_dp_remove(struct platform_device *pdev)
{
	struct baikal_dp *dp = platform_get_drvdata(pdev);

	baikal_dp_write(dp->dp_base, BAIKAL_DP_TRANSMITTER_ENABLE, 0);
	drm_dp_aux_unregister(&dp->aux);
}*/

static int baikal_dp_stats_show(struct seq_file *m, void *unused)
{
	struct baikal_dp *dp = m->private;

	int i;

	for (i = 0; i < ARRAY_SIZE(dp->counters); i++) {
		seq_printf(m, "COUNTER[%d]: 0x%08x\n", i, dp->counters[i]);
	}

	return 0;
}

DEFINE_SHOW_ATTRIBUTE(baikal_dp_stats);

static void baikal_dp_debugfs_init(struct baikal_dp *dp)
{
	struct dentry *debug_dir = debugfs_create_dir("baikal_dp", NULL);
	if (debug_dir == NULL) {
		dev_err(dp->dev, "failed to create debugfs directory\n");
	}
	debugfs_create_file("baikal_dp_stats", 0444, debug_dir, dp,
			    &baikal_dp_stats_fops);
}

static int baikal_dp_mode_configure(struct baikal_dp *dp, int pclock,
				    u8 current_bw)
{
	int max_rate = dp->link_config.max_rate;
	u8 bw_code;
	u8 max_lanes = dp->link_config.max_lanes;
	u8 max_link_rate_code = drm_dp_link_rate_to_bw_code(max_rate);
	u8 bpp = dp->config.bpp;
	u8 lane_cnt;

	/* Downshift from current bandwidth */
	switch (current_bw) {
	case DP_LINK_BW_5_4:
		bw_code = DP_LINK_BW_2_7;
		break;
	case DP_LINK_BW_2_7:
		bw_code = DP_LINK_BW_1_62;
		break;
	case DP_LINK_BW_1_62:
		dev_err(dp->dev, "can't downshift. already lowest link rate\n");
		return -EINVAL;
	default:
		/* If not given, start with max supported */
		bw_code = max_link_rate_code;
		break;
	}

	for (lane_cnt = 1; lane_cnt <= max_lanes; lane_cnt <<= 1) {
		int bw;
		u32 rate;

		bw = drm_dp_bw_code_to_link_rate(bw_code);
		rate = baikal_dp_max_rate(bw, lane_cnt, bpp);
		if (pclock <= rate) {
			dp->mode.bw_code = bw_code;
			dp->mode.lane_cnt = lane_cnt;
			dp->mode.pclock = pclock;
			return dp->mode.bw_code;
		}
	}

	dev_err(dp->dev, "failed to configure link values\n");

	return -EINVAL;
}

static void baikal_dp_adjust_train(struct baikal_dp *dp,
				   u8 link_status[DP_LINK_STATUS_SIZE])
{
	u8 *train_set = dp->train_set;
	u8 voltage = 0, preemphasis = 0;
	u8 i;

	for (i = 0; i < dp->mode.lane_cnt; i++) {
		u8 v = drm_dp_get_adjust_request_voltage(link_status, i);
		u8 p = drm_dp_get_adjust_request_pre_emphasis(link_status, i);

		if (v > voltage)
			voltage = v;

		if (p > preemphasis)
			preemphasis = p;
	}

	if (voltage >= DP_TRAIN_VOLTAGE_SWING_LEVEL_3)
		voltage |= DP_TRAIN_MAX_SWING_REACHED;

	if (preemphasis >= DP_TRAIN_PRE_EMPH_LEVEL_3)
		preemphasis |= DP_TRAIN_MAX_PRE_EMPHASIS_REACHED;

	for (i = 0; i < dp->mode.lane_cnt; i++)
		train_set[i] = voltage | preemphasis;
}

static int baikal_dp_update_vs_emph(struct baikal_dp *dp)
{
	struct phy_configure_opts_dp *phy_cfg = &dp->phy_opts.dp;
	unsigned int i;
	int ret;

	for (i = 0; i < dp->mode.lane_cnt; i++) {
		u8 train = dp->train_set[i];
		phy_cfg->voltage[i] = (train & DP_TRAIN_VOLTAGE_SWING_MASK)
				   >> DP_TRAIN_VOLTAGE_SWING_SHIFT;
		phy_cfg->pre[i] = (train & DP_TRAIN_PRE_EMPHASIS_MASK)
			       >> DP_TRAIN_PRE_EMPHASIS_SHIFT;
	}
        phy_cfg->set_voltages = 1;
        phy_cfg->set_lanes = 0;
        phy_cfg->set_rate = 0;
        phy_cfg->lanes = dp->mode.lane_cnt;
        phy_configure(dp->phy[0], &dp->phy_opts);

	usleep_range(1000, 1100); /* TODO sort out */

	ret = drm_dp_dpcd_write(&dp->aux, DP_TRAINING_LANE0_SET, dp->train_set,
				dp->mode.lane_cnt);
	if (ret < 0)
		return ret;

	return 0;
}

static int baikal_dp_link_train_cr(struct baikal_dp *dp)
{
	u8 link_status[DP_LINK_STATUS_SIZE];
	u8 lane_cnt = dp->mode.lane_cnt;
	u8 vs = 0, tries = 0;
	u16 max_tries, i;
	bool cr_done;
	int ret;

	baikal_dp_write(dp->dp_base, BAIKAL_DP_TRAINING_PATTERN_SET,
			DP_TRAINING_PATTERN_1);
	ret = drm_dp_dpcd_writeb(&dp->aux, DP_TRAINING_PATTERN_SET,
				 DP_TRAINING_PATTERN_1 |
				 DP_LINK_SCRAMBLING_DISABLE);
	if (ret < 0)
		return ret;

	/*
	 * 256 loops should be maximum iterations for 4 lanes and 4 values.
	 * So, This loop should exit before 512 iterations
	 */
	for (max_tries = 0; max_tries < 512; max_tries++) {
		ret = baikal_dp_update_vs_emph(dp);
		if (ret)
			return ret;

		drm_dp_link_train_clock_recovery_delay(&dp->aux, dp->dpcd);
		ret = drm_dp_dpcd_read_link_status(&dp->aux, link_status);
		if (ret < 0) {
			return ret;
		}

		cr_done = drm_dp_clock_recovery_ok(link_status, lane_cnt);
		if (cr_done)
			break;

		for (i = 0; i < lane_cnt; i++)
			if (!(dp->train_set[i] & DP_TRAIN_MAX_SWING_REACHED))
				break;
		if (i == lane_cnt)
			break;

		if ((dp->train_set[0] & DP_TRAIN_VOLTAGE_SWING_MASK) == vs)
			tries++;
		else
			tries = 0;

		if (tries == DP_MAX_TRAINING_TRIES)
			break;

		vs = dp->train_set[0] & DP_TRAIN_VOLTAGE_SWING_MASK;
		baikal_dp_adjust_train(dp, link_status);
	}

	if (!cr_done)
		return -EIO;

	return 0;
}

static int baikal_dp_link_train_ce(struct baikal_dp *dp)
{
	u8 link_status[DP_LINK_STATUS_SIZE];
	u8 lane_cnt = dp->mode.lane_cnt;
	u32 pat, tries;
	int ret;
	bool ce_done;

	/*if (dp->dpcd[DP_DPCD_REV] >= DP_DPCD_REV_12 &&
	    dp->dpcd[DP_MAX_LANE_COUNT] & DP_TPS3_SUPPORTED)
		pat = DP_TRAINING_PATTERN_3;
	else*/
		pat = DP_TRAINING_PATTERN_2;

	baikal_dp_write(dp->dp_base, BAIKAL_DP_TRAINING_PATTERN_SET, pat);
	ret = drm_dp_dpcd_writeb(&dp->aux, DP_TRAINING_PATTERN_SET,
				 pat | DP_LINK_SCRAMBLING_DISABLE);
	if (ret < 0)
		return ret;

	for (tries = 0; tries < DP_MAX_TRAINING_TRIES; tries++) {
		ret = baikal_dp_update_vs_emph(dp);
		if (ret)
			return ret;

		/*drm_dp_link_train_channel_eq_delay(&dp->aux, dp->dpcd);*/ /* TODO sort out */
		usleep_range(160000, 320000);
		ret = drm_dp_dpcd_read_link_status(&dp->aux, link_status);
		if (ret < 0)
			return ret;

		ce_done = drm_dp_channel_eq_ok(link_status, lane_cnt);
		if (ce_done)
			break;

		baikal_dp_adjust_train(dp, link_status);
	}

	if (!ce_done)
		return -EIO;

	return 0;
}

static int baikal_dp_train(struct baikal_dp *dp)
{
	u8 bw_code = dp->mode.bw_code;
	u8 lane_cnt = dp->mode.lane_cnt;
	u8 aux_lane_cnt = lane_cnt;
	bool enhanced;
	int ret;

	baikal_dp_write(dp->dp_base, BAIKAL_DP_LANE_COUNT_SET, lane_cnt);
	enhanced = drm_dp_enhanced_frame_cap(dp->dpcd);
	if (enhanced) {
		baikal_dp_write(dp->dp_base, BAIKAL_DP_ENHANCED_FRAME_EN, 1);
		aux_lane_cnt |= DP_LANE_COUNT_ENHANCED_FRAME_EN;
	}

	drm_dp_dpcd_writeb(&dp->aux, DP_DOWNSPREAD_CTRL, 0);

	ret = drm_dp_dpcd_writeb(&dp->aux, DP_LINK_BW_SET, bw_code);
	if (ret < 0) {
		dev_err(dp->dev, "failed to set DP bandwidth\n");
		return ret;
	}

	baikal_dp_set_linkrate(dp, bw_code);

	baikal_dp_write(dp->dp_base, BAIKAL_DP_SCRAMBLING_DISABLE, 1);

	memset(dp->train_set, 0, sizeof(dp->train_set));
	ret = baikal_dp_link_train_cr(dp);
	if (ret)
		return ret;

	ret = baikal_dp_link_train_ce(dp);
	if (ret) {
		return ret;
	}

	ret = drm_dp_dpcd_writeb(&dp->aux, DP_TRAINING_PATTERN_SET,
				 DP_TRAINING_PATTERN_DISABLE);
	if (ret < 0) {
		dev_err(dp->dev, "failed to disable training pattern\n");
		return ret;
	}
	baikal_dp_write(dp->dp_base, BAIKAL_DP_TRAINING_PATTERN_SET,
			DP_TRAINING_PATTERN_DISABLE);

	baikal_dp_write(dp->dp_base, BAIKAL_DP_SCRAMBLING_DISABLE, 0);

	return 0;
}

static int baikal_dp_train_loop(struct baikal_dp *dp)
{
	struct baikal_dp_mode *mode = &dp->mode;
	u8 bw = mode->bw_code;
	int ret;

	do {
		/*if (dp->status == connector_status_disconnected ||
		    !dp->enabled)
			return -1;*/

		ret = baikal_dp_train(dp);
		if (!ret)
			return ret;

		ret = baikal_dp_mode_configure(dp, mode->pclock, bw);
		if (ret < 0)
			goto err_out;

		bw = ret;
	} while (bw >= DP_LINK_BW_1_62);

err_out:
	dev_err(dp->dev, "failed to train the DP link\n");
	return -1;
}

static int baikal_dp_aux_cmd_submit(struct baikal_dp *dp, u32 cmd, u16 addr,
				    u8 *buf, u8 bytes, u8 *reply)
{
	bool is_read = (cmd & AUX_READ_BIT) ? true : false;
	u32 reg, i;

	/*
	//reg = baikal_dp_read(dp->dp_base, BAIKAL_DP_INTERRUPT_SIGNAL_STATE);
	reg = int_sig_state;
	if (reg & BAIKAL_DP_INTERRUPT_SIGNAL_STATE_REQUEST)
		return -EBUSY;
	*/

	baikal_dp_write(dp->dp_base, BAIKAL_DP_AUX_ADDRESS, addr);
	if (!is_read)
		for (i = 0; i < bytes; i++)
			baikal_dp_write(dp->dp_base, BAIKAL_DP_AUX_WRITE_FIFO,
					buf[i]);

	reg = cmd << BAIKAL_DP_AUX_COMMAND_CMD_SHIFT;
	if (!buf || !bytes)
		reg |= BAIKAL_DP_AUX_COMMAND_ADDRESS_ONLY;
	else
		reg |= (bytes - 1) << BAIKAL_DP_AUX_COMMAND_BYTES_SHIFT;
	baikal_dp_write(dp->dp_base, BAIKAL_DP_AUX_COMMAND, reg);

	/* Wait for reply to be delivered upto 2ms */
	for (i = 0; ; i++) {
		//reg = baikal_dp_read(dp->dp_base, BAIKAL_DP_INTERRUPT_SIGNAL_STATE);
		reg = int_sig_state;
		if (reg & BAIKAL_DP_INTERRUPT_STATE_REPLY)
			break;

		if (reg & BAIKAL_DP_INTERRUPT_STATE_REPLY_TIMEOUT ||
		    i == 2)
			return -ETIMEDOUT;

		usleep_range(1000, 1100);
	}

	reg = baikal_dp_read(dp->dp_base, BAIKAL_DP_AUX_REPLY_CODE);
	if (reply)
		*reply = reg;

	if (is_read &&
	    (reg == BAIKAL_DP_AUX_REPLY_CODE_AUX_ACK ||
	     reg == BAIKAL_DP_AUX_REPLY_CODE_I2C_ACK)) {
		usleep_range(300, 600);
		/*reg = baikal_dp_read(dp->dp_base, BAIKAL_DP_REPLY_DATA_COUNT);
		if ((reg & BAIKAL_DP_REPLY_DATA_COUNT_MASK) != bytes) {
			return -EIO;
		}*/

		for (i = 0; i < bytes; i++)
			buf[i] = baikal_dp_read(dp->dp_base, BAIKAL_DP_AUX_REPLY_DATA);
	}
	int_sig_state &= ~(BAIKAL_DP_INTERRUPT_STATE_REPLY + BAIKAL_DP_INTERRUPT_STATE_REPLY_TIMEOUT);

	return 0;
}

static ssize_t
baikal_dp_aux_transfer(struct drm_dp_aux *aux, struct drm_dp_aux_msg *msg)
{
	struct baikal_dp *dp = container_of(aux, struct baikal_dp, aux);
	int ret;
	unsigned int i, iter;

	/* Number of loops = timeout in msec / aux delay (400 usec) */
	iter = 50 * 1000 / 400;
	iter = iter ? iter : 1;

	for (i = 0; i < iter; i++) {
		ret = baikal_dp_aux_cmd_submit(dp, msg->request, msg->address,
					       msg->buffer, msg->size,
					       &msg->reply);
		if (!ret) {
			//dev_dbg(dp->dev, "aux %d retries\n", i);
			return msg->size;
		}

		if (dp->status == connector_status_disconnected) {
			dev_dbg(dp->dev, "no connected aux device\n");
			return -ENODEV;
		}

		usleep_range(400, 500);
	}

	dev_dbg(dp->dev, "failed to do aux transfer (%d)\n", ret);

	return ret;
}
