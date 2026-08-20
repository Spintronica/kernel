// SPDX-License-Identifier: GPL-2.0
/*
 * Baikal Electronics DisplayPort Sound Card Driver
 *
 * Copyright (C) 2026 Baikal Electronics JSC
 *
 * Author: Pavel Parkhomenko <Pavel.Parkhomenko@baikalelectronics.ru>
 *
 */

#include <linux/module.h>
#include <linux/rcupdate.h>

#include <drm/drm_eld.h>
#include <sound/pcm.h>
#include <sound/pcm_drm_eld.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>

#include "baikal_bl1000_dp.h"
#include "baikal_bl1000_dp_snd.h"

#define BUFFER_BYTES_MAX	(3 * 2 * 8 * PERIOD_BYTES_MIN)
#define PERIOD_BYTES_MIN	4096
#define PERIODS_MIN		2

#define BAIKAL_DP_GP_HOST_TIMER			0x158
#define BAIKAL_DP_GP_HOST_TIMER_ENABLE		BIT(31)
#define BAIKAL_DP_GP_HOST_TIMER_RELOAD		BIT(30)
#define BAIKAL_DP_GP_HOST_TIMER_INTR		BIT(29)
#define BAIKAL_DP_SEC_AUDIO_CTRL 		0x900
#define BAIKAL_DP_SEC_AUDIO_CTRL_ENABLE 	BIT(0)
#define BAIKAL_DP_SEC_AUDIO_CTRL_MUTE 		BIT(1)
#define BAIKAL_DP_SEC_AUDIO_INPUT_SELECT	0x904
#define BAIKAL_DP_SEC_CHANNEL_COUNT		0x908
#define BAIKAL_DP_SEC_CS_CATEGORY_CODE		0x944
#define BAIKAL_DP_SEC_AUDIO_CHANNEL_MAP 	0x954
#define BAIKAL_DP_SEC_MAUD 			0x918
#define BAIKAL_DP_SEC_NAUD 			0x91c
#define BAIKAL_DP_SEC_AUDIO_CLOCK_MODE 		0x920
#define BAIKAL_DP_SEC_AUDIO_FIFO 		0x928
#define BAIKAL_DP_SEC_AUDIO_FIFO_LAST 		0x92c
#define BAIKAL_DP_SEC_AUDIO_FIFO_READY 		0x930
#define BAIKAL_DP_SEC_TIMESTAMP_INTERVAL 	0x93c

#define BAIKAL_DP_MAX_SND_DEV			1
#define BAIKAL_DP_SAMPLES_PER_TIMER_IRQ		4

/*
 * dp_codec_channel_alloc: speaker configuration available for CEA
 *
 * This is an ordered list that must match with dp_codec_8ch_chmaps struct
 * The preceding ones have better chances to be selected by
 * dp_codec_get_ch_alloc_table_idx().
 */
