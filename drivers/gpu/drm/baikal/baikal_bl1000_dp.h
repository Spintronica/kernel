#ifndef __BAIKAL_VDU_DP_H__
#define __BAIKAL_VDU_DP_H__

#include <drm/drm_bridge.h>
#include <drm/display/drm_dp_helper.h>
#include <drm/display/drm_dp_mst_helper.h>
#include <linux/phy/phy.h>

#include "baikal_bl1000_drm.h"

#define BAIKAL_DP_REG_ADDR_OFFSET			2

#define BAIKAL_DP_MAX_LANES				4

/* Extended Receiver Capability: See DP_DPCD_REV for definitions */
#define DP_DP13_MAX_LINK_RATE               		0x2201

#define BAIKAL_DP_LINK_BW_SET				0x0
#define BAIKAL_DP_LANE_COUNT_SET			0x4
#define BAIKAL_DP_ENHANCED_FRAME_EN			0x8
#define BAIKAL_DP_TRAINING_PATTERN_SET			0xc
#define BAIKAL_DP_SCRAMBLING_DISABLE			0x14
#define BAIKAL_EDP_CAPABILITY_CONFIG			0x1c
#define BAIKAL_EDP_ENABLE_REDUCED_AUX_SYNC		BIT(1)

#define BAIKAL_DP_TRANSMITTER_ENABLE			0x80

#define BAIKAL_DP_SOFT_RESET				0x90
#define BAIKAL_DP_SOFT_RESET_LINK_RESET			BIT(0)
#define BAIKAL_DP_SOFT_RESET_VIDEO_RESET		BIT(1)

#define BAIKAL_DP_INPUT_SOURCE_ENABLE			0x94

#define BAIKAL_DP_AUX_COMMAND				0x100
#define BAIKAL_DP_AUX_COMMAND_CMD_SHIFT			8
#define BAIKAL_DP_AUX_COMMAND_ADDRESS_ONLY		BIT(12)
#define BAIKAL_DP_AUX_COMMAND_BYTES_SHIFT		0
#define BAIKAL_DP_AUX_WRITE_FIFO			0x104
#define BAIKAL_DP_AUX_ADDRESS				0x108
#define BAIKAL_DP_AUX_CLK_DIVIDER			0x10c
#define BAIKAL_DP_AUX_CLK_MHZ				1000000
#define BAIKAL_DP_AUX_CLK_DIVIDER_AUX_FILTER_SHIFT	8
#define BAIKAL_DP_INTERRUPT_STATE			0x130
#define BAIKAL_DP_INTERRUPT_STATE_HPD			BIT(0)
#define BAIKAL_DP_INTERRUPT_STATE_REQUEST		BIT(1)
#define BAIKAL_DP_INTERRUPT_STATE_REPLY			BIT(2)
#define BAIKAL_DP_INTERRUPT_STATE_REPLY_TIMEOUT		BIT(3)
#define BAIKAL_DP_AUX_REPLY_DATA			0x134
#define BAIKAL_DP_AUX_REPLY_CODE			0x138
#define BAIKAL_DP_AUX_REPLY_CODE_AUX_ACK		(0)
#define BAIKAL_DP_AUX_REPLY_CODE_AUX_NACK		BIT(0)
#define BAIKAL_DP_AUX_REPLY_CODE_AUX_DEFER		BIT(1)
#define BAIKAL_DP_AUX_REPLY_CODE_I2C_ACK		(0)
#define BAIKAL_DP_AUX_REPLY_CODE_I2C_NACK		BIT(2)
#define BAIKAL_DP_AUX_REPLY_CODE_I2C_DEFER		BIT(3)
#define BAIKAL_DP_AUX_REPLY_COUNT			0x13c
#define BAIKAL_DP_INTERRUPT_CAUSE			0x140
#define BAIKAL_DP_INTERRUPT_MASK			0x144
#define BAIKAL_DP_INTERRUPT_HPDPULSE_MASK		BIT(0)
#define BAIKAL_DP_INTERRUPT_HPDEVENT_MASK		BIT(1)
#define BAIKAL_DP_INTERRUPT_REPLY_RCVD_MASK		BIT(2)
#define BAIKAL_DP_REPLY_DATA_COUNT			0x148
#define BAIKAL_DP_REPLY_DATA_COUNT_MASK			0xff

#define BAIKAL_DP_SST_SOURCE_SELECT			0x50c

#define BAIKAL_DP_SRC0_STREAM_ENABLE			0x800

#define BAIKAL_DP_INPUT_STATUS				0x80c
#define BAIKAL_DP_INPUT_STATUS_ODDEVEN			BIT(3)
#define BAIKAL_DP_INPUT_STATUS_DEN 			BIT(2)
#define BAIKAL_DP_INPUT_STATUS_HSYNC 			BIT(1)
#define BAIKAL_DP_INPUT_STATUS_VSYNC 			BIT(0)

#define BAIKAL_DP_SRC0_DATA_CONTROL			0x810
#define BAIKAL_DP_SRC0_DATA_CONTROL_ACC_DELAY_SHIFT	8
#define BAIKAL_DP_SRC0_COLORIMETRY_OVERRIDE		0x814

