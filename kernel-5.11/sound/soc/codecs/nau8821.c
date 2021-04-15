/*
 * Nuvoton NAU88L21 audio codec driver
 *
 * Copyright 2020 Nuvoton Technology Corp.
 * Author: John Hsu <KCHSU0@nuvoton.com>
 *
 * Licensed under the GPL-2.
 */

//#define DEBUG

#include <linux/module.h>
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/i2c.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <linux/clk.h>
#include <linux/acpi.h>
#include <linux/math64.h>
#include <linux/semaphore.h>
#include <sound/initval.h>
#include <sound/tlv.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/jack.h>
#include "nau8821.h"

#define NUVOTON_CODEC_DAI "nau8821-hifi"

#define NAU_FREF_MAX 13500000
#define NAU_FVCO_MAX 124000000
#define NAU_FVCO_MIN 90000000

/* the maximum frequency of CLK_ADC and CLK_DAC */
#define CLK_DA_AD_MAX 6144000

static int nau8821_configure_sysclk(struct nau8821 *nau8821,
	int clk_id, unsigned int freq);

struct nau8821_fll {
	int mclk_src;
	int ratio;
	int fll_frac;
	int fll_int;
	int clk_ref_div;
};

struct nau8821_fll_attr {
	unsigned int param;
	unsigned int val;
};

/* scaling for mclk from sysclk_src output */
static const struct nau8821_fll_attr mclk_src_scaling[] = {
	{ 1, 0x0 },
	{ 2, 0x2 },
	{ 4, 0x3 },
	{ 8, 0x4 },
	{ 16, 0x5 },
	{ 32, 0x6 },
	{ 3, 0x7 },
	{ 6, 0xa },
	{ 12, 0xb },
	{ 24, 0xc },
	{ 48, 0xd },
	{ 96, 0xe },
	{ 5, 0xf },
};

/* ratio for input clk freq */
static const struct nau8821_fll_attr fll_ratio[] = {
	{ 512000, 0x01 },
	{ 256000, 0x02 },
	{ 128000, 0x04 },
	{ 64000, 0x08 },
	{ 32000, 0x10 },
	{ 8000, 0x20 },
	{ 4000, 0x40 },
};

static const struct nau8821_fll_attr fll_pre_scalar[] = {
	{ 1, 0x0 },
	{ 2, 0x1 },
	{ 4, 0x2 },
	{ 8, 0x3 },
};

/* over sampling rate */
struct nau8821_osr_attr {
	unsigned int osr;
	unsigned int clk_src;
};

static const struct nau8821_osr_attr osr_dac_sel[] = {
	{ 64, 2 },		/* OSR 64, SRC 1/4 */
	{ 256, 0 },	/* OSR 256, SRC 1 */
	{ 128, 1 },	/* OSR 128, SRC 1/2 */
	{ 0, 0 },
	{ 32, 3 },		/* OSR 32, SRC 1/8 */
};

static const struct nau8821_osr_attr osr_adc_sel[] = {
	{ 32, 3 },		/* OSR 32, SRC 1/8 */
	{ 64, 2 },		/* OSR 64, SRC 1/4 */
	{ 128, 1 },	/* OSR 128, SRC 1/2 */
	{ 256, 0 },	/* OSR 256, SRC 1 */
};

static const struct reg_default nau8821_reg_defaults[] = {
	{ NAU8821_REG_ENA_CTRL, 0x00ff },
	{ NAU8821_REG_CLK_DIVIDER, 0x0050 },
	{ NAU8821_REG_FLL1, 0x0 },
	{ NAU8821_REG_FLL2, 0x00bc },
	{ NAU8821_REG_FLL3, 0x0008 },
	{ NAU8821_REG_FLL4, 0x0010 },
	{ NAU8821_REG_FLL5, 0x4000 },
	{ NAU8821_REG_FLL6, 0x6900 },
	{ NAU8821_REG_FLL7, 0x0031 },
	{ NAU8821_REG_FLL8, 0x26e9 },
	{ NAU8821_REG_JACK_DET_CTRL, 0x0 },
	{ NAU8821_REG_INTERRUPT_MASK, 0x0 },
	{ NAU8821_REG_INTERRUPT_DIS_CTRL, 0xffff },
	{ NAU8821_REG_DMIC_CTRL, 0x0 },
	{ NAU8821_REG_GPIO12_CTRL, 0x0 },
	{ NAU8821_REG_TDM_CTRL, 0x0 },
	{ NAU8821_REG_I2S_PCM_CTRL1, 0x000a },
	{ NAU8821_REG_I2S_PCM_CTRL2, 0x8010 },
	{ NAU8821_REG_LEFT_TIME_SLOT, 0x0 },
	{ NAU8821_REG_RIGHT_TIME_SLOT, 0x0 },
	{ NAU8821_REG_BIQ0_COF1, 0x0 },
	{ NAU8821_REG_BIQ0_COF2, 0x0 },
	{ NAU8821_REG_BIQ0_COF3, 0x0 },
	{ NAU8821_REG_BIQ0_COF4, 0x0 },
	{ NAU8821_REG_BIQ0_COF5, 0x0 },
	{ NAU8821_REG_BIQ0_COF6, 0x0 },
	{ NAU8821_REG_BIQ0_COF7, 0x0 },
	{ NAU8821_REG_BIQ0_COF8, 0x0 },
	{ NAU8821_REG_BIQ0_COF9, 0x0 },
	{ NAU8821_REG_BIQ0_COF10, 0x0 },
	{ NAU8821_REG_ADC_RATE, 0x0002 },
	{ NAU8821_REG_DAC_CTRL1, 0x0082 },
	{ NAU8821_REG_DAC_CTRL2, 0x0 },
	{ NAU8821_REG_DAC_DGAIN_CTRL, 0x0 },
	{ NAU8821_REG_ADC_DGAIN_CTRL, 0x0 },
	{ NAU8821_REG_MUTE_CTRL, 0x0 },
	{ NAU8821_REG_HSVOL_CTRL, 0x0 },
	{ NAU8821_REG_DACR_CTRL, 0xcfcf },
	{ NAU8821_REG_ADC_DGAIN_CTRL1, 0xcfcf },
	{ NAU8821_REG_ADC_DRC_KNEE_IP12, 0x1486 },
	{ NAU8821_REG_ADC_DRC_KNEE_IP34, 0x0f12 },
	{ NAU8821_REG_ADC_DRC_SLOPES, 0x25ff },
	{ NAU8821_REG_ADC_DRC_ATKDCY, 0x3457 },
	{ NAU8821_REG_DAC_DRC_KNEE_IP12, 0x1486 },
	{ NAU8821_REG_DAC_DRC_KNEE_IP34, 0x0f12 },
	{ NAU8821_REG_DAC_DRC_SLOPES, 0x25f9 },
	{ NAU8821_REG_DAC_DRC_ATKDCY, 0x3457 },
	{ NAU8821_REG_BIQ1_COF1, 0x0 },
	{ NAU8821_REG_BIQ1_COF2, 0x0 },
	{ NAU8821_REG_BIQ1_COF3, 0x0 },
	{ NAU8821_REG_BIQ1_COF4, 0x0 },
	{ NAU8821_REG_BIQ1_COF5, 0x0 },
	{ NAU8821_REG_BIQ1_COF6, 0x0 },
	{ NAU8821_REG_BIQ1_COF7, 0x0 },
	{ NAU8821_REG_BIQ1_COF8, 0x0 },
	{ NAU8821_REG_BIQ1_COF9, 0x0 },
	{ NAU8821_REG_BIQ1_COF10, 0x0 },
	{ NAU8821_REG_CLASSG_CTRL, 0x0 },
	{ NAU8821_REG_IMM_MODE_CTRL, 0x0 },
	{ NAU8821_REG_IMM_RMS_L, 0x0 },
	{ NAU8821_REG_OTPDOUT_1, 0xaad8 },
	{ NAU8821_REG_OTPDOUT_2, 0x0002 },
	{ NAU8821_REG_MISC_CTRL, 0x0 },
	{ NAU8821_REG_BIAS_ADJ, 0x0 },
	{ NAU8821_REG_TRIM_SETTINGS, 0x0 },
	{ NAU8821_REG_ANALOG_CONTROL_1, 0x0 },
	{ NAU8821_REG_ANALOG_CONTROL_2, 0x0 },
	{ NAU8821_REG_PGA_MUTE, 0x0 },
	{ NAU8821_REG_ANALOG_ADC_1, 0x0011 },
	{ NAU8821_REG_ANALOG_ADC_2, 0x0020 },
	{ NAU8821_REG_RDAC, 0x0008 },
	{ NAU8821_REG_MIC_BIAS, 0x0006 },
	{ NAU8821_REG_BOOST, 0x0 },
	{ NAU8821_REG_FEPGA, 0x0 },
	{ NAU8821_REG_PGA_GAIN, 0x0 },
	{ NAU8821_REG_POWER_UP_CONTROL, 0x0 },
	{ NAU8821_REG_CHARGE_PUMP, 0x0 },
};

/**
 * nau8821_sema_acquire - acquire the semaphore of nau8821
 * @nau8821:  component to register the codec private data with
 * @timeout: how long in jiffies to wait before failure or zero to wait
 * until release
 *
 * Attempts to acquire the semaphore with number of jiffies. If no more
 * tasks are allowed to acquire the semaphore, calling this function will
 * put the task to sleep. If the semaphore is not released within the
 * specified number of jiffies, this function returns.
 * If the semaphore is not released within the specified number of jiffies,
 * this function returns -ETIME. If the sleep is interrupted by a signal,
 * this function will return -EINTR. It returns 0 if the semaphore was
 * acquired successfully.
 *
 * Acquires the semaphore without jiffies. Try to acquire the semaphore
 * atomically. Returns 0 if the semaphore has been acquired successfully
 * or 1 if it it cannot be acquired.
 */
static int nau8821_sema_acquire(struct nau8821 *nau8821, long timeout)
{
	int ret;

	if (!nau8821->irq)
		return 1;

	if (timeout) {
		ret = down_timeout(&nau8821->jd_sem, timeout);
		if (ret < 0)
			dev_warn(nau8821->dev, "Acquire semaphore timeout\n");
	} else {
		ret = down_trylock(&nau8821->jd_sem);
		if (ret)
			dev_warn(nau8821->dev, "Acquire semaphore fail\n");
	}

	return ret;
}

/**
 * nau8821_sema_release - release the semaphore of nau8821
 * @nau8821:  component to register the codec private data with
 *
 * Release the semaphore which may be called from any context and
 * even by tasks which have never called down().
 */
