/*
 * kirkwood-i2s.c
 *
 * (c) 2010 Arnaud Patard <apatard@mandriva.com>
 * (c) 2010 Arnaud Patard <arnaud.patard@rtp-net.org>
 *
 *  This program is free software; you can redistribute  it and/or modify it
 *  under  the terms of  the GNU General  Public License as published by the
 *  Free Software Foundation;  either version 2 of the  License, or (at your
 *  option) any later version.
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/io.h>
#include <linux/slab.h>
#include <linux/mbus.h>
#include <linux/delay.h>
#include <linux/clk.h>
#include <sound/asoundef.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <plat/audio.h>
#include "kirkwood.h"

#define DRV_NAME	"kirkwood-i2s"

#define KIRKWOOD_I2S_RATES \
	(SNDRV_PCM_RATE_44100 | \
	 SNDRV_PCM_RATE_48000 | \
	 SNDRV_PCM_RATE_96000)

#define KIRKWOOD_I2S_FORMATS \
	(SNDRV_PCM_FMTBIT_S16_LE | \
	 SNDRV_PCM_FMTBIT_S24_LE | \
	 SNDRV_PCM_FMTBIT_S32_LE | \
	 SNDRV_PCM_FMTBIT_IEC958_SUBFRAME_LE | \
	 SNDRV_PCM_FMTBIT_IEC958_SUBFRAME_BE)

static inline int kirkwood_set_dco(void __iomem *io, unsigned long rate);

static void kirkwood_i2s_dump_spdif(struct kirkwood_dma_data *priv)
{
	u32 reg;

	reg = readl(priv->io + KIRKWOOD_SPDIF_PLAYCTL);
	printk(">>> SPDIF Playback Ctrl = %08x\n", reg);
	printk(">>>  - Non-PCM             = %d\n", (reg & KIRKWOOD_SPDIF_NON_PCM) ? 1 : 0);
	printk(">>>  - Register Validity   = %d\n", (reg & KIRKWOOD_SPDIF_REG_VALIDITY) ? 1 : 0);
	printk(">>>  - Force Parity Error  = %d\n", (reg & KIRKWOOD_SPDIF_FORCE_PARERR) ? 1 : 0);
	printk(">>>  - Mem User Enable     = %d\n", (reg & KIRKWOOD_SPDIF_MEM_USER_EN) ? 1 : 0);
	printk(">>>  - Mem Validity Enable = %d\n", (reg & KIRKWOOD_SPDIF_MEM_VALIDITY_EN) ? 1 : 0);
	printk(">>>  - Block Start Mode    = %d\n", (reg & KIRKWOOD_SPDIF_BLOCK_START_MODE) ? 1 : 0);
}

static void kirkwood_i2s_dump_iec958(struct snd_aes_iec958 *iec)
{
	int i;

	printk(">>> status   = ");
	for(i=0; i < 4; i++)
		printk("%02x ", iec->status[i]);
	printk("\n");
	printk(">>> con/pro = %s, non-audio = %d\n",
	       (iec->status[0] & IEC958_AES0_PROFESSIONAL) ? "pro" : "con",
	       (iec->status[0] & IEC958_AES0_NONAUDIO) ? 1 : 0);
}

int kirkwood_i2s_control_iec958_dflt_info(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_info *uinfo)
{
	printk(">>> %s\n", __FUNCTION__);

	uinfo->type = SNDRV_CTL_ELEM_TYPE_IEC958;
	uinfo->count = 1;
	return 0;
}

int kirkwood_i2s_control_iec958_dflt_get(struct snd_kcontrol *kcontrol,
					 struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_dai *cpu_dai = snd_kcontrol_chip(kcontrol);
	struct kirkwood_dma_data *priv = snd_soc_dai_get_drvdata(cpu_dai);
	u32 reg;
	int n;

	printk(">>> %s :: kcontrol = %p, kcontrol->private_data = %p, ucontrol = %p\n", __FUNCTION__, kcontrol, kcontrol->private_data, ucontrol);
	kirkwood_i2s_dump_iec958(&ucontrol->value.iec958);

#if 0
	reg = readl(priv->io+KIRKWOOD_SPDIF_PLAYCTL);
	ucontrol->value.iec958.status[0] &=
		~(IEC958_AES0_PROFESSIONAL | IEC958_AES0_NONAUDIO);
	if (reg & KIRKWOOD_SPDIF_NON_PCM)
		ucontrol->value.iec958.status[0] |= IEC958_AES0_NONAUDIO;
#endif

	for(n=0; n<6; n++) {
		reg = readl(priv->io + KIRKWOOD_SPDIF_STATUS0_L + (4*n));
		ucontrol->value.iec958.status[4*n+3] = (uint8_t)(reg >> 24) & 0xff;
		ucontrol->value.iec958.status[4*n+2] = (uint8_t)(reg >> 16) & 0xff;
		ucontrol->value.iec958.status[4*n+1] = (uint8_t)(reg >>  8) & 0xff;
		ucontrol->value.iec958.status[4*n+0] = (uint8_t)(reg >>  0) & 0xff;
	}

	return 0;
}

int kirkwood_i2s_control_iec958_dflt_put(struct snd_kcontrol *kcontrol,
					 struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_dai *cpu_dai = snd_kcontrol_chip(kcontrol);
	struct kirkwood_dma_data *priv = snd_soc_dai_get_drvdata(cpu_dai);
	u32 reg, nreg;
	int n;

	printk(">>> %s :: kcontrol = %p, ucontrol = %p\n", __FUNCTION__, kcontrol, ucontrol);
	kirkwood_i2s_dump_iec958(&ucontrol->value.iec958);

	reg  = readl(priv->io+KIRKWOOD_SPDIF_PLAYCTL);
	nreg = reg & ~(KIRKWOOD_SPDIF_NON_PCM | KIRKWOOD_SPDIF_REG_VALIDITY);
	if (ucontrol->value.iec958.status[0] & IEC958_AES0_NONAUDIO)
		nreg |= KIRKWOOD_SPDIF_NON_PCM | KIRKWOOD_SPDIF_REG_VALIDITY;
	writel(nreg, priv->io+KIRKWOOD_SPDIF_PLAYCTL);

	for(n=0; n<6; n++) {
		reg = (ucontrol->value.iec958.status[4*n+3] << 24) |
			(ucontrol->value.iec958.status[4*n+2] << 16) |
			(ucontrol->value.iec958.status[4*n+1] << 8) |
			(ucontrol->value.iec958.status[4*n+0]);
		writel(reg, priv->io + KIRKWOOD_SPDIF_STATUS0_L + (4*n));
		writel(reg, priv->io + KIRKWOOD_SPDIF_STATUS0_R + (4*n));
	}
	return (nreg != reg);
}

const struct snd_kcontrol_new kirkwood_i2s_iec958_controls[] = {
{
	.iface =	SNDRV_CTL_ELEM_IFACE_PCM,
	.name =		SNDRV_CTL_NAME_IEC958("", PLAYBACK, DEFAULT),
	.access =	SNDRV_CTL_ELEM_ACCESS_READWRITE |
			SNDRV_CTL_ELEM_ACCESS_VOLATILE,
	.info =		kirkwood_i2s_control_iec958_dflt_info,
	.get =		kirkwood_i2s_control_iec958_dflt_get,
	.put =		kirkwood_i2s_control_iec958_dflt_put,
},
};
EXPORT_SYMBOL(kirkwood_i2s_iec958_controls);


static int kirkwood_i2s_set_fmt(struct snd_soc_dai *cpu_dai,
		unsigned int fmt)
{
	struct kirkwood_dma_data *priv = snd_soc_dai_get_drvdata(cpu_dai);
	unsigned long mask;
	unsigned long value;
	switch (fmt & SND_SOC_DAIFMT_FORMAT_MASK) {
	case SND_SOC_DAIFMT_RIGHT_J:
		mask = KIRKWOOD_I2S_CTL_RJ;
		break;
	case SND_SOC_DAIFMT_LEFT_J:
		mask = KIRKWOOD_I2S_CTL_LJ;
		break;
	case SND_SOC_DAIFMT_I2S:
		mask = KIRKWOOD_I2S_CTL_I2S;
		break;
	default:
		return -EINVAL;
	}

	/*
	 * Set same format for playback and record
	 * This avoids some troubles.
	 */
	value = readl(priv->io+KIRKWOOD_I2S_PLAYCTL);
	value &= ~KIRKWOOD_I2S_CTL_JUST_MASK;
	value |= mask;
	writel(value, priv->io+KIRKWOOD_I2S_PLAYCTL);

	value = readl(priv->io+KIRKWOOD_I2S_RECCTL);
	value &= ~KIRKWOOD_I2S_CTL_JUST_MASK;
	value |= mask;
	writel(value, priv->io+KIRKWOOD_I2S_RECCTL);

	return 0;
}

