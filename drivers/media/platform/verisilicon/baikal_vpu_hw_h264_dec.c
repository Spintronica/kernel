// SPDX-License-Identifier: GPL-2.0
/*
 * Hantro VPU codec driver
 *
 * Copyright (c) 2014 Rockchip Electronics Co., Ltd.
 *	Hertz Wong <hertz.wong@rock-chips.com>
 *	Herman Chen <herman.chen@rock-chips.com>
 *
 * Copyright (C) 2014 Google, Inc.
 *	Tomasz Figa <tfiga@chromium.org>
 */

#include <linux/types.h>
#include <linux/sort.h>

#include <media/v4l2-mem2mem.h>

#include "hantro_hw.h"
#include "hantro_v4l2.h"
#include "baikal_vpu_regs.h"
#include "hantro_g2_regs.h"

static void set_params(struct hantro_ctx *ctx, struct vb2_v4l2_buffer *src_buf)
{
	const struct hantro_h264_dec_ctrls *ctrls = &ctx->h264_dec.ctrls;
	const struct v4l2_ctrl_h264_decode_params *dec_param = ctrls->decode;
	const struct v4l2_ctrl_h264_sps *sps = ctrls->sps;
	const struct v4l2_ctrl_h264_pps *pps = ctrls->pps;
	struct hantro_dev *vpu = ctx->dev;
	u32 reg;

	/* Decoder control register 0. */
	reg = 0;
	if (sps->flags & V4L2_H264_SPS_FLAG_MB_ADAPTIVE_FRAME_FIELD)
		reg |= BAIKAL_REG_DEC_CTL0_SEQ_MBAFF_E;
	if (sps->profile_idc > 66) {
		reg |= BAIKAL_REG_DEC_CTL0_PICORD_COUNT_E;
		if (dec_param->nal_ref_idc)
			reg |= BAIKAL_REG_DEC_CTL0_WRITE_MVS_E;
	}
	if (!(sps->flags & V4L2_H264_SPS_FLAG_FRAME_MBS_ONLY) &&
	    (sps->flags & V4L2_H264_SPS_FLAG_MB_ADAPTIVE_FRAME_FIELD ||
	     dec_param->flags & V4L2_H264_DECODE_PARAM_FLAG_FIELD_PIC))
		reg |= BAIKAL_REG_DEC_CTL0_PIC_INTERLACE_E;
	if (dec_param->flags & V4L2_H264_DECODE_PARAM_FLAG_FIELD_PIC)
		reg |= BAIKAL_REG_DEC_CTL0_PIC_FIELDMODE_E;
	if (!(dec_param->flags & V4L2_H264_DECODE_PARAM_FLAG_BOTTOM_FIELD))
		reg |= BAIKAL_REG_DEC_CTL0_PIC_TOPFIELD_E;
	vdpu_write_relaxed(vpu, reg, BAIKAL_REG_DEC_CTL0);

	/* Decoder control register 1. */
	reg = BAIKAL_REG_DEC_CTL1_PIC_WIDTH_IN_CBS(DIV_ROUND_UP(ctx->src_fmt.width, 8)) |
	      BAIKAL_REG_DEC_CTL1_PIC_HEIGHT_IN_CBS(DIV_ROUND_UP(ctx->src_fmt.height, 8)) |
	      BAIKAL_REG_DEC_CTL1_REF_FRAMES(sps->max_num_ref_frames);
	vdpu_write_relaxed(vpu, reg, BAIKAL_REG_DEC_CTL1);

	/* Decoder control register 2. */
	reg = BAIKAL_REG_DEC_CTL2_CH_QP_OFFSET(pps->chroma_qp_index_offset) |
	      BAIKAL_REG_DEC_CTL2_CH_QP_OFFSET2(pps->second_chroma_qp_index_offset);
	if (pps->flags & V4L2_H264_PPS_FLAG_SCALING_MATRIX_PRESENT)
		reg |= BAIKAL_REG_DEC_CTL2_TYPE1_QUANT_E;
	if (!(sps->flags & V4L2_H264_SPS_FLAG_FRAME_MBS_ONLY))
		reg |= BAIKAL_REG_DEC_CTL2_FIELDPIC_FLAG_E;
	vdpu_write_relaxed(vpu, reg, BAIKAL_REG_DEC_CTL2);

	/* Decoder control register 3. */
	vdpu_write_relaxed(vpu, vb2_get_plane_payload(&src_buf->vb2_buf, 0), BAIKAL_REG_DEC_CTL3);

	/* Decoder control register 9. */
	reg = BAIKAL_REG_DEC_CTL9_START_CODE_E |
	      BAIKAL_REG_DEC_CTL9_INIT_QP(pps->pic_init_qp_minus26 + 26);
	vdpu_write_relaxed(vpu, reg, BAIKAL_REG_DEC_CTL9);

	/* Decoder control register 4. */
	reg = BAIKAL_REG_DEC_CTL4_FRAMENUM_LEN(sps->log2_max_frame_num_minus4 + 4) |
	      BAIKAL_REG_DEC_CTL4_FRAMENUM(dec_param->frame_num) |
	      BAIKAL_REG_DEC_CTL4_WEIGHT_BIPR_IDC(pps->weighted_bipred_idc);
	if (pps->flags & V4L2_H264_PPS_FLAG_ENTROPY_CODING_MODE)
		reg |= BAIKAL_REG_DEC_CTL4_CABAC_E;
	if (sps->flags & V4L2_H264_SPS_FLAG_DIRECT_8X8_INFERENCE)
		reg |= BAIKAL_REG_DEC_CTL4_DIR_8X8_INFER_E;
	if (sps->profile_idc >= 100 && sps->chroma_format_idc == 0)
		reg |= BAIKAL_REG_DEC_CTL4_BLACKWHITE_E;
	if (pps->flags & V4L2_H264_PPS_FLAG_WEIGHTED_PRED)
		reg |= BAIKAL_REG_DEC_CTL4_WEIGHT_PRED_E;
	vdpu_write_relaxed(vpu, reg, BAIKAL_REG_DEC_CTL4);

	/* Decoder control register 5. */
	reg = BAIKAL_REG_DEC_CTL5_REFPIC_MK_LEN(dec_param->dec_ref_pic_marking_bit_size);
	if (pps->flags & V4L2_H264_PPS_FLAG_CONSTRAINED_INTRA_PRED)
		reg |= BAIKAL_REG_DEC_CTL5_CONST_INTRA_E;
	if (pps->flags & V4L2_H264_PPS_FLAG_DEBLOCKING_FILTER_CONTROL_PRESENT)
		reg |= BAIKAL_REG_DEC_CTL5_FILT_CTRL_PRES;
	if (pps->flags & V4L2_H264_PPS_FLAG_REDUNDANT_PIC_CNT_PRESENT)
		reg |= BAIKAL_REG_DEC_CTL5_RDPIC_CNT_PRES;
	if (pps->flags & V4L2_H264_PPS_FLAG_TRANSFORM_8X8_MODE)
		reg |= BAIKAL_REG_DEC_CTL5_8X8TRANS_FLAG_E;
	if (dec_param->flags & V4L2_H264_DECODE_PARAM_FLAG_IDR_PIC)
		reg |= BAIKAL_REG_DEC_CTL5_IDR_PIC_E;
	vdpu_write_relaxed(vpu, reg, BAIKAL_REG_DEC_CTL5);
	hantro_reg_write(vpu, &g2_bit_depth_y_minus8, sps->bit_depth_luma_minus8);
	hantro_reg_write(vpu, &g2_bit_depth_c_minus8, sps->bit_depth_chroma_minus8);

	/* Decoder control register 6. */
	reg = BAIKAL_REG_DEC_CTL6_PPS_ID(pps->pic_parameter_set_id) |
	      BAIKAL_REG_DEC_CTL6_REFIDX1_ACTIVE(pps->num_ref_idx_l1_default_active_minus1 + 1) |
	      BAIKAL_REG_DEC_CTL6_REFIDX0_ACTIVE(pps->num_ref_idx_l0_default_active_minus1 + 1) |
	      BAIKAL_REG_DEC_CTL6_POC_LENGTH(dec_param->pic_order_cnt_bit_size);
	vdpu_write_relaxed(vpu, reg, BAIKAL_REG_DEC_CTL6);

	/* Decoder control register 8. */
	reg = BAIKAL_REG_DEC_CTL8_IDR_PIC_ID_H10(dec_param->idr_pic_id);
	reg |= (3 << 13) | (3 << 10);
	vdpu_write_relaxed(vpu, reg, BAIKAL_REG_DEC_CTL8);

	/* Error concealment register. */
	vdpu_write_relaxed(vpu, 0, BAIKAL_REG_ERR_CONC);

	/* Prediction filter tap register. */
	vdpu_write_relaxed(vpu,
			   BAIKAL_REG_PRED_FLT_PRED_BC_TAP_0_0(1) |
			   BAIKAL_REG_PRED_FLT_PRED_BC_TAP_0_1(-5 & 0x3ff) |
			   BAIKAL_REG_PRED_FLT_PRED_BC_TAP_0_2(20),
			   BAIKAL_REG_PRED_FLT);

	vdpu_write_relaxed(vpu, BAIKAL_REG_ADV_PREFETCH_CTRL_APF_THRESHOLD(8),
			   BAIKAL_REG_ADV_PREFETCH_CTRL);

	vdpu_write_relaxed(vpu,
			   BAIKAL_REG_CONFIG_DEC_CLK_GATE_E,
			   BAIKAL_REG_CONFIG);

	hantro_reg_write(vpu, &g2_apf_threshold, 8);
}