static inline void nau8821_sema_release(struct nau8821 *nau8821)
{
	if (!nau8821->irq)
		return;
	up(&nau8821->jd_sem);
}

/**
 * nau8821_sema_reset - reset the semaphore for nau8821
 * @nau8821:  component to register the codec private data with
 *
 * Reset the counter of the semaphore. Call this function to restart
 * a new round task management.
 */
static inline void nau8821_sema_reset(struct nau8821 *nau8821)
{
	nau8821->jd_sem.count = 1;
}

static bool nau8821_readable_reg(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case NAU8821_REG_RESET ... NAU8821_REG_ENA_CTRL:
	case NAU8821_REG_CLK_DIVIDER ... NAU8821_REG_FLL8:
	case NAU8821_REG_JACK_DET_CTRL:
	case NAU8821_REG_INTERRUPT_MASK ... NAU8821_REG_DMIC_CTRL:
	case NAU8821_REG_GPIO12_CTRL ... NAU8821_REG_RIGHT_TIME_SLOT:
	case NAU8821_REG_BIQ0_COF1 ... NAU8821_REG_DAC_CTRL2:
	case NAU8821_REG_DAC_DGAIN_CTRL ... NAU8821_REG_HSVOL_CTRL:
	case NAU8821_REG_DACR_CTRL ... NAU8821_REG_DAC_DRC_ATKDCY:
	case NAU8821_REG_BIQ1_COF1 ... NAU8821_REG_FUSE_CTRL3:
	case NAU8821_REG_FUSE_CTRL1:
	case NAU8821_REG_OTPDOUT_1 ... NAU8821_REG_MISC_CTRL:
	case NAU8821_REG_I2C_DEVICE_ID ... NAU8821_REG_SOFTWARE_RST:
	case NAU8821_REG_BIAS_ADJ:
	case NAU8821_REG_TRIM_SETTINGS ... NAU8821_REG_PGA_MUTE:
	case NAU8821_REG_ANALOG_ADC_1 ... NAU8821_REG_MIC_BIAS:
	case NAU8821_REG_BOOST ... NAU8821_REG_FEPGA:
	case NAU8821_REG_PGA_GAIN ... NAU8821_REG_GENERAL_STATUS:
		return true;
	default:
		return false;
	}
}

static bool nau8821_writeable_reg(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case NAU8821_REG_RESET ... NAU8821_REG_ENA_CTRL:
	case NAU8821_REG_CLK_DIVIDER ... NAU8821_REG_FLL8:
	case NAU8821_REG_JACK_DET_CTRL:
	case NAU8821_REG_INTERRUPT_MASK:
	case NAU8821_REG_INT_CLR_KEY_STATUS ... NAU8821_REG_DMIC_CTRL:
	case NAU8821_REG_GPIO12_CTRL ... NAU8821_REG_RIGHT_TIME_SLOT:
	case NAU8821_REG_BIQ0_COF1 ... NAU8821_REG_DAC_CTRL2:
	case NAU8821_REG_DAC_DGAIN_CTRL ... NAU8821_REG_HSVOL_CTRL:
	case NAU8821_REG_DACR_CTRL ... NAU8821_REG_DAC_DRC_ATKDCY:
	case NAU8821_REG_BIQ1_COF1 ... NAU8821_REG_IMM_MODE_CTRL:
	case NAU8821_REG_FUSE_CTRL2 ... NAU8821_REG_FUSE_CTRL3:
	case NAU8821_REG_FUSE_CTRL1:
	case NAU8821_REG_MISC_CTRL:
	case NAU8821_REG_SOFTWARE_RST:
	case NAU8821_REG_BIAS_ADJ:
	case NAU8821_REG_TRIM_SETTINGS ... NAU8821_REG_PGA_MUTE:
	case NAU8821_REG_ANALOG_ADC_1 ... NAU8821_REG_MIC_BIAS:
	case NAU8821_REG_BOOST ... NAU8821_REG_FEPGA:
	case NAU8821_REG_PGA_GAIN ... NAU8821_REG_CHARGE_PUMP:
		return true;
	default:
		return false;
	}
}

static bool nau8821_volatile_reg(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case NAU8821_REG_RESET:
	case NAU8821_REG_IRQ_STATUS ... NAU8821_REG_INT_CLR_KEY_STATUS:
	case NAU8821_REG_BIQ0_COF1 ... NAU8821_REG_BIQ0_COF10:
	case NAU8821_REG_BIQ1_COF1 ... NAU8821_REG_BIQ1_COF10:
	case NAU8821_REG_IMM_RMS_L:
	case NAU8821_REG_OTPDOUT_1 ... NAU8821_REG_OTPDOUT_2:
	case NAU8821_REG_I2C_DEVICE_ID ... NAU8821_REG_SOFTWARE_RST:
	case NAU8821_REG_CHARGE_PUMP_INPUT_READ ... NAU8821_REG_GENERAL_STATUS:
		return true;
	default:
		return false;
	}
}

static int nau8821_biq_coeff_get(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component = snd_kcontrol_chip(kcontrol);
	struct soc_bytes_ext *params = (void *)kcontrol->private_value;

	if (!component->regmap)
		return -EINVAL;

	regmap_raw_read(component->regmap, NAU8821_REG_BIQ1_COF1,
		ucontrol->value.bytes.data, params->max);
	return 0;
}

static int nau8821_biq_coeff_put(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component = snd_kcontrol_chip(kcontrol);
	struct soc_bytes_ext *params = (void *)kcontrol->private_value;
	void *data;

	if (!component->regmap)
		return -EINVAL;

	data = kmemdup(ucontrol->value.bytes.data,
		params->max, GFP_KERNEL | GFP_DMA);
	if (!data)
		return -ENOMEM;

	//regmap_update_bits(component->regmap, NAU8821_REG_BIQ_CTRL,
	//	NAU8821_BIQ_WRT_EN, 0);
	regmap_raw_write(component->regmap, NAU8821_REG_BIQ1_COF1,
		data, params->max);
	//regmap_update_bits(component->regmap, NAU8821_REG_BIQ_CTRL,
	//	NAU8821_BIQ_WRT_EN, NAU8821_BIQ_WRT_EN);

	kfree(data);
	return 0;
}

/*static const char * const nau8821_biq_path[] = {
	"ADC", "DAC"
};

static const struct soc_enum nau8821_biq_path_enum =
	SOC_ENUM_SINGLE(NAU8821_REG_BIQ_CTRL, NAU8821_BIQ_PATH_SFT,
		ARRAY_SIZE(nau8821_biq_path), nau8821_biq_path);*/

static const char * const nau8821_adc_decimation[] = {
	"32", "64", "128", "256" };

static const struct soc_enum nau8821_adc_decimation_enum =
	SOC_ENUM_SINGLE(NAU8821_REG_ADC_RATE, NAU8821_ADC_SYNC_DOWN_SFT,
		ARRAY_SIZE(nau8821_adc_decimation), nau8821_adc_decimation);

static const char * const nau8821_dac_oversampl[] = {
	"64", "256", "128", "", "32" };

static const struct soc_enum nau8821_dac_oversampl_enum =
	SOC_ENUM_SINGLE(NAU8821_REG_DAC_CTRL1, NAU8821_DAC_OVERSAMPLE_SFT,
		ARRAY_SIZE(nau8821_dac_oversampl), nau8821_dac_oversampl);

static const DECLARE_TLV_DB_MINMAX_MUTE(adc_vol_tlv, -6600, 2400);
static const DECLARE_TLV_DB_MINMAX_MUTE(sidetone_vol_tlv, -4200, 0);
static const DECLARE_TLV_DB_MINMAX(hp_vol_tlv, -900, 0);
static const DECLARE_TLV_DB_SCALE(playback_vol_tlv, -6600, 50, 1);
static const DECLARE_TLV_DB_MINMAX(fepga_gain_tlv, -100, 3600);
static const DECLARE_TLV_DB_MINMAX_MUTE(crosstalk_vol_tlv, -9600, 2400);

static const struct snd_kcontrol_new nau8821_controls[] = {
	SOC_DOUBLE_TLV("Mic Volume", NAU8821_REG_ADC_DGAIN_CTRL1,
		NAU8821_ADCL_CH_VOL_SFT, NAU8821_ADCR_CH_VOL_SFT,
		0xff, 0, adc_vol_tlv),
	SOC_DOUBLE_TLV("Headphone Bypass Volume", NAU8821_REG_ADC_DGAIN_CTRL,
		12, 8, 0x0f, 0, sidetone_vol_tlv),
	SOC_DOUBLE_TLV("Headphone Volume", NAU8821_REG_HSVOL_CTRL,
		NAU8821_HPL_VOL_SFT, NAU8821_HPR_VOL_SFT, 0x3, 1, hp_vol_tlv),
	SOC_DOUBLE_TLV("Digital Playback Volume", NAU8821_REG_DACR_CTRL,
		NAU8821_DACL_CH_VOL_SFT, NAU8821_DACR_CH_VOL_SFT,
		0xcf, 0, playback_vol_tlv),
	SOC_DOUBLE_TLV("Frontend PGA Volume", NAU8821_REG_PGA_GAIN,
		NAU8821_PGA_GAIN_L_SFT, NAU8821_PGA_GAIN_R_SFT,
		37, 0, fepga_gain_tlv),
	SOC_DOUBLE_TLV("Headphone Crosstalk Volume", NAU8821_REG_DAC_DGAIN_CTRL,
		0, 8, 0xff, 0, crosstalk_vol_tlv),

	SOC_ENUM("ADC Decimation Rate", nau8821_adc_decimation_enum),
	SOC_ENUM("DAC Oversampling Rate", nau8821_dac_oversampl_enum),
	/* programmable biquad filter */
	//SOC_ENUM("BIQ Path Select", nau8821_biq_path_enum),
	SND_SOC_BYTES_EXT("BIQ Coefficients", 20,
		  nau8821_biq_coeff_get, nau8821_biq_coeff_put),
};