static inline int kirkwood_set_dco(void __iomem *io, unsigned long rate)
{
	unsigned long value;

	value = KIRKWOOD_DCO_CTL_OFFSET_0;
	switch (rate) {
	case 44100:
		value |= KIRKWOOD_DCO_CTL_FREQ_11;
		break;
	case 48000:
		value |= KIRKWOOD_DCO_CTL_FREQ_12;
		break;
	case 96000:
		value |= KIRKWOOD_DCO_CTL_FREQ_24;
		break;
	default:
		printk (KERN_ERR "%s Unsupported DCO rate %lu\n",
			__FUNCTION__, rate);
		return -EINVAL;
	}
	writel(value, io + KIRKWOOD_DCO_CTL);

	/* wait for dco locked */
	do {
		cpu_relax();
		value = readl(io + KIRKWOOD_DCO_SPCR_STATUS);
		value &= KIRKWOOD_DCO_SPCR_STATUS_DCO_LOCK;
	} while (value == 0);
	return 0;
}

static inline int kirkwood_set_rate(struct kirkwood_dma_data* priv,
				     unsigned long rate)
{
	int ret = -EINVAL;
	/* First check if ext clk is available */
	if (!IS_ERR(priv->extclk)) {
		/* use optional external clk for other rates */
		printk (">>> %s :: extclk set rate = %lu -> %lu\n",
			__FUNCTION__, rate, 256*rate);
		ret = clk_set_rate(priv->extclk, 256*rate);
		if (!ret) writel(KIRKWOOD_MCLK_SOURCE_EXTCLK,
				priv->io+KIRKWOOD_CLOCKS_CTRL);
	}
	if (ret && (rate == 44100 || rate == 48000 || rate == 96000)) {
		/* use internal dco for supported rates */
		printk (">>> %s :: dco set rate = %lu\n", 
			__FUNCTION__, rate);
		ret = kirkwood_set_dco(priv->io, rate);
		if (!ret) writel(KIRKWOOD_MCLK_SOURCE_DCO, 
			priv->io+KIRKWOOD_CLOCKS_CTRL);
	}
	return ret;
}