static const struct dp_codec_cea_spk_alloc dp_codec_channel_alloc[] = {
	{ .ca_id = 0x00, .n_ch = 2,
	  .mask = FL | FR},
	/* 2.1 */
	{ .ca_id = 0x01, .n_ch = 4,
	  .mask = FL | FR | LFE},
	/* Dolby Surround */
	{ .ca_id = 0x02, .n_ch = 4,
	  .mask = FL | FR | FC },
	/* surround51 */
	{ .ca_id = 0x0b, .n_ch = 6,
	  .mask = FL | FR | LFE | FC | RL | RR},
	/* surround40 */
	{ .ca_id = 0x08, .n_ch = 6,
	  .mask = FL | FR | RL | RR },
	/* surround41 */
	{ .ca_id = 0x09, .n_ch = 6,
	  .mask = FL | FR | LFE | RL | RR },
	/* surround50 */
	{ .ca_id = 0x0a, .n_ch = 6,
	  .mask = FL | FR | FC | RL | RR },
	/* 6.1 */
	{ .ca_id = 0x0f, .n_ch = 8,
	  .mask = FL | FR | LFE | FC | RL | RR | RC },
	/* surround71 */
	{ .ca_id = 0x13, .n_ch = 8,
	  .mask = FL | FR | LFE | FC | RL | RR | RLC | RRC },
	/* others */
	{ .ca_id = 0x03, .n_ch = 8,
	  .mask = FL | FR | LFE | FC },
	{ .ca_id = 0x04, .n_ch = 8,
	  .mask = FL | FR | RC},
	{ .ca_id = 0x05, .n_ch = 8,
	  .mask = FL | FR | LFE | RC },
	{ .ca_id = 0x06, .n_ch = 8,
	  .mask = FL | FR | FC | RC },
	{ .ca_id = 0x07, .n_ch = 8,
	  .mask = FL | FR | LFE | FC | RC },
	{ .ca_id = 0x0c, .n_ch = 8,
	  .mask = FL | FR | RC | RL | RR },
	{ .ca_id = 0x0d, .n_ch = 8,
	  .mask = FL | FR | LFE | RL | RR | RC },
	{ .ca_id = 0x0e, .n_ch = 8,
	  .mask = FL | FR | FC | RL | RR | RC },
	{ .ca_id = 0x10, .n_ch = 8,
	  .mask = FL | FR | RL | RR | RLC | RRC },
	{ .ca_id = 0x11, .n_ch = 8,
	  .mask = FL | FR | LFE | RL | RR | RLC | RRC },
	{ .ca_id = 0x12, .n_ch = 8,
	  .mask = FL | FR | FC | RL | RR | RLC | RRC },
	{ .ca_id = 0x14, .n_ch = 8,
	  .mask = FL | FR | FLC | FRC },
	{ .ca_id = 0x15, .n_ch = 8,
	  .mask = FL | FR | LFE | FLC | FRC },
	{ .ca_id = 0x16, .n_ch = 8,
	  .mask = FL | FR | FC | FLC | FRC },
	{ .ca_id = 0x17, .n_ch = 8,
	  .mask = FL | FR | LFE | FC | FLC | FRC },
	{ .ca_id = 0x18, .n_ch = 8,
	  .mask = FL | FR | RC | FLC | FRC },
	{ .ca_id = 0x19, .n_ch = 8,
	  .mask = FL | FR | LFE | RC | FLC | FRC },
	{ .ca_id = 0x1a, .n_ch = 8,
	  .mask = FL | FR | RC | FC | FLC | FRC },
	{ .ca_id = 0x1b, .n_ch = 8,
	  .mask = FL | FR | LFE | RC | FC | FLC | FRC },
	{ .ca_id = 0x1c, .n_ch = 8,
	  .mask = FL | FR | RL | RR | FLC | FRC },
	{ .ca_id = 0x1d, .n_ch = 8,
	  .mask = FL | FR | LFE | RL | RR | FLC | FRC },
	{ .ca_id = 0x1e, .n_ch = 8,
	  .mask = FL | FR | FC | RL | RR | FLC | FRC },
	{ .ca_id = 0x1f, .n_ch = 8,
	  .mask = FL | FR | LFE | FC | RL | RR | FLC | FRC },
};

static DEFINE_IDA(baikal_bl1000_dp_snd_card_dev);

static const char *baikal_bl1000_dp_snd_card_name = "baikal-dp";

SND_SOC_DAILINK_DEFS(baikal_dp_tx,
		     DAILINK_COMP_ARRAY(COMP_DUMMY()),
		     DAILINK_COMP_ARRAY(COMP_CODEC(NULL, BAIKAL_DP_SND_CODEC_DAI_NAME)),
		     DAILINK_COMP_ARRAY(COMP_PLATFORM(NULL)));

static struct snd_soc_dai_link baikal_bl1000_dp_dai = {
	.name = "baikal-dp-playback",
	SND_SOC_DAILINK_REG(baikal_dp_tx),
	.dai_fmt = SND_SOC_DAIFMT_I2S |
		   SND_SOC_DAIFMT_NB_NF |
		   SND_SOC_DAIFMT_CBS_CFS,
};

struct dp_snd_card_data {
	int dev_id;
};

static const struct snd_pcm_hardware baikal_dp_pcm_hardware = {
	.info = SNDRV_PCM_INFO_INTERLEAVED |
		SNDRV_PCM_INFO_MMAP |
		SNDRV_PCM_INFO_MMAP_VALID |
		SNDRV_PCM_INFO_BLOCK_TRANSFER,
	.buffer_bytes_max = BUFFER_BYTES_MAX,
	.period_bytes_min = PERIOD_BYTES_MIN,
	.period_bytes_max = BUFFER_BYTES_MAX / PERIODS_MIN,
	.periods_min = PERIODS_MIN,
	.periods_max = BUFFER_BYTES_MAX / PERIOD_BYTES_MIN,
};

