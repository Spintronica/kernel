// SPDX-License-Identifier: GPL-2.0
/*
 * Baikal VPU codec driver
 *
 * Copyright (C) 2024 Baikal Electronics, JSC
 */

#include <linux/clk.h>

#include "hantro.h"
#include "hantro_g2_regs.h"
#include "hantro_h1_regs.h"
#include "hantro_jpeg.h"
#include "baikal_vpu_regs.h"

#define BAIKAL_VDPU_ACLK_MAX_FREQ (400 * 1000 * 1000)
#define BAIKAL_VEPU_ACLK_MAX_FREQ (600 * 1000 * 1000)

static const struct hantro_fmt baikal_vdpu_dec_fmts[] = {
	{
		.fourcc = V4L2_PIX_FMT_NV12_4L4,
		.codec_mode = HANTRO_MODE_NONE,
		.match_depth = true,
		.frmsize = {
			.min_width = 144,
			.max_width = FMT_4K_WIDTH,
			.step_width = 8,
			.min_height = 144,
			.max_height = FMT_4K_WIDTH,
			.step_height = 8,
		},
	},
	{
		.fourcc = V4L2_PIX_FMT_NV15_4L4,
		.codec_mode = HANTRO_MODE_NONE,
		.match_depth = true,
		.frmsize = {
			.min_width = 144,
			.max_width = FMT_4K_WIDTH,
			.step_width = 8,
			.min_height = 144,
			.max_height = FMT_4K_WIDTH,
			.step_height = 8,
		},
	},
	{
		.fourcc = V4L2_PIX_FMT_H264_SLICE,
		.codec_mode = HANTRO_MODE_H264_DEC,
		.max_depth = 2,
		.frmsize = {
			.min_width = 144,
			.max_width = FMT_4K_WIDTH,
			.step_width = 16,
			.min_height = 144,
			.max_height = FMT_4K_WIDTH,
			.step_height = 16,
		},
	},
	{
		.fourcc = V4L2_PIX_FMT_VP9_FRAME,
		.codec_mode = HANTRO_MODE_VP9_DEC,
		.max_depth = 2,
		.frmsize = {
			.min_width = 144,
			.max_width = FMT_4K_WIDTH,
			.step_width = 8,
			.min_height = 144,
			.max_height = FMT_4K_WIDTH,
			.step_height = 8,
		},
	},
	{
		.fourcc = V4L2_PIX_FMT_HEVC_SLICE,
		.codec_mode = HANTRO_MODE_HEVC_DEC,
		.max_depth = 2,
		.frmsize = {
			.min_width = 144,
			.max_width = FMT_4K_WIDTH,
			.step_width = 8,
			.min_height = 144,
			.max_height = FMT_4K_WIDTH,
			.step_height = 8,
		},
	},
};

static const struct hantro_fmt baikal_vdpu_postproc_fmts[] = {
	{
		.fourcc = V4L2_PIX_FMT_NV12,
		.codec_mode = HANTRO_MODE_NONE,
		.postprocessed = true,
		.frmsize = {
			.min_width = 144,
			.max_width = FMT_4K_WIDTH,
			.step_width = 2,
			.min_height = 144,
			.max_height = FMT_4K_WIDTH,
			.step_height = 2,
		},
	},
	{
		.fourcc = V4L2_PIX_FMT_P010,
		.codec_mode = HANTRO_MODE_NONE,
		.postprocessed = true,
		.frmsize = {
			.min_width = 144,
			.max_width = FMT_4K_WIDTH,
			.step_width = 2,
			.min_height = 144,
			.max_height = FMT_4K_WIDTH,
			.step_height = 2,
		},
	},
};