static void set_ref(struct hantro_ctx *ctx)
{
	const struct v4l2_h264_reference *b0_reflist, *b1_reflist, *p_reflist;
	struct hantro_dev *vpu = ctx->dev;
	size_t cr_offset = ctx->dst_fmt.width * ctx->dst_fmt.height * ctx->bit_depth / 8;
	size_t mv_offset = ALIGN((cr_offset * 3) / 2, 16);
	u32 reg;
	dma_addr_t luma_addr;
	int i;

	vdpu_write_relaxed(vpu, ctx->h264_dec.dpb_longterm, BAIKAL_REG_LONG_TERM_FLAGS);
	vdpu_write_relaxed(vpu, ctx->h264_dec.dpb_valid, BAIKAL_REG_VALID_FLAGS);

	for (i = 0; i < 8; ++i) {
		reg = BAIKAL_REG_REF_PIC_REFER_NBR(0, hantro_h264_get_ref_nbr(ctx, 2 * i)) |
		      BAIKAL_REG_REF_PIC_REFER_NBR(1, hantro_h264_get_ref_nbr(ctx, 2 * i + 1));
		vdpu_write_relaxed(vpu, reg, BAIKAL_REG_REF_PIC(i));
	}

	b0_reflist = ctx->h264_dec.reflists.b0;
	b1_reflist = ctx->h264_dec.reflists.b1;
	p_reflist = ctx->h264_dec.reflists.p;

	for (i = 0; i < 5; ++i) {
		reg = BAIKAL_REG_BD_REF_PIC_BINIT_RLIST_F(0, b0_reflist[3 * i].index) |
		      BAIKAL_REG_BD_REF_PIC_BINIT_RLIST_F(1, b0_reflist[3 * i + 1].index) |
		      BAIKAL_REG_BD_REF_PIC_BINIT_RLIST_F(2, b0_reflist[3 * i + 2].index) |
		      BAIKAL_REG_BD_REF_PIC_BINIT_RLIST_B(0, b1_reflist[3 * i].index) |
		      BAIKAL_REG_BD_REF_PIC_BINIT_RLIST_B(1, b1_reflist[3 * i + 1].index) |
		      BAIKAL_REG_BD_REF_PIC_BINIT_RLIST_B(2, b1_reflist[3 * i + 2].index);
		vdpu_write_relaxed(vpu, reg, BAIKAL_REG_BD_REF_PIC(i));
	}

	reg = BAIKAL_REG_REF_PIC_INIT_RLIST_F(b0_reflist[15].index) |
	      BAIKAL_REG_REF_PIC_INIT_RLIST_B(b1_reflist[15].index);
	vdpu_write_relaxed(vpu, reg, BAIKAL_REG_REF_PIC_INIT_RLIST);

	reg = BAIKAL_REG_BD_P_REF_PIC_PINIT_RLIST_F(0, p_reflist[0].index) |
	      BAIKAL_REG_BD_P_REF_PIC_PINIT_RLIST_F(1, p_reflist[1].index) |
	      BAIKAL_REG_BD_P_REF_PIC_PINIT_RLIST_F(2, p_reflist[2].index) |
	      BAIKAL_REG_BD_P_REF_PIC_PINIT_RLIST_F(3, p_reflist[3].index);
	vdpu_write_relaxed(vpu, reg, BAIKAL_REG_BD_P_REF_PIC);

	for (i = 0; i < 2; ++i) {
		reg = BAIKAL_REG_FWD_PIC_PINIT_RLIST_F(0, p_reflist[6 * i + 4].index) |
		      BAIKAL_REG_FWD_PIC_PINIT_RLIST_F(1, p_reflist[6 * i + 5].index) |
		      BAIKAL_REG_FWD_PIC_PINIT_RLIST_F(2, p_reflist[6 * i + 6].index) |
		      BAIKAL_REG_FWD_PIC_PINIT_RLIST_F(3, p_reflist[6 * i + 7].index) |
		      BAIKAL_REG_FWD_PIC_PINIT_RLIST_F(4, p_reflist[6 * i + 8].index) |
		      BAIKAL_REG_FWD_PIC_PINIT_RLIST_F(5, p_reflist[6 * i + 9].index);
		vdpu_write_relaxed(vpu, reg, BAIKAL_REG_FWD_PIC(i));
	}

	/* Set up addresses of DPB buffers. */
	for (i = 0; i < HANTRO_H264_DPB_SIZE; i++) {
		luma_addr = hantro_h264_get_ref_buf(ctx, i);
		hantro_write_addr(vpu, G2_REF_LUMA_ADDR(i), luma_addr);
		hantro_write_addr(vpu, G2_REF_CHROMA_ADDR(i), luma_addr + cr_offset);
		hantro_write_addr(vpu, G2_REF_MV_ADDR(i), luma_addr + mv_offset);
	}
}