static int timer_not_set = 1;

#define baikal_dp_pcm_tx_fn(sample_bits, num_channels)					\
static unsigned int baikal_dp_pcm_tx_##sample_bits(struct baikal_dp_aud_dev *dev,	\
		struct snd_pcm_runtime *runtime, unsigned int ptr,			\
		bool *period_elapsed)							\
{											\
	const u##sample_bits(*p)[num_channels] = (void *)runtime->dma_area;		\
	unsigned int tx_ptr = ptr;							\
	unsigned int period_pos = tx_ptr % runtime->period_size;			\
	u32 chan[2];									\
	int i;										\
											\
	for (i = 0; i < dev->samples_per_irq; i++) { 					\
		if (!baikal_dp_read(dev->audio_base, BAIKAL_DP_SEC_AUDIO_FIFO_READY)) {	\
			goto end;							\
		}									\
		switch (sample_bits) {							\
		case 32:								\
			chan[0] = p[tx_ptr][0] >> 8;					\
			chan[1] = p[tx_ptr][1] >> 8;					\
			break;								\
		case 16:								\
		default:								\
			chan[0] = p[tx_ptr][0] << 8;					\
			chan[1] = p[tx_ptr][1] << 8;					\
		}									\
		baikal_dp_write(dev->audio_base, BAIKAL_DP_SEC_AUDIO_FIFO_LAST, 0);	\
		baikal_dp_write(dev->audio_base, BAIKAL_DP_SEC_AUDIO_FIFO, chan[0]);	\
		baikal_dp_write(dev->audio_base, BAIKAL_DP_SEC_AUDIO_FIFO_LAST, 1);	\
		baikal_dp_write(dev->audio_base, BAIKAL_DP_SEC_AUDIO_FIFO, chan[1]);	\
		period_pos++;								\
		if (++tx_ptr >= runtime->buffer_size) { 				\
			tx_ptr = 0; 							\
		} 									\
	}										\
end:											\
	*period_elapsed = period_pos >= runtime->period_size;				\
	return tx_ptr;									\
}

baikal_dp_pcm_tx_fn(16, 2);
baikal_dp_pcm_tx_fn(32, 2);

#undef baikal_dp_pcm_tx_fn

static void baikal_dp_pcm_empty_packet(struct baikal_dp_aud_dev *dev)
{
	int i;

	for (i = 0; i < dev->samples_per_irq; i++) {
		if (!baikal_dp_read(dev->audio_base, BAIKAL_DP_SEC_AUDIO_FIFO_READY)) {
			return;
		}
		baikal_dp_write(dev->audio_base, BAIKAL_DP_SEC_AUDIO_FIFO_LAST, 0);
		baikal_dp_write(dev->audio_base, BAIKAL_DP_SEC_AUDIO_FIFO, 0);
		baikal_dp_write(dev->audio_base, BAIKAL_DP_SEC_AUDIO_FIFO_LAST, 1);
		baikal_dp_write(dev->audio_base, BAIKAL_DP_SEC_AUDIO_FIFO, 0);
	}
}

void baikal_dp_pcm_push_tx(struct baikal_dp_aud_dev *dev)
{
	struct snd_pcm_substream *substream;
	bool active, period_elapsed;

	rcu_read_lock();
	substream = rcu_dereference(dev->tx_substream);
	active = substream && snd_pcm_running(substream);
	if (active) {
		unsigned int ptr;
		unsigned int new_ptr;

		ptr = READ_ONCE(dev->tx_ptr);
		new_ptr = dev->tx_fn(dev, substream->runtime, ptr,
				&period_elapsed);
		cmpxchg(&dev->tx_ptr, ptr, new_ptr);

		if (period_elapsed)
			snd_pcm_period_elapsed(substream);
	} else {
		baikal_dp_pcm_empty_packet(dev);
	}
	if (dev->rate_changed) {
		dev->rate_changed = 0;
		baikal_dp_write(dev->audio_base,
				BAIKAL_DP_GP_HOST_TIMER,
				BAIKAL_DP_GP_HOST_TIMER_ENABLE |
				BAIKAL_DP_GP_HOST_TIMER_RELOAD |
				BAIKAL_DP_GP_HOST_TIMER_INTR |
				dev->timer_interval - 1);
	}
	rcu_read_unlock();
}