static void baikal_vdpu_reset(struct hantro_ctx *ctx)
{
	struct hantro_dev *vpu = ctx->dev;
	int i;

	/* TODO rework reset */
	for (i = 1; i < 415; ++i)
		writel(0, vpu->priv + BAIKAL_VDPU_CMDBUF_OFFSET + 4 * i);
	writel(0, vpu->priv + BAIKAL_VDPU_IRQ_CMDBUF_OFFSET + 0x4);
	writel((BAIKAL_VDPU_CMDBUF_SIZE + BAIKAL_VDPU_IRQ_CMDBUF_SIZE) / 8,
		vpu->dec_base + BAIKAL_VDPU_REG_CMDBUF_LENGTH);
	writel(vpu->dma_handle + BAIKAL_VDPU_CMDBUF_OFFSET,
		vpu->dec_base + BAIKAL_VDPU_REG_CMDBUF_ADDR_LSB);
	writel(0, vpu->dec_base + BAIKAL_VDPU_REG_CMDBUF_ID);
	writel(0, vpu->dec_base + BAIKAL_VDPU_REG_CMDBUF_COUNT);
	writel(1, vpu->dec_base + BAIKAL_VDPU_REG_CMDBUF_READY_COUNT);
	writel(readl(vpu->dec_base + BAIKAL_VDPU_REG_CMDBUF_CTRL) | BIT(0),
	       vpu->dec_base + BAIKAL_VDPU_REG_CMDBUF_CTRL);

	for (i = 0; i < BAIKAL_VDPU_CMDBUF_MAX_RETRIES; ++i) {
		if (!(readl(vpu->dec_base + BAIKAL_VDPU_REG_CMDBUF_CTRL) & BIT(0))) {
			writel(0x5f, vpu->dec_base + 0x44);
			break;
		}

		ndelay(BAIKAL_VDPU_CMDBUF_DELAY_NS);
	}
}

static const struct hantro_codec_ops baikal_vdpu_codec_ops[] = {
	[HANTRO_MODE_H264_DEC] = {
		.run = baikal_vpu_h264_dec_run,
		.reset = baikal_vdpu_reset,
		.init = hantro_h264_dec_init,
		.exit = hantro_h264_dec_exit,
	},
	[HANTRO_MODE_HEVC_DEC] = {
		.run = hantro_g2_hevc_dec_run,
		.reset = baikal_vdpu_reset,
		.init = hantro_hevc_dec_init,
		.exit = hantro_hevc_dec_exit,
	},
	[HANTRO_MODE_VP9_DEC] = {
		.run = hantro_g2_vp9_dec_run,
		.reset = baikal_vdpu_reset,
		.done = hantro_g2_vp9_dec_done,
		.init = hantro_vp9_dec_init,
		.exit = hantro_vp9_dec_exit,
	},
};

static irqreturn_t baikal_vdpu_irq(int irq, void *dev_id)
{
	struct hantro_dev *vpu = dev_id;
	enum vb2_buffer_state state;
	u32 status;

	status = vdpu_read(vpu, G2_REG_INTERRUPT);
	state = (status & G2_REG_INTERRUPT_DEC_RDY_INT) ?
		 VB2_BUF_STATE_DONE : VB2_BUF_STATE_ERROR;

	vdpu_write(vpu, 0, G2_REG_INTERRUPT);

	hantro_irq_done(vpu, state);

	return IRQ_HANDLED;
}

static const struct hantro_irq baikal_vdpu_irqs[] = {
	{ "vdpu", baikal_vdpu_irq },
};

static int baikal_vdpu_hw_init(struct hantro_dev *vpu)
{
	clk_set_rate(vpu->clocks[0].clk, BAIKAL_VDPU_ACLK_MAX_FREQ);

	vpu->priv = dma_alloc_coherent(vpu->dev,
		BAIKAL_VDPU_MAX_TILE_INFO_SIZE + BAIKAL_VDPU_CMDBUF_SIZE +
		BAIKAL_VDPU_IRQ_CMDBUF_SIZE + BAIKAL_VDPU_READ_CMDBUF_SIZE,
		&vpu->dma_handle, GFP_KERNEL);
	if (!vpu->priv) {
		dev_err(vpu->dev, "Could not alloc DMA region.\n");
		return -ENOMEM;
	}

	writel(0x099e0808, vpu->priv + BAIKAL_VDPU_CMDBUF_OFFSET);

	writel(0x08010804, vpu->priv + BAIKAL_VDPU_IRQ_CMDBUF_OFFSET);
	writel(BIT(28), vpu->priv + BAIKAL_VDPU_IRQ_CMDBUF_OFFSET + 0x8);

	writel(vpu->dma_handle + BAIKAL_VDPU_READ_CMDBUF_OFFSET + 0x18,
		vpu->priv + BAIKAL_VDPU_READ_CMDBUF_OFFSET + 0x4);
	writel(0, vpu->priv + BAIKAL_VDPU_READ_CMDBUF_OFFSET + 0x8);
	writel(BIT(28), vpu->priv + BAIKAL_VDPU_READ_CMDBUF_OFFSET + 0x10);

	return 0;
}