#define BAIKAL_DP_SRC0_STREAM_HTOTAL			0x820
#define BAIKAL_DP_SRC0_STREAM_VTOTAL			0x824
#define BAIKAL_DP_SRC0_STREAM_POL			0x828
#define BAIKAL_DP_SRC0_STREAM_POLHSYNC_SHIFT		0
#define BAIKAL_DP_SRC0_STREAM_POLVSYNC_SHIFT		1
#define BAIKAL_DP_SRC0_STREAM_HSWIDTH			0x82c
#define BAIKAL_DP_SRC0_STREAM_VSWIDTH			0x830
#define BAIKAL_DP_SRC0_STREAM_HRES			0x834
#define BAIKAL_DP_SRC0_STREAM_VRES			0x838
#define BAIKAL_DP_SRC0_STREAM_HSTART			0x83c
#define BAIKAL_DP_SRC0_STREAM_VSTART			0x840

#define BAIKAL_DP_SRC0_STREAM_MISC0			0x844
#define BAIKAL_DP_SRC0_STREAM_MISC0_MASK		BIT(0)
#define BAIKAL_DP_SRC0_RGB_MASK				(0)
#define BAIKAL_DP_SRC0_YCRCB422_MASK			(5 << 1)
#define BAIKAL_DP_SRC0_YCRCB444_MASK			GENMASK(3, 2)
#define BAIKAL_DP_SRC0_FORMAT_MASK			GENMASK(3, 1)
#define BAIKAL_DP_SRC0_BPC6_MASK			(0 << 5)
#define BAIKAL_DP_SRC0_BPC8_MASK			BIT(5)
#define BAIKAL_DP_SRC0_BPC10_MASK			BIT(6)
#define BAIKAL_DP_SRC0_BPC12_MASK			GENMASK(6, 5)
#define BAIKAL_DP_SRC0_BPC16_MASK			BIT(7)
#define BAIKAL_DP_SRC0_BPC_MASK				GENMASK(7, 5)

#define BAIKAL_DP_SRC0_STREAM_MISC1			0x848
#define BAIKAL_DP_SRC0_STREAM_MISC1_VSC_COLORIMETRY	BIT(6)
#define BAIKAL_DP_SRC0_STREAM_MISC1_YONLY_MASK		BIT(7)

#define BAIKAL_DP_SRC0_M_VID				0x84c
#define BAIKAL_DP_SRC0_TRANSFER_UNIT_CONFIG		0x850
#define BAIKAL_DP_TU_CONFIG_SYMBOLS_PER_TU_SHIFT	16
#define BAIKAL_DP_TU_CONFIG_FRAC_SYMBOLS_PER_TU_SHIFT	24
#define BAIKAL_DP_DEFAULT_TRANSFER_UNITSIZE		0x40
#define BAIKAL_DP_SRC0_N_VID				0x854
#define BAIKAL_DP_SRC0_USER_DATA_COUNT			0x85c

#define BAIKAL_DP_SRC0_USER_SYNC_POLARITY		0x864
#define BAIKAL_DP_SRC0_USER_CONTROL			0x868

#define BAIKAL_DP_REDUCED_BIT_RATE			162000
#define BAIKAL_DP_HIGH_BIT_RATE_1			270000
#define BAIKAL_DP_HIGH_BIT_RATE_2			540000
#define BAIKAL_DP_HIGH_BIT_RATE_3			810000

#define DP_MAX_TRAINING_TRIES				5

struct baikal_dp_link_config {
	int max_rate;
	u8 max_lanes;
	int link_rate;
	u8 lane_count;
	u8 cr_done_cnt;
	u8 cr_done_oldstate;
};

struct baikal_dp_tx_link_config {
	u8 vs_level;
	u8 pe_level;
};

struct baikal_dp_mode {
	int pclock;
	u8 bw_code;
	u8 lane_cnt;
};

struct baikal_dp_config {
	u32 max_lanes;
	u32 max_link_rate;
	u8 misc0;
	u8 bpp;
	u8 bpc;
	u8 num_colors;
	u8 fmt;
};

enum baikal_dp_train_state {
	XLNX_DP_TRAIN_CR = 0,
	XLNX_DP_TRAIN_CE = 1,
	XLNX_DP_ADJUST_LINKRATE = 2,
	XLNX_DP_ADJUST_LANECOUNT = 3,
	XLNX_DP_TRAIN_FAILURE = 4,
	XLNX_DP_TRAIN_SUCCESS = 5
};

struct baikal_dp;

struct baikal_dp_connector {
	struct drm_connector base;
	struct baikal_dp *dp;
};

struct baikal_dp {
	struct device *dev;
	struct baikal_vdu_crossbar *crossbar;
	struct drm_encoder encoder;
	struct drm_connector connector;
	struct drm_bridge bridge;
	struct drm_property *sync_prop;
	struct drm_property *bpc_prop;
	struct drm_dp_aux aux;
	struct baikal_dp_config config;
	struct baikal_dp_tx_link_config tx_link_config;
	struct baikal_dp_link_config link_config;
	struct drm_device *drm;
	struct baikal_dp_mode mode;
	struct phy *phy[BAIKAL_DP_MAX_LANES];
	struct clk *axi_lite_clk;
	struct clk *tx_vid_clk;
	struct gpio_desc *reset_gpio;
	struct delayed_work hpd_work;
	struct delayed_work hpd_pulse_work;

	struct drm_display_mode *adjusted_mode;
	union phy_configure_opts phy_opts;
	enum drm_connector_status status;
	void __iomem *dp_base;
	int dpms;
	u8 dpcd[DP_RECEIVER_CAP_SIZE];
	u8 train_set[BAIKAL_DP_MAX_LANES];
	u8 num_lanes;
	unsigned int enabled : 1;
	bool have_edid;
	unsigned int colorimetry_through_vsc : 1;

	struct drm_dp_mst_topology_mgr mst_mgr;

	u32 counters[20];
};

int baikal_dp_probe(struct platform_device *pdev);

#endif