static int baikal_dp_pcm_open(struct snd_soc_component *component,
		       struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct platform_device *pdev = to_platform_device(component->dev);
	struct drm_device *drm = dev_get_drvdata(&pdev->dev);
	struct baikal_vdu_crossbar *crossbar = drm_to_baikal_vdu_crossbar(drm);
	struct baikal_dp *dp = crossbar->dp;

	if (dp->status == connector_status_disconnected) {
		dev_dbg(component->dev, "DP monitor not connected\n");
		return -ENODEV;
	}

	snd_soc_set_runtime_hwparams(substream, &baikal_dp_pcm_hardware);
	snd_pcm_hw_constraint_integer(runtime, SNDRV_PCM_HW_PARAM_PERIODS);
	runtime->private_data = &dp->aud_dev;

	return 0;
}

static int baikal_dp_pcm_close(struct snd_soc_component *component,
			struct snd_pcm_substream *substream)
{
	synchronize_rcu();
	return 0;
}

static int baikal_dp_pcm_hw_params(struct snd_soc_component *component,
			    struct snd_pcm_substream *substream,
			    struct snd_pcm_hw_params *hw_params)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct baikal_dp_aud_dev *dev = runtime->private_data;

	switch (params_channels(hw_params)) {
	case 2:
		break;
	default:
		dev_err(dev->dev, "invalid number of channels\n");
		return -EINVAL;
	}

	switch (params_format(hw_params)) {
	case SNDRV_PCM_FORMAT_S16_LE:
		dev->tx_fn = baikal_dp_pcm_tx_16;
		break;
	case SNDRV_PCM_FORMAT_S24_LE:
	case SNDRV_PCM_FORMAT_S32_LE:
		dev->tx_fn = baikal_dp_pcm_tx_32;
		break;
	default:
		dev_err(dev->dev, "invalid sample format\n");
		return -EINVAL;
	}

	return 0;
}

static int baikal_dp_pcm_trigger(struct snd_soc_component *component,
			  struct snd_pcm_substream *substream, int cmd)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct baikal_dp_aud_dev *dev = runtime->private_data;
	int ret = 0;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
			WRITE_ONCE(dev->tx_ptr, 0);
			rcu_assign_pointer(dev->tx_substream, substream);
		}
		break;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
			rcu_assign_pointer(dev->tx_substream, NULL);
		}
		break;
	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

static snd_pcm_uframes_t baikal_dp_pcm_pointer(struct snd_soc_component *component,
					struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct baikal_dp_aud_dev *dev = runtime->private_data;
	snd_pcm_uframes_t pos;

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		pos = READ_ONCE(dev->tx_ptr);
	}

	return pos < runtime->buffer_size ? pos : 0;
}

static int baikal_dp_pcm_new(struct snd_soc_component *component,
		      struct snd_soc_pcm_runtime *rtd)
{
	size_t size = baikal_dp_pcm_hardware.buffer_bytes_max;

	snd_pcm_set_managed_buffer_all(rtd->pcm,
			SNDRV_DMA_TYPE_CONTINUOUS,
			NULL, size, size);
	return 0;
}

static const struct snd_soc_component_driver baikal_dp_pcm_component = {
	.open		= baikal_dp_pcm_open,
	.close		= baikal_dp_pcm_close,
	.hw_params	= baikal_dp_pcm_hw_params,
	.trigger	= baikal_dp_pcm_trigger,
	.pointer	= baikal_dp_pcm_pointer,
	.pcm_construct	= baikal_dp_pcm_new,
};

static int baikal_dp_pcm_register(struct device *dev)
{
	return devm_snd_soc_register_component(
			dev,
			&baikal_dp_pcm_component,
			NULL,
			0);
}