static void baikal_vdpu_hw_deinit(struct hantro_dev *vpu)
{
	dma_free_coherent(vpu->dev,
			  BAIKAL_VDPU_MAX_TILE_INFO_SIZE + BAIKAL_VDPU_CMDBUF_SIZE +
			  BAIKAL_VDPU_IRQ_CMDBUF_SIZE + BAIKAL_VDPU_READ_CMDBUF_SIZE,
			  vpu->priv, vpu->dma_handle);
}

static const char * const baikal_vdpu_clk_names[] = {
	"aclk", "pclk"
};

const struct hantro_variant baikal_vdpu_variant = {
	.dec_offset = 0x0,
	.dec_fmts = baikal_vdpu_dec_fmts,
	.num_dec_fmts = ARRAY_SIZE(baikal_vdpu_dec_fmts),
	.postproc_fmts = baikal_vdpu_postproc_fmts,
	.num_postproc_fmts = ARRAY_SIZE(baikal_vdpu_postproc_fmts),
	.postproc_ops = &hantro_g2_postproc_ops,
	.codec = HANTRO_HEVC_DECODER |
		 HANTRO_VP9_DECODER | HANTRO_H264_DECODER,
	.codec_ops = baikal_vdpu_codec_ops,
	.irqs = baikal_vdpu_irqs,
	.num_irqs = ARRAY_SIZE(baikal_vdpu_irqs),
	.init = baikal_vdpu_hw_init,
	.deinit = baikal_vdpu_hw_deinit,
	.clk_names = baikal_vdpu_clk_names,
	.num_clocks = ARRAY_SIZE(baikal_vdpu_clk_names),
	.baikal_regs = 1
};

static const struct hantro_fmt baikal_vepu_enc_fmts[] = {
	{
		.fourcc = V4L2_PIX_FMT_YUV420M,
		.codec_mode = HANTRO_MODE_NONE,
		.enc_fmt = ROCKCHIP_VPU_ENC_FMT_YUV420P,
	},
	{
		.fourcc = V4L2_PIX_FMT_NV12M,
		.codec_mode = HANTRO_MODE_NONE,
		.enc_fmt = ROCKCHIP_VPU_ENC_FMT_YUV420SP,
	},
	{
		.fourcc = V4L2_PIX_FMT_YUYV,
		.codec_mode = HANTRO_MODE_NONE,
		.enc_fmt = ROCKCHIP_VPU_ENC_FMT_YUYV422,
	},
	{
		.fourcc = V4L2_PIX_FMT_UYVY,
		.codec_mode = HANTRO_MODE_NONE,
		.enc_fmt = ROCKCHIP_VPU_ENC_FMT_UYVY422,
	},
	{
		.fourcc = V4L2_PIX_FMT_JPEG,
		.codec_mode = HANTRO_MODE_JPEG_ENC,
		.max_depth = 2,
		.header_size = JPEG_HEADER_SIZE,
		.frmsize = {
			.min_width = 96,
			.max_width = 8176,
			.step_width = 16,
			.min_height = 32,
			.max_height = 8176,
			.step_height = 16,
		},
	},
	{
		.fourcc = V4L2_PIX_FMT_VP8_FRAME,
		.codec_mode = HANTRO_MODE_VP8_ENC,
		.max_depth = 2,
		.frmsize = {
			.min_width = 144,
			.max_width = 4080,
			.step_width = 16,
			.min_height = 96,
			.max_height = 4080,
			.step_height = 16,
		},
	},
	{
		.fourcc = V4L2_PIX_FMT_H264_SLICE,
		.codec_mode = HANTRO_MODE_H264_ENC,
		.max_depth = 2,
		.frmsize = {
			.min_width = 144,
			.max_width = 4080,
			.step_width = 16,
			.min_height = 96,
			.max_height = 4080,
			.step_height = 16,
		},
	},
};