static int kirkwood_i2s_startup(struct snd_pcm_substream *substream,
		struct snd_soc_dai *dai)
{
	struct kirkwood_dma_data *priv = snd_soc_dai_get_drvdata(dai);

	snd_soc_dai_set_dma_data(dai, substream, priv);
	return 0;
}

static int kirkwood_i2s_hw_params(struct snd_pcm_substream *substream,
				 struct snd_pcm_hw_params *params,
				 struct snd_soc_dai *dai)
{
	struct kirkwood_dma_data *priv = snd_soc_dai_get_drvdata(dai);
	unsigned int i2s_reg, reg;
	unsigned long i2s_value, value;
	int ret;
	priv->i2s = 1;
	priv->spdif = 1;
	priv->iec958 = 0;

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		i2s_reg = KIRKWOOD_I2S_PLAYCTL;
		reg = KIRKWOOD_PLAYCTL;
	} else {
		i2s_reg = KIRKWOOD_I2S_RECCTL;
		reg = KIRKWOOD_RECCTL;
		priv->spdif = 0;
	}


	/* set rate */
	ret = kirkwood_set_rate(priv, params_rate(params));
	if (ret) return ret;

	i2s_value = readl(priv->io+i2s_reg);
	i2s_value &= ~KIRKWOOD_I2S_CTL_SIZE_MASK;

	value = readl(priv->io+reg);
	value &= ~KIRKWOOD_PLAYCTL_SIZE_MASK;

	/*
	 * Size settings in play/rec i2s control regs and play/rec control
	 * regs must be the same.
	 */
	switch (params_format(params)) {
	case SNDRV_PCM_FORMAT_S16_LE:
		i2s_value |= KIRKWOOD_I2S_CTL_SIZE_16;
		value |= KIRKWOOD_PLAYCTL_SIZE_16_C;
		break;
	/*
	 * doesn't work... S20_3LE != kirkwood 20bit format ?
	 *
	case SNDRV_PCM_FORMAT_S20_3LE:
		i2s_value |= KIRKWOOD_I2S_CTL_SIZE_20;
		value |= KIRKWOOD_PLAYCTL_SIZE_20;
		break;
	*/
	case SNDRV_PCM_FORMAT_S24_LE:
		i2s_value |= KIRKWOOD_I2S_CTL_SIZE_24;
		value |= KIRKWOOD_PLAYCTL_SIZE_24;
		break;
	case SNDRV_PCM_FORMAT_S32_LE:
		i2s_value |= KIRKWOOD_I2S_CTL_SIZE_32;
		value |= KIRKWOOD_PLAYCTL_SIZE_32;
		priv->spdif = 0;
		break;
	case SNDRV_PCM_FORMAT_IEC958_SUBFRAME_LE:
	case SNDRV_PCM_FORMAT_IEC958_SUBFRAME_BE:
		if (substream->stream == SNDRV_PCM_STREAM_CAPTURE)
			return -EINVAL;
		i2s_value |= KIRKWOOD_I2S_CTL_SIZE_16;
		value |= KIRKWOOD_PLAYCTL_SIZE_16_C;
		priv->i2s = 0;
		priv->iec958 = 1;
		break;
	default:
		return -EINVAL;
	}

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		value &= ~KIRKWOOD_PLAYCTL_MONO_MASK;
		if (params_channels(params) == 1)
			value |= KIRKWOOD_PLAYCTL_MONO_BOTH;
		else
			value |= KIRKWOOD_PLAYCTL_MONO_OFF;
	}

	writel(i2s_value, priv->io+i2s_reg);
	writel(value, priv->io+reg);

	return 0;
}