static int nau8821_left_adc_event(struct snd_soc_dapm_widget *w,
	struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_component *component = snd_soc_dapm_to_component(w->dapm);
	struct nau8821 *nau8821 = snd_soc_component_get_drvdata(component);

	switch (event) {
	case SND_SOC_DAPM_POST_PMU:
		msleep(125);
		regmap_update_bits(nau8821->regmap, NAU8821_REG_ENA_CTRL,
			NAU8821_EN_ADCL, NAU8821_EN_ADCL);
		break;
	case SND_SOC_DAPM_POST_PMD:
		if (!nau8821->irq)
			regmap_update_bits(nau8821->regmap,
				NAU8821_REG_ENA_CTRL, NAU8821_EN_ADCL, 0);
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int nau8821_right_adc_event(struct snd_soc_dapm_widget *w,
	struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_component *component = snd_soc_dapm_to_component(w->dapm);
	struct nau8821 *nau8821 = snd_soc_component_get_drvdata(component);

	switch (event) {
	case SND_SOC_DAPM_POST_PMU:
		msleep(125);
		regmap_update_bits(nau8821->regmap, NAU8821_REG_ENA_CTRL,
			NAU8821_EN_ADCR, NAU8821_EN_ADCR);
		break;
	case SND_SOC_DAPM_POST_PMD:
		if (!nau8821->irq)
			regmap_update_bits(nau8821->regmap,
				NAU8821_REG_ENA_CTRL, NAU8821_EN_ADCR, 0);
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int nau8821_pump_event(struct snd_soc_dapm_widget *w,
	struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_component *component = snd_soc_dapm_to_component(w->dapm);
	struct nau8821 *nau8821 = snd_soc_component_get_drvdata(component);

	switch (event) {
	case SND_SOC_DAPM_POST_PMU:
		/* Prevent startup click by letting charge pump to ramp up */
		msleep(10);
		regmap_update_bits(nau8821->regmap, NAU8821_REG_CHARGE_PUMP,
			NAU8821_JAMNODCLOW, NAU8821_JAMNODCLOW);
		break;
	case SND_SOC_DAPM_PRE_PMD:
		regmap_update_bits(nau8821->regmap, NAU8821_REG_CHARGE_PUMP,
			NAU8821_JAMNODCLOW, 0);
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int nau8821_output_dac_event(struct snd_soc_dapm_widget *w,
	struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_component *component = snd_soc_dapm_to_component(w->dapm);
	struct nau8821 *nau8821 = snd_soc_component_get_drvdata(component);

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		/* Disables the TESTDAC to let DAC signal pass through. */
		regmap_update_bits(nau8821->regmap, NAU8821_REG_BIAS_ADJ,
			NAU8821_BIAS_TESTDAC_EN, 0);
		break;
	case SND_SOC_DAPM_POST_PMD:
		regmap_update_bits(nau8821->regmap, NAU8821_REG_BIAS_ADJ,
			NAU8821_BIAS_TESTDAC_EN, NAU8821_BIAS_TESTDAC_EN);
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static const struct snd_soc_dapm_widget nau8821_dapm_widgets[] = {
	SND_SOC_DAPM_INPUT("MIC"),
	SND_SOC_DAPM_MICBIAS("MICBIAS", NAU8821_REG_MIC_BIAS,
		NAU8821_MICBIAS_POWERUP_SFT, 0),

	SND_SOC_DAPM_PGA("Frontend PGA L", NAU8821_REG_POWER_UP_CONTROL,
		NAU8821_PUP_PGA_L_SFT, 0, NULL, 0),
	SND_SOC_DAPM_PGA("Frontend PGA R", NAU8821_REG_POWER_UP_CONTROL,
		NAU8821_PUP_PGA_R_SFT, 0, NULL, 0),

	SND_SOC_DAPM_SUPPLY("ADCL Power", NAU8821_REG_ANALOG_ADC_2,
		NAU8821_POWERUP_ADCL_SFT, 0, NULL, 0),
	SND_SOC_DAPM_SUPPLY("ADCR Power", NAU8821_REG_ANALOG_ADC_2,
		NAU8821_POWERUP_ADCR_SFT, 0, NULL, 0),
	SND_SOC_DAPM_ADC_E("ADCL", NULL, SND_SOC_NOPM, 0, 0,
		nau8821_left_adc_event, SND_SOC_DAPM_POST_PMU |
		SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_ADC_E("ADCR", NULL, SND_SOC_NOPM, 0, 0,
		nau8821_right_adc_event, SND_SOC_DAPM_POST_PMU |
		SND_SOC_DAPM_POST_PMD),

	SND_SOC_DAPM_AIF_OUT("AIFTX", "Capture", 0, NAU8821_REG_I2S_PCM_CTRL2,
		NAU8821_I2S_TRISTATE_SFT, 1),

	SND_SOC_DAPM_AIF_IN("AIFRX", "Playback", 0, SND_SOC_NOPM, 0, 0),

	SND_SOC_DAPM_PGA_S("ADACL", 2, NAU8821_REG_RDAC,
		NAU8821_DACL_EN_SFT, 0, NULL, 0),
	SND_SOC_DAPM_PGA_S("ADACR", 2, NAU8821_REG_RDAC,
		NAU8821_DACR_EN_SFT, 0, NULL, 0),
	SND_SOC_DAPM_PGA_S("ADACL Clock", 3, NAU8821_REG_RDAC,
		NAU8821_DACL_CLK_EN_SFT, 0, NULL, 0),
	SND_SOC_DAPM_PGA_S("ADACR Clock", 3, NAU8821_REG_RDAC,
		NAU8821_DACR_CLK_EN_SFT, 0, NULL, 0),
	SND_SOC_DAPM_DAC("DDACR", NULL, NAU8821_REG_ENA_CTRL,
		NAU8821_EN_DACR_SFT, 0),
	SND_SOC_DAPM_DAC("DDACL", NULL, NAU8821_REG_ENA_CTRL,
		NAU8821_EN_DACL_SFT, 0),
	SND_SOC_DAPM_PGA_S("HP amp L", 0, NAU8821_REG_CLASSG_CTRL,
		NAU8821_CLASSG_LDAC_EN_SFT, 0, NULL, 0),
	SND_SOC_DAPM_PGA_S("HP amp R", 0, NAU8821_REG_CLASSG_CTRL,
		NAU8821_CLASSG_RDAC_EN_SFT, 0, NULL, 0),

	SND_SOC_DAPM_PGA_S("Charge Pump", 1, NAU8821_REG_CHARGE_PUMP,
		NAU8821_CHANRGE_PUMP_EN_SFT, 0, nau8821_pump_event,
		SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_PRE_PMD),

	SND_SOC_DAPM_PGA_S("Output Driver R Stage 1", 4,
		NAU8821_REG_POWER_UP_CONTROL,
		NAU8821_PUP_INTEG_R_SFT, 0, NULL, 0),
	SND_SOC_DAPM_PGA_S("Output Driver L Stage 1", 4,
		NAU8821_REG_POWER_UP_CONTROL,
		NAU8821_PUP_INTEG_L_SFT, 0, NULL, 0),
	SND_SOC_DAPM_PGA_S("Output Driver R Stage 2", 5,
		NAU8821_REG_POWER_UP_CONTROL,
		NAU8821_PUP_DRV_INSTG_R_SFT, 0, NULL, 0),
	SND_SOC_DAPM_PGA_S("Output Driver L Stage 2", 5,
		NAU8821_REG_POWER_UP_CONTROL,
		NAU8821_PUP_DRV_INSTG_L_SFT, 0, NULL, 0),
	SND_SOC_DAPM_PGA_S("Output Driver R Stage 3", 6,
		NAU8821_REG_POWER_UP_CONTROL,
		NAU8821_PUP_MAIN_DRV_R_SFT, 0, NULL, 0),
	SND_SOC_DAPM_PGA_S("Output Driver L Stage 3", 6,
		NAU8821_REG_POWER_UP_CONTROL,
		NAU8821_PUP_MAIN_DRV_L_SFT, 0, NULL, 0),

	SND_SOC_DAPM_PGA_S("Output DACL", 7,
		NAU8821_REG_CHARGE_PUMP, NAU8821_POWER_DOWN_DACL_SFT,
		0, nau8821_output_dac_event,
		SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_PGA_S("Output DACR", 7,
		NAU8821_REG_CHARGE_PUMP, NAU8821_POWER_DOWN_DACR_SFT,
		0, nau8821_output_dac_event,
		SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMD),

	/* HPOL/R are ungrounded by disabling 16 Ohm pull-downs on playback */
	SND_SOC_DAPM_PGA_S("HPOL Pulldown", 8,
		NAU8821_REG_JACK_DET_CTRL,
		NAU8821_SPKR_DWN1L_SFT, 0, NULL, 0),
	SND_SOC_DAPM_PGA_S("HPOR Pulldown", 8,
		NAU8821_REG_JACK_DET_CTRL,
		NAU8821_SPKR_DWN1R_SFT, 0, NULL, 0),

	/* High current HPOL/R boost driver */
	SND_SOC_DAPM_PGA_S("HP Boost Driver", 9,
		NAU8821_REG_BOOST, NAU8821_HP_BOOST_DIS_SFT, 1, NULL, 0),

	SND_SOC_DAPM_PGA("Class G", NAU8821_REG_CLASSG_CTRL,
		NAU8821_CLASSG_EN_SFT, 0, NULL, 0),

	SND_SOC_DAPM_OUTPUT("HPOL"),
	SND_SOC_DAPM_OUTPUT("HPOR"),
};

static const struct snd_soc_dapm_route nau8821_dapm_routes[] = {
	{"Frontend PGA L", NULL, "MIC"},
	{"Frontend PGA R", NULL, "MIC"},
	{"ADCL", NULL, "Frontend PGA L"},
	{"ADCR", NULL, "Frontend PGA R"},
	{"ADCL", NULL, "ADCL Power"},
	{"ADCR", NULL, "ADCR Power"},
	{"AIFTX", NULL, "ADCL"},
	{"AIFTX", NULL, "ADCR"},

	{"DDACL", NULL, "AIFRX"},
	{"DDACR", NULL, "AIFRX"},

	{"HP amp L", NULL, "DDACL"},
	{"HP amp R", NULL, "DDACR"},
	{"Charge Pump", NULL, "HP amp L"},
	{"Charge Pump", NULL, "HP amp R"},
	{"ADACL", NULL, "Charge Pump"},
	{"ADACR", NULL, "Charge Pump"},
	{"ADACL Clock", NULL, "ADACL"},
	{"ADACR Clock", NULL, "ADACR"},
	{"Output Driver L Stage 1", NULL, "ADACL Clock"},
	{"Output Driver R Stage 1", NULL, "ADACR Clock"},
	{"Output Driver L Stage 2", NULL, "Output Driver L Stage 1"},
	{"Output Driver R Stage 2", NULL, "Output Driver R Stage 1"},
	{"Output Driver L Stage 3", NULL, "Output Driver L Stage 2"},
	{"Output Driver R Stage 3", NULL, "Output Driver R Stage 2"},
	{"Output DACL", NULL, "Output Driver L Stage 3"},
	{"Output DACR", NULL, "Output Driver R Stage 3"},
	{"HPOL Pulldown", NULL, "Output DACL"},
	{"HPOR Pulldown", NULL, "Output DACR"},
	{"HP Boost Driver", NULL, "HPOL Pulldown"},
	{"HP Boost Driver", NULL, "HPOR Pulldown"},
	{"Class G", NULL, "HP Boost Driver"},
	{"HPOL", NULL, "Class G"},
	{"HPOR", NULL, "Class G"},
};

static int nau8821_clock_check(struct nau8821 *nau8821,
	int stream, int rate, int osr)
{
	int osrate = 0;

	if (stream == SNDRV_PCM_STREAM_PLAYBACK) {
		if (osr >= ARRAY_SIZE(osr_dac_sel))
			return -EINVAL;
		osrate = osr_dac_sel[osr].osr;
	} else {
		if (osr >= ARRAY_SIZE(osr_adc_sel))
			return -EINVAL;
		osrate = osr_adc_sel[osr].osr;
	}

	if (!osrate || rate * osrate > CLK_DA_AD_MAX) {
		dev_err(nau8821->dev, "exceed the maximum frequency of CLK_ADC or CLK_DAC\n");
		return -EINVAL;
	}

	return 0;
}

static int nau8821_hw_params(struct snd_pcm_substream *substream,
	struct snd_pcm_hw_params *params, struct snd_soc_dai *dai)
{
	struct snd_soc_component *component = dai->component;
	struct nau8821 *nau8821 = snd_soc_component_get_drvdata(component);
	unsigned int val_len = 0, osr, ctrl_val, bclk_fs, bclk_div;

	nau8821_sema_acquire(nau8821, HZ);

	/* CLK_DAC or CLK_ADC = OSR * FS
	 * DAC or ADC clock frequency is defined as Over Sampling Rate (OSR)
	 * multiplied by the audio sample rate (Fs). Note that the OSR and Fs
	 * values must be selected such that the maximum frequency is less
	 * than 6.144 MHz.
	 */
	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		regmap_read(nau8821->regmap, NAU8821_REG_DAC_CTRL1, &osr);
		osr &= NAU8821_DAC_OVERSAMPLE_MASK;
		if (nau8821_clock_check(nau8821, substream->stream,
			params_rate(params), osr)) {
			nau8821_sema_release(nau8821);
			return -EINVAL;
		}
		regmap_update_bits(nau8821->regmap, NAU8821_REG_CLK_DIVIDER,
			NAU8821_CLK_DAC_SRC_MASK,
			osr_dac_sel[osr].clk_src << NAU8821_CLK_DAC_SRC_SFT);
	} else {
		regmap_read(nau8821->regmap, NAU8821_REG_ADC_RATE, &osr);
		osr &= NAU8821_ADC_SYNC_DOWN_MASK;
		if (nau8821_clock_check(nau8821, substream->stream,
			params_rate(params), osr)) {
			nau8821_sema_release(nau8821);
			return -EINVAL;
		}
		regmap_update_bits(nau8821->regmap, NAU8821_REG_CLK_DIVIDER,
			NAU8821_CLK_ADC_SRC_MASK,
			osr_adc_sel[osr].clk_src << NAU8821_CLK_ADC_SRC_SFT);
	}

	/* make BCLK and LRC divde configuration if the codec as master. */
	regmap_read(nau8821->regmap, NAU8821_REG_I2S_PCM_CTRL2, &ctrl_val);
	if (ctrl_val & NAU8821_I2S_MS_MASTER) {
		/* get the bclk and fs ratio */
		bclk_fs = snd_soc_params_to_bclk(params) / params_rate(params);
		if (bclk_fs <= 32)
			bclk_div = 2;
		else if (bclk_fs <= 64)
			bclk_div = 1;
		else if (bclk_fs <= 128)
			bclk_div = 0;
		else {
			nau8821_sema_release(nau8821);
			return -EINVAL;
		}
		regmap_update_bits(nau8821->regmap, NAU8821_REG_I2S_PCM_CTRL2,
			NAU8821_I2S_LRC_DIV_MASK | NAU8821_I2S_BLK_DIV_MASK,
			((bclk_div + 1) << NAU8821_I2S_LRC_DIV_SFT) | bclk_div);
	}

	switch (params_width(params)) {
	case 16:
		val_len |= NAU8821_I2S_DL_16;
		break;
	case 20:
		val_len |= NAU8821_I2S_DL_20;
		break;
	case 24:
		val_len |= NAU8821_I2S_DL_24;
		break;
	case 32:
		val_len |= NAU8821_I2S_DL_32;
		break;
	default:
		nau8821_sema_release(nau8821);
		return -EINVAL;
	}

	regmap_update_bits(nau8821->regmap, NAU8821_REG_I2S_PCM_CTRL1,
		NAU8821_I2S_DL_MASK, val_len);

	nau8821_sema_release(nau8821);

	return 0;
}

static int nau8821_set_dai_fmt(struct snd_soc_dai *codec_dai, unsigned int fmt)
{
	struct snd_soc_component *component = codec_dai->component;
	struct nau8821 *nau8821 = snd_soc_component_get_drvdata(component);
	unsigned int ctrl1_val = 0, ctrl2_val = 0;

	switch (fmt & SND_SOC_DAIFMT_MASTER_MASK) {
	case SND_SOC_DAIFMT_CBM_CFM:
		ctrl2_val |= NAU8821_I2S_MS_MASTER;
		break;
	case SND_SOC_DAIFMT_CBS_CFS:
		break;
	default:
		return -EINVAL;
	}

	switch (fmt & SND_SOC_DAIFMT_INV_MASK) {
	case SND_SOC_DAIFMT_NB_NF:
		break;
	case SND_SOC_DAIFMT_IB_NF:
		ctrl1_val |= NAU8821_I2S_BP_INV;
		break;
	default:
		return -EINVAL;
	}

	switch (fmt & SND_SOC_DAIFMT_FORMAT_MASK) {
	case SND_SOC_DAIFMT_I2S:
		ctrl1_val |= NAU8821_I2S_DF_I2S;
		break;
	case SND_SOC_DAIFMT_LEFT_J:
		ctrl1_val |= NAU8821_I2S_DF_LEFT;
		break;
	case SND_SOC_DAIFMT_RIGHT_J:
		ctrl1_val |= NAU8821_I2S_DF_RIGTH;
		break;
	case SND_SOC_DAIFMT_DSP_A:
		ctrl1_val |= NAU8821_I2S_DF_PCM_AB;
		break;
	case SND_SOC_DAIFMT_DSP_B:
		ctrl1_val |= NAU8821_I2S_DF_PCM_AB;
		ctrl1_val |= NAU8821_I2S_PCMB_EN;
		break;
	default:
		return -EINVAL;
	}

	nau8821_sema_acquire(nau8821, HZ);

	regmap_update_bits(nau8821->regmap, NAU8821_REG_I2S_PCM_CTRL1,
		NAU8821_I2S_DL_MASK | NAU8821_I2S_DF_MASK |
		NAU8821_I2S_BP_MASK | NAU8821_I2S_PCMB_MASK, ctrl1_val);
	regmap_update_bits(nau8821->regmap, NAU8821_REG_I2S_PCM_CTRL2,
		NAU8821_I2S_MS_MASK, ctrl2_val);

	nau8821_sema_release(nau8821);

	return 0;
}

static int nau8821_digital_mute(struct snd_soc_dai *dai, int mute, int direction)
{
	struct snd_soc_component *component = dai->component;
	struct nau8821 *nau8821 = snd_soc_component_get_drvdata(component);
	unsigned int val;

	val = mute ? NAU8821_DAC_SOFT_MUTE : 0;

	return regmap_update_bits(nau8821->regmap,
		NAU8821_REG_MUTE_CTRL, NAU8821_DAC_SOFT_MUTE, val);
}

static const struct snd_soc_dai_ops nau8821_dai_ops = {
	.hw_params = nau8821_hw_params,
	.set_fmt = nau8821_set_dai_fmt,
	.mute_stream = nau8821_digital_mute,
};

#define NAU8821_RATES SNDRV_PCM_RATE_8000_192000
#define NAU8821_FORMATS (SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S20_3LE \
	| SNDRV_PCM_FMTBIT_S24_3LE | SNDRV_PCM_FMTBIT_S32_LE)

static struct snd_soc_dai_driver nau8821_dai = {
	.name = NUVOTON_CODEC_DAI,
	.playback = {
		.stream_name = "Playback",
		.channels_min = 1,
		.channels_max = 2,
		.rates = NAU8821_RATES,
		.formats = NAU8821_FORMATS,
	},
	.capture = {
		.stream_name = "Capture",
		.channels_min = 1,
		.channels_max = 2,
		.rates = NAU8821_RATES,
		.formats = NAU8821_FORMATS,
	},
	.ops = &nau8821_dai_ops,
};


static bool nau8821_is_jack_inserted(struct regmap *regmap)
{
	bool active_high, is_high;
	int status, jkdet;

	regmap_read(regmap, NAU8821_REG_JACK_DET_CTRL, &jkdet);
	active_high = jkdet & NAU8821_JACK_POLARITY;
	regmap_read(regmap, NAU8821_REG_GENERAL_STATUS, &status);
	is_high = status & NAU8821_GPIO2_IN;
	/* return jack connection status according to jack insertion logic
	 * active high or active low.
	 */
	return active_high == is_high;
}

static void nau8821_restart_jack_detection(struct regmap *regmap)
{
	/* this will restart the entire jack detection process including MIC/GND
	 * switching and create interrupts. We have to go from 0 to 1 and back
	 * to 0 to restart.
	 */
	regmap_update_bits(regmap, NAU8821_REG_JACK_DET_CTRL,
		NAU8821_JACK_DET_RESTART, NAU8821_JACK_DET_RESTART);
	regmap_update_bits(regmap, NAU8821_REG_JACK_DET_CTRL,
		NAU8821_JACK_DET_RESTART, 0);
}

static void nau8821_int_status_clear_all(struct regmap *regmap)
{
	int active_irq, clear_irq, i;

	/* Reset the intrruption status from rightmost bit if the corres-
	 * ponding irq event occurs.
	 */
	regmap_read(regmap, NAU8821_REG_IRQ_STATUS, &active_irq);
	for (i = 0; i < NAU8821_REG_DATA_LEN; i++) {
		clear_irq = (0x1 << i);
		if (active_irq & clear_irq)
			regmap_write(regmap,
				NAU8821_REG_INT_CLR_KEY_STATUS, clear_irq);
	}
}

static void nau8821_eject_jack(struct nau8821 *nau8821)
{
	struct snd_soc_dapm_context *dapm = nau8821->dapm;
	struct regmap *regmap = nau8821->regmap;

	/* Reset semaphore */
	nau8821_sema_reset(nau8821);

	/* Detach 2kOhm Resistors from MICBIAS to MICGND */
	regmap_update_bits(regmap, NAU8821_REG_MIC_BIAS,
		NAU8821_MICBIAS_JKR2, 0);
	/* HPL/HPR short to ground */
	regmap_update_bits(regmap, NAU8821_REG_JACK_DET_CTRL,
		NAU8821_SPKR_DWN1R | NAU8821_SPKR_DWN1L, 0);
	//snd_soc_dapm_disable_pin(dapm, "MICBIAS");
	snd_soc_dapm_sync(dapm);

	/* Clear all interruption status */
	nau8821_int_status_clear_all(regmap);

	/* Enable the insertion interruption, disable the ejection inter-
	 * ruption, and then bypass de-bounce circuit.
	 */
	regmap_update_bits(regmap, NAU8821_REG_INTERRUPT_DIS_CTRL,
		NAU8821_IRQ_EJECT_DIS | NAU8821_IRQ_INSERT_DIS,
		NAU8821_IRQ_EJECT_DIS);
	/* Mask unneeded IRQs: 1 - disable, 0 - enable */
	regmap_update_bits(regmap, NAU8821_REG_INTERRUPT_MASK,
		NAU8821_IRQ_EJECT_EN | NAU8821_IRQ_INSERT_EN,
		NAU8821_IRQ_EJECT_EN);
	regmap_update_bits(regmap, NAU8821_REG_JACK_DET_CTRL,
		NAU8821_JACK_DET_DB_BYPASS, NAU8821_JACK_DET_DB_BYPASS);

	/* Disable ADC needed for interruptions at audo mode */
	regmap_update_bits(regmap, NAU8821_REG_ENA_CTRL,
		NAU8821_EN_ADCR | NAU8821_EN_ADCL, 0);

	/* Close clock for jack type detection at manual mode */
	nau8821_configure_sysclk(nau8821, NAU8821_CLK_DIS, 0);
}

/* Enable audo mode interruptions with internal clock. */
static void nau8821_setup_auto_irq(struct nau8821 *nau8821)
{
	struct regmap *regmap = nau8821->regmap;

	/* Enable internal VCO needed for interruptions */
	nau8821_configure_sysclk(nau8821, NAU8821_CLK_INTERNAL, 0);
	/* Enable ADC needed for interruptions */
	regmap_update_bits(regmap, NAU8821_REG_ENA_CTRL,
		NAU8821_EN_ADCR | NAU8821_EN_ADCL,
		NAU8821_EN_ADCR | NAU8821_EN_ADCL);
	/* Chip needs one FSCLK cycle in order to generate interruptions,
	 * as we cannot guarantee one will be provided by the system. Turning
	 * master mode on then off enables us to generate that FSCLK cycle
	 * with a minimum of contention on the clock bus.
	 */
	regmap_update_bits(regmap, NAU8821_REG_I2S_PCM_CTRL2,
		NAU8821_I2S_MS_MASK, NAU8821_I2S_MS_MASTER);
	regmap_update_bits(regmap, NAU8821_REG_I2S_PCM_CTRL2,
		NAU8821_I2S_MS_MASK, NAU8821_I2S_MS_SLAVE);

	/* Not bypass de-bounce circuit */
	regmap_update_bits(regmap, NAU8821_REG_JACK_DET_CTRL,
		NAU8821_JACK_DET_DB_BYPASS, 0);

	/* Unmask detection interruptions */
	regmap_update_bits(regmap, NAU8821_REG_INTERRUPT_MASK,
		NAU8821_IRQ_EJECT_EN | NAU8821_IRQ_MIC_DET_EN |
		NAU8821_IRQ_KEY_RELEASE_EN | NAU8821_IRQ_KEY_PRESS_EN, 0);
	/* Enable detection interruptions */
	regmap_update_bits(regmap, NAU8821_REG_INTERRUPT_DIS_CTRL,
		NAU8821_IRQ_EJECT_DIS | NAU8821_IRQ_MIC_DIS |
		NAU8821_IRQ_KEY_RELEASE_DIS | NAU8821_IRQ_KEY_PRESS_DIS, 0);

	/* Restart the jack detection process at auto mode */
	nau8821_restart_jack_detection(regmap);
}

static int nau8821_jack_insert(struct nau8821 *nau8821)
{
	struct snd_soc_dapm_context *dapm = nau8821->dapm;
	struct regmap *regmap = nau8821->regmap;
	int jack_status_reg, mic_detected;
	int type = 0;

	regmap_read(regmap, NAU8821_REG_I2C_DEVICE_ID, &jack_status_reg);
	mic_detected = jack_status_reg & NAU8821_MICDET;

	if (mic_detected) {
		dev_dbg(nau8821->dev, "OMTP (micgnd1) mic connected\n");
		type = SND_JACK_HEADSET;

		/* Attach 2kOhm Resistor from MICBIAS to MICGND1 */
		regmap_update_bits(regmap, NAU8821_REG_MIC_BIAS,
			NAU8821_MICBIAS_JKR2, NAU8821_MICBIAS_JKR2);
		snd_soc_dapm_force_enable_pin(dapm, "MICBIAS");
		snd_soc_dapm_sync(dapm);
	} else {
		type = SND_JACK_HEADPHONE;
	}

	return type;
}

#define NAU8821_BUTTON SND_JACK_BTN_0

static irqreturn_t nau8821_interrupt(int irq, void *data)
{
	struct nau8821 *nau8821 = (struct nau8821 *)data;
	struct regmap *regmap = nau8821->regmap;
	int active_irq, clear_irq = 0, event = 0, event_mask = 0;

	if (regmap_read(regmap, NAU8821_REG_IRQ_STATUS, &active_irq)) {
		dev_err(nau8821->dev, "failed to read irq status\n");
		return IRQ_NONE;
	}
	dev_dbg(nau8821->dev, "IRQ 0x%x\n", active_irq);

	if ((active_irq & NAU8821_JACK_EJECT_IRQ_MASK) == 
		NAU8821_JACK_EJECT_DETECTED) {
		nau8821_eject_jack(nau8821);
		event_mask |= SND_JACK_HEADSET;
		clear_irq = NAU8821_JACK_EJECT_IRQ_MASK;
	} else if (active_irq & NAU8821_KEY_SHORT_PRESS_IRQ) {
		event |= NAU8821_BUTTON;
		event_mask |= NAU8821_BUTTON;
		clear_irq = NAU8821_KEY_SHORT_PRESS_IRQ;
	} else if (active_irq & NAU8821_KEY_RELEASE_IRQ) {
		event_mask = NAU8821_BUTTON;
		clear_irq = NAU8821_KEY_RELEASE_IRQ;
	} else if ((active_irq & NAU8821_JACK_INSERT_IRQ_MASK) ==
		NAU8821_JACK_INSERT_DETECTED) {
		/* One more step to check GPIO status directly. Thus, the
		 * driver can confirm the real insertion interruption because
		 * the intrruption at manual mode has bypassed debounce
		 * circuit which can get rid of unstable status.
		 */
		if (nau8821_is_jack_inserted(regmap)) {
			if (nau8821->clk_id == NAU8821_CLK_DIS) {
				/* Turn off insertion interruption at manual mode */
				regmap_update_bits(regmap,
					NAU8821_REG_INTERRUPT_DIS_CTRL,
					NAU8821_IRQ_INSERT_DIS,
					NAU8821_IRQ_INSERT_DIS);
				regmap_update_bits(regmap,
					NAU8821_REG_INTERRUPT_MASK,
					NAU8821_IRQ_INSERT_EN,
					NAU8821_IRQ_INSERT_EN);
				/* Enable interruption for jack type detection
				 * which can detect microphone and jack type.
				 */
				nau8821_setup_auto_irq(nau8821);
			} else {
				event |= nau8821_jack_insert(nau8821);
				event_mask |= SND_JACK_HEADSET;
				nau8821_sema_release(nau8821);
			}
		} else {
			dev_warn(nau8821->dev, "Headset completion IRQ fired but no headset connected\n");
			nau8821_eject_jack(nau8821);
		}
	}

	if (!clear_irq)
		clear_irq = active_irq;
	/* clears the rightmost interruption */
	regmap_write(regmap, NAU8821_REG_INT_CLR_KEY_STATUS, clear_irq);

	if (event_mask)
		snd_soc_jack_report(nau8821->jack, event, event_mask);

	return IRQ_HANDLED;
}

#ifdef DEBUG
static int nau8821_reg_write(void *context, unsigned int reg,
			      unsigned int value)
{
	struct i2c_client *client = context;
	u8 buf[4];
	int ret;

	buf[0] = (reg >> 8) & 0xff;
	buf[1] = reg & 0xff;
	buf[2] = (value >> 8) & 0xff;
	buf[3] = value & 0xff;

	ret = i2c_master_send(client, buf, sizeof(buf));
	if (ret == sizeof(buf)) {
		dev_info(&client->dev, "%x <= %x\n", reg, value);
		return 0;
	} else if (ret < 0)
		return ret;
	else
		return -EIO;
}

static int nau8821_reg_read(void *context, unsigned int reg,
			     unsigned int *value)
{
	struct i2c_client *client = context;
	struct i2c_msg xfer[2];
	u16 reg_buf, val_buf;
	int ret;

	reg_buf = cpu_to_be16(reg);
	xfer[0].addr = client->addr;
	xfer[0].len = sizeof(reg_buf);
	xfer[0].buf = (u8 *)&reg_buf;
	xfer[0].flags = 0;

	xfer[1].addr = client->addr;
	xfer[1].len = sizeof(val_buf);
	xfer[1].buf = (u8 *)&val_buf;
	xfer[1].flags = I2C_M_RD;

	ret = i2c_transfer(client->adapter, xfer, ARRAY_SIZE(xfer));
	
	if (ret < 0){
		dev_info(&client->dev, "%x <= %x\n", reg, value);
		return ret;
	}else if (ret != ARRAY_SIZE(xfer))
		return -EIO;

	*value = be16_to_cpu(val_buf);

	return 0;
}
#endif

static const struct regmap_config nau8821_regmap_config = {
	.val_bits = NAU8821_REG_DATA_LEN,
	.reg_bits = NAU8821_REG_ADDR_LEN,

	.max_register = NAU8821_REG_MAX,
	.readable_reg = nau8821_readable_reg,
	.writeable_reg = nau8821_writeable_reg,
	.volatile_reg = nau8821_volatile_reg,
#ifdef DEBUG
	.reg_read = nau8821_reg_read,
	.reg_write = nau8821_reg_write,
#endif

	.cache_type = REGCACHE_RBTREE,
	.reg_defaults = nau8821_reg_defaults,
	.num_reg_defaults = ARRAY_SIZE(nau8821_reg_defaults),
};

static int nau8821_component_probe(struct snd_soc_component *component)
{
	struct nau8821 *nau8821 = snd_soc_component_get_drvdata(component);
	struct snd_soc_dapm_context *dapm = snd_soc_component_get_dapm(component);

	nau8821->dapm = dapm;
	//Enable mic bias temprarily when no jack detection running.
	snd_soc_dapm_force_enable_pin(nau8821->dapm, "MICBIAS");
	snd_soc_dapm_sync(nau8821->dapm);
	return 0;
}

static void nau8821_codec_remove(struct snd_soc_component *component)
{
	struct nau8821 *nau8821 = snd_soc_component_get_drvdata(component);

	if (nau8821->irq)
		/* Reset semaphore */
		nau8821_sema_reset(nau8821);

}

/**
 * nau8821_calc_fll_param - Calculate FLL parameters.
 * @fll_in: external clock provided to codec.
 * @fs: sampling rate.
 * @fll_param: Pointer to structure of FLL parameters.
 *
 * Calculate FLL parameters to configure codec.
 *
 * Returns 0 for success or negative error code.
 */
static int nau8821_calc_fll_param(unsigned int fll_in,
	unsigned int fs, struct nau8821_fll *fll_param)
{
	u64 fvco, fvco_max;
	unsigned int fref, i, fvco_sel;

	/* Ensure the reference clock frequency (FREF) is <= 13.5MHz by dividing
	 * freq_in by 1, 2, 4, or 8 using FLL pre-scalar.
	 * FREF = freq_in / NAU8821_FLL_REF_DIV_MASK
	 */
	for (i = 0; i < ARRAY_SIZE(fll_pre_scalar); i++) {
		fref = fll_in / fll_pre_scalar[i].param;
		if (fref <= NAU_FREF_MAX)
			break;
	}
	if (i == ARRAY_SIZE(fll_pre_scalar))
		return -EINVAL;
	fll_param->clk_ref_div = fll_pre_scalar[i].val;

	/* Choose the FLL ratio based on FREF */
	for (i = 0; i < ARRAY_SIZE(fll_ratio); i++) {
		if (fref >= fll_ratio[i].param)
			break;
	}
	if (i == ARRAY_SIZE(fll_ratio))
		return -EINVAL;
	fll_param->ratio = fll_ratio[i].val;

	/* Calculate the frequency of DCO (FDCO) given freq_out = 256 * Fs.
	 * FDCO must be within the 90MHz - 124MHz or the FFL cannot be
	 * guaranteed across the full range of operation.
	 * FDCO = freq_out * 2 * mclk_src_scaling
	 */
	fvco_max = 0;
	fvco_sel = ARRAY_SIZE(mclk_src_scaling);
	for (i = 0; i < ARRAY_SIZE(mclk_src_scaling); i++) {
		fvco = 256ULL * fs * 2 * mclk_src_scaling[i].param;
		if (fvco > NAU_FVCO_MIN && fvco < NAU_FVCO_MAX &&
			fvco_max < fvco) {
			fvco_max = fvco;
			fvco_sel = i;
		}
	}
	if (ARRAY_SIZE(mclk_src_scaling) == fvco_sel)
		return -EINVAL;
	fll_param->mclk_src = mclk_src_scaling[fvco_sel].val;

	/* Calculate the FLL 10-bit integer input and the FLL 24-bit fractional
	 * input based on FDCO, FREF and FLL ratio.
	 */
	fvco = div_u64(fvco_max << 24, fref * fll_param->ratio);
	fll_param->fll_int = (fvco >> 24) & 0x3ff;
	fll_param->fll_frac = fvco & 0xffffff;

	return 0;
}

static void nau8821_fll_apply(struct nau8821 *nau8821,
		struct nau8821_fll *fll_param)
{
	struct regmap *regmap = nau8821->regmap;

	regmap_update_bits(regmap, NAU8821_REG_CLK_DIVIDER,
		NAU8821_CLK_SRC_MASK | NAU8821_CLK_MCLK_SRC_MASK,
		NAU8821_CLK_SRC_MCLK | fll_param->mclk_src);
	/* Make DSP operate at high speed for better performance. */
	regmap_update_bits(regmap, NAU8821_REG_FLL1,
		NAU8821_FLL_RATIO_MASK | NAU8821_ICTRL_LATCH_MASK,
		fll_param->ratio | (0x6 << NAU8821_ICTRL_LATCH_SFT));
	/* FLL 24-bit fractional input */
	regmap_write(regmap, NAU8821_REG_FLL7,
		(fll_param->fll_frac >> 16) & 0xff);
	regmap_write(regmap, NAU8821_REG_FLL8, fll_param->fll_frac & 0xffff);
	/* FLL 10-bit integer input */
	regmap_update_bits(regmap, NAU8821_REG_FLL3,
		NAU8821_FLL_INTEGER_MASK, fll_param->fll_int);
	/* FLL pre-scaler */
	regmap_update_bits(regmap, NAU8821_REG_FLL4,
		NAU8821_HIGHBW_EN | NAU8821_FLL_REF_DIV_MASK,
		NAU8821_HIGHBW_EN |
		(fll_param->clk_ref_div << NAU8821_FLL_REF_DIV_SFT));
	/* select divided VCO input */
	regmap_update_bits(regmap, NAU8821_REG_FLL5,
		NAU8821_FLL_CLK_SW_MASK, NAU8821_FLL_CLK_SW_REF);
	/* Disable free-running mode */
	regmap_update_bits(regmap,
		NAU8821_REG_FLL6, NAU8821_DCO_EN, 0);
	if (fll_param->fll_frac) {
		/* set FLL loop filter enable and cutoff frequency at 500Khz */
		regmap_update_bits(regmap, NAU8821_REG_FLL5,
			NAU8821_FLL_PDB_DAC_EN | NAU8821_FLL_LOOP_FTR_EN |
			NAU8821_FLL_FTR_SW_MASK,
			NAU8821_FLL_PDB_DAC_EN | NAU8821_FLL_LOOP_FTR_EN |
			NAU8821_FLL_FTR_SW_FILTER);
		regmap_update_bits(regmap, NAU8821_REG_FLL6,
			NAU8821_SDM_EN | NAU8821_CUTOFF500,
			NAU8821_SDM_EN | NAU8821_CUTOFF500);
	} else {
		/* disable FLL loop filter and cutoff frequency */
		regmap_update_bits(regmap, NAU8821_REG_FLL5,
			NAU8821_FLL_PDB_DAC_EN | NAU8821_FLL_LOOP_FTR_EN |
			NAU8821_FLL_FTR_SW_MASK, NAU8821_FLL_FTR_SW_ACCU);
		regmap_update_bits(regmap, NAU8821_REG_FLL6,
			NAU8821_SDM_EN | NAU8821_CUTOFF500, 0);
	}
}

/**
 * nau8821_set_fll - FLL configuration of nau8821
 * @codec:  codec component
 * @freq_in:  frequency of input clock source
 * @freq_out:  must be 256*Fs in order to achieve the best performance
 *
 * The FLL function can select BCLK or MCLK as the input clock source.
 *
 * Returns 0 if the parameters have been applied successfully
 * or negative error code.
 */
static int nau8821_set_fll(struct snd_soc_component *component, int pll_id, int source,
	unsigned int freq_in, unsigned int freq_out)
{
	struct nau8821 *nau8821 = snd_soc_component_get_drvdata(component);
	struct nau8821_fll fll_set_param, *fll_param = &fll_set_param;
	int ret, fs;

	fs = freq_out >> 8;
	ret = nau8821_calc_fll_param(freq_in, fs, fll_param);
	if (ret) {
		dev_err(nau8821->dev, "Unsupported input clock %d to output clock %d\n",
			freq_in, freq_out);
		return ret;
	}
	dev_dbg(nau8821->dev, "mclk_src=%x ratio=%x fll_frac=%x fll_int=%x clk_ref_div=%x\n",
		fll_param->mclk_src, fll_param->ratio, fll_param->fll_frac,
		fll_param->fll_int, fll_param->clk_ref_div);

	nau8821_fll_apply(nau8821, fll_param);
	mdelay(2);
	regmap_update_bits(nau8821->regmap, NAU8821_REG_CLK_DIVIDER,
		NAU8821_CLK_SRC_MASK, NAU8821_CLK_SRC_VCO);
	return 0;
}

static void nau8821_configure_mclk_as_sysclk(struct regmap *regmap)
{
	regmap_update_bits(regmap, NAU8821_REG_CLK_DIVIDER,
		NAU8821_CLK_SRC_MASK, NAU8821_CLK_SRC_MCLK);
	regmap_update_bits(regmap, NAU8821_REG_FLL6,
		NAU8821_DCO_EN, 0);
	/* Make DSP operate as default setting for power saving. */
	regmap_update_bits(regmap, NAU8821_REG_FLL1,
		NAU8821_ICTRL_LATCH_MASK, 0);
}

static int nau8821_configure_sysclk(struct nau8821 *nau8821,
	int clk_id, unsigned int freq)
{
	struct regmap *regmap = nau8821->regmap;

	switch (clk_id) {
	case NAU8821_CLK_DIS:
		/* Clock provided externally and disable internal VCO clock */
		nau8821_configure_mclk_as_sysclk(regmap);
		break;
	case NAU8821_CLK_MCLK:
		nau8821_sema_acquire(nau8821, HZ);
		nau8821_configure_mclk_as_sysclk(regmap);
		/* MCLK not changed by clock tree */
		regmap_update_bits(regmap, NAU8821_REG_CLK_DIVIDER,
			NAU8821_CLK_MCLK_SRC_MASK, 0);
		nau8821_sema_release(nau8821);
		break;
	case NAU8821_CLK_INTERNAL:
		if (nau8821_is_jack_inserted(regmap)) {
			regmap_update_bits(regmap, NAU8821_REG_FLL6,
				NAU8821_DCO_EN, NAU8821_DCO_EN);
			regmap_update_bits(regmap, NAU8821_REG_CLK_DIVIDER,
				NAU8821_CLK_SRC_MASK, NAU8821_CLK_SRC_VCO);
			/* Decrease the VCO frequency and make DSP operate
			 * as default setting for power saving.
			 */
			regmap_update_bits(regmap, NAU8821_REG_CLK_DIVIDER,
				NAU8821_CLK_MCLK_SRC_MASK, 0xf);
			regmap_update_bits(regmap, NAU8821_REG_FLL1,
				NAU8821_ICTRL_LATCH_MASK |
				NAU8821_FLL_RATIO_MASK, 0x10);
			regmap_update_bits(regmap, NAU8821_REG_FLL6,
				NAU8821_SDM_EN, NAU8821_SDM_EN);
		} else {
			/* The clock turns off intentionally for power saving
			 * when no headset connected.
			 */
			nau8821_configure_mclk_as_sysclk(regmap);
			dev_warn(nau8821->dev, "Disable clock for power saving when no headset connected\n");
		}
		break;
	case NAU8821_CLK_FLL_MCLK:
		nau8821_sema_acquire(nau8821, HZ);
		/* Higher FLL reference input frequency can only set lower
		 * gain error, such as 0000 for input reference from MCLK
		 * 12.288Mhz.
		 */
		regmap_update_bits(regmap, NAU8821_REG_FLL3,
			NAU8821_FLL_CLK_SRC_MASK | NAU8821_GAIN_ERR_MASK,
			NAU8821_FLL_CLK_SRC_MCLK | 0);
		nau8821_sema_release(nau8821);
		break;
	case NAU8821_CLK_FLL_BLK:
		nau8821_sema_acquire(nau8821, HZ);
		/* If FLL reference input is from low frequency source,
		 * higher error gain can apply such as 0xf which has
		 * the most sensitive gain error correction threshold,
		 * Therefore, FLL has the most accurate DCO to
		 * target frequency.
		 */
		regmap_update_bits(regmap, NAU8821_REG_FLL3,
			NAU8821_FLL_CLK_SRC_MASK | NAU8821_GAIN_ERR_MASK,
			NAU8821_FLL_CLK_SRC_BLK |
			(0xf << NAU8821_GAIN_ERR_SFT));
		nau8821_sema_release(nau8821);
		break;
	case NAU8821_CLK_FLL_FS:
		nau8821_sema_acquire(nau8821, HZ);
		/* If FLL reference input is from low frequency source,
		 * higher error gain can apply such as 0xf which has
		 * the most sensitive gain error correction threshold,
		 * Therefore, FLL has the most accurate DCO to
		 * target frequency.
		 */
		regmap_update_bits(regmap, NAU8821_REG_FLL3,
			NAU8821_FLL_CLK_SRC_MASK | NAU8821_GAIN_ERR_MASK,
			NAU8821_FLL_CLK_SRC_FS |
			(0xf << NAU8821_GAIN_ERR_SFT));
		nau8821_sema_release(nau8821);
		break;
	default:
		dev_err(nau8821->dev, "Invalid clock id (%d)\n", clk_id);
		return -EINVAL;
	}
	nau8821->clk_id = clk_id;
	dev_dbg(nau8821->dev, "Sysclk is %dHz and clock id is %d\n", freq,
		nau8821->clk_id);

	return 0;
}

static int nau8821_set_sysclk(struct snd_soc_component *component, int clk_id,
	int source, unsigned int freq, int dir)
{
	struct nau8821 *nau8821 = snd_soc_component_get_drvdata(component);
	return nau8821_configure_sysclk(nau8821, clk_id, freq);
}

static int nau8821_resume_setup(struct nau8821 *nau8821)
{
	struct regmap *regmap = nau8821->regmap;

	/* Close clock when jack type detection at manual mode */
	nau8821_configure_sysclk(nau8821, NAU8821_CLK_DIS, 0);
	if (nau8821->irq) {
		/* Clear all interruption status */
		nau8821_int_status_clear_all(regmap);

		/* Enable both insertion and ejection interruptions, and then
		 * bypass de-bounce circuit.
		 */
		regmap_update_bits(regmap, NAU8821_REG_INTERRUPT_MASK,
			NAU8821_IRQ_EJECT_EN | NAU8821_IRQ_INSERT_EN, 0);
		regmap_update_bits(regmap, NAU8821_REG_JACK_DET_CTRL,
			NAU8821_JACK_DET_DB_BYPASS, NAU8821_JACK_DET_DB_BYPASS);
		regmap_update_bits(regmap, NAU8821_REG_INTERRUPT_DIS_CTRL,
			NAU8821_IRQ_INSERT_DIS | NAU8821_IRQ_EJECT_DIS, 0);
	}

	return 0;
}

static int nau8821_set_bias_level(struct snd_soc_component *component,
				   enum snd_soc_bias_level level)
{
	struct nau8821 *nau8821 = snd_soc_component_get_drvdata(component);
	struct regmap *regmap = nau8821->regmap;

	switch (level) {
	case SND_SOC_BIAS_ON:
		break;

	case SND_SOC_BIAS_PREPARE:
		break;

	case SND_SOC_BIAS_STANDBY:
		/* Setup codec configuration after resume */
		if (snd_soc_component_get_bias_level(component) == SND_SOC_BIAS_OFF)
			nau8821_resume_setup(nau8821);
		break;

	case SND_SOC_BIAS_OFF:
		/* HPL/HPR short to ground */
		regmap_update_bits(regmap, NAU8821_REG_JACK_DET_CTRL,
			NAU8821_SPKR_DWN1R | NAU8821_SPKR_DWN1L, 0);
		if (nau8821->irq) {
			/* Reset semaphore */
			nau8821_sema_reset(nau8821);
			/* Reset the configuration of jack type for detection */
			/* Detach 2kOhm Resistors from MICBIAS to MICGND1/2 */
			regmap_update_bits(regmap, NAU8821_REG_MIC_BIAS,
				NAU8821_MICBIAS_JKR2, 0);
			/* Turn off all interruptions before system shutdown. Keep the
			 * interruption quiet before resume setup completes.
			 */
			regmap_write(regmap,
				NAU8821_REG_INTERRUPT_DIS_CTRL, 0xffff);
			regmap_update_bits(regmap, NAU8821_REG_INTERRUPT_MASK,
				NAU8821_IRQ_EJECT_EN | NAU8821_IRQ_INSERT_EN,
				NAU8821_IRQ_EJECT_EN | NAU8821_IRQ_INSERT_EN);
			/* Disable ADC needed for interruptions at audo mode */
			regmap_update_bits(regmap, NAU8821_REG_ENA_CTRL,
				NAU8821_EN_ADCR | NAU8821_EN_ADCL, 0);
		}
		break;
	}
	return 0;
}

static int __maybe_unused nau8821_suspend(struct snd_soc_component *component)
{
	struct nau8821 *nau8821 = snd_soc_component_get_drvdata(component);

	if (nau8821->irq)
		disable_irq(nau8821->irq);
	snd_soc_component_force_bias_level(component, SND_SOC_BIAS_OFF);
	/* Power down codec power; don't suppoet button wakeup */
	snd_soc_dapm_disable_pin(nau8821->dapm, "MICBIAS");
	snd_soc_dapm_sync(nau8821->dapm);
	regcache_cache_only(nau8821->regmap, true);
	regcache_mark_dirty(nau8821->regmap);

	return 0;
}

static int __maybe_unused nau8821_resume(struct snd_soc_component *component)
{
	struct nau8821 *nau8821 = snd_soc_component_get_drvdata(component);

	regcache_cache_only(nau8821->regmap, false);
	regcache_sync(nau8821->regmap);
	if (nau8821->irq) {
		/* Hold semaphore to postpone playback happening
		 * until jack detection done.
		 */
		nau8821_sema_acquire(nau8821, 0);
		enable_irq(nau8821->irq);
	}

	return 0;
}

static const struct snd_soc_component_driver nau8821_component_driver = {
	.probe = nau8821_component_probe,
	.remove = nau8821_codec_remove,
	.set_sysclk = nau8821_set_sysclk,
	.set_pll = nau8821_set_fll,
	.set_bias_level = nau8821_set_bias_level,
	.suspend = nau8821_suspend,
	.resume = nau8821_resume,
	.controls = nau8821_controls,
	.num_controls = ARRAY_SIZE(nau8821_controls),
	.dapm_widgets = nau8821_dapm_widgets,
	.num_dapm_widgets = ARRAY_SIZE(nau8821_dapm_widgets),
	.dapm_routes = nau8821_dapm_routes,
	.num_dapm_routes = ARRAY_SIZE(nau8821_dapm_routes),
	.suspend_bias_off = 1,
	
};

/**
 * nau8821_enable_jack_detect - Specify a jack for event reporting
 *
 * @component:  component to register the jack with
 * @jack: jack to use to report headset and button events on
 *
 * After this function has been called the headset insert/remove and button
 * events will be routed to the given jack.  Jack can be null to stop
 * reporting.
 */
int nau8821_enable_jack_detect(struct snd_soc_component *component,
	struct snd_soc_jack *jack)
{
	struct nau8821 *nau8821 = snd_soc_component_get_drvdata(component);
	int ret;
#ifdef DEBUG
	ret = devm_request_threaded_irq(nau8821->dev, nau8821->irq, NULL,
		nau8821_interrupt, IRQF_TRIGGER_LOW | IRQF_ONESHOT,
		"nau8821", nau8821);
	if (ret) {
		dev_err(nau8821->dev, "Cannot request irq %d (%d)\n",
			nau8821->irq, ret);
		return ret;
	}
#endif
	nau8821->jack = jack;

	return ret;
}
EXPORT_SYMBOL_GPL(nau8821_enable_jack_detect);

static void nau8821_reset_chip(struct regmap *regmap)
{
	regmap_write(regmap, NAU8821_REG_RESET, 0xffff);
	regmap_write(regmap, NAU8821_REG_RESET, 0xffff);
}

static void nau8821_print_device_properties(struct nau8821 *nau8821)
{
	struct device *dev = nau8821->dev;

	dev_dbg(dev, "jkdet-enable:         %d\n", nau8821->jkdet_enable);
	dev_dbg(dev, "jkdet-pull-enable:    %d\n", nau8821->jkdet_pull_enable);
	dev_dbg(dev, "jkdet-pull-up:        %d\n", nau8821->jkdet_pull_up);
	dev_dbg(dev, "jkdet-polarity:       %d\n", nau8821->jkdet_polarity);
	dev_dbg(dev, "micbias-voltage:      %d\n", nau8821->micbias_voltage);
	dev_dbg(dev, "vref-impedance:       %d\n", nau8821->vref_impedance);
	dev_dbg(dev, "jack-insert-debounce: %d\n",
		nau8821->jack_insert_debounce);
	dev_dbg(dev, "jack-eject-debounce:  %d\n",
		nau8821->jack_eject_debounce);
}

static int nau8821_read_device_properties(struct device *dev,
	struct nau8821 *nau8821) {
	int ret;

	nau8821->jkdet_enable = device_property_read_bool(dev,
		"nuvoton,jkdet-enable");
	nau8821->jkdet_pull_enable = device_property_read_bool(dev,
		"nuvoton,jkdet-pull-enable");
	nau8821->jkdet_pull_up = device_property_read_bool(dev,
		"nuvoton,jkdet-pull-up");
	ret = device_property_read_u32(dev, "nuvoton,jkdet-polarity",
		&nau8821->jkdet_polarity);
	if (ret)
		nau8821->jkdet_polarity = 1;
	ret = device_property_read_u32(dev, "nuvoton,micbias-voltage",
		&nau8821->micbias_voltage);
	if (ret)
		nau8821->micbias_voltage = 6;
	ret = device_property_read_u32(dev, "nuvoton,vref-impedance",
		&nau8821->vref_impedance);
	if (ret)
		nau8821->vref_impedance = 2;
	ret = device_property_read_u32(dev, "nuvoton,jack-insert-debounce",
		&nau8821->jack_insert_debounce);
	if (ret)
		nau8821->jack_insert_debounce = 7;
	ret = device_property_read_u32(dev, "nuvoton,jack-eject-debounce",
		&nau8821->jack_eject_debounce);
	if (ret)
		nau8821->jack_eject_debounce = 0;

	return 0;
}

static void nau8821_init_regs(struct nau8821 *nau8821)
{
	struct regmap *regmap = nau8821->regmap;

	/* Enable Bias/Vmid */
	regmap_update_bits(regmap, NAU8821_REG_BIAS_ADJ,
		NAU8821_BIAS_VMID, NAU8821_BIAS_VMID);
	regmap_update_bits(regmap, NAU8821_REG_BOOST,
		NAU8821_GLOBAL_BIAS_EN, NAU8821_GLOBAL_BIAS_EN);
	/* VMID Tieoff setting and enable TESTDAC.
	 * This sets the analog DAC inputs to a '0' input signal to avoid
	 * any glitches due to power up transients in both the analog and
	 * digital DAC circuit.
	 */
	regmap_update_bits(regmap, NAU8821_REG_BIAS_ADJ,
		NAU8821_BIAS_VMID_SEL_MASK | NAU8821_BIAS_TESTDAC_EN,
		(nau8821->vref_impedance << NAU8821_BIAS_VMID_SEL_SFT) |
		NAU8821_BIAS_TESTDAC_EN);
	/* Disable short Frame Sync detection logic */
	regmap_update_bits(regmap, NAU8821_REG_LEFT_TIME_SLOT,
		NAU8821_DIS_FS_SHORT_DET, NAU8821_DIS_FS_SHORT_DET);
	/* Disable Boost Driver, Automatic Short circuit protection enable */
	regmap_update_bits(regmap, NAU8821_REG_BOOST,
		NAU8821_PRECHARGE_DIS | NAU8821_HP_BOOST_DIS |
		NAU8821_HP_BOOST_G_DIS | NAU8821_SHORT_SHUTDOWN_EN,
		NAU8821_PRECHARGE_DIS | NAU8821_HP_BOOST_DIS |
		NAU8821_HP_BOOST_G_DIS | NAU8821_SHORT_SHUTDOWN_EN);
	/* Class G timer 64ms */
	regmap_update_bits(regmap, NAU8821_REG_CLASSG_CTRL,
		NAU8821_CLASSG_TIMER_MASK,
		0x20 << NAU8821_CLASSG_TIMER_SFT);
	/* Class AB bias current to 2x, DAC Capacitor enable MSB/LSB */
	regmap_update_bits(regmap, NAU8821_REG_ANALOG_CONTROL_2,
		NAU8821_HP_NON_CLASSG_CURRENT_2xADJ |
		NAU8821_DAC_CAPACITOR_MSB | NAU8821_DAC_CAPACITOR_LSB,
		NAU8821_HP_NON_CLASSG_CURRENT_2xADJ |
		NAU8821_DAC_CAPACITOR_MSB | NAU8821_DAC_CAPACITOR_LSB);
	/* Disable DACR/L power */
	regmap_update_bits(regmap, NAU8821_REG_CHARGE_PUMP,
		NAU8821_POWER_DOWN_DACR | NAU8821_POWER_DOWN_DACL, 0);
	/* DAC clock delay 2ns, VREF */
	regmap_update_bits(regmap, NAU8821_REG_RDAC,
		NAU8821_DAC_CLK_DELAY_MASK | NAU8821_DAC_VREF_MASK,
		(0x2 << NAU8821_DAC_CLK_DELAY_SFT) |
		(0x3 << NAU8821_DAC_VREF_SFT));

	regmap_update_bits(regmap, NAU8821_REG_MIC_BIAS,
		NAU8821_MICBIAS_VOLTAGE_MASK, nau8821->micbias_voltage);
	/* Default oversampling/decimations settings are unusable
	 * (audible hiss). Set it to something better.
	 */
	regmap_update_bits(regmap, NAU8821_REG_ADC_RATE,
		NAU8821_ADC_SYNC_DOWN_MASK, NAU8821_ADC_SYNC_DOWN_64);
	regmap_update_bits(regmap, NAU8821_REG_DAC_CTRL1,
		NAU8821_DAC_OVERSAMPLE_MASK, NAU8821_DAC_OVERSAMPLE_64);
}

static int nau8821_setup_irq(struct nau8821 *nau8821)
{
	struct regmap *regmap = nau8821->regmap;

	sema_init(&nau8821->jd_sem, 1);

	/* Jack detection */
	regmap_update_bits(regmap, NAU8821_REG_GPIO12_CTRL,
		NAU8821_JKDET_OUTPUT_EN,
		nau8821->jkdet_enable ? 0 : NAU8821_JKDET_OUTPUT_EN);
	regmap_update_bits(regmap, NAU8821_REG_GPIO12_CTRL,
		NAU8821_JKDET_PULL_EN,
		nau8821->jkdet_pull_enable ? 0 : NAU8821_JKDET_PULL_EN);
	regmap_update_bits(regmap, NAU8821_REG_GPIO12_CTRL,
		NAU8821_JKDET_PULL_UP,
		nau8821->jkdet_pull_up ? NAU8821_JKDET_PULL_UP : 0);
	regmap_update_bits(regmap, NAU8821_REG_JACK_DET_CTRL,
		NAU8821_JACK_POLARITY,
		/* jkdet_polarity - 1  is for active-low */
		nau8821->jkdet_polarity ? 0 : NAU8821_JACK_POLARITY);
	regmap_update_bits(regmap, NAU8821_REG_JACK_DET_CTRL,
		NAU8821_JACK_INSERT_DEBOUNCE_MASK,
		nau8821->jack_insert_debounce <<
		NAU8821_JACK_INSERT_DEBOUNCE_SFT);
	regmap_update_bits(regmap, NAU8821_REG_JACK_DET_CTRL,
		NAU8821_JACK_EJECT_DEBOUNCE_MASK,
		nau8821->jack_eject_debounce <<
		NAU8821_JACK_EJECT_DEBOUNCE_SFT);
	/* Pull up IRQ pin */
	regmap_update_bits(regmap, NAU8821_REG_INTERRUPT_MASK,
		NAU8821_IRQ_PIN_PULL_UP | NAU8821_IRQ_PIN_PULL_EN |
		NAU8821_IRQ_OUTPUT_EN, NAU8821_IRQ_PIN_PULL_UP |
		NAU8821_IRQ_PIN_PULL_EN | NAU8821_IRQ_OUTPUT_EN);
	/* Disable interruption before codec initiation done */
	/* Mask unneeded IRQs: 1 - disable, 0 - enable */
	regmap_update_bits(regmap, NAU8821_REG_INTERRUPT_MASK, 0x3f5, 0x3f5);

	return 0;
}

static int nau8821_i2c_probe(struct i2c_client *i2c,
	const struct i2c_device_id *id)
{
	struct device *dev = &i2c->dev;
	struct nau8821 *nau8821 = dev_get_platdata(&i2c->dev);
	int ret, value;

	if (!nau8821) {
		nau8821 = devm_kzalloc(dev, sizeof(*nau8821), GFP_KERNEL);
		if (!nau8821)
			return -ENOMEM;
		nau8821_read_device_properties(dev, nau8821);
	}
	i2c_set_clientdata(i2c, nau8821);

#ifdef DEBUG
	nau8821->regmap = devm_regmap_init(dev, NULL,
		i2c, &nau8821_regmap_config);
#else
	nau8821->regmap = devm_regmap_init_i2c(i2c, &nau8821_regmap_config);
#endif

#if 0
	ret = regmap_write(nau8821->regmap, NAU8821_REG_RESET, 0x00);
		if (ret) {
			dev_err(&i2c->dev,"i2c write error");
		}

	ret = regmap_read(nau8821->regmap, NAU8821_REG_I2C_DEVICE_ID, &val);
		if (ret) {
			dev_err(&i2c->dev,"i2c read error");
		}
#endif		
	if (IS_ERR(nau8821->regmap))
		return PTR_ERR(nau8821->regmap);
	nau8821->dev = dev;
	nau8821->irq = i2c->irq;
	nau8821_print_device_properties(nau8821);

	nau8821_reset_chip(nau8821->regmap);
	ret = regmap_read(nau8821->regmap, NAU8821_REG_I2C_DEVICE_ID, &value);
	if (ret) {
		dev_err(dev, "Failed to read device id (%d)\n", ret);
		return ret;
	}
	nau8821_init_regs(nau8821);

	if (i2c->irq)
		nau8821_setup_irq(nau8821);
	
	return devm_snd_soc_register_component(&i2c->dev, &nau8821_component_driver,
		&nau8821_dai, 1);
}

static int nau8821_i2c_remove(struct i2c_client *client)
{
	return 0;
}

static const struct i2c_device_id nau8821_i2c_ids[] = {
	{ "nau8821", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, nau8821_i2c_ids);

#ifdef CONFIG_OF
static const struct of_device_id nau8821_of_ids[] = {
	{ .compatible = "nuvoton,nau8821", },
	{}
};
MODULE_DEVICE_TABLE(of, nau8821_of_ids);
#endif

#ifdef CONFIG_ACPI
static const struct acpi_device_id nau8821_acpi_match[] = {
	{ "NVTN2020", 0 },
	{},
};
MODULE_DEVICE_TABLE(acpi, nau8821_acpi_match);
#endif

static struct i2c_driver nau8821_driver = {
	.driver = {
		.name = "nau8821",
		.of_match_table = of_match_ptr(nau8821_of_ids),
		.acpi_match_table = ACPI_PTR(nau8821_acpi_match),
	},
	.probe = nau8821_i2c_probe,
	.remove = nau8821_i2c_remove,
	.id_table = nau8821_i2c_ids,
};
module_i2c_driver(nau8821_driver);

MODULE_DESCRIPTION("ASoC nau8821 driver");
MODULE_AUTHOR("John Hsu <KCHSU0@nuvoton.com>");
MODULE_LICENSE("GPL v2");