static void set_buffers(struct hantro_ctx *ctx, struct vb2_v4l2_buffer *src_buf)
{
	const struct hantro_h264_dec_ctrls *ctrls = &ctx->h264_dec.ctrls;
	struct vb2_v4l2_buffer *dst_buf;
	struct hantro_dev *vpu = ctx->dev;
	dma_addr_t src_dma, dst_dma;
	size_t offset = 0;

	/* Source (stream) buffer. */
	src_dma = vb2_dma_contig_plane_dma_addr(&src_buf->vb2_buf, 0);
	hantro_write_addr(vpu, G2_STREAM_ADDR, src_dma);
	hantro_reg_write(vpu, &g2_stream_len, vb2_get_plane_payload(&src_buf->vb2_buf, 0));
	hantro_reg_write(vpu, &g2_strm_buffer_len, vb2_plane_size(&src_buf->vb2_buf, 0));
	hantro_reg_write(vpu, &g2_strm_start_offset, 0);

	/* Destination (decoded frame) buffer. */
	dst_buf = hantro_get_dst_buf(ctx);
	dst_dma = hantro_get_dec_buf_addr(ctx, &dst_buf->vb2_buf);
	/* Adjust dma addr to start at second line for bottom field */
	if (ctrls->decode->flags & V4L2_H264_DECODE_PARAM_FLAG_BOTTOM_FIELD)
		offset = ALIGN(ctx->src_fmt.width * ctx->bit_depth / 8, MB_DIM);
	vdpu_write_relaxed(vpu, (dst_dma + offset) & ~0x3, BAIKAL_REG_DEC_OUT_BASE);
	vdpu_write_relaxed(vpu, (dst_dma + offset +
				 (ctx->dst_fmt.width * ctx->dst_fmt.height * ctx->bit_depth / 8)) & ~0x3,
			   BAIKAL_REG(99));

	/* Higher profiles require DMV buffer appended to reference frames. */
	if (ctrls->sps->profile_idc > 66 && ctrls->decode->nal_ref_idc) {
		unsigned int bytes_per_mb = 384;

		/* DMV buffer for monochrome start directly after Y-plane */
		if (ctrls->sps->profile_idc >= 100 &&
		    ctrls->sps->chroma_format_idc == 0)
			bytes_per_mb = 256;
		offset = bytes_per_mb * ctx->bit_depth / 8 * MB_WIDTH(ctx->src_fmt.width) *
			 MB_HEIGHT(ctx->src_fmt.height);

		/*
		 * DMV buffer is split in two for field encoded frames,
		 * adjust offset for bottom field
		 */
		if (ctrls->decode->flags & V4L2_H264_DECODE_PARAM_FLAG_BOTTOM_FIELD)
			offset += 32 * ctx->bit_depth / 8 * MB_WIDTH(ctx->src_fmt.width) *
				  MB_HEIGHT(ctx->src_fmt.height);
		vdpu_write_relaxed(vpu, (dst_dma + offset) & ~0x3, BAIKAL_REG_DIR_MV_BASE);
	}

	/* Auxiliary buffer prepared in hantro_g1_h264_dec_prepare_table(). */
	vdpu_write_relaxed(vpu, ctx->h264_dec.priv.dma & ~0x3, BAIKAL_REG_QTABLE_BASE);
}

