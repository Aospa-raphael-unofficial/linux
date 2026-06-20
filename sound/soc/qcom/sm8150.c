// SPDX-License-Identifier: GPL-2.0
/*
 * sm8150_new.c - SM8150 ASoC machine driver (Xiaomi Raphael)
 *
 * Ported from Android techpack/audio/asoc/sm8150.c to upstream Linux APIs.
 * Supports:
 *   - WCD9340/WCD934x (tavil) codec on SLIMBUS for headset audio
 *   - TFA9874 speaker amp on Quaternary MI2S
 *
 * Copyright (c) 2022, The Linux Foundation. All rights reserved.
 */

#include <dt-bindings/sound/qcom,q6afe.h>
#include <dt-bindings/sound/qcom,q6dsp-lpass-ports.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of_device.h>
#include <linux/soundwire/sdw.h>
#include <linux/slimbus.h>
#include <sound/core.h>
#include <sound/soc.h>
#include <sound/soc-dapm.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/jack.h>
#include <uapi/linux/input-event-codes.h>
#include "common.h"
#include "qdsp6/q6afe.h"

#define DRIVER_NAME		"sm8150_raphael"

#define SLIM_MAX_TX_PORTS	16
#define SLIM_MAX_RX_PORTS	13
#define WCD934X_DEFAULT_MCLK_RATE	9600000
#define MI2S_BCLK_RATE		1536000

struct sm8150_snd_data {
	struct snd_soc_jack jack;
	bool jack_setup;
	bool slim_port_setup;
	bool stream_prepared[AFE_PORT_MAX];
	struct snd_soc_card *card;
	uint32_t quat_mi2s_clk_count;
	struct sdw_stream_runtime *sruntime[AFE_PORT_MAX];
};

/* ==================== BE hw_params fixup ==================== */

/*
 * Default fixup: 48kHz / 2ch / S24_LE
 * Used for most backend links (SLIMBUS, AFE, USB, etc.)
 */
static int sm8150_be_hw_params_fixup(struct snd_soc_pcm_runtime *rtd,
				     struct snd_pcm_hw_params *params)
{
	struct snd_interval *rate = hw_param_interval(params,
					SNDRV_PCM_HW_PARAM_RATE);
	struct snd_interval *channels = hw_param_interval(params,
					SNDRV_PCM_HW_PARAM_CHANNELS);
	struct snd_mask *fmt = hw_param_mask(params, SNDRV_PCM_HW_PARAM_FORMAT);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);

	rate->min = rate->max = 48000;
	channels->min = channels->max = 2;
	snd_mask_none(fmt);
	snd_mask_set_format(fmt, SNDRV_PCM_FORMAT_S24_LE);

	/* Set SLIM channel map on the CPU DAI so q6afe_slim_port_prepare
	 * gets correct num_channels and ch_mapping
	 */
	if (cpu_dai->id >= SLIMBUS_0_RX && cpu_dai->id <= SLIMBUS_6_TX) {
		const unsigned int slim_rx_ch[2] = {144, 145};
		const unsigned int slim_tx_ch[2] = {128, 129};
		snd_soc_dai_set_channel_map(cpu_dai, 2, slim_tx_ch,
					    2, slim_rx_ch);

		/* Send CDC SLIMBUS slave config - needed by Raphael FW */
		{
			u64 eaddr = 0;
			struct snd_soc_dai *codec_dai = snd_soc_rtd_to_codec(rtd, 0);
			if (codec_dai->component->dev->parent) {
				struct slim_device *slim = to_slim_device(codec_dai->component->dev->parent);
				memcpy(&eaddr, &slim->e_addr, sizeof(slim->e_addr));
			}
			q6afe_send_cdc_slimbus_slave_cfg(cpu_dai->dev, eaddr);
		}
	}

	return 0;
}

/*
 * MI2S fixup for TFA9874: 48kHz / 1ch / S16_LE
 * TFA9874 only supports S16_LE format.
 */
static int sm8150_mi2s_be_hw_params_fixup(struct snd_soc_pcm_runtime *rtd,
					  struct snd_pcm_hw_params *params)
{
	struct snd_interval *rate = hw_param_interval(params,
					SNDRV_PCM_HW_PARAM_RATE);
	struct snd_interval *channels = hw_param_interval(params,
					SNDRV_PCM_HW_PARAM_CHANNELS);
	struct snd_mask *fmt = hw_param_mask(params, SNDRV_PCM_HW_PARAM_FORMAT);

	rate->min = rate->max = 48000;
	channels->min = channels->max = 1;
	snd_mask_none(fmt);
	snd_mask_set_format(fmt, SNDRV_PCM_FORMAT_S16_LE);

	return 0;
}