static int kirkwood_i2s_play_trigger(struct snd_pcm_substream *substream,
				int cmd, struct snd_soc_dai *dai)
{
	struct kirkwood_dma_data *priv = snd_soc_dai_get_drvdata(dai);
	unsigned long value;

	/*
	 * specs says KIRKWOOD_PLAYCTL must be read 2 times before
	 * changing it. So read 1 time here and 1 later.
	 */
	value = readl(priv->io + KIRKWOOD_PLAYCTL);

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
		/* stop audio, enable interrupts */
		value = readl(priv->io + KIRKWOOD_PLAYCTL);
		value |= KIRKWOOD_PLAYCTL_PAUSE;
		writel(value, priv->io + KIRKWOOD_PLAYCTL);

		value = readl(priv->io + KIRKWOOD_INT_MASK);
		value |= KIRKWOOD_INT_CAUSE_PLAY_BYTES;
		writel(value, priv->io + KIRKWOOD_INT_MASK);

		/* configure audio & enable i2s playback */
		value = readl(priv->io + KIRKWOOD_PLAYCTL);
		value &= ~KIRKWOOD_PLAYCTL_BURST_MASK;
		value &= ~(KIRKWOOD_PLAYCTL_PAUSE | KIRKWOOD_PLAYCTL_I2S_MUTE
				| KIRKWOOD_PLAYCTL_SPDIF_EN);

		if (priv->burst == 32)
			value |= KIRKWOOD_PLAYCTL_BURST_32;
		else
			value |= KIRKWOOD_PLAYCTL_BURST_128;
		if (priv->i2s)
			value |= KIRKWOOD_PLAYCTL_I2S_EN;
		if (priv->spdif)
			value |= KIRKWOOD_PLAYCTL_SPDIF_EN;
		writel(value, priv->io + KIRKWOOD_PLAYCTL);

		kirkwood_i2s_dump_spdif(priv);
		
		break;

	case SNDRV_PCM_TRIGGER_STOP:
		/* stop audio, disable interrupts */
		value = readl(priv->io + KIRKWOOD_PLAYCTL);
		value |= KIRKWOOD_PLAYCTL_PAUSE | KIRKWOOD_PLAYCTL_I2S_MUTE;
		writel(value, priv->io + KIRKWOOD_PLAYCTL);

		value = readl(priv->io + KIRKWOOD_INT_MASK);
		value &= ~KIRKWOOD_INT_CAUSE_PLAY_BYTES;
		writel(value, priv->io + KIRKWOOD_INT_MASK);

		/* disable all playbacks */
		value = readl(priv->io + KIRKWOOD_PLAYCTL);
		value &= ~(KIRKWOOD_PLAYCTL_I2S_EN | KIRKWOOD_PLAYCTL_SPDIF_EN);
		writel(value, priv->io + KIRKWOOD_PLAYCTL);
		break;

	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
	case SNDRV_PCM_TRIGGER_SUSPEND:
		value = readl(priv->io + KIRKWOOD_PLAYCTL);
		value |= KIRKWOOD_PLAYCTL_PAUSE | KIRKWOOD_PLAYCTL_I2S_MUTE;
		writel(value, priv->io + KIRKWOOD_PLAYCTL);
		break;

	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		value = readl(priv->io + KIRKWOOD_PLAYCTL);
		value &= ~(KIRKWOOD_PLAYCTL_PAUSE | KIRKWOOD_PLAYCTL_I2S_MUTE);
		writel(value, priv->io + KIRKWOOD_PLAYCTL);
		break;

	default:
		return -EINVAL;
	}

	return 0;
}