int baikal_vpu_h264_dec_run(struct hantro_ctx *ctx)
{
	struct hantro_dev *vpu = ctx->dev;
	struct vb2_v4l2_buffer *src_buf;
	int ret;

	ctx->codec_ops->reset(ctx);

	/* Prepare the H264 decoder context. */
	ret = hantro_h264_dec_prepare_run(ctx);
	if (ret)
		return ret;

	src_buf = hantro_get_src_buf(ctx);
	set_params(ctx, src_buf);
	set_ref(ctx);
	set_buffers(ctx, src_buf);

	hantro_end_prepare_run(ctx);

	hantro_reg_write(vpu, &g2_mode, H264_HIGH_10_DEC_MODE);
	hantro_reg_write(vpu, &g2_clk_gate_e_baikal, 1);

	/* Don't disable output */
	hantro_reg_write(vpu, &g2_out_dis, 0);

	/* Don't compress buffers */
	hantro_reg_write(ctx->dev, &g2_ref_comp_bps_baikal, 1);

	/* Bus width and max burst */
	hantro_reg_write(vpu, &g2_buswidth, BUS_WIDTH_128);
	hantro_reg_write(vpu, &g2_max_burst, 16);

	/* Swap */
	hantro_reg_write(vpu, &g2_strm_swap, 0);
	hantro_reg_write(vpu, &g2_dirmv_swap, 0);
	hantro_reg_write(ctx->dev, &g2_pic_swap_baikal, 0);
	hantro_reg_write(ctx->dev, &g2_tab_swap_baikal, 0x3);

	vdpu_write(vpu, ctx->dst_fmt.width * (ctx->bit_depth == 10 ? 5 : 4) |
			ctx->dst_fmt.width * (ctx->bit_depth == 10 ? 5 : 4) << 16, BAIKAL_OUT_STRIDE);

	/* Start decoding! */
	vdpu_write(vpu, G2_REG_INTERRUPT_DEC_E, G2_REG_INTERRUPT);

	return 0;
}
