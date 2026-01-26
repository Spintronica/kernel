/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2023 Baikal Electronics, JSC
 *
 * Author: Pavel Parkhomenko <Pavel.Parkhomenko@baikalelectronics.ru>
 *
 */

#ifndef __BAIKAL_VDU_L1000_H__
#define __BAIKAL_VDU_L1000_H__

#define CRTC_DP0	0
#define CRTC_DP1	1
#define CRTC_EDP	2

#define BAIKAL_VDU_L1000_XRES_DEF	3840
#define BAIKAL_VDU_L1000_YRES_DEF	2160

#define BAIKAL_VDU_L1000_XRES_MAX	3840
#define BAIKAL_VDU_L1000_YRES_MAX	2160

#define VDU_CONF			0x000
#define INT_CTRL			0x010
#define INT_MASK			0x014
#define INT_STAT			0x018
#define INT_AXI_RESP			(1 << 0)
#define INT_VS_START			(1 << 1)
#define INT_VBP_START			(1 << 2)
#define INT_VA_START			(1 << 3)
#define INT_VFP_START			(1 << 4)
#define PWM_CTRL_0			0x080
#define PWM_CTRL_1			0x084

#define POLAR_CTRL			0x100
#define POLAR_CTRL_VS_INV	0x1
#define POLAR_CTRL_HS_INV	0x2
#define POLAR_CTRL_DE_INV	0x4

#define TE_EN				0x430

#define TE_CTRL_0			0x434
#define TE_CTRL_0_HBP_MASK	GENMASK(25, 16)
#define TE_CTRL_0_HBP(x)	((x) << 16)
#define TE_CTRL_0_HFP_MASK	GENMASK(9, 0)
#define TE_CTRL_0_HFP(x)	((x) << 0)

#define TE_CTRL_1			0x438
#define TE_CTRL_1_HS_MASK	GENMASK(25, 16)
#define TE_CTRL_1_HS(x)		((x) << 16)
#define TE_CTRL_1_HA_MASK	GENMASK(12, 0)
#define TE_CTRL_1_HA(x)		((x) << 0)

#define TE_CTRL_2			0x43c
#define TE_CTRL_2_VBP_MASK	GENMASK(25, 16)
#define TE_CTRL_2_VBP(x)	((x) << 16)
#define TE_CTRL_2_VFP_MASK	GENMASK(9, 0)
#define TE_CTRL_2_VFP(x)	((x) << 0)

#define TE_CTRL_3			0x440
#define TE_CTRL_3_VS_MASK	GENMASK(25, 16)
#define TE_CTRL_3_VS(x)		((x) << 16)
#define TE_CTRL_3_VA_MASK	GENMASK(12, 0)
#define TE_CTRL_3_VA(x)		((x) << 0)

#define PIPE_DMA_ADDR_0(x)	(0x800 + 0x800 * (x))
#define PIPE_DMA_ADDR_1(x)	(0x804 + 0x800 * (x))

#define PIPE_DMA_CTRL(x)			(0x808 + 0x800 * (x))
#define PIPE_DMA_CTRL_WORDS_MASK	GENMASK(1, 0)
#define PIPE_DMA_CTRL_WORDS(x)		(((x) >> 3) << 0)
#define PIPE_DMA_CTRL_OUTST_MASK	GENMASK(10, 8)
#define PIPE_DMA_CTRL_OUTST(x)		(((x) - 1) << 8)

#define PIPE_AXI_PARAM(x)	(0x810 + 0x800 * (x))
#define PIPE_WINDOW_X(x)	(0x900 + 0x800 * (x))
#define PIPE_WINDOW_Y(x)	(0x904 + 0x800 * (x))
#define PIPE_WINDOW_EN(x)	(0x908 + 0x800 * (x))
#define PIPE_PU_CTRL(x)		(0x910 + 0x800 * (x))

#define pipe_pu_ctrl_reg(pu_bpp, pu_alpha, pu_bgr, pu_mode) \
		(((pu_bpp) << 24) | \
		((pu_alpha) << 16) | \
		((pu_bgr) << 8) | \
		(pu_mode))

#define pipe_window_reg(start, end) \
		(((start) << 16) | \
		(end))

struct baikal_vdu_drm_format {
	u32 reg;
	u32 format;
};

#endif /* __BAIKAL_VDU_L1000_H__ */