/* ==================== SLIMBus hw_params ==================== */

static int sm8150_slim_snd_hw_params(struct snd_pcm_substream *substream,
				     struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);
	struct snd_soc_dai *codec_dai;
	struct sm8150_snd_data *pdata = snd_soc_card_get_drvdata(rtd->card);
	u32 rx_ch[SLIM_MAX_RX_PORTS], tx_ch[SLIM_MAX_TX_PORTS];
	struct sdw_stream_runtime *sruntime;
	u32 rx_ch_cnt = 0, tx_ch_cnt = 0;
	int ret = 0, i;

	for_each_rtd_codec_dais(rtd, i, codec_dai) {
		sruntime = snd_soc_dai_get_stream(codec_dai,
						  substream->stream);
		if (sruntime != ERR_PTR(-ENOTSUPP))
			pdata->sruntime[cpu_dai->id] = sruntime;

		ret = snd_soc_dai_get_channel_map(codec_dai,
				&tx_ch_cnt, tx_ch, &rx_ch_cnt, rx_ch);

		/* If get_channel_map returns error or 0 channels (e.g. after
		 * headphone unplug cleared the MUX), fall back to defaults.
		 */
		if (ret != 0 && ret != -ENOTSUPP) {
			pr_err("failed to get codec chan map, err:%d\n", ret);
			return ret;
		} else if (ret == -ENOTSUPP) {
			continue;
		}

		/* Fallback: 0 channels means MUX was cleared by unplug */
		if (rx_ch_cnt == 0 &&
		    substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
			rx_ch_cnt = 2;
			rx_ch[0] = 144; rx_ch[1] = 145;
		}
		if (tx_ch_cnt == 0 &&
		    substream->stream == SNDRV_PCM_STREAM_CAPTURE) {
			tx_ch_cnt = 2;
			tx_ch[0] = 128; tx_ch[1] = 129;
		}

		if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
			ret = snd_soc_dai_set_channel_map(cpu_dai, 0, NULL,
							  rx_ch_cnt, rx_ch);
		else
			ret = snd_soc_dai_set_channel_map(cpu_dai, tx_ch_cnt,
							  tx_ch, 0, NULL);
	}

	return 0;
}

/* ==================== hw_params dispatcher ==================== */

static int sm8150_snd_hw_params(struct snd_pcm_substream *substream,
					struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);
	int ret = 0;

	switch (cpu_dai->id) {
	case SLIMBUS_0_RX ... SLIMBUS_6_TX:
		ret = sm8150_slim_snd_hw_params(substream, params);
		break;
	default:
		break;
	}
	return ret;
}

/* ==================== DAI init (jack setup, SLIMBUS channel map) ==================== */