static int baikal_dp_snd_probe(struct platform_device *pdev)
{
	struct baikal_dp_aud_dev *aud_dev;
	struct snd_soc_dai_link *dai;
	struct dp_snd_card_data *prv;
	struct snd_soc_card *card;
	size_t sz;
	char *buf;
	int ret;

	aud_dev = pdev->dev.platform_data;
	if (!aud_dev)
		return -ENODEV;

	card = devm_kzalloc(&pdev->dev, sizeof(struct snd_soc_card),
			    GFP_KERNEL);
	if (!card)
		return -ENOMEM;

	card->dev = &pdev->dev;
	card->dai_link = devm_kzalloc(card->dev,
				      sizeof(*dai),
				      GFP_KERNEL);
	if (!card->dai_link)
		return -ENOMEM;

	prv = devm_kzalloc(card->dev,
			   sizeof(struct dp_snd_card_data),
			   GFP_KERNEL);
	if (!prv)
		return -ENOMEM;

	dai = &card->dai_link[0];
	*dai = baikal_bl1000_dp_dai;
	dai->platforms->name = dev_name(aud_dev->dev);
	dai->codecs->name = dev_name(aud_dev->dev);
	card->num_links = 1;
	snd_soc_card_set_drvdata(card, prv);
	dev_dbg(card->dev, "%s registered\n",
		card->dai_link[0].name);

	/*
	 *  Example : card name = baikal-dp-0
	 *  length = number of chars in "baikal-dp"
	 *	    + 1 ('-'), + 1 (card instance num)
	 *	    + 1 ('\0')
	 */
	sz = strlen(baikal_bl1000_dp_snd_card_name) + 3;
	buf = devm_kzalloc(card->dev, sz, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	prv->dev_id = ida_simple_get(&baikal_bl1000_dp_snd_card_dev, 0,
				     BAIKAL_DP_MAX_SND_DEV,
				     GFP_KERNEL);

	if (prv->dev_id < 0)
		return prv->dev_id;

	snprintf(buf, sz, "%s-%d", baikal_bl1000_dp_snd_card_name,
		 prv->dev_id);
	card->name = buf;

	ret = devm_snd_soc_register_card(card->dev, card);
	if (ret) {
		dev_err(card->dev, "%s registration failed\n",
			card->name);
		ida_simple_remove(&baikal_bl1000_dp_snd_card_dev,
				  prv->dev_id);
		return ret;
	}

	dev_set_drvdata(card->dev, prv);
	dev_info(card->dev, "%s registered\n", card->name);

	return 0;
}

static void baikal_dp_snd_remove(struct platform_device *pdev)
{
	struct dp_snd_card_data *pdata = dev_get_drvdata(&pdev->dev);

	ida_simple_remove(&baikal_bl1000_dp_snd_card_dev, pdata->dev_id);
}

static struct platform_driver baikal_dp_snd_driver = {
	.driver = {
		.name = BAIKAL_DP_SND_CARD_NAME,
	},
	.probe = baikal_dp_snd_probe,
	.remove = baikal_dp_snd_remove,
};

static int baikal_dp_get_eld(struct device *dev, u8 *buf, size_t len)
{
	struct drm_device *drm = dev_get_drvdata(dev);
	struct baikal_vdu_crossbar *crossbar = drm_to_baikal_vdu_crossbar(drm);
	struct baikal_dp *dp = crossbar->dp;

	size_t size;

	if (!dp->have_edid)
		return -EIO;

	size = drm_eld_size(dp->connector.eld);
	if (!size)
		return -EINVAL;

	if (len < size)
		size = len;
	memcpy(buf, dp->connector.eld, size);

	return 0;
}

static unsigned long dp_codec_spk_mask_from_alloc(int spk_alloc)
{
	int i;
	static const unsigned long dp_codec_eld_spk_alloc_bits[] = {
		[0] = FL | FR, [1] = LFE, [2] = FC, [3] = RL | RR,
		[4] = RC, [5] = FLC | FRC, [6] = RLC | RRC,
	};
	unsigned long spk_mask = 0;

	for (i = 0; i < ARRAY_SIZE(dp_codec_eld_spk_alloc_bits); i++) {
		if (spk_alloc & (1 << i))
			spk_mask |= dp_codec_eld_spk_alloc_bits[i];
	}

	return spk_mask;
}

static int dp_codec_get_ch_alloc_table_idx(u8 *eld, u8 channels)
{
	int i;
	u8 spk_alloc;
	unsigned long spk_mask;
	const struct dp_codec_cea_spk_alloc *cap = dp_codec_channel_alloc;

	spk_alloc = drm_eld_get_spk_alloc(eld);
	spk_mask = dp_codec_spk_mask_from_alloc(spk_alloc);

	for (i = 0; i < ARRAY_SIZE(dp_codec_channel_alloc); i++, cap++) {
		/* If spk_alloc == 0, DP is unplugged return stereo config */
		if (!spk_alloc && cap->ca_id == 0)
			return i;
		if (cap->n_ch != channels)
			continue;
		if (!(cap->mask == (spk_mask & cap->mask)))
			continue;
		return i;
	}

	return -EINVAL;
}

static int dp_codec_fill_cea_params(struct snd_pcm_substream *substream,
				    struct snd_soc_dai *dai,
				    unsigned int channels,
				    struct hdmi_audio_infoframe *cea)
{
	int idx;
	int ret;
	u8 eld[MAX_ELD_BYTES];

	ret = baikal_dp_get_eld(dai->dev, eld, sizeof(eld));
	if (ret)
		return ret;

	ret = snd_pcm_hw_constraint_eld(substream->runtime, eld);
	if (ret)
		return ret;

	/* Select a channel allocation that matches with ELD and pcm channels */
	idx = dp_codec_get_ch_alloc_table_idx(eld, channels);
	if (idx < 0) {
		dev_err(dai->dev, "Not able to map channels to speakers (%d)\n",
			idx);
		return idx;
	}

	hdmi_audio_infoframe_init(cea);
	cea->channels = channels;
	cea->coding_type = HDMI_AUDIO_CODING_TYPE_STREAM;
	cea->sample_size = HDMI_AUDIO_SAMPLE_SIZE_STREAM;
	cea->sample_frequency = HDMI_AUDIO_SAMPLE_FREQUENCY_STREAM;
	cea->channel_allocation = dp_codec_channel_alloc[idx].ca_id;

	return 0;
}

static int baikal_dp_dai_hw_params(struct snd_pcm_substream *substream,
				   struct snd_pcm_hw_params *params,
				   struct snd_soc_dai *dai)
{
	struct hdmi_audio_infoframe infoframe;
	struct drm_device *drm = dev_get_drvdata(dai->dev);
	struct baikal_vdu_crossbar *crossbar = drm_to_baikal_vdu_crossbar(drm);
	struct baikal_dp *dp = crossbar->dp;
	struct baikal_dp_aud_dev *aud_dev = &dp->aud_dev;
	int maud, naud, r;
	int ret;

	u8 infopckt[DP_INFOFRAME_SIZE(AUDIO)] = {0};
	u8 *ptr = (u8 *)dp->tx_audio_data->buffer;

	ret = dp_codec_fill_cea_params(substream, dai,
				       params_channels(params),
				       &infoframe);
	if (ret < 0)
		return ret;

	/* Setting audio channels */
	baikal_dp_write(dp->dp_base, BAIKAL_DP_SEC_CHANNEL_COUNT,
		      infoframe.channels);

	hdmi_audio_infoframe_pack(&infoframe, infopckt,
				  DP_INFOFRAME_SIZE(AUDIO));
	/* Setting audio infoframe packet header. Please refer to PG 299 */
	ptr[0] = 0x00;
	ptr[1] = 0x84;
	ptr[2] = 0x1B;
	ptr[3] = 0x44;
	memcpy((void *)(&ptr[4]), (void *)(&infopckt[4]),
	       (DP_INFOFRAME_SIZE(AUDIO) - DP_INFOFRAME_HEADER_SIZE));

	r = params_rate(params);
	if (r != aud_dev->current_rate) {
		if (r != 48000) {
			dev_err(aud_dev->dev, "invalid audio rate\n");
			return -EINVAL;
		}
		maud = 512;
		switch (dp->mode.bw_code) {
		case DP_LINK_BW_8_1:
			naud = 16875;
			break;
		case DP_LINK_BW_5_4:
			naud = 11250;
			break;
		case DP_LINK_BW_2_7:
			naud = 5625;
			break;
		case DP_LINK_BW_1_62:
			naud = 3375;
			break;
		default:
			dev_err(aud_dev->dev, "invalid DP link rate\n");
			return -EINVAL;
		}
		baikal_dp_write(aud_dev->audio_base, BAIKAL_DP_SEC_MAUD, maud);
		baikal_dp_write(aud_dev->audio_base, BAIKAL_DP_SEC_NAUD, naud);
		baikal_dp_write(aud_dev->audio_base, BAIKAL_DP_SEC_AUDIO_CLOCK_MODE, 1);
		msleep(100);
		aud_dev->rate_changed = 1;
		aud_dev->timer_interval = 1000000 / (r / aud_dev->samples_per_irq);
	}

	if (timer_not_set) {
		timer_not_set = 0;
		baikal_dp_write(aud_dev->audio_base,
				BAIKAL_DP_GP_HOST_TIMER,
				BAIKAL_DP_GP_HOST_TIMER_ENABLE |
				BAIKAL_DP_GP_HOST_TIMER_RELOAD |
				BAIKAL_DP_GP_HOST_TIMER_INTR |
				aud_dev->timer_interval - 1);
	}

	return 0;
}

static const struct snd_soc_dai_ops baikal_dp_dai_ops = {
	.hw_params = baikal_dp_dai_hw_params,
	.no_capture_mute = 1,
};

static struct snd_soc_dai_driver baikal_dp_dai = {
	.name = BAIKAL_DP_SND_CODEC_DAI_NAME,
	.playback = {
		.stream_name = "DP Playback",
		.channels_min = 2,
		.channels_max = 2,
		.rates = SNDRV_PCM_RATE_48000,
		.formats = SNDRV_PCM_FMTBIT_S16_LE |
			   SNDRV_PCM_FMTBIT_S32_LE,
		.sig_bits = 24,
	},
	.ops = &baikal_dp_dai_ops,
};

static int baikal_dp_audio_codec_probe(struct snd_soc_component *component)
{
	return 0;
}

static void baikal_dp_audio_codec_remove(struct snd_soc_component *component)
{
}

static const struct snd_soc_component_driver baikal_dp_audio_component = {
	.probe = baikal_dp_audio_codec_probe,
	.remove = baikal_dp_audio_codec_remove,
};

void baikal_dp_audio_init(struct baikal_dp *dp)
{
	/* APB host writes */
	baikal_dp_write(dp->dp_base, BAIKAL_DP_SEC_AUDIO_INPUT_SELECT, 0x2);
	/* Channel map */
	baikal_dp_write(dp->dp_base, BAIKAL_DP_SEC_AUDIO_CHANNEL_MAP, 0x87654321);
	/* Category code */
	baikal_dp_write(dp->dp_base, BAIKAL_DP_SEC_CS_CATEGORY_CODE, 0xb5);
	/* Enable audio */
	baikal_dp_clr(dp->dp_base, BAIKAL_DP_SEC_AUDIO_CTRL, BAIKAL_DP_SEC_AUDIO_CTRL_MUTE);
	baikal_dp_set(dp->dp_base, BAIKAL_DP_SEC_AUDIO_CTRL, BAIKAL_DP_SEC_AUDIO_CTRL_ENABLE);
}

void baikal_dp_audio_shutdown(struct baikal_dp *dp)
{
	baikal_dp_clr(dp->dp_base, BAIKAL_DP_SEC_AUDIO_CTRL, BAIKAL_DP_SEC_AUDIO_CTRL_ENABLE);
}

int baikal_dp_register_aud_dev(struct baikal_dp *dp)
{
	int ret;

	dp->aud_dev.dev = dp->dev;
	dp->aud_dev.audio_base = dp->dp_base;
	dp->aud_dev.samples_per_irq = BAIKAL_DP_SAMPLES_PER_TIMER_IRQ;
	ret = devm_snd_soc_register_component(dp->dev, &baikal_dp_audio_component,
			&baikal_dp_dai, 1);
	if (ret)
		return ret;
	ret = baikal_dp_pcm_register(dp->dev);
	if (ret)
		return ret;
	struct platform_device *pdev = platform_device_register_resndata(
				dp->dev,
				BAIKAL_DP_SND_CARD_NAME,
				PLATFORM_DEVID_AUTO,
				NULL, 0,
				&dp->aud_dev,
				sizeof(dp->aud_dev));
	if (!pdev)
		dev_err(dp->dev, "sound card device creation failed\n");

	return 0;
}

module_platform_driver(baikal_dp_snd_driver);

MODULE_DESCRIPTION("Baikal Electronics DisplayPort sound card driver");
MODULE_AUTHOR("Pavel Parkhomenko <Pavel.Parkhomenko@baikalelectronics.ru>");
MODULE_LICENSE("GPL");