static int kirkwood_i2s_rec_trigger(struct snd_pcm_substream *substream,
				int cmd, struct snd_soc_dai *dai)
{
	struct kirkwood_dma_data *priv = snd_soc_dai_get_drvdata(dai);
	unsigned long value;

	value = readl(priv->io + KIRKWOOD_RECCTL);

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
		/* stop audio, enable interrupts */
		value = readl(priv->io + KIRKWOOD_RECCTL);
		value |= KIRKWOOD_RECCTL_PAUSE;
		writel(value, priv->io + KIRKWOOD_RECCTL);

		value = readl(priv->io + KIRKWOOD_INT_MASK);
		value |= KIRKWOOD_INT_CAUSE_REC_BYTES;
		writel(value, priv->io + KIRKWOOD_INT_MASK);

		/* configure audio & enable i2s record */
		value = readl(priv->io + KIRKWOOD_RECCTL);
		value &= ~KIRKWOOD_RECCTL_BURST_MASK;
		value &= ~KIRKWOOD_RECCTL_MONO;
		value &= ~(KIRKWOOD_RECCTL_PAUSE | KIRKWOOD_RECCTL_MUTE
			| KIRKWOOD_RECCTL_SPDIF_EN);

		if (priv->burst == 32)
			value |= KIRKWOOD_RECCTL_BURST_32;
		else
			value |= KIRKWOOD_RECCTL_BURST_128;
		value |= KIRKWOOD_RECCTL_I2S_EN;

		writel(value, priv->io + KIRKWOOD_RECCTL);
		break;

	case SNDRV_PCM_TRIGGER_STOP:
		/* stop audio, disable interrupts */
		value = readl(priv->io + KIRKWOOD_RECCTL);
		value |= KIRKWOOD_RECCTL_PAUSE | KIRKWOOD_RECCTL_MUTE;
		writel(value, priv->io + KIRKWOOD_RECCTL);

		value = readl(priv->io + KIRKWOOD_INT_MASK);
		value &= ~KIRKWOOD_INT_CAUSE_REC_BYTES;
		writel(value, priv->io + KIRKWOOD_INT_MASK);

		/* disable all records */
		value = readl(priv->io + KIRKWOOD_RECCTL);
		value &= ~(KIRKWOOD_RECCTL_I2S_EN | KIRKWOOD_RECCTL_SPDIF_EN);
		writel(value, priv->io + KIRKWOOD_RECCTL);
		break;

	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
	case SNDRV_PCM_TRIGGER_SUSPEND:
		value = readl(priv->io + KIRKWOOD_RECCTL);
		value |= KIRKWOOD_RECCTL_PAUSE | KIRKWOOD_RECCTL_MUTE;
		writel(value, priv->io + KIRKWOOD_RECCTL);
		break;

	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		value = readl(priv->io + KIRKWOOD_RECCTL);
		value &= ~(KIRKWOOD_RECCTL_PAUSE | KIRKWOOD_RECCTL_MUTE);
		writel(value, priv->io + KIRKWOOD_RECCTL);
		break;

	default:
		return -EINVAL;
	}

	return 0;
}