static int sm8150_dai_init(struct snd_soc_pcm_runtime *rtd)
{
	struct snd_soc_card *card = rtd->card;
	struct snd_soc_dai *codec_dai = snd_soc_rtd_to_codec(rtd, 0);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);
	struct sm8150_snd_data *pdata = snd_soc_card_get_drvdata(card);
	struct snd_soc_dai_link *link = rtd->dai_link;
	struct snd_jack *jack;
	unsigned int rx_ch[SLIM_MAX_RX_PORTS] = {144, 145, 146, 147, 148, 149,
					150, 151, 152, 153, 154, 155, 156};
	unsigned int tx_ch[SLIM_MAX_TX_PORTS] = {128, 129, 130, 131, 132, 133,
					    134, 135, 136, 137, 138, 139,
					    140, 141, 142, 143};
	int rval, i;

	dev_info(card->dev, "dai_init: link=%s no_pcm=%d cpu_id=0x%x\n",
		 link->name, link->no_pcm, cpu_dai->id);

	/* Set up headset jack detection */
	if (!pdata->jack_setup) {
		rval = snd_soc_card_jack_new(card, "Headset Jack",
				SND_JACK_HEADSET |
				SND_JACK_HEADPHONE |
				SND_JACK_BTN_0 | SND_JACK_BTN_1 |
				SND_JACK_BTN_2 | SND_JACK_BTN_3,
				&pdata->jack);

		if (rval < 0) {
			dev_err(card->dev, "Unable to add Headphone Jack\n");
			return rval;
		}

		jack = pdata->jack.jack;

		snd_jack_set_key(jack, SND_JACK_BTN_0, KEY_PLAYPAUSE);
		snd_jack_set_key(jack, SND_JACK_BTN_1, KEY_VOICECOMMAND);
		snd_jack_set_key(jack, SND_JACK_BTN_2, KEY_VOLUMEUP);
		snd_jack_set_key(jack, SND_JACK_BTN_3, KEY_VOLUMEDOWN);
		pdata->jack_setup = true;
	}

	switch (cpu_dai->id) {
	case SLIMBUS_0_RX ... SLIMBUS_6_TX:
		/* setup SLIM channel map once */
		if (pdata->slim_port_setup)
			return 0;

		/* Send CDC SLIMBUS slave config to DSP (required by Raphael FW) */
		for_each_rtd_codec_dais(rtd, i, codec_dai) {
			u64 eaddr = 0;
			/* codec component dev's parent is the SLIMbus device */
			struct device *parent = codec_dai->component->dev->parent;
			if (parent) {
				struct slim_device *slim = to_slim_device(parent);
				memcpy(&eaddr, &slim->e_addr, sizeof(slim->e_addr));
			}
			q6afe_send_cdc_slimbus_slave_cfg(cpu_dai->dev, eaddr);
		}

		for_each_rtd_codec_dais(rtd, i, codec_dai) {
			rval = snd_soc_dai_set_channel_map(codec_dai,
							  ARRAY_SIZE(tx_ch),
							  tx_ch,
							  ARRAY_SIZE(rx_ch),
							  rx_ch);
			if (rval != 0 && rval != -ENOTSUPP)
				return rval;

			snd_soc_dai_set_sysclk(codec_dai, 0,
					       WCD934X_DEFAULT_MCLK_RATE,
					       SNDRV_PCM_STREAM_PLAYBACK);

			rval = snd_soc_component_set_jack(codec_dai->component,
							  &pdata->jack, NULL);
			if (rval != 0 && rval != -ENOTSUPP) {
				dev_warn(card->dev, "Failed to set jack: %d\n", rval);
				return rval;
			}
		}

		/* Also set channel map on the CPU DAI so q6afe_slim_port_prepare
		 * gets correct num_channels and ch_mapping
		 * Use 2 channels (stereo) for SLIMBUS playback
		 */
		{
			unsigned int slim_rx_ch[2] = {144, 145};
			unsigned int slim_tx_ch[2] = {128, 129};
			int ret;

			dev_info(card->dev, "Setting CPU DAI channel map: tx=2 rx=2\n");
			ret = snd_soc_dai_set_channel_map(cpu_dai,
						    2, slim_tx_ch,
						    2, slim_rx_ch);
			dev_info(card->dev, "snd_soc_dai_set_channel_map returned: %d\n", ret);
		}

		pdata->slim_port_setup = true;
		break;
	default:
		break;
	}

	return 0;
}

/* ==================== Codec mixer setup (late_probe) ==================== */

static int sm8150_late_probe(struct snd_soc_card *card)
{
	dev_info(card->dev, "late_probe: card registered, run amixer to configure routing\n");
	return 0;
}

/* ==================== Startup (MI2S clock) ==================== */

static int sm8150_snd_startup(struct snd_pcm_substream *substream)
{
	unsigned int fmt = SND_SOC_DAIFMT_BP_FP;
	unsigned int codec_dai_fmt = SND_SOC_DAIFMT_BC_FC;
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);
	struct snd_soc_dai *codec_dai = snd_soc_rtd_to_codec(rtd, 0);

	switch (cpu_dai->id) {
	case QUATERNARY_MI2S_RX:
		/*
		 * Set MI2S format:
		 * BP_FP (CPU is consumer, codec provides clocks)
		 * BC_FC (codec is consumer = slave)
		 * NB_NF + I2S
		 */
		codec_dai_fmt |= SND_SOC_DAIFMT_NB_NF | SND_SOC_DAIFMT_I2S;
		snd_soc_dai_set_sysclk(cpu_dai,
			Q6AFE_LPASS_CLK_ID_QUAD_MI2S_IBIT,
			MI2S_BCLK_RATE, SNDRV_PCM_STREAM_PLAYBACK);
		snd_soc_dai_set_fmt(cpu_dai, fmt);
		snd_soc_dai_set_fmt(codec_dai, codec_dai_fmt);
		break;
	case SLIMBUS_0_RX ... SLIMBUS_6_TX:
		break;
	default:
		pr_err("%s: invalid dai id 0x%x\n", __func__, cpu_dai->id);
		break;
	}
	return 0;
}

static void sm8150_snd_shutdown(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct sm8150_snd_data *data = snd_soc_card_get_drvdata(rtd->card);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);

	switch (cpu_dai->id) {
	case SLIMBUS_0_RX ... SLIMBUS_6_TX:
		/* Don't clear slim_port_setup on shutdown - the channel
		 * map only needs to be set once after boot. Re-clearing it
		 * causes unnecessary re-initialization and can lead to
		 * audio instability (extclk/MCLK being toggled).
		 */
		break;
	case QUATERNARY_MI2S_RX:
		break;
	default:
		pr_err("%s: invalid dai id 0x%x\n", __func__, cpu_dai->id);
		break;
	}
}

