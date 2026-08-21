// SPDX-License-Identifier: GPL-2.0
/*
 * Baikal Electronics DisplayPort Sound Card Driver header file
 *
 * Copyright (C) 2026 Baikal Electronics JSC
 *
 * Author: Pavel Parkhomenko <Pavel.Parkhomenko@baikalelectronics.ru>
 *
 */

#ifndef __BAIKAL_DP_SND_H__
#define __BAIKAL_DP_SND_H__

#include <sound/dmaengine_pcm.h>
#include <sound/pcm.h>

#define	BAIKAL_DP_SND_CARD_NAME 	"baikal_snd_card"
#define	BAIKAL_DP_SND_CODEC_DAI_NAME 	"baikal_dp_tx"

/*
 * CEA speaker placement
 *
 *  FL  FLC   FC   FRC   FR   FRW
 *
 *                                  LFE
 *
 *  RL  RLC   RC   RRC   RR
 */
enum dp_codec_cea_spk_placement {
	FL  = BIT(0),	/* Front Left           */
	FC  = BIT(1),	/* Front Center         */
	FR  = BIT(2),	/* Front Right          */
	FLC = BIT(3),	/* Front Left Center    */
	FRC = BIT(4),	/* Front Right Center   */
	RL  = BIT(5),	/* Rear Left            */
	RC  = BIT(6),	/* Rear Center          */
	RR  = BIT(7),	/* Rear Right           */
	RLC = BIT(8),	/* Rear Left Center     */
	RRC = BIT(9),	/* Rear Right Center    */
	LFE = BIT(10),	/* Low Frequency Effect */
};

/*
 * cea Speaker allocation structure
 */
struct dp_codec_cea_spk_alloc {
	const int ca_id;
	unsigned int n_ch;
	unsigned long mask;
};

struct baikal_dp;

struct baikal_dp_aud_dev {
	void __iomem *audio_base;
	struct device *dev;
	struct snd_pcm_substream __rcu *tx_substream;
	unsigned int (*tx_fn)(struct baikal_dp_aud_dev *dev,
			      struct snd_pcm_runtime *runtime,
			      unsigned int tx_ptr,
			      bool *period_elapsed);
	unsigned int tx_ptr;
	u32 samples_per_irq;
	int rate_changed;
	u32 current_rate;
	u32 timer_interval;
};

void baikal_dp_audio_init(struct baikal_dp *dp);
void baikal_dp_audio_shutdown(struct baikal_dp *dp);
int baikal_dp_register_aud_dev(struct baikal_dp *dp);
void baikal_dp_pcm_push_tx(struct baikal_dp_aud_dev *dev);

#endif