static int kirkwood_i2s_trigger(struct snd_pcm_substream *substream, int cmd,
			       struct snd_soc_dai *dai)
{
	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
		return kirkwood_i2s_play_trigger(substream, cmd, dai);
	else
		return kirkwood_i2s_rec_trigger(substream, cmd, dai);

	return 0;
}

static int kirkwood_i2s_probe(struct snd_soc_dai *dai)
{
	struct kirkwood_dma_data *priv = snd_soc_dai_get_drvdata(dai);
	unsigned long value;
	unsigned int reg_data;
	int ret;

	ret = snd_soc_add_dai_controls(dai, kirkwood_i2s_iec958_controls,
				ARRAY_SIZE(kirkwood_i2s_iec958_controls));
	if (ret) {
		dev_err(dai->dev, "unable to add soc card controls\n");
		return ret;
	}
	/* put system in a "safe" state : */
	/* disable audio interrupts */
	writel(0xffffffff, priv->io + KIRKWOOD_INT_CAUSE);
	writel(0, priv->io + KIRKWOOD_INT_MASK);

	reg_data = readl(priv->io + 0x120c);
	reg_data = readl(priv->io + 0x1200);
	reg_data &= (~(0x333FF8));
	reg_data |= 0x111D18;
	writel(reg_data, priv->io + 0x1200);
	msleep(500);
	reg_data = readl(priv->io + 0x1200);
	reg_data &= (~(0x333FF8));
	reg_data |= 0x111D18;
	msleep(500);
	writel(reg_data, priv->io + 0x1200);

	/* disable playback/record */
	value = readl(priv->io + KIRKWOOD_PLAYCTL);
	value &= ~(KIRKWOOD_PLAYCTL_I2S_EN|KIRKWOOD_PLAYCTL_SPDIF_EN);
	writel(value, priv->io + KIRKWOOD_PLAYCTL);

	value = readl(priv->io + KIRKWOOD_RECCTL);
	value &= ~(KIRKWOOD_RECCTL_I2S_EN | KIRKWOOD_RECCTL_SPDIF_EN);
	writel(value, priv->io + KIRKWOOD_RECCTL);

	return 0;

}

static int kirkwood_i2s_remove(struct snd_soc_dai *dai)
{
	return 0;
}

static const struct snd_soc_dai_ops kirkwood_i2s_dai_ops = {
	.startup	= kirkwood_i2s_startup,
	.trigger	= kirkwood_i2s_trigger,
	.hw_params      = kirkwood_i2s_hw_params,
	.set_fmt        = kirkwood_i2s_set_fmt,
};


static struct snd_soc_dai_driver kirkwood_i2s_dai = {
	.probe = kirkwood_i2s_probe,
	.remove = kirkwood_i2s_remove,
	.playback = {
		.channels_min = 1,
		.channels_max = 2,
		.rates = KIRKWOOD_I2S_RATES,
		.formats = KIRKWOOD_I2S_FORMATS,
	},
	.capture = {
		.channels_min = 1,
		.channels_max = 2,
		.rates = KIRKWOOD_I2S_RATES,
		.formats = KIRKWOOD_I2S_FORMATS,
	},
	.ops = &kirkwood_i2s_dai_ops,
};