/* ==================== SoundWire prepare/free ==================== */

static int sm8150_snd_prepare(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct sm8150_snd_data *data = snd_soc_card_get_drvdata(rtd->card);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);
	struct sdw_stream_runtime *sruntime = data->sruntime[cpu_dai->id];
	int ret;

	if (!sruntime)
		return 0;

	if (data->stream_prepared[cpu_dai->id]) {
		sdw_disable_stream(sruntime);
		sdw_deprepare_stream(sruntime);
		data->stream_prepared[cpu_dai->id] = false;
	}

	ret = sdw_prepare_stream(sruntime);
	if (ret)
		return ret;

	ret = sdw_enable_stream(sruntime);
	if (ret) {
		sdw_deprepare_stream(sruntime);
		return ret;
	}
	data->stream_prepared[cpu_dai->id] = true;

	return ret;
}

static int sm8150_snd_hw_free(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct sm8150_snd_data *data = snd_soc_card_get_drvdata(rtd->card);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);
	struct sdw_stream_runtime *sruntime = data->sruntime[cpu_dai->id];

	if (sruntime && data->stream_prepared[cpu_dai->id]) {
		sdw_disable_stream(sruntime);
		sdw_deprepare_stream(sruntime);
		data->stream_prepared[cpu_dai->id] = false;
	}

	return 0;
}

/* ==================== BE ops ==================== */

static const struct snd_soc_ops sm8150_be_ops = {
	.hw_params = sm8150_snd_hw_params,
	.hw_free = sm8150_snd_hw_free,
	.prepare = sm8150_snd_prepare,
	.startup = sm8150_snd_startup,
	.shutdown = sm8150_snd_shutdown,
};

/* ==================== Card setup ==================== */

static void sm8150_add_be_ops(struct snd_soc_card *card)
{
	struct snd_soc_dai_link *link;
	int i;

	for_each_card_prelinks(card, i, link) {
		if (link->no_pcm == 1) {
			/*
			 * Assign MI2S-specific fixup for Quaternary MI2S.
			 * TFA9874 requires S16_LE / 1ch / 48kHz.
			 */
			if (link->id == QUATERNARY_MI2S_RX)
				link->be_hw_params_fixup = sm8150_mi2s_be_hw_params_fixup;
			else
				link->be_hw_params_fixup = sm8150_be_hw_params_fixup;
			link->ops = &sm8150_be_ops;
		}
		link->init = sm8150_dai_init;
	}
}

/* ==================== Platform probe ==================== */

static int sm8150_platform_probe(struct platform_device *pdev)
{
	struct snd_soc_card *card;
	struct sm8150_snd_data *data;
	struct device *dev = &pdev->dev;
	int ret;

	card = devm_kzalloc(dev, sizeof(*card), GFP_KERNEL);
	if (!card)
		return -ENOMEM;

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	card->driver_name = DRIVER_NAME;
	card->dev = dev;
	card->owner = THIS_MODULE;
	card->late_probe = sm8150_late_probe;
	dev_set_drvdata(dev, card);

	dev_info(dev, "sm8150-raphael probe start\n");

	/*
	 * Parse DAI links from device tree.
	 * The DT sound node should use compatible "qcom,sm8150-sndcard"
	 * with child nodes for each DAI link (cpu/codec/platform).
	 */
	ret = qcom_snd_parse_of(card);
	if (ret) {
		dev_err(dev, "qcom_snd_parse_of failed: %d\n", ret);
		return ret;
	}
	dev_info(dev, "qcom_snd_parse_of OK: %d links\n", card->num_links);

	data->card = card;
	snd_soc_card_set_drvdata(card, data);

	sm8150_add_be_ops(card);

	dev_info(dev, "registering sound card\n");
	return devm_snd_soc_register_card(dev, card);
}

/* ==================== Driver registration ==================== */

static const struct of_device_id snd_sm8150_dt_match[] = {
	{.compatible = "qcom,sm8150-sndcard"},
	{}
};

MODULE_DEVICE_TABLE(of, snd_sm8150_dt_match);

static struct platform_driver snd_sm8150_driver = {
	.probe  = sm8150_platform_probe,
	.driver = {
		.name = "snd-sm8150-raphael",
		.of_match_table = snd_sm8150_dt_match,
	},
};
module_platform_driver(snd_sm8150_driver);

MODULE_AUTHOR("map220v <map220v300@gmail.com>");
MODULE_DESCRIPTION("SM8150 ASoC Machine Driver (Xiaomi Raphael)");
MODULE_LICENSE("GPL v2");