static void baikal_vepu_reset(struct hantro_ctx *ctx)
{
	struct hantro_dev *vpu = ctx->dev;

	vepu_write(vpu, H1_REG_INTERRUPT_DIS_BIT, H1_REG_INTERRUPT);
	vepu_write(vpu, 0, H1_REG_ENC_CTRL);
	vepu_write(vpu, 0, H1_REG_AXI_CTRL);
	vepu_write(vpu, (uint32_t)~H1_REG_INTERRUPT_DIS_BIT, H1_REG_INTERRUPT);
}

static const struct hantro_codec_ops baikal_vepu_codec_ops[] = {
	[HANTRO_MODE_JPEG_ENC] = {
		.run = hantro_h1_jpeg_enc_run,
		.reset = baikal_vepu_reset,
		.done = hantro_h1_jpeg_enc_done,
	},
	[HANTRO_MODE_VP8_ENC] = {
		.run = hantro_h1_vp8_enc_run,
		.reset = baikal_vepu_reset,
		.init = hantro_vp8_enc_init,
		.done = hantro_h1_vp8_enc_done,
		.exit = hantro_vp8_enc_exit,
	},
	[HANTRO_MODE_H264_ENC] = {
		.run = hantro_h1_h264_enc_run,
		.reset = baikal_vepu_reset,
		.init = hantro_h264_enc_init,
		.done = hantro_h1_h264_enc_done,
		.exit = hantro_h264_enc_exit,
	},
};

static irqreturn_t baikal_vepu_irq(int irq, void *dev_id)
{
	struct hantro_dev *vpu = dev_id;
	enum vb2_buffer_state state;
	u32 status;

	status = vepu_read(vpu, H1_REG_INTERRUPT);
	state = (status & H1_REG_INTERRUPT_FRAME_RDY) ?
		VB2_BUF_STATE_DONE : VB2_BUF_STATE_ERROR;

	vepu_write(vpu, H1_REG_INTERRUPT_DIS_BIT, H1_REG_INTERRUPT);
	vepu_write(vpu, 0, H1_REG_ENC_CTRL);
	vepu_write(vpu, 0, H1_REG_AXI_CTRL);
	vepu_write(vpu, (uint32_t)~H1_REG_INTERRUPT_DIS_BIT, H1_REG_INTERRUPT);

	hantro_irq_done(vpu, state);

	return IRQ_HANDLED;
}

static const struct hantro_irq baikal_vepu_irqs[] = {
	{ "vepu", baikal_vepu_irq },
};

static int baikal_vepu_hw_init(struct hantro_dev *vpu)
{
	clk_set_rate(vpu->clocks[0].clk, BAIKAL_VEPU_ACLK_MAX_FREQ);
	return 0;
}

static const char * const baikal_vepu_clk_names[] = {
	"aclk"
};

const struct hantro_variant baikal_vepu_variant = {
	.enc_offset = 0x0,
	.enc_fmts = baikal_vepu_enc_fmts,
	.num_enc_fmts = ARRAY_SIZE(baikal_vepu_enc_fmts),
	.codec = HANTRO_JPEG_ENCODER | HANTRO_VP8_ENCODER | HANTRO_H264_ENCODER,
	.codec_ops = baikal_vepu_codec_ops,
	.irqs = baikal_vepu_irqs,
	.num_irqs = ARRAY_SIZE(baikal_vepu_irqs),
	.init = baikal_vepu_hw_init,
	.clk_names = baikal_vepu_clk_names,
	.num_clocks = ARRAY_SIZE(baikal_vepu_clk_names),
	.baikal_regs = 1
};