static __devinit int kirkwood_i2s_dev_probe(struct platform_device *pdev)
{
	struct resource *mem;
	struct kirkwood_asoc_platform_data *data =
		pdev->dev.platform_data;
	struct kirkwood_dma_data *priv;
	int err;

	priv = kzalloc(sizeof(struct kirkwood_dma_data), GFP_KERNEL);
	if (!priv) {
		dev_err(&pdev->dev, "allocation failed\n");
		err = -ENOMEM;
		goto error;
	}
	dev_set_drvdata(&pdev->dev, priv);

	mem = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!mem) {
		dev_err(&pdev->dev, "platform_get_resource failed\n");
		err = -ENXIO;
		goto err_alloc;
	}

	priv->mem = request_mem_region(mem->start, SZ_16K, DRV_NAME);
	if (!priv->mem) {
		dev_err(&pdev->dev, "request_mem_region failed\n");
		err = -EBUSY;
		goto err_alloc;
	}

	priv->io = ioremap(priv->mem->start, SZ_16K);
	if (!priv->io) {
		dev_err(&pdev->dev, "ioremap failed\n");
		err = -ENOMEM;
		goto err_iomem;
	}

	priv->irq = platform_get_irq(pdev, 0);
	if (priv->irq <= 0) {
		dev_err(&pdev->dev, "platform_get_irq failed\n");
		err = -ENXIO;
		goto err_ioremap;
	}

	if (!data) {
		dev_err(&pdev->dev, "no platform data ?!\n");
		err = -EINVAL;
		goto err_ioremap;
	}

	priv->burst = data->burst;
	priv->clk = clk_get(&pdev->dev, NULL);
	if (IS_ERR(priv->clk)) {
		dev_err(&pdev->dev, "no clock\n");
		err = PTR_ERR(priv->clk);
		goto err_ioremap;
	}
	clk_prepare_enable(priv->clk);
	writel(KIRKWOOD_MCLK_SOURCE_DCO, 
	       priv->io+KIRKWOOD_CLOCKS_CTRL);

	priv->extclk = clk_get(&pdev->dev, "extclk");
	if (!IS_ERR(priv->extclk)) {
		if (priv->extclk == priv->clk) {
			clk_put(priv->extclk);
			priv->extclk = NULL;
		} else {
			dev_info(&pdev->dev, "found external clock\n");
			clk_prepare_enable(priv->extclk);
			kirkwood_i2s_dai.playback.channels_min = 1;
			kirkwood_i2s_dai.playback.channels_max = 8;
			kirkwood_i2s_dai.playback.rates = 
				SNDRV_PCM_RATE_8000_192000 |
				SNDRV_PCM_RATE_CONTINUOUS |
				SNDRV_PCM_RATE_KNOT;
			kirkwood_i2s_dai.capture.rates = 
				SNDRV_PCM_RATE_8000_192000 |
				SNDRV_PCM_RATE_CONTINUOUS |
				SNDRV_PCM_RATE_KNOT;
		}
	}

	return snd_soc_register_dai(&pdev->dev, &kirkwood_i2s_dai);

err_ioremap:
	iounmap(priv->io);
err_iomem:
	release_mem_region(priv->mem->start, SZ_16K);
err_alloc:
	kfree(priv);
error:
	return err;
}

static __devexit int kirkwood_i2s_dev_remove(struct platform_device *pdev)
{
	struct kirkwood_dma_data *priv = dev_get_drvdata(&pdev->dev);

	snd_soc_unregister_dai(&pdev->dev);
	if (!IS_ERR(priv->extclk)) {
		clk_disable_unprepare(priv->extclk);
		clk_put(priv->extclk);	
	}
	clk_disable_unprepare(priv->clk);
	clk_put(priv->clk);	
	iounmap(priv->io);
	release_mem_region(priv->mem->start, SZ_16K);
	kfree(priv);

	return 0;
}

static struct platform_driver kirkwood_i2s_driver = {
	.probe  = kirkwood_i2s_dev_probe,
	.remove = __devexit_p(kirkwood_i2s_dev_remove),
	.driver = {
		.name = DRV_NAME,
		.owner = THIS_MODULE,
	},
};

module_platform_driver(kirkwood_i2s_driver);

/* Module information */
MODULE_AUTHOR("Arnaud Patard, <arnaud.patard@rtp-net.org>");
MODULE_DESCRIPTION("Kirkwood I2S SoC Interface");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:kirkwood-i2s");
