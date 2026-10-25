// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright 2014 Emilio López <emilio@elopez.com.ar>
 * Copyright 2014 Jon Smirl <jonsmirl@gmail.com>
 * Copyright 2015 Maxime Ripard <maxime.ripard@free-electrons.com>
 * Copyright 2015 Adam Sampson <ats@offog.org>
 * Copyright 2016 Chen-Yu Tsai <wens@csie.org>
 * Copyright 2018 Mesih Kilinc <mesihkilinc@gmail.com>
 *
 * Based on the Allwinner SDK driver, released under the GPL.
 */

#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/workqueue.h>
#include <linux/mutex.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/clk.h>
#include <linux/regmap.h>
#include <linux/reset.h>
#include <linux/regulator/consumer.h>
#include <linux/gpio/consumer.h>

#include <sound/core.h>
#include <sound/dmaengine_pcm.h>
#include <sound/initval.h>
#include <sound/jack.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/tlv.h>

/* Codec DAC digital controls and FIFO registers */
#define SUN4I_CODEC_DAC_DPC			(0x00)
#define SUN4I_CODEC_DAC_DPC_EN_DA			(31)
#define SUN4I_CODEC_DAC_DPC_DVOL			(12)
#define SUN4I_CODEC_DAC_FIFOC			(0x04)
#define SUN4I_CODEC_DAC_FIFOC_DAC_FS			(29)
#define SUN4I_CODEC_DAC_FIFOC_FIR_VERSION		(28)
#define SUN4I_CODEC_DAC_FIFOC_SEND_LASAT		(26)
#define SUN4I_CODEC_DAC_FIFOC_TX_FIFO_MODE		(24)
#define SUN4I_CODEC_DAC_FIFOC_DRQ_CLR_CNT		(21)
#define SUN4I_CODEC_DAC_FIFOC_TX_TRIG_LEVEL		(8)
#define SUN4I_CODEC_DAC_FIFOC_MONO_EN			(6)
#define SUN4I_CODEC_DAC_FIFOC_TX_SAMPLE_BITS		(5)
#define SUN4I_CODEC_DAC_FIFOC_DAC_DRQ_EN		(4)
#define SUN4I_CODEC_DAC_FIFOC_FIFO_FLUSH		(0)
#define SUN4I_CODEC_DAC_FIFOS			(0x08)
#define SUN4I_CODEC_DAC_TXDATA			(0x0c)

/* Codec DAC side analog signal controls */
#define SUN4I_CODEC_DAC_ACTL			(0x10)
#define SUN4I_CODEC_DAC_ACTL_DACAENR			(31)
#define SUN4I_CODEC_DAC_ACTL_DACAENL			(30)
#define SUN4I_CODEC_DAC_ACTL_MIXEN			(29)
#define SUN4I_CODEC_DAC_ACTL_LNG			(26)
#define SUN4I_CODEC_DAC_ACTL_FMG			(23)
#define SUN4I_CODEC_DAC_ACTL_MICG			(20)
#define SUN4I_CODEC_DAC_ACTL_LLNS			(19)
#define SUN4I_CODEC_DAC_ACTL_RLNS			(18)
#define SUN4I_CODEC_DAC_ACTL_LFMS			(17)
#define SUN4I_CODEC_DAC_ACTL_RFMS			(16)
#define SUN4I_CODEC_DAC_ACTL_LDACLMIXS			(15)
#define SUN4I_CODEC_DAC_ACTL_RDACRMIXS			(14)
#define SUN4I_CODEC_DAC_ACTL_LDACRMIXS			(13)
#define SUN4I_CODEC_DAC_ACTL_MIC1LS			(12)
#define SUN4I_CODEC_DAC_ACTL_MIC1RS			(11)
#define SUN4I_CODEC_DAC_ACTL_MIC2LS			(10)
#define SUN4I_CODEC_DAC_ACTL_MIC2RS			(9)
#define SUN4I_CODEC_DAC_ACTL_DACPAS			(8)
#define SUN4I_CODEC_DAC_ACTL_MIXPAS			(7)
#define SUN4I_CODEC_DAC_ACTL_PA_MUTE			(6)
#define SUN4I_CODEC_DAC_ACTL_PA_VOL			(0)
#define SUN4I_CODEC_DAC_TUNE			(0x14)
#define SUN4I_CODEC_DAC_DEBUG			(0x18)

/* Codec ADC digital controls and FIFO registers */
#define SUN4I_CODEC_ADC_FIFOC			(0x1c)
#define SUN4I_CODEC_ADC_FIFOC_ADC_FS			(29)
#define SUN4I_CODEC_ADC_FIFOC_EN_AD			(28)
#define SUN4I_CODEC_ADC_FIFOC_RX_FIFO_MODE		(24)
#define SUN4I_CODEC_ADC_FIFOC_RX_TRIG_LEVEL		(8)
#define SUN4I_CODEC_ADC_FIFOC_MONO_EN			(7)
#define SUN4I_CODEC_ADC_FIFOC_RX_SAMPLE_BITS		(6)
#define SUN4I_CODEC_ADC_FIFOC_ADC_DRQ_EN		(4)
#define SUN4I_CODEC_ADC_FIFOC_FIFO_FLUSH		(0)
#define SUN4I_CODEC_ADC_FIFOS			(0x20)
#define SUN4I_CODEC_ADC_RXDATA			(0x24)

/* Codec ADC side analog signal controls */
#define SUN4I_CODEC_ADC_ACTL			(0x28)
#define SUN4I_CODEC_ADC_ACTL_ADC_R_EN			(31)
#define SUN4I_CODEC_ADC_ACTL_ADC_L_EN			(30)
#define SUN4I_CODEC_ADC_ACTL_PREG1EN			(29)
#define SUN4I_CODEC_ADC_ACTL_PREG2EN			(28)
#define SUN4I_CODEC_ADC_ACTL_VMICEN			(27)
#define SUN4I_CODEC_ADC_ACTL_PREG1			(25)
#define SUN4I_CODEC_ADC_ACTL_PREG2			(23)
#define SUN4I_CODEC_ADC_ACTL_VADCG			(20)
#define SUN4I_CODEC_ADC_ACTL_ADCIS			(17)
#define SUN4I_CODEC_ADC_ACTL_LNPREG			(13)
#define SUN4I_CODEC_ADC_ACTL_PA_EN			(4)
#define SUN4I_CODEC_ADC_ACTL_DDE			(3)
#define SUN4I_CODEC_ADC_DEBUG			(0x2c)

/* FIFO counters */
#define SUN4I_CODEC_DAC_TXCNT			(0x30)
#define SUN4I_CODEC_ADC_RXCNT			(0x34)

/* Calibration register (sun7i only) */
#define SUN7I_CODEC_AC_DAC_CAL			(0x38)

/* Microphone controls (sun7i only) */
#define SUN7I_CODEC_AC_MIC_PHONE_CAL		(0x3c)

#define SUN7I_CODEC_AC_MIC_PHONE_CAL_PREG1		(29)
#define SUN7I_CODEC_AC_MIC_PHONE_CAL_PREG2		(26)

/*
 * sun6i specific registers
 *
 * sun6i shares the same digital control and FIFO registers as sun4i,
 * but only the DAC digital controls are at the same offset. The others
 * have been moved around to accommodate extra analog controls.
 */

/* Codec DAC digital controls and FIFO registers */
#define SUN6I_CODEC_ADC_FIFOC			(0x10)
#define SUN6I_CODEC_ADC_FIFOC_EN_AD			(28)
#define SUN6I_CODEC_ADC_FIFOS			(0x14)
#define SUN6I_CODEC_ADC_RXDATA			(0x18)

/* Output mixer and gain controls */
#define SUN6I_CODEC_OM_DACA_CTRL		(0x20)
#define SUN6I_CODEC_OM_DACA_CTRL_DACAREN		(31)
#define SUN6I_CODEC_OM_DACA_CTRL_DACALEN		(30)
#define SUN6I_CODEC_OM_DACA_CTRL_RMIXEN			(29)
#define SUN6I_CODEC_OM_DACA_CTRL_LMIXEN			(28)
#define SUN6I_CODEC_OM_DACA_CTRL_RMIX_MIC1		(23)
#define SUN6I_CODEC_OM_DACA_CTRL_RMIX_MIC2		(22)
#define SUN6I_CODEC_OM_DACA_CTRL_RMIX_PHONE		(21)
#define SUN6I_CODEC_OM_DACA_CTRL_RMIX_PHONEP		(20)
#define SUN6I_CODEC_OM_DACA_CTRL_RMIX_LINEINR		(19)
#define SUN6I_CODEC_OM_DACA_CTRL_RMIX_DACR		(18)
#define SUN6I_CODEC_OM_DACA_CTRL_RMIX_DACL		(17)
#define SUN6I_CODEC_OM_DACA_CTRL_LMIX_MIC1		(16)
#define SUN6I_CODEC_OM_DACA_CTRL_LMIX_MIC2		(15)
#define SUN6I_CODEC_OM_DACA_CTRL_LMIX_PHONE		(14)
#define SUN6I_CODEC_OM_DACA_CTRL_LMIX_PHONEN		(13)
#define SUN6I_CODEC_OM_DACA_CTRL_LMIX_LINEINL		(12)
#define SUN6I_CODEC_OM_DACA_CTRL_LMIX_DACL		(11)
#define SUN6I_CODEC_OM_DACA_CTRL_LMIX_DACR		(10)
#define SUN6I_CODEC_OM_DACA_CTRL_RHPIS			(9)
#define SUN6I_CODEC_OM_DACA_CTRL_LHPIS			(8)
#define SUN6I_CODEC_OM_DACA_CTRL_RHPPAMUTE		(7)
#define SUN6I_CODEC_OM_DACA_CTRL_LHPPAMUTE		(6)
#define SUN6I_CODEC_OM_DACA_CTRL_HPVOL			(0)
#define SUN6I_CODEC_OM_PA_CTRL			(0x24)
#define SUN6I_CODEC_OM_PA_CTRL_HPPAEN			(31)
#define SUN6I_CODEC_OM_PA_CTRL_HPCOM_CTL		(29)
#define SUN6I_CODEC_OM_PA_CTRL_COMPTEN			(28)
#define SUN6I_CODEC_OM_PA_CTRL_MIC1G			(15)
#define SUN6I_CODEC_OM_PA_CTRL_MIC2G			(12)
#define SUN6I_CODEC_OM_PA_CTRL_LINEING			(9)
#define SUN6I_CODEC_OM_PA_CTRL_PHONEG			(6)
#define SUN6I_CODEC_OM_PA_CTRL_PHONEPG			(3)
#define SUN6I_CODEC_OM_PA_CTRL_PHONENG			(0)

/* Microphone, line out and phone out controls */
#define SUN6I_CODEC_MIC_CTRL			(0x28)
#define SUN6I_CODEC_MIC_CTRL_HBIASEN			(31)
#define SUN6I_CODEC_MIC_CTRL_MBIASEN			(30)
#define SUN6I_CODEC_MIC_CTRL_MIC1AMPEN			(28)
#define SUN6I_CODEC_MIC_CTRL_MIC1BOOST			(25)
#define SUN6I_CODEC_MIC_CTRL_MIC2AMPEN			(24)
#define SUN6I_CODEC_MIC_CTRL_MIC2BOOST			(21)
#define SUN6I_CODEC_MIC_CTRL_MIC2SLT			(20)
#define SUN6I_CODEC_MIC_CTRL_LINEOUTLEN			(19)
#define SUN6I_CODEC_MIC_CTRL_LINEOUTREN			(18)
#define SUN6I_CODEC_MIC_CTRL_LINEOUTLSRC		(17)
#define SUN6I_CODEC_MIC_CTRL_LINEOUTRSRC		(16)
#define SUN6I_CODEC_MIC_CTRL_LINEOUTVC			(11)
#define SUN6I_CODEC_MIC_CTRL_PHONEPREG			(8)

/* ADC mixer controls */
#define SUN6I_CODEC_ADC_ACTL			(0x2c)
#define SUN6I_CODEC_ADC_ACTL_ADCREN			(31)
#define SUN6I_CODEC_ADC_ACTL_ADCLEN			(30)
#define SUN6I_CODEC_ADC_ACTL_ADCRG			(27)
#define SUN6I_CODEC_ADC_ACTL_ADCLG			(24)
#define SUN6I_CODEC_ADC_ACTL_RADCMIX_MIC1		(13)
#define SUN6I_CODEC_ADC_ACTL_RADCMIX_MIC2		(12)
#define SUN6I_CODEC_ADC_ACTL_RADCMIX_PHONE		(11)
#define SUN6I_CODEC_ADC_ACTL_RADCMIX_PHONEP		(10)
#define SUN6I_CODEC_ADC_ACTL_RADCMIX_LINEINR		(9)
#define SUN6I_CODEC_ADC_ACTL_RADCMIX_OMIXR		(8)
#define SUN6I_CODEC_ADC_ACTL_RADCMIX_OMIXL		(7)
#define SUN6I_CODEC_ADC_ACTL_LADCMIX_MIC1		(6)
#define SUN6I_CODEC_ADC_ACTL_LADCMIX_MIC2		(5)
#define SUN6I_CODEC_ADC_ACTL_LADCMIX_PHONE		(4)
#define SUN6I_CODEC_ADC_ACTL_LADCMIX_PHONEN		(3)
#define SUN6I_CODEC_ADC_ACTL_LADCMIX_LINEINL		(2)
#define SUN6I_CODEC_ADC_ACTL_LADCMIX_OMIXL		(1)
#define SUN6I_CODEC_ADC_ACTL_LADCMIX_OMIXR		(0)

/* Analog performance tuning controls */
#define SUN6I_CODEC_ADDA_TUNE			(0x30)

/* Calibration controls */
#define SUN6I_CODEC_CALIBRATION			(0x34)

/* FIFO counters */
#define SUN6I_CODEC_DAC_TXCNT			(0x40)
#define SUN6I_CODEC_ADC_RXCNT			(0x44)

/* headset jack detection and button support registers */
#define SUN6I_CODEC_HMIC_CTL			(0x50)
#define SUN6I_CODEC_HMIC_DATA			(0x54)

/* TODO sun6i DAP (Digital Audio Processing) bits */

/* FIFO counters moved on A23 */
#define SUN8I_A23_CODEC_DAC_TXCNT		(0x1c)
#define SUN8I_A23_CODEC_ADC_RXCNT		(0x20)

/* TX FIFO moved on H3 */
#define SUN8I_H3_CODEC_DAC_TXDATA		(0x20)
#define SUN8I_H3_CODEC_DAC_DBG			(0x48)
#define SUN8I_H3_CODEC_ADC_DBG			(0x4c)

/* H616 specific registers */
#define SUN50I_H616_CODEC_DAC_FIFOC		(0x10)

#define SUN50I_DAC_FIFO_STA			(0x14)
#define SUN50I_DAC_TXE_INT			(3)
#define SUN50I_DAC_TXU_INT			(2)
#define SUN50I_DAC_TXO_INT			(1)

#define SUN50I_DAC_CNT				(0x24)
#define SUN50I_DAC_DG_REG			(0x28)
#define SUN50I_DAC_DAP_CTL			(0xf0)

#define SUN50I_H616_DAC_AC_DAC_REG		(0x310)
#define SUN50I_H616_DAC_LEN			(15)
#define SUN50I_H616_DAC_REN			(14)
#define SUN50I_H616_LINEOUTL_EN			(13)
#define SUN50I_H616_LMUTE			(12)
#define SUN50I_H616_LINEOUTR_EN			(11)
#define SUN50I_H616_RMUTE			(10)
#define SUN50I_H616_RSWITCH			(9)
#define SUN50I_H616_RAMPEN			(8)
#define SUN50I_H616_LINEOUTL_SEL		(6)
#define SUN50I_H616_LINEOUTR_SEL		(5)
#define SUN50I_H616_LINEOUT_VOL			(0)

#define SUN50I_H616_DAC_AC_MIXER_REG		(0x314)
#define SUN50I_H616_LMIX_LDAC			(21)
#define SUN50I_H616_LMIX_RDAC			(20)
#define SUN50I_H616_RMIX_RDAC			(17)
#define SUN50I_H616_RMIX_LDAC			(16)
#define SUN50I_H616_LMIXEN			(11)
#define SUN50I_H616_RMIXEN			(10)

#define SUN50I_H616_DAC_AC_RAMP_REG		(0x31c)
#define SUN50I_H616_RAMP_STEP			(4)
#define SUN50I_H616_RDEN			(0)

/* A133 direct-MMIO codec registers. */
#define SUN50I_A133_DAC_DPC		0x000
#define SUN50I_A133_DAC_VOL		0x004
#define SUN50I_A133_DAC_FIFOS		0x014
#define SUN50I_A133_DAC_CNT		0x024
#define SUN50I_A133_DAC_DEBUG		0x028
#define SUN50I_A133_ADC_FIFOC		0x030
#define SUN50I_A133_ADC_VOL		0x034
#define SUN50I_A133_ADC_FIFOS		0x038
#define SUN50I_A133_ADC_CNT		0x044
#define SUN50I_A133_DAC_DAP		0x0f0
#define SUN50I_A133_DAC_DRC_CTRL		0x108
#define SUN50I_A133_ADC_DAP		0x0f8
#define SUN50I_A133_ADC_HPF_H		0x200
#define SUN50I_A133_ADC_HPF_L		0x204
#define SUN50I_A133_ADC_DRC_CTRL		0x208
#define SUN50I_A133_ADCL			0x300
#define SUN50I_A133_ADCR			0x304
#define SUN50I_A133_MICBIAS		0x318
#define SUN50I_A133_DAC_ANALOG		0x310
#define SUN50I_A133_HEADPHONE		0x324

/* TODO H3 DAP (Digital Audio Processing) bits */

#define SUN4I_DMA_MAX_BURST			(8)

/* suniv specific registers */

#define SUNIV_DMA_MAX_BURST			(4)

/* Codec DAC digital controls and FIFO registers */
#define SUNIV_CODEC_ADC_FIFOC			(0x10)
#define SUNIV_CODEC_ADC_FIFOC_EN_AD		(28)
#define SUNIV_CODEC_ADC_FIFOS			(0x14)
#define SUNIV_CODEC_ADC_RXDATA			(0x18)

/* Output mixer and gain controls */
#define SUNIV_CODEC_OM_DACA_CTRL			(0x20)
#define SUNIV_CODEC_OM_DACA_CTRL_DACAREN		(31)
#define SUNIV_CODEC_OM_DACA_CTRL_DACALEN		(30)
#define SUNIV_CODEC_OM_DACA_CTRL_RMIXEN			(29)
#define SUNIV_CODEC_OM_DACA_CTRL_LMIXEN			(28)
#define SUNIV_CODEC_OM_DACA_CTRL_RHPPAMUTE		(27)
#define SUNIV_CODEC_OM_DACA_CTRL_LHPPAMUTE		(26)
#define SUNIV_CODEC_OM_DACA_CTRL_RHPIS			(25)
#define SUNIV_CODEC_OM_DACA_CTRL_LHPIS			(24)
#define SUNIV_CODEC_OM_DACA_CTRL_HPCOM_CTL		(22)
#define SUNIV_CODEC_OM_DACA_CTRL_COMPTEN		(21)
#define SUNIV_CODEC_OM_DACA_CTRL_RMIXMUTE_MICIN		(20)
#define SUNIV_CODEC_OM_DACA_CTRL_RMIXMUTE_LINEIN	(19)
#define SUNIV_CODEC_OM_DACA_CTRL_RMIXMUTE_FMIN		(18)
#define SUNIV_CODEC_OM_DACA_CTRL_RMIXMUTE_RDAC		(17)
#define SUNIV_CODEC_OM_DACA_CTRL_RMIXMUTE_LDAC		(16)
#define SUNIV_CODEC_OM_DACA_CTRL_HPPAEN			(15)
#define SUNIV_CODEC_OM_DACA_CTRL_LMIXMUTE_MICIN		(12)
#define SUNIV_CODEC_OM_DACA_CTRL_LMIXMUTE_LINEIN	(11)
#define SUNIV_CODEC_OM_DACA_CTRL_LMIXMUTE_FMIN		(10)
#define SUNIV_CODEC_OM_DACA_CTRL_LMIXMUTE_LDAC		(9)
#define SUNIV_CODEC_OM_DACA_CTRL_LMIXMUTE_RDAC		(8)
#define SUNIV_CODEC_OM_DACA_CTRL_LTLNMUTE		(7)
#define SUNIV_CODEC_OM_DACA_CTRL_RTLNMUTE		(6)
#define SUNIV_CODEC_OM_DACA_CTRL_HPVOL			(0)

/* Analog Input Mixer controls */
#define SUNIV_CODEC_ADC_ACTL		(0x24)
#define SUNIV_CODEC_ADC_ADCEN		(31)
#define SUNIV_CODEC_ADC_MICG		(24)
#define SUNIV_CODEC_ADC_LINEINVOL	(21)
#define SUNIV_CODEC_ADC_ADCG		(16)
#define SUNIV_CODEC_ADC_ADCMIX_MIC	(13)
#define SUNIV_CODEC_ADC_ADCMIX_FMINL	(12)
#define SUNIV_CODEC_ADC_ADCMIX_FMINR	(11)
#define SUNIV_CODEC_ADC_ADCMIX_LINEIN	(10)
#define SUNIV_CODEC_ADC_ADCMIX_LOUT	(9)
#define SUNIV_CODEC_ADC_ADCMIX_ROUT	(8)
#define SUNIV_CODEC_ADC_PASPEEDSELECT	(7)
#define SUNIV_CODEC_ADC_FMINVOL		(4)
#define SUNIV_CODEC_ADC_MICAMPEN	(3)
#define SUNIV_CODEC_ADC_MICBOOST	(0)

#define SUNIV_CODEC_ADC_DBG		(0x4c)

struct a133_stream_clocks {
	struct clk *module;
	struct clk *parent;
	bool module_claimed, module_enabled;
	bool audio_claimed, audio_enabled;
};

struct sun4i_codec {
	struct device	*dev;
	struct regmap	*regmap;
	struct clk	*clk_apb;
	struct clk	*clk_module;
	bool a133;
	bool a133_apb_on;
	struct regulator *a133_supply[2];
	bool a133_supply_on[2];
	struct clk *a133_pll_audio;
	struct clk *a133_pll_com_audio;
	struct mutex a133_clock_lock;
	struct mutex a133_volume_lock;
	unsigned int a133_dac_volume;
	bool a133_dac_muted;
	struct a133_stream_clocks a133_stream[2];
	struct reset_control *rst;
	struct gpio_desc *gpio_pa;
	struct gpio_desc *gpio_hp;
	u32 pa_delay_ms;
	struct mutex jack_lock;
	struct delayed_work jack_work;
	unsigned int jack_pending;
	unsigned int jack_saved_bias;
	unsigned int jack_saved_ctrl;
	int jack_irq;
	bool jack_active;
	bool jack_fault;
	bool jack_registered;
	struct snd_soc_card *jack_card;
	struct snd_soc_jack headphone_jack;

	/* ADC_FIFOC register is at different offset on different SoCs */
	struct regmap_field *reg_adc_fifoc;
	/* DAC_FIFOC register is at different offset on different SoCs */
	struct regmap_field *reg_dac_fifoc;

	struct snd_dmaengine_dai_dma_data	capture_dma_data;
	struct snd_dmaengine_dai_dma_data	playback_dma_data;
};

/* Each direction owns its module and PLL votes; shared PLL transactions serialize. */
static void sun50i_a133_release_clocks(struct sun4i_codec *scodec,
				     struct a133_stream_clocks *stream)
{
	if (stream->module_enabled) {
		clk_disable_unprepare(stream->module);
		stream->module_enabled = false;
	}
	if (stream->module_claimed) {
		clk_rate_exclusive_put(stream->module);
		stream->module_claimed = false;
	}
	if (stream->parent) {
		clk_rate_exclusive_put(stream->parent);
		stream->parent = NULL;
	}
	if (stream->audio_enabled) {
		clk_disable_unprepare(scodec->a133_pll_audio);
		stream->audio_enabled = false;
	}
	if (stream->audio_claimed) {
		clk_rate_exclusive_put(scodec->a133_pll_audio);
		stream->audio_claimed = false;
	}
}

static void sun50i_a133_release_stream_clocks(void *data)
{
	struct sun4i_codec *scodec = data;

	mutex_lock(&scodec->a133_clock_lock);
	for (int i = 0; i < ARRAY_SIZE(scodec->a133_stream); i++)
		sun50i_a133_release_clocks(scodec, &scodec->a133_stream[i]);
	mutex_unlock(&scodec->a133_clock_lock);
}

static unsigned long sun50i_a133_module_rate(unsigned int rate)
{
	switch (rate) {
	case 11025:
	case 22050:
	case 44100:
		return 22579200;
	case 8000:
	case 12000:
	case 16000:
	case 24000:
	case 32000:
	case 48000:
	case 96000:
	case 192000:
		return 24576000;
	default:
		return 0;
	}
}

static int sun50i_a133_set_stream_clocks(struct sun4i_codec *scodec,
				       unsigned int rate, int direction)
{
	struct a133_stream_clocks *stream = &scodec->a133_stream[direction];
	struct clk *parent;
	unsigned long parent_rate, module_rate;
	int ret;

	if (sun50i_a133_module_rate(rate) == 22579200) {
		parent = scodec->a133_pll_com_audio;
		parent_rate = 90316800;
		module_rate = 22579200;
	} else if (sun50i_a133_module_rate(rate) == 24576000) {
		parent = scodec->a133_pll_audio;
		parent_rate = 98304000;
		module_rate = 24576000;
	} else {
		return -EINVAL;
	}

	/* Repeated hw_params must not reparent an enabled module. */
	sun50i_a133_release_clocks(scodec, stream);
	/*
	 * A133 playback with COM selected stalls unless PLL_AUDIO is also
	 * enabled. Holding it at 98.304 MHz, as the vendor does, restores
	 * sample-counter progress. At 48 kHz the DAC parent already enables
	 * PLL_AUDIO, so do not claim it twice.
	 */
	if (sun50i_a133_module_rate(rate) == 22579200) {
		ret = clk_set_rate_exclusive(scodec->a133_pll_audio, 98304000);
		if (ret)
			goto err_release;
		stream->audio_claimed = true;
		if (clk_get_rate(scodec->a133_pll_audio) != 98304000) {
			ret = -EINVAL;
			goto err_release;
		}
		ret = clk_prepare_enable(scodec->a133_pll_audio);
		if (ret)
			goto err_release;
		stream->audio_enabled = true;
	}
	ret = clk_set_rate_exclusive(parent, parent_rate);
	if (ret)
		goto err_release;
	stream->parent = parent;
	if (clk_get_rate(parent) != parent_rate) {
		ret = -EINVAL;
		goto err_release;
	}
	ret = clk_set_parent(stream->module, parent);
	if (ret)
		goto err_release;
	ret = clk_set_rate_exclusive(stream->module, module_rate);
	if (ret)
		goto err_release;
	stream->module_claimed = true;
	if (clk_get_rate(stream->module) != module_rate ||
	    !clk_is_match(clk_get_parent(stream->module), parent)) {
		ret = -EINVAL;
		goto err_release;
	}
	ret = clk_prepare_enable(stream->module);
	if (ret)
		goto err_release;
	stream->module_enabled = true;
	return 0;

err_release:
	sun50i_a133_release_clocks(scodec, stream);
	return ret;
}

static void sun4i_codec_start_playback(struct sun4i_codec *scodec)
{
	/* Flush TX FIFO */
	regmap_field_set_bits(scodec->reg_dac_fifoc,
			      BIT(SUN4I_CODEC_DAC_FIFOC_FIFO_FLUSH));

	/* Enable DAC DRQ */
	regmap_field_set_bits(scodec->reg_dac_fifoc,
			      BIT(SUN4I_CODEC_DAC_FIFOC_DAC_DRQ_EN));
}

static void sun4i_codec_stop_playback(struct sun4i_codec *scodec)
{
	/* Disable DAC DRQ */
	regmap_field_clear_bits(scodec->reg_dac_fifoc,
				BIT(SUN4I_CODEC_DAC_FIFOC_DAC_DRQ_EN));
}

static void sun4i_codec_start_capture(struct sun4i_codec *scodec)
{
	/* Enable ADC DRQ */
	regmap_field_set_bits(scodec->reg_adc_fifoc,
			      BIT(SUN4I_CODEC_ADC_FIFOC_ADC_DRQ_EN));
}

static void sun4i_codec_stop_capture(struct sun4i_codec *scodec)
{
	/* Disable ADC DRQ */
	regmap_field_clear_bits(scodec->reg_adc_fifoc,
				 BIT(SUN4I_CODEC_ADC_FIFOC_ADC_DRQ_EN));
}

static int sun4i_codec_trigger(struct snd_pcm_substream *substream, int cmd,
			       struct snd_soc_dai *dai)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct sun4i_codec *scodec = snd_soc_card_get_drvdata(rtd->card);

	if (scodec->a133 && substream->stream == SNDRV_PCM_STREAM_CAPTURE) {
		switch (cmd) {
		case SNDRV_PCM_TRIGGER_START:
		case SNDRV_PCM_TRIGGER_RESUME:
		case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
			return regmap_field_set_bits(scodec->reg_adc_fifoc, BIT(28) | BIT(3));
		case SNDRV_PCM_TRIGGER_STOP:
		case SNDRV_PCM_TRIGGER_SUSPEND:
		case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
			return regmap_field_clear_bits(scodec->reg_adc_fifoc, BIT(28) | BIT(3));
		default:
			return -EINVAL;
		}
	}

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
			sun4i_codec_start_playback(scodec);
		else
			sun4i_codec_start_capture(scodec);
		break;

	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
			sun4i_codec_stop_playback(scodec);
		else
			sun4i_codec_stop_capture(scodec);
		break;

	default:
		return -EINVAL;
	}

	return 0;
}

static int sun4i_codec_prepare_capture(struct snd_pcm_substream *substream,
				       struct snd_soc_dai *dai)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct sun4i_codec *scodec = snd_soc_card_get_drvdata(rtd->card);

	int ret;

	if (scodec->a133) {
		ret = regmap_field_set_bits(scodec->reg_adc_fifoc, BIT(0));
		if (ret)
			return ret;
		ret = regmap_write(scodec->regmap, SUN50I_A133_ADC_FIFOS, BIT(3) | BIT(1));
		if (ret)
			return ret;
		return regmap_write(scodec->regmap, SUN50I_A133_ADC_CNT, 0);
	}


	/* Flush RX FIFO */
	regmap_field_set_bits(scodec->reg_adc_fifoc,
				 BIT(SUN4I_CODEC_ADC_FIFOC_FIFO_FLUSH));


	/* Set RX FIFO trigger level */
	regmap_field_update_bits(scodec->reg_adc_fifoc,
				 0xf << SUN4I_CODEC_ADC_FIFOC_RX_TRIG_LEVEL,
				 0x7 << SUN4I_CODEC_ADC_FIFOC_RX_TRIG_LEVEL);

	/*
	 * FIXME: Undocumented in the datasheet, but
	 *        Allwinner's code mentions that it is
	 *        related to microphone gain
	 */
	if (of_device_is_compatible(scodec->dev->of_node,
				    "allwinner,sun4i-a10-codec") ||
	    of_device_is_compatible(scodec->dev->of_node,
				    "allwinner,sun7i-a20-codec")) {
		regmap_update_bits(scodec->regmap, SUN4I_CODEC_ADC_ACTL,
				   0x3 << 25,
				   0x1 << 25);
	}

	if (of_device_is_compatible(scodec->dev->of_node,
				    "allwinner,sun7i-a20-codec"))
		/* FIXME: Undocumented bits */
		regmap_update_bits(scodec->regmap, SUN4I_CODEC_DAC_TUNE,
				   0x3 << 8,
				   0x1 << 8);

	return 0;
}

static int sun4i_codec_prepare_playback(struct snd_pcm_substream *substream,
					struct snd_soc_dai *dai)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct sun4i_codec *scodec = snd_soc_card_get_drvdata(rtd->card);
	u32 val;
	int ret;

	/* Flush the TX FIFO */
	ret = regmap_field_set_bits(scodec->reg_dac_fifoc,
				   BIT(SUN4I_CODEC_DAC_FIFOC_FIFO_FLUSH));
	if (scodec->a133) {
		if (ret)
			return ret;
		/* W1C: TX empty, underrun and overrun status, not FIFO count. */
		ret = regmap_write(scodec->regmap, SUN50I_A133_DAC_FIFOS,
				   BIT(3) | BIT(2) | BIT(1));
		if (ret)
			return ret;
		ret = regmap_write(scodec->regmap, SUN50I_A133_DAC_CNT, 0);
		if (ret)
			return ret;
	}

	/* Set TX FIFO Empty Trigger Level */
	regmap_field_update_bits(scodec->reg_dac_fifoc,
				 0x3f << SUN4I_CODEC_DAC_FIFOC_TX_TRIG_LEVEL,
				 0xf << SUN4I_CODEC_DAC_FIFOC_TX_TRIG_LEVEL);

	if (substream->runtime->rate > 32000)
		/* Use 64 bits FIR filter */
		val = 0;
	else
		/* Use 32 bits FIR filter */
		val = BIT(SUN4I_CODEC_DAC_FIFOC_FIR_VERSION);

	regmap_field_update_bits(scodec->reg_dac_fifoc,
				 BIT(SUN4I_CODEC_DAC_FIFOC_FIR_VERSION),
				 val);

	/* Send zeros when we have an underrun */
	regmap_field_clear_bits(scodec->reg_dac_fifoc,
				BIT(SUN4I_CODEC_DAC_FIFOC_SEND_LASAT));

	return 0;
};

static int sun4i_codec_prepare(struct snd_pcm_substream *substream,
			       struct snd_soc_dai *dai)
{
	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
		return sun4i_codec_prepare_playback(substream, dai);

	return sun4i_codec_prepare_capture(substream, dai);
}

static unsigned long sun4i_codec_get_mod_freq(struct snd_pcm_hw_params *params)
{
	unsigned int rate = params_rate(params);

	switch (rate) {
	case 176400:
	case 88200:
	case 44100:
	case 33075:
	case 22050:
	case 14700:
	case 11025:
	case 7350:
		return 22579200;

	case 192000:
	case 96000:
	case 48000:
	case 32000:
	case 24000:
	case 16000:
	case 12000:
	case 8000:
		return 24576000;

	default:
		return 0;
	}
}

static int sun4i_codec_get_hw_rate(struct snd_pcm_hw_params *params)
{
	unsigned int rate = params_rate(params);

	switch (rate) {
	case 192000:
	case 176400:
		return 6;

	case 96000:
	case 88200:
		return 7;

	case 48000:
	case 44100:
		return 0;

	case 32000:
	case 33075:
		return 1;

	case 24000:
	case 22050:
		return 2;

	case 16000:
	case 14700:
		return 3;

	case 12000:
	case 11025:
		return 4;

	case 8000:
	case 7350:
		return 5;

	default:
		return -EINVAL;
	}
}

static int sun4i_codec_hw_params_capture(struct sun4i_codec *scodec,
					 struct snd_pcm_hw_params *params,
					 unsigned int hwrate)
{
	/* Set ADC sample rate */
	regmap_field_update_bits(scodec->reg_adc_fifoc,
				 7 << SUN4I_CODEC_ADC_FIFOC_ADC_FS,
				 hwrate << SUN4I_CODEC_ADC_FIFOC_ADC_FS);

	/* Set the number of channels we want to use */
	if (params_channels(params) == 1)
		regmap_field_set_bits(scodec->reg_adc_fifoc,
					 BIT(SUN4I_CODEC_ADC_FIFOC_MONO_EN));
	else
		regmap_field_clear_bits(scodec->reg_adc_fifoc,
					 BIT(SUN4I_CODEC_ADC_FIFOC_MONO_EN));

	/* Set the number of sample bits to either 16 or 24 bits */
	if (hw_param_interval(params, SNDRV_PCM_HW_PARAM_SAMPLE_BITS)->min == 32) {
		regmap_field_set_bits(scodec->reg_adc_fifoc,
				   BIT(SUN4I_CODEC_ADC_FIFOC_RX_SAMPLE_BITS));

		regmap_field_clear_bits(scodec->reg_adc_fifoc,
				   BIT(SUN4I_CODEC_ADC_FIFOC_RX_FIFO_MODE));

		scodec->capture_dma_data.addr_width = DMA_SLAVE_BUSWIDTH_4_BYTES;
	} else {
		regmap_field_clear_bits(scodec->reg_adc_fifoc,
				   BIT(SUN4I_CODEC_ADC_FIFOC_RX_SAMPLE_BITS));

		/* Fill most significant bits with valid data MSB */
		regmap_field_set_bits(scodec->reg_adc_fifoc,
				   BIT(SUN4I_CODEC_ADC_FIFOC_RX_FIFO_MODE));

		scodec->capture_dma_data.addr_width = DMA_SLAVE_BUSWIDTH_2_BYTES;
	}

	return 0;
}

static int sun4i_codec_hw_params_playback(struct sun4i_codec *scodec,
					  struct snd_pcm_hw_params *params,
					  unsigned int hwrate)
{
	u32 val;

	if (scodec->a133) {
		val = hwrate << 29;
		if (params_format(params) == SNDRV_PCM_FORMAT_S24_LE) {
			val |= BIT(5);
			scodec->playback_dma_data.addr_width = DMA_SLAVE_BUSWIDTH_4_BYTES;
		} else {
			val |= GENMASK(25, 24);
			scodec->playback_dma_data.addr_width = DMA_SLAVE_BUSWIDTH_2_BYTES;
		}
		return regmap_field_update_bits(scodec->reg_dac_fifoc,
			GENMASK(31, 29) | GENMASK(25, 24) | BIT(6) | BIT(5), val);
	}

	/* Set DAC sample rate */
	regmap_field_update_bits(scodec->reg_dac_fifoc,
				 7 << SUN4I_CODEC_DAC_FIFOC_DAC_FS,
				 hwrate << SUN4I_CODEC_DAC_FIFOC_DAC_FS);

	/* Set the number of channels we want to use */
	if (params_channels(params) == 1)
		val = BIT(SUN4I_CODEC_DAC_FIFOC_MONO_EN);
	else
		val = 0;

	regmap_field_update_bits(scodec->reg_dac_fifoc,
				 BIT(SUN4I_CODEC_DAC_FIFOC_MONO_EN),
				 val);

	/* Set the number of sample bits to either 16 or 24 bits */
	if (hw_param_interval(params, SNDRV_PCM_HW_PARAM_SAMPLE_BITS)->min == 32) {
		regmap_field_set_bits(scodec->reg_dac_fifoc,
				      BIT(SUN4I_CODEC_DAC_FIFOC_TX_SAMPLE_BITS));

		/* Set TX FIFO mode to padding the LSBs with 0 */
		regmap_field_clear_bits(scodec->reg_dac_fifoc,
					BIT(SUN4I_CODEC_DAC_FIFOC_TX_FIFO_MODE));

		scodec->playback_dma_data.addr_width = DMA_SLAVE_BUSWIDTH_4_BYTES;
	} else {
		regmap_field_clear_bits(scodec->reg_dac_fifoc,
					BIT(SUN4I_CODEC_DAC_FIFOC_TX_SAMPLE_BITS));

		/* Set TX FIFO mode to repeat the MSB */
		regmap_field_set_bits(scodec->reg_dac_fifoc,
				      BIT(SUN4I_CODEC_DAC_FIFOC_TX_FIFO_MODE));

		scodec->playback_dma_data.addr_width = DMA_SLAVE_BUSWIDTH_2_BYTES;
	}

	return 0;
}

/* Stock capture HPF coefficients depend on the PCM rate, not the PLL rate. */
static int a133_adc_hpf_rate(struct sun4i_codec *scodec, unsigned int rate)
{
	u32 coefficient;
	int ret;

	if (rate == 16000)
		coefficient = 0x00f623a5;
	else if (rate == 44100)
		coefficient = 0x00fc60db;
	else
		coefficient = 0x00fcabb3;

	ret = regmap_write(scodec->regmap, SUN50I_A133_ADC_HPF_H, coefficient >> 16);
	if (ret)
		return ret;
	return regmap_write(scodec->regmap, SUN50I_A133_ADC_HPF_L, coefficient & 0xffff);
}

static int sun4i_codec_hw_params(struct snd_pcm_substream *substream,
				 struct snd_pcm_hw_params *params,
				 struct snd_soc_dai *dai)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct sun4i_codec *scodec = snd_soc_card_get_drvdata(rtd->card);
	unsigned long clk_freq;
	int ret, hwrate;

	if (scodec->a133) {
		bool capture = substream->stream == SNDRV_PCM_STREAM_CAPTURE;
		struct a133_stream_clocks *stream = &scodec->a133_stream[substream->stream];
		u32 val;

		if (!sun50i_a133_module_rate(params_rate(params)) ||
		    params_channels(params) < 1 || params_channels(params) > 2 ||
		    (!capture && params_channels(params) != 2) ||
		    (capture && params_rate(params) > 48000) ||
		    (params_format(params) != SNDRV_PCM_FORMAT_S16_LE &&
		     params_format(params) != SNDRV_PCM_FORMAT_S24_LE))
			return -EINVAL;
		hwrate = sun4i_codec_get_hw_rate(params);
		if (hwrate < 0)
			return hwrate;
		mutex_lock(&scodec->a133_clock_lock);
		ret = sun50i_a133_set_stream_clocks(scodec, params_rate(params),
						  substream->stream);
		if (ret)
			goto a133_unlock;
		if (capture) {
			val = hwrate << 29;
			val |= (params_channels(params) == 1 ? 1 : 3) << 12;
			if (params_format(params) == SNDRV_PCM_FORMAT_S24_LE) {
				val |= BIT(16);
				scodec->capture_dma_data.addr_width = DMA_SLAVE_BUSWIDTH_4_BYTES;
			} else {
				val |= BIT(24);
				scodec->capture_dma_data.addr_width = DMA_SLAVE_BUSWIDTH_2_BYTES;
			}
			ret = regmap_field_update_bits(scodec->reg_adc_fifoc,
				GENMASK(31, 29) | BIT(24) | BIT(16) | GENMASK(13, 12), val);
			if (!ret)
				ret = a133_adc_hpf_rate(scodec, params_rate(params));
		} else {
			ret = sun4i_codec_hw_params_playback(scodec, params, hwrate);
		}
		if (ret)
			sun50i_a133_release_clocks(scodec, stream);
a133_unlock:
		mutex_unlock(&scodec->a133_clock_lock);
		return ret;
	}

	clk_freq = sun4i_codec_get_mod_freq(params);
	if (!clk_freq)
		return -EINVAL;

	ret = clk_set_rate(scodec->clk_module, clk_freq);
	if (ret)
		return ret;

	hwrate = sun4i_codec_get_hw_rate(params);
	if (hwrate < 0)
		return hwrate;

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
		return sun4i_codec_hw_params_playback(scodec, params,
						      hwrate);

	return sun4i_codec_hw_params_capture(scodec, params,
					     hwrate);
}

static int sun4i_codec_startup(struct snd_pcm_substream *substream,
			       struct snd_soc_dai *dai)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct sun4i_codec *scodec = snd_soc_card_get_drvdata(rtd->card);

	if (scodec->a133 && substream->stream == SNDRV_PCM_STREAM_CAPTURE)
		return 0;

	/*
	 * Stop issuing DRQ when we have room for less than 16 samples
	 * in our TX FIFO
	 */
	regmap_field_set_bits(scodec->reg_dac_fifoc,
			      3 << SUN4I_CODEC_DAC_FIFOC_DRQ_CLR_CNT);

	/* A133 hw_params selects its family before enabling the module. */
	if (scodec->a133)
		return 0;
	return clk_prepare_enable(scodec->clk_module);
}

static void sun4i_codec_shutdown(struct snd_pcm_substream *substream,
				 struct snd_soc_dai *dai)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct sun4i_codec *scodec = snd_soc_card_get_drvdata(rtd->card);

	if (scodec->a133) {
		mutex_lock(&scodec->a133_clock_lock);
		sun50i_a133_release_clocks(scodec, &scodec->a133_stream[substream->stream]);
		mutex_unlock(&scodec->a133_clock_lock);
	} else {
		clk_disable_unprepare(scodec->clk_module);
	}
}

/* Keep the requested stereo gains separate from the stream's mute state. */
static int a133_codec_mute_stream(struct snd_soc_dai *dai, int mute, int direction)
{
	struct sun4i_codec *scodec = snd_soc_component_get_drvdata(dai->component);
	int ret;

	if (direction != SNDRV_PCM_STREAM_PLAYBACK)
		return 0;

	mutex_lock(&scodec->a133_volume_lock);
	/* Stock clears selection and gains on mute, restoring selection on unmute. */
	ret = regmap_update_bits(scodec->regmap, SUN50I_A133_DAC_VOL,
				GENMASK(16, 0), mute ? 0 : BIT(16) | scodec->a133_dac_volume);
	if (!ret)
		scodec->a133_dac_muted = !!mute;
	mutex_unlock(&scodec->a133_volume_lock);
	return ret;
}

static const struct snd_soc_dai_ops a133_codec_dai_ops = {
	.startup = sun4i_codec_startup,
	.shutdown = sun4i_codec_shutdown,
	.trigger = sun4i_codec_trigger,
	.hw_params = sun4i_codec_hw_params,
	.prepare = sun4i_codec_prepare,
	.mute_stream = a133_codec_mute_stream,
	.no_capture_mute = 1,
};

static const struct snd_soc_dai_ops sun4i_codec_dai_ops = {
	.startup	= sun4i_codec_startup,
	.shutdown	= sun4i_codec_shutdown,
	.trigger	= sun4i_codec_trigger,
	.hw_params	= sun4i_codec_hw_params,
	.prepare	= sun4i_codec_prepare,
};

#define SUN4I_CODEC_RATES (			\
		SNDRV_PCM_RATE_8000_48000 |	\
		SNDRV_PCM_RATE_12000 |		\
		SNDRV_PCM_RATE_24000 |		\
		SNDRV_PCM_RATE_96000 |		\
		SNDRV_PCM_RATE_192000)

static struct snd_soc_dai_driver sun4i_codec_dai = {
	.name	= "Codec",
	.ops	= &sun4i_codec_dai_ops,
	.playback = {
		.stream_name	= "Codec Playback",
		.channels_min	= 1,
		.channels_max	= 2,
		.rate_min	= 8000,
		.rate_max	= 192000,
		.rates		= SUN4I_CODEC_RATES,
		.formats	= SNDRV_PCM_FMTBIT_S16_LE |
				  SNDRV_PCM_FMTBIT_S32_LE,
		.sig_bits	= 24,
	},
	.capture = {
		.stream_name	= "Codec Capture",
		.channels_min	= 1,
		.channels_max	= 2,
		.rate_min	= 8000,
		.rate_max	= 48000,
		.rates		= SUN4I_CODEC_RATES,
		.formats	= SNDRV_PCM_FMTBIT_S16_LE |
				  SNDRV_PCM_FMTBIT_S32_LE,
		.sig_bits	= 24,
	},
};

/*** sun4i Codec ***/
static const struct snd_kcontrol_new sun4i_codec_pa_mute =
	SOC_DAPM_SINGLE("Switch", SUN4I_CODEC_DAC_ACTL,
			SUN4I_CODEC_DAC_ACTL_PA_MUTE, 1, 0);

static DECLARE_TLV_DB_SCALE(sun4i_codec_pa_volume_scale, -6300, 100, 1);
static DECLARE_TLV_DB_SCALE(sun4i_codec_linein_loopback_gain_scale, -150, 150,
			    0);
static DECLARE_TLV_DB_SCALE(sun4i_codec_linein_preamp_gain_scale, -1200, 300,
			    0);
static DECLARE_TLV_DB_SCALE(sun4i_codec_fmin_loopback_gain_scale, -450, 150,
			    0);
static DECLARE_TLV_DB_SCALE(sun4i_codec_micin_loopback_gain_scale, -450, 150,
			    0);
static DECLARE_TLV_DB_RANGE(sun4i_codec_micin_preamp_gain_scale,
			    0, 0, TLV_DB_SCALE_ITEM(0, 0, 0),
			    1, 7, TLV_DB_SCALE_ITEM(3500, 300, 0));
static DECLARE_TLV_DB_RANGE(sun7i_codec_micin_preamp_gain_scale,
			    0, 0, TLV_DB_SCALE_ITEM(0, 0, 0),
			    1, 7, TLV_DB_SCALE_ITEM(2400, 300, 0));

static const struct snd_kcontrol_new sun4i_codec_controls[] = {
	SOC_SINGLE_TLV("Power Amplifier Volume", SUN4I_CODEC_DAC_ACTL,
		       SUN4I_CODEC_DAC_ACTL_PA_VOL, 0x3F, 0,
		       sun4i_codec_pa_volume_scale),
	SOC_SINGLE_TLV("Line Playback Volume", SUN4I_CODEC_DAC_ACTL,
		       SUN4I_CODEC_DAC_ACTL_LNG, 1, 0,
		       sun4i_codec_linein_loopback_gain_scale),
	SOC_SINGLE_TLV("Line Boost Volume", SUN4I_CODEC_ADC_ACTL,
		       SUN4I_CODEC_ADC_ACTL_LNPREG, 7, 0,
		       sun4i_codec_linein_preamp_gain_scale),
	SOC_SINGLE_TLV("FM Playback Volume", SUN4I_CODEC_DAC_ACTL,
		       SUN4I_CODEC_DAC_ACTL_FMG, 3, 0,
		       sun4i_codec_fmin_loopback_gain_scale),
	SOC_SINGLE_TLV("Mic Playback Volume", SUN4I_CODEC_DAC_ACTL,
		       SUN4I_CODEC_DAC_ACTL_MICG, 7, 0,
		       sun4i_codec_micin_loopback_gain_scale),
	SOC_SINGLE_TLV("Mic1 Boost Volume", SUN4I_CODEC_ADC_ACTL,
		       SUN4I_CODEC_ADC_ACTL_PREG1, 3, 0,
		       sun4i_codec_micin_preamp_gain_scale),
	SOC_SINGLE_TLV("Mic2 Boost Volume", SUN4I_CODEC_ADC_ACTL,
		       SUN4I_CODEC_ADC_ACTL_PREG2, 3, 0,
		       sun4i_codec_micin_preamp_gain_scale),
};

static const struct snd_kcontrol_new sun7i_codec_controls[] = {
	SOC_SINGLE_TLV("Power Amplifier Volume", SUN4I_CODEC_DAC_ACTL,
		       SUN4I_CODEC_DAC_ACTL_PA_VOL, 0x3F, 0,
		       sun4i_codec_pa_volume_scale),
	SOC_SINGLE_TLV("Line Playback Volume", SUN4I_CODEC_DAC_ACTL,
		       SUN4I_CODEC_DAC_ACTL_LNG, 1, 0,
		       sun4i_codec_linein_loopback_gain_scale),
	SOC_SINGLE_TLV("Line Boost Volume", SUN4I_CODEC_ADC_ACTL,
		       SUN4I_CODEC_ADC_ACTL_LNPREG, 7, 0,
		       sun4i_codec_linein_preamp_gain_scale),
	SOC_SINGLE_TLV("FM Playback Volume", SUN4I_CODEC_DAC_ACTL,
		       SUN4I_CODEC_DAC_ACTL_FMG, 3, 0,
		       sun4i_codec_fmin_loopback_gain_scale),
	SOC_SINGLE_TLV("Mic Playback Volume", SUN4I_CODEC_DAC_ACTL,
		       SUN4I_CODEC_DAC_ACTL_MICG, 7, 0,
		       sun4i_codec_micin_loopback_gain_scale),
	SOC_SINGLE_TLV("Mic1 Boost Volume", SUN7I_CODEC_AC_MIC_PHONE_CAL,
		       SUN7I_CODEC_AC_MIC_PHONE_CAL_PREG1, 7, 0,
		       sun7i_codec_micin_preamp_gain_scale),
	SOC_SINGLE_TLV("Mic2 Boost Volume", SUN7I_CODEC_AC_MIC_PHONE_CAL,
		       SUN7I_CODEC_AC_MIC_PHONE_CAL_PREG2, 7, 0,
		       sun7i_codec_micin_preamp_gain_scale),
};

static const struct snd_kcontrol_new sun4i_codec_mixer_controls[] = {
	SOC_DAPM_SINGLE("Left Mixer Left DAC Playback Switch",
			SUN4I_CODEC_DAC_ACTL, SUN4I_CODEC_DAC_ACTL_LDACLMIXS,
			1, 0),
	SOC_DAPM_SINGLE("Right Mixer Right DAC Playback Switch",
			SUN4I_CODEC_DAC_ACTL, SUN4I_CODEC_DAC_ACTL_RDACRMIXS,
			1, 0),
	SOC_DAPM_SINGLE("Right Mixer Left DAC Playback Switch",
			SUN4I_CODEC_DAC_ACTL,
			SUN4I_CODEC_DAC_ACTL_LDACRMIXS, 1, 0),
	SOC_DAPM_DOUBLE("Line Playback Switch", SUN4I_CODEC_DAC_ACTL,
			SUN4I_CODEC_DAC_ACTL_LLNS,
			SUN4I_CODEC_DAC_ACTL_RLNS, 1, 0),
	SOC_DAPM_DOUBLE("FM Playback Switch", SUN4I_CODEC_DAC_ACTL,
			SUN4I_CODEC_DAC_ACTL_LFMS,
			SUN4I_CODEC_DAC_ACTL_RFMS, 1, 0),
	SOC_DAPM_DOUBLE("Mic1 Playback Switch", SUN4I_CODEC_DAC_ACTL,
			SUN4I_CODEC_DAC_ACTL_MIC1LS,
			SUN4I_CODEC_DAC_ACTL_MIC1RS, 1, 0),
	SOC_DAPM_DOUBLE("Mic2 Playback Switch", SUN4I_CODEC_DAC_ACTL,
			SUN4I_CODEC_DAC_ACTL_MIC2LS,
			SUN4I_CODEC_DAC_ACTL_MIC2RS, 1, 0),
};

static const struct snd_kcontrol_new sun4i_codec_pa_mixer_controls[] = {
	SOC_DAPM_SINGLE("DAC Playback Switch", SUN4I_CODEC_DAC_ACTL,
			SUN4I_CODEC_DAC_ACTL_DACPAS, 1, 0),
	SOC_DAPM_SINGLE("Mixer Playback Switch", SUN4I_CODEC_DAC_ACTL,
			SUN4I_CODEC_DAC_ACTL_MIXPAS, 1, 0),
};

static const struct snd_soc_dapm_widget sun4i_codec_codec_dapm_widgets[] = {
	/* Digital parts of the ADCs */
	SND_SOC_DAPM_SUPPLY("ADC", SUN4I_CODEC_ADC_FIFOC,
			    SUN4I_CODEC_ADC_FIFOC_EN_AD, 0,
			    NULL, 0),

	/* Digital parts of the DACs */
	SND_SOC_DAPM_SUPPLY("DAC", SUN4I_CODEC_DAC_DPC,
			    SUN4I_CODEC_DAC_DPC_EN_DA, 0,
			    NULL, 0),

	/* Analog parts of the ADCs */
	SND_SOC_DAPM_ADC("Left ADC", "Codec Capture", SUN4I_CODEC_ADC_ACTL,
			 SUN4I_CODEC_ADC_ACTL_ADC_L_EN, 0),
	SND_SOC_DAPM_ADC("Right ADC", "Codec Capture", SUN4I_CODEC_ADC_ACTL,
			 SUN4I_CODEC_ADC_ACTL_ADC_R_EN, 0),

	/* Analog parts of the DACs */
	SND_SOC_DAPM_DAC("Left DAC", "Codec Playback", SUN4I_CODEC_DAC_ACTL,
			 SUN4I_CODEC_DAC_ACTL_DACAENL, 0),
	SND_SOC_DAPM_DAC("Right DAC", "Codec Playback", SUN4I_CODEC_DAC_ACTL,
			 SUN4I_CODEC_DAC_ACTL_DACAENR, 0),

	/* Mixers */
	SND_SOC_DAPM_MIXER("Left Mixer", SND_SOC_NOPM, 0, 0,
			   sun4i_codec_mixer_controls,
			   ARRAY_SIZE(sun4i_codec_mixer_controls)),
	SND_SOC_DAPM_MIXER("Right Mixer", SND_SOC_NOPM, 0, 0,
			   sun4i_codec_mixer_controls,
			   ARRAY_SIZE(sun4i_codec_mixer_controls)),

	/* Global Mixer Enable */
	SND_SOC_DAPM_SUPPLY("Mixer Enable", SUN4I_CODEC_DAC_ACTL,
			    SUN4I_CODEC_DAC_ACTL_MIXEN, 0, NULL, 0),

	/* VMIC */
	SND_SOC_DAPM_SUPPLY("VMIC", SUN4I_CODEC_ADC_ACTL,
			    SUN4I_CODEC_ADC_ACTL_VMICEN, 0, NULL, 0),

	/* Mic Pre-Amplifiers */
	SND_SOC_DAPM_PGA("MIC1 Pre-Amplifier", SUN4I_CODEC_ADC_ACTL,
			 SUN4I_CODEC_ADC_ACTL_PREG1EN, 0, NULL, 0),
	SND_SOC_DAPM_PGA("MIC2 Pre-Amplifier", SUN4I_CODEC_ADC_ACTL,
			 SUN4I_CODEC_ADC_ACTL_PREG2EN, 0, NULL, 0),

	/* Power Amplifier */
	SND_SOC_DAPM_MIXER("Power Amplifier", SUN4I_CODEC_ADC_ACTL,
			   SUN4I_CODEC_ADC_ACTL_PA_EN, 0,
			   sun4i_codec_pa_mixer_controls,
			   ARRAY_SIZE(sun4i_codec_pa_mixer_controls)),
	SND_SOC_DAPM_SWITCH("Power Amplifier Mute", SND_SOC_NOPM, 0, 0,
			    &sun4i_codec_pa_mute),

	SND_SOC_DAPM_INPUT("Line Right"),
	SND_SOC_DAPM_INPUT("Line Left"),
	SND_SOC_DAPM_INPUT("FM Right"),
	SND_SOC_DAPM_INPUT("FM Left"),
	SND_SOC_DAPM_INPUT("Mic1"),
	SND_SOC_DAPM_INPUT("Mic2"),

	SND_SOC_DAPM_OUTPUT("HP Right"),
	SND_SOC_DAPM_OUTPUT("HP Left"),
};

static const struct snd_soc_dapm_route sun4i_codec_codec_dapm_routes[] = {
	/* Left ADC / DAC Routes */
	{ "Left ADC", NULL, "ADC" },
	{ "Left DAC", NULL, "DAC" },

	/* Right ADC / DAC Routes */
	{ "Right ADC", NULL, "ADC" },
	{ "Right DAC", NULL, "DAC" },

	/* Right Mixer Routes */
	{ "Right Mixer", NULL, "Mixer Enable" },
	{ "Right Mixer", "Right Mixer Left DAC Playback Switch", "Left DAC" },
	{ "Right Mixer", "Right Mixer Right DAC Playback Switch", "Right DAC" },
	{ "Right Mixer", "Line Playback Switch", "Line Right" },
	{ "Right Mixer", "FM Playback Switch", "FM Right" },
	{ "Right Mixer", "Mic1 Playback Switch", "MIC1 Pre-Amplifier" },
	{ "Right Mixer", "Mic2 Playback Switch", "MIC2 Pre-Amplifier" },

	/* Left Mixer Routes */
	{ "Left Mixer", NULL, "Mixer Enable" },
	{ "Left Mixer", "Left Mixer Left DAC Playback Switch", "Left DAC" },
	{ "Left Mixer", "Line Playback Switch", "Line Left" },
	{ "Left Mixer", "FM Playback Switch", "FM Left" },
	{ "Left Mixer", "Mic1 Playback Switch", "MIC1 Pre-Amplifier" },
	{ "Left Mixer", "Mic2 Playback Switch", "MIC2 Pre-Amplifier" },

	/* Power Amplifier Routes */
	{ "Power Amplifier", "Mixer Playback Switch", "Left Mixer" },
	{ "Power Amplifier", "Mixer Playback Switch", "Right Mixer" },
	{ "Power Amplifier", "DAC Playback Switch", "Left DAC" },
	{ "Power Amplifier", "DAC Playback Switch", "Right DAC" },

	/* Headphone Output Routes */
	{ "Power Amplifier Mute", "Switch", "Power Amplifier" },
	{ "HP Right", NULL, "Power Amplifier Mute" },
	{ "HP Left", NULL, "Power Amplifier Mute" },

	/* Mic1 Routes */
	{ "Left ADC", NULL, "MIC1 Pre-Amplifier" },
	{ "Right ADC", NULL, "MIC1 Pre-Amplifier" },
	{ "MIC1 Pre-Amplifier", NULL, "Mic1"},
	{ "Mic1", NULL, "VMIC" },

	/* Mic2 Routes */
	{ "Left ADC", NULL, "MIC2 Pre-Amplifier" },
	{ "Right ADC", NULL, "MIC2 Pre-Amplifier" },
	{ "MIC2 Pre-Amplifier", NULL, "Mic2"},
	{ "Mic2", NULL, "VMIC" },
};

static const struct snd_soc_component_driver sun4i_codec_codec = {
	.controls		= sun4i_codec_controls,
	.num_controls		= ARRAY_SIZE(sun4i_codec_controls),
	.dapm_widgets		= sun4i_codec_codec_dapm_widgets,
	.num_dapm_widgets	= ARRAY_SIZE(sun4i_codec_codec_dapm_widgets),
	.dapm_routes		= sun4i_codec_codec_dapm_routes,
	.num_dapm_routes	= ARRAY_SIZE(sun4i_codec_codec_dapm_routes),
	.idle_bias_on		= 1,
	.use_pmdown_time	= 1,
	.endianness		= 1,
};

static const struct snd_soc_component_driver sun7i_codec_codec = {
	.controls		= sun7i_codec_controls,
	.num_controls		= ARRAY_SIZE(sun7i_codec_controls),
	.dapm_widgets		= sun4i_codec_codec_dapm_widgets,
	.num_dapm_widgets	= ARRAY_SIZE(sun4i_codec_codec_dapm_widgets),
	.dapm_routes		= sun4i_codec_codec_dapm_routes,
	.num_dapm_routes	= ARRAY_SIZE(sun4i_codec_codec_dapm_routes),
	.idle_bias_on		= 1,
	.use_pmdown_time	= 1,
	.endianness		= 1,
};

/*** sun6i Codec ***/

/* mixer controls */
static const struct snd_kcontrol_new sun6i_codec_mixer_controls[] = {
	SOC_DAPM_DOUBLE("DAC Playback Switch",
			SUN6I_CODEC_OM_DACA_CTRL,
			SUN6I_CODEC_OM_DACA_CTRL_LMIX_DACL,
			SUN6I_CODEC_OM_DACA_CTRL_RMIX_DACR, 1, 0),
	SOC_DAPM_DOUBLE("DAC Reversed Playback Switch",
			SUN6I_CODEC_OM_DACA_CTRL,
			SUN6I_CODEC_OM_DACA_CTRL_LMIX_DACR,
			SUN6I_CODEC_OM_DACA_CTRL_RMIX_DACL, 1, 0),
	SOC_DAPM_DOUBLE("Line In Playback Switch",
			SUN6I_CODEC_OM_DACA_CTRL,
			SUN6I_CODEC_OM_DACA_CTRL_LMIX_LINEINL,
			SUN6I_CODEC_OM_DACA_CTRL_RMIX_LINEINR, 1, 0),
	SOC_DAPM_DOUBLE("Mic1 Playback Switch",
			SUN6I_CODEC_OM_DACA_CTRL,
			SUN6I_CODEC_OM_DACA_CTRL_LMIX_MIC1,
			SUN6I_CODEC_OM_DACA_CTRL_RMIX_MIC1, 1, 0),
	SOC_DAPM_DOUBLE("Mic2 Playback Switch",
			SUN6I_CODEC_OM_DACA_CTRL,
			SUN6I_CODEC_OM_DACA_CTRL_LMIX_MIC2,
			SUN6I_CODEC_OM_DACA_CTRL_RMIX_MIC2, 1, 0),
};

/* ADC mixer controls */
static const struct snd_kcontrol_new sun6i_codec_adc_mixer_controls[] = {
	SOC_DAPM_DOUBLE("Mixer Capture Switch",
			SUN6I_CODEC_ADC_ACTL,
			SUN6I_CODEC_ADC_ACTL_LADCMIX_OMIXL,
			SUN6I_CODEC_ADC_ACTL_RADCMIX_OMIXR, 1, 0),
	SOC_DAPM_DOUBLE("Mixer Reversed Capture Switch",
			SUN6I_CODEC_ADC_ACTL,
			SUN6I_CODEC_ADC_ACTL_LADCMIX_OMIXR,
			SUN6I_CODEC_ADC_ACTL_RADCMIX_OMIXL, 1, 0),
	SOC_DAPM_DOUBLE("Line In Capture Switch",
			SUN6I_CODEC_ADC_ACTL,
			SUN6I_CODEC_ADC_ACTL_LADCMIX_LINEINL,
			SUN6I_CODEC_ADC_ACTL_RADCMIX_LINEINR, 1, 0),
	SOC_DAPM_DOUBLE("Mic1 Capture Switch",
			SUN6I_CODEC_ADC_ACTL,
			SUN6I_CODEC_ADC_ACTL_LADCMIX_MIC1,
			SUN6I_CODEC_ADC_ACTL_RADCMIX_MIC1, 1, 0),
	SOC_DAPM_DOUBLE("Mic2 Capture Switch",
			SUN6I_CODEC_ADC_ACTL,
			SUN6I_CODEC_ADC_ACTL_LADCMIX_MIC2,
			SUN6I_CODEC_ADC_ACTL_RADCMIX_MIC2, 1, 0),
};

/* headphone controls */
static const char * const sun6i_codec_hp_src_enum_text[] = {
	"DAC", "Mixer",
};

static SOC_ENUM_DOUBLE_DECL(sun6i_codec_hp_src_enum,
			    SUN6I_CODEC_OM_DACA_CTRL,
			    SUN6I_CODEC_OM_DACA_CTRL_LHPIS,
			    SUN6I_CODEC_OM_DACA_CTRL_RHPIS,
			    sun6i_codec_hp_src_enum_text);

static const struct snd_kcontrol_new sun6i_codec_hp_src[] = {
	SOC_DAPM_ENUM("Headphone Source Playback Route",
		      sun6i_codec_hp_src_enum),
};

/* microphone controls */
static const char * const sun6i_codec_mic2_src_enum_text[] = {
	"Mic2", "Mic3",
};

static SOC_ENUM_SINGLE_DECL(sun6i_codec_mic2_src_enum,
			    SUN6I_CODEC_MIC_CTRL,
			    SUN6I_CODEC_MIC_CTRL_MIC2SLT,
			    sun6i_codec_mic2_src_enum_text);

static const struct snd_kcontrol_new sun6i_codec_mic2_src[] = {
	SOC_DAPM_ENUM("Mic2 Amplifier Source Route",
		      sun6i_codec_mic2_src_enum),
};

/* line out controls */
static const char * const sun6i_codec_lineout_src_enum_text[] = {
	"Stereo", "Mono Differential",
};

static SOC_ENUM_DOUBLE_DECL(sun6i_codec_lineout_src_enum,
			    SUN6I_CODEC_MIC_CTRL,
			    SUN6I_CODEC_MIC_CTRL_LINEOUTLSRC,
			    SUN6I_CODEC_MIC_CTRL_LINEOUTRSRC,
			    sun6i_codec_lineout_src_enum_text);

static const struct snd_kcontrol_new sun6i_codec_lineout_src[] = {
	SOC_DAPM_ENUM("Line Out Source Playback Route",
		      sun6i_codec_lineout_src_enum),
};

/* volume / mute controls */
static const DECLARE_TLV_DB_SCALE(sun6i_codec_dvol_scale, -7308, 116, 0);
static const DECLARE_TLV_DB_SCALE(sun6i_codec_hp_vol_scale, -6300, 100, 1);
static const DECLARE_TLV_DB_SCALE(sun6i_codec_out_mixer_pregain_scale,
				  -450, 150, 0);
static const DECLARE_TLV_DB_RANGE(sun6i_codec_lineout_vol_scale,
	0, 1, TLV_DB_SCALE_ITEM(TLV_DB_GAIN_MUTE, 0, 1),
	2, 31, TLV_DB_SCALE_ITEM(-4350, 150, 0),
);
static const DECLARE_TLV_DB_RANGE(sun6i_codec_mic_gain_scale,
	0, 0, TLV_DB_SCALE_ITEM(0, 0, 0),
	1, 7, TLV_DB_SCALE_ITEM(2400, 300, 0),
);

static const struct snd_kcontrol_new sun6i_codec_codec_widgets[] = {
	SOC_SINGLE_TLV("DAC Playback Volume", SUN4I_CODEC_DAC_DPC,
		       SUN4I_CODEC_DAC_DPC_DVOL, 0x3f, 1,
		       sun6i_codec_dvol_scale),
	SOC_SINGLE_TLV("Headphone Playback Volume",
		       SUN6I_CODEC_OM_DACA_CTRL,
		       SUN6I_CODEC_OM_DACA_CTRL_HPVOL, 0x3f, 0,
		       sun6i_codec_hp_vol_scale),
	SOC_SINGLE_TLV("Line Out Playback Volume",
		       SUN6I_CODEC_MIC_CTRL,
		       SUN6I_CODEC_MIC_CTRL_LINEOUTVC, 0x1f, 0,
		       sun6i_codec_lineout_vol_scale),
	SOC_DOUBLE("Headphone Playback Switch",
		   SUN6I_CODEC_OM_DACA_CTRL,
		   SUN6I_CODEC_OM_DACA_CTRL_LHPPAMUTE,
		   SUN6I_CODEC_OM_DACA_CTRL_RHPPAMUTE, 1, 0),
	SOC_DOUBLE("Line Out Playback Switch",
		   SUN6I_CODEC_MIC_CTRL,
		   SUN6I_CODEC_MIC_CTRL_LINEOUTLEN,
		   SUN6I_CODEC_MIC_CTRL_LINEOUTREN, 1, 0),
	/* Mixer pre-gains */
	SOC_SINGLE_TLV("Line In Playback Volume",
		       SUN6I_CODEC_OM_PA_CTRL, SUN6I_CODEC_OM_PA_CTRL_LINEING,
		       0x7, 0, sun6i_codec_out_mixer_pregain_scale),
	SOC_SINGLE_TLV("Mic1 Playback Volume",
		       SUN6I_CODEC_OM_PA_CTRL, SUN6I_CODEC_OM_PA_CTRL_MIC1G,
		       0x7, 0, sun6i_codec_out_mixer_pregain_scale),
	SOC_SINGLE_TLV("Mic2 Playback Volume",
		       SUN6I_CODEC_OM_PA_CTRL, SUN6I_CODEC_OM_PA_CTRL_MIC2G,
		       0x7, 0, sun6i_codec_out_mixer_pregain_scale),

	/* Microphone Amp boost gains */
	SOC_SINGLE_TLV("Mic1 Boost Volume", SUN6I_CODEC_MIC_CTRL,
		       SUN6I_CODEC_MIC_CTRL_MIC1BOOST, 0x7, 0,
		       sun6i_codec_mic_gain_scale),
	SOC_SINGLE_TLV("Mic2 Boost Volume", SUN6I_CODEC_MIC_CTRL,
		       SUN6I_CODEC_MIC_CTRL_MIC2BOOST, 0x7, 0,
		       sun6i_codec_mic_gain_scale),
	SOC_DOUBLE_TLV("ADC Capture Volume",
		       SUN6I_CODEC_ADC_ACTL, SUN6I_CODEC_ADC_ACTL_ADCLG,
		       SUN6I_CODEC_ADC_ACTL_ADCRG, 0x7, 0,
		       sun6i_codec_out_mixer_pregain_scale),
};

static const struct snd_soc_dapm_widget sun6i_codec_codec_dapm_widgets[] = {
	/* Microphone inputs */
	SND_SOC_DAPM_INPUT("MIC1"),
	SND_SOC_DAPM_INPUT("MIC2"),
	SND_SOC_DAPM_INPUT("MIC3"),

	/* Microphone Bias */
	SND_SOC_DAPM_SUPPLY("HBIAS", SUN6I_CODEC_MIC_CTRL,
			    SUN6I_CODEC_MIC_CTRL_HBIASEN, 0, NULL, 0),
	SND_SOC_DAPM_SUPPLY("MBIAS", SUN6I_CODEC_MIC_CTRL,
			    SUN6I_CODEC_MIC_CTRL_MBIASEN, 0, NULL, 0),

	/* Mic input path */
	SND_SOC_DAPM_MUX("Mic2 Amplifier Source Route",
			 SND_SOC_NOPM, 0, 0, sun6i_codec_mic2_src),
	SND_SOC_DAPM_PGA("Mic1 Amplifier", SUN6I_CODEC_MIC_CTRL,
			 SUN6I_CODEC_MIC_CTRL_MIC1AMPEN, 0, NULL, 0),
	SND_SOC_DAPM_PGA("Mic2 Amplifier", SUN6I_CODEC_MIC_CTRL,
			 SUN6I_CODEC_MIC_CTRL_MIC2AMPEN, 0, NULL, 0),

	/* Line In */
	SND_SOC_DAPM_INPUT("LINEIN"),

	/* Digital parts of the ADCs */
	SND_SOC_DAPM_SUPPLY("ADC Enable", SUN6I_CODEC_ADC_FIFOC,
			    SUN6I_CODEC_ADC_FIFOC_EN_AD, 0,
			    NULL, 0),

	/* Analog parts of the ADCs */
	SND_SOC_DAPM_ADC("Left ADC", "Codec Capture", SUN6I_CODEC_ADC_ACTL,
			 SUN6I_CODEC_ADC_ACTL_ADCLEN, 0),
	SND_SOC_DAPM_ADC("Right ADC", "Codec Capture", SUN6I_CODEC_ADC_ACTL,
			 SUN6I_CODEC_ADC_ACTL_ADCREN, 0),

	/* ADC Mixers */
	SOC_MIXER_ARRAY("Left ADC Mixer", SND_SOC_NOPM, 0, 0,
			sun6i_codec_adc_mixer_controls),
	SOC_MIXER_ARRAY("Right ADC Mixer", SND_SOC_NOPM, 0, 0,
			sun6i_codec_adc_mixer_controls),

	/* Digital parts of the DACs */
	SND_SOC_DAPM_SUPPLY("DAC Enable", SUN4I_CODEC_DAC_DPC,
			    SUN4I_CODEC_DAC_DPC_EN_DA, 0,
			    NULL, 0),

	/* Analog parts of the DACs */
	SND_SOC_DAPM_DAC("Left DAC", "Codec Playback",
			 SUN6I_CODEC_OM_DACA_CTRL,
			 SUN6I_CODEC_OM_DACA_CTRL_DACALEN, 0),
	SND_SOC_DAPM_DAC("Right DAC", "Codec Playback",
			 SUN6I_CODEC_OM_DACA_CTRL,
			 SUN6I_CODEC_OM_DACA_CTRL_DACAREN, 0),

	/* Mixers */
	SOC_MIXER_ARRAY("Left Mixer", SUN6I_CODEC_OM_DACA_CTRL,
			SUN6I_CODEC_OM_DACA_CTRL_LMIXEN, 0,
			sun6i_codec_mixer_controls),
	SOC_MIXER_ARRAY("Right Mixer", SUN6I_CODEC_OM_DACA_CTRL,
			SUN6I_CODEC_OM_DACA_CTRL_RMIXEN, 0,
			sun6i_codec_mixer_controls),

	/* Headphone output path */
	SND_SOC_DAPM_MUX("Headphone Source Playback Route",
			 SND_SOC_NOPM, 0, 0, sun6i_codec_hp_src),
	SND_SOC_DAPM_OUT_DRV("Headphone Amp", SUN6I_CODEC_OM_PA_CTRL,
			     SUN6I_CODEC_OM_PA_CTRL_HPPAEN, 0, NULL, 0),
	SND_SOC_DAPM_SUPPLY("HPCOM Protection", SUN6I_CODEC_OM_PA_CTRL,
			    SUN6I_CODEC_OM_PA_CTRL_COMPTEN, 0, NULL, 0),
	SND_SOC_DAPM_REG(snd_soc_dapm_supply, "HPCOM", SUN6I_CODEC_OM_PA_CTRL,
			 SUN6I_CODEC_OM_PA_CTRL_HPCOM_CTL, 0x3, 0x3, 0),
	SND_SOC_DAPM_OUTPUT("HP"),

	/* Line Out path */
	SND_SOC_DAPM_MUX("Line Out Source Playback Route",
			 SND_SOC_NOPM, 0, 0, sun6i_codec_lineout_src),
	SND_SOC_DAPM_OUTPUT("LINEOUT"),
};

static const struct snd_soc_dapm_route sun6i_codec_codec_dapm_routes[] = {
	/* DAC Routes */
	{ "Left DAC", NULL, "DAC Enable" },
	{ "Right DAC", NULL, "DAC Enable" },

	/* Microphone Routes */
	{ "Mic1 Amplifier", NULL, "MIC1"},
	{ "Mic2 Amplifier Source Route", "Mic2", "MIC2" },
	{ "Mic2 Amplifier Source Route", "Mic3", "MIC3" },
	{ "Mic2 Amplifier", NULL, "Mic2 Amplifier Source Route"},

	/* Left Mixer Routes */
	{ "Left Mixer", "DAC Playback Switch", "Left DAC" },
	{ "Left Mixer", "DAC Reversed Playback Switch", "Right DAC" },
	{ "Left Mixer", "Line In Playback Switch", "LINEIN" },
	{ "Left Mixer", "Mic1 Playback Switch", "Mic1 Amplifier" },
	{ "Left Mixer", "Mic2 Playback Switch", "Mic2 Amplifier" },

	/* Right Mixer Routes */
	{ "Right Mixer", "DAC Playback Switch", "Right DAC" },
	{ "Right Mixer", "DAC Reversed Playback Switch", "Left DAC" },
	{ "Right Mixer", "Line In Playback Switch", "LINEIN" },
	{ "Right Mixer", "Mic1 Playback Switch", "Mic1 Amplifier" },
	{ "Right Mixer", "Mic2 Playback Switch", "Mic2 Amplifier" },

	/* Left ADC Mixer Routes */
	{ "Left ADC Mixer", "Mixer Capture Switch", "Left Mixer" },
	{ "Left ADC Mixer", "Mixer Reversed Capture Switch", "Right Mixer" },
	{ "Left ADC Mixer", "Line In Capture Switch", "LINEIN" },
	{ "Left ADC Mixer", "Mic1 Capture Switch", "Mic1 Amplifier" },
	{ "Left ADC Mixer", "Mic2 Capture Switch", "Mic2 Amplifier" },

	/* Right ADC Mixer Routes */
	{ "Right ADC Mixer", "Mixer Capture Switch", "Right Mixer" },
	{ "Right ADC Mixer", "Mixer Reversed Capture Switch", "Left Mixer" },
	{ "Right ADC Mixer", "Line In Capture Switch", "LINEIN" },
	{ "Right ADC Mixer", "Mic1 Capture Switch", "Mic1 Amplifier" },
	{ "Right ADC Mixer", "Mic2 Capture Switch", "Mic2 Amplifier" },

	/* Headphone Routes */
	{ "Headphone Source Playback Route", "DAC", "Left DAC" },
	{ "Headphone Source Playback Route", "DAC", "Right DAC" },
	{ "Headphone Source Playback Route", "Mixer", "Left Mixer" },
	{ "Headphone Source Playback Route", "Mixer", "Right Mixer" },
	{ "Headphone Amp", NULL, "Headphone Source Playback Route" },
	{ "HP", NULL, "Headphone Amp" },
	{ "HPCOM", NULL, "HPCOM Protection" },

	/* Line Out Routes */
	{ "Line Out Source Playback Route", "Stereo", "Left Mixer" },
	{ "Line Out Source Playback Route", "Stereo", "Right Mixer" },
	{ "Line Out Source Playback Route", "Mono Differential", "Left Mixer" },
	{ "Line Out Source Playback Route", "Mono Differential", "Right Mixer" },
	{ "LINEOUT", NULL, "Line Out Source Playback Route" },

	/* ADC Routes */
	{ "Left ADC", NULL, "ADC Enable" },
	{ "Right ADC", NULL, "ADC Enable" },
	{ "Left ADC", NULL, "Left ADC Mixer" },
	{ "Right ADC", NULL, "Right ADC Mixer" },
};

static const struct snd_soc_component_driver sun6i_codec_codec = {
	.controls		= sun6i_codec_codec_widgets,
	.num_controls		= ARRAY_SIZE(sun6i_codec_codec_widgets),
	.dapm_widgets		= sun6i_codec_codec_dapm_widgets,
	.num_dapm_widgets	= ARRAY_SIZE(sun6i_codec_codec_dapm_widgets),
	.dapm_routes		= sun6i_codec_codec_dapm_routes,
	.num_dapm_routes	= ARRAY_SIZE(sun6i_codec_codec_dapm_routes),
	.idle_bias_on		= 1,
	.use_pmdown_time	= 1,
	.endianness		= 1,
};

/* sun8i A23 codec */
static const struct snd_kcontrol_new sun8i_a23_codec_codec_controls[] = {
	SOC_SINGLE_TLV("DAC Playback Volume", SUN4I_CODEC_DAC_DPC,
		       SUN4I_CODEC_DAC_DPC_DVOL, 0x3f, 1,
		       sun6i_codec_dvol_scale),
};

static const struct snd_soc_dapm_widget sun8i_a23_codec_codec_widgets[] = {
	/* Digital parts of the ADCs */
	SND_SOC_DAPM_SUPPLY("ADC Enable", SUN6I_CODEC_ADC_FIFOC,
			    SUN6I_CODEC_ADC_FIFOC_EN_AD, 0, NULL, 0),
	/* Digital parts of the DACs */
	SND_SOC_DAPM_SUPPLY("DAC Enable", SUN4I_CODEC_DAC_DPC,
			    SUN4I_CODEC_DAC_DPC_EN_DA, 0, NULL, 0),

};

static const struct snd_soc_component_driver sun8i_a23_codec_codec = {
	.controls		= sun8i_a23_codec_codec_controls,
	.num_controls		= ARRAY_SIZE(sun8i_a23_codec_codec_controls),
	.dapm_widgets		= sun8i_a23_codec_codec_widgets,
	.num_dapm_widgets	= ARRAY_SIZE(sun8i_a23_codec_codec_widgets),
	.idle_bias_on		= 1,
	.use_pmdown_time	= 1,
	.endianness		= 1,
};

/*suniv F1C100s codec */

/* headphone controls */
static const char * const suniv_codec_hp_src_enum_text[] = {
	"DAC", "Mixer",
};

static SOC_ENUM_DOUBLE_DECL(suniv_codec_hp_src_enum,
			    SUNIV_CODEC_OM_DACA_CTRL,
			    SUNIV_CODEC_OM_DACA_CTRL_LHPIS,
			    SUNIV_CODEC_OM_DACA_CTRL_RHPIS,
			    suniv_codec_hp_src_enum_text);

static const struct snd_kcontrol_new suniv_codec_hp_src[] = {
	SOC_DAPM_ENUM("Headphone Source Playback Route",
		      suniv_codec_hp_src_enum),
};

/* mixer controls */
static const struct snd_kcontrol_new suniv_codec_adc_mixer_controls[] = {
	SOC_DAPM_SINGLE("Right Out Capture Switch", SUNIV_CODEC_ADC_ACTL,
			SUNIV_CODEC_ADC_ADCMIX_ROUT, 1, 0),
	SOC_DAPM_SINGLE("Left Out Capture Switch", SUNIV_CODEC_ADC_ACTL,
			SUNIV_CODEC_ADC_ADCMIX_LOUT, 1, 0),
	SOC_DAPM_SINGLE("Line In Capture Switch", SUNIV_CODEC_ADC_ACTL,
			SUNIV_CODEC_ADC_ADCMIX_LINEIN, 1, 0),
	SOC_DAPM_SINGLE("Right FM In Capture Switch", SUNIV_CODEC_ADC_ACTL,
			SUNIV_CODEC_ADC_ADCMIX_FMINR, 1, 0),
	SOC_DAPM_SINGLE("Left FM In Capture Switch", SUNIV_CODEC_ADC_ACTL,
			SUNIV_CODEC_ADC_ADCMIX_FMINL, 1, 0),
	SOC_DAPM_SINGLE("Mic Capture Switch", SUNIV_CODEC_ADC_ACTL,
			SUNIV_CODEC_ADC_ADCMIX_MIC, 1, 0),
};

static const struct snd_kcontrol_new suniv_codec_dac_lmixer_controls[] = {
	SOC_DAPM_SINGLE("Right DAC Playback Switch", SUNIV_CODEC_OM_DACA_CTRL,
			SUNIV_CODEC_OM_DACA_CTRL_LMIXMUTE_RDAC, 1, 0),
	SOC_DAPM_SINGLE("Left DAC Playback Switch", SUNIV_CODEC_OM_DACA_CTRL,
			SUNIV_CODEC_OM_DACA_CTRL_LMIXMUTE_LDAC, 1, 0),
	SOC_DAPM_SINGLE("FM In Playback Switch", SUNIV_CODEC_OM_DACA_CTRL,
			SUNIV_CODEC_OM_DACA_CTRL_LMIXMUTE_FMIN, 1, 0),
	SOC_DAPM_SINGLE("Line In Playback Switch", SUNIV_CODEC_OM_DACA_CTRL,
			SUNIV_CODEC_OM_DACA_CTRL_LMIXMUTE_LINEIN, 1, 0),
	SOC_DAPM_SINGLE("Mic In Playback Switch", SUNIV_CODEC_OM_DACA_CTRL,
			SUNIV_CODEC_OM_DACA_CTRL_LMIXMUTE_MICIN, 1, 0),
};

static const struct snd_kcontrol_new suniv_codec_dac_rmixer_controls[] = {
	SOC_DAPM_SINGLE("Left DAC Playback Switch", SUNIV_CODEC_OM_DACA_CTRL,
			SUNIV_CODEC_OM_DACA_CTRL_RMIXMUTE_LDAC, 1, 0),
	SOC_DAPM_SINGLE("Right DAC Playback Switch", SUNIV_CODEC_OM_DACA_CTRL,
			SUNIV_CODEC_OM_DACA_CTRL_RMIXMUTE_RDAC, 1, 0),
	SOC_DAPM_SINGLE("FM In Playback Switch", SUNIV_CODEC_OM_DACA_CTRL,
			SUNIV_CODEC_OM_DACA_CTRL_RMIXMUTE_FMIN, 1, 0),
	SOC_DAPM_SINGLE("Line In Playback Switch", SUNIV_CODEC_OM_DACA_CTRL,
			SUNIV_CODEC_OM_DACA_CTRL_RMIXMUTE_LINEIN, 1, 0),
	SOC_DAPM_SINGLE("Mic In Playback Switch", SUNIV_CODEC_OM_DACA_CTRL,
			SUNIV_CODEC_OM_DACA_CTRL_RMIXMUTE_MICIN, 1, 0),
};

static const DECLARE_TLV_DB_SCALE(suniv_codec_dvol_scale, -7308, 116, 0);
static const DECLARE_TLV_DB_SCALE(suniv_codec_hp_vol_scale, -6300, 100, 1);
static const DECLARE_TLV_DB_SCALE(suniv_codec_out_mixer_pregain_scale,
				  -450, 150, 0);

static const DECLARE_TLV_DB_RANGE(suniv_codec_mic_gain_scale,
	0, 0, TLV_DB_SCALE_ITEM(0, 0, 0),
	1, 7, TLV_DB_SCALE_ITEM(2400, 300, 0),
);

static const struct snd_kcontrol_new suniv_codec_codec_widgets[] = {
	SOC_SINGLE_TLV("DAC Playback Volume", SUN4I_CODEC_DAC_DPC,
		       SUN4I_CODEC_DAC_DPC_DVOL, 0x3f, 1,
		       suniv_codec_dvol_scale),
	SOC_SINGLE_TLV("Headphone Playback Volume",
		       SUNIV_CODEC_OM_DACA_CTRL,
		       SUNIV_CODEC_OM_DACA_CTRL_HPVOL, 0x3f, 0,
		       suniv_codec_hp_vol_scale),
	SOC_DOUBLE("Headphone Playback Switch",
		   SUNIV_CODEC_OM_DACA_CTRL,
		   SUNIV_CODEC_OM_DACA_CTRL_LHPPAMUTE,
		   SUNIV_CODEC_OM_DACA_CTRL_RHPPAMUTE, 1, 0),
	SOC_SINGLE_TLV("Line In Playback Volume",
		       SUNIV_CODEC_ADC_ACTL, SUNIV_CODEC_ADC_LINEINVOL,
		       0x7, 0, suniv_codec_out_mixer_pregain_scale),
	SOC_SINGLE_TLV("FM In Playback Volume",
		       SUNIV_CODEC_ADC_ACTL, SUNIV_CODEC_ADC_FMINVOL,
		       0x7, 0, suniv_codec_out_mixer_pregain_scale),
	SOC_SINGLE_TLV("Mic In Playback Volume",
		       SUNIV_CODEC_ADC_ACTL, SUNIV_CODEC_ADC_MICG,
		       0x7, 0, suniv_codec_out_mixer_pregain_scale),

	/* Microphone Amp boost gains */
	SOC_SINGLE_TLV("Mic Boost Volume", SUNIV_CODEC_ADC_ACTL,
		       SUNIV_CODEC_ADC_MICBOOST, 0x7, 0,
		       suniv_codec_mic_gain_scale),
	SOC_SINGLE_TLV("ADC Capture Volume",
		       SUNIV_CODEC_ADC_ACTL, SUNIV_CODEC_ADC_ADCG,
		       0x7, 0, suniv_codec_out_mixer_pregain_scale),
};

static const struct snd_soc_dapm_widget suniv_codec_codec_dapm_widgets[] = {
	/* Microphone inputs */
	SND_SOC_DAPM_INPUT("MIC"),

	/* Microphone Bias */
	/* deleted: HBIAS, MBIAS */

	/* Mic input path */
	SND_SOC_DAPM_PGA("Mic Amplifier", SUNIV_CODEC_ADC_ACTL,
			 SUNIV_CODEC_ADC_MICAMPEN, 0, NULL, 0),

	/* Line In */
	SND_SOC_DAPM_INPUT("LINEIN"),

	/* FM In */
	SND_SOC_DAPM_INPUT("FMINR"),
	SND_SOC_DAPM_INPUT("FMINL"),

	/* Digital parts of the ADCs */
	SND_SOC_DAPM_SUPPLY("ADC Enable", SUNIV_CODEC_ADC_FIFOC,
			    SUNIV_CODEC_ADC_FIFOC_EN_AD, 0,
			    NULL, 0),

	/* Analog parts of the ADCs */
	SND_SOC_DAPM_ADC("ADC", "Codec Capture", SUNIV_CODEC_ADC_ACTL,
			 SUNIV_CODEC_ADC_ADCEN, 0),

	/* ADC Mixers */
	SOC_MIXER_ARRAY("ADC Mixer", SUNIV_CODEC_ADC_ACTL,
			SND_SOC_NOPM, 0,
			suniv_codec_adc_mixer_controls),

	/* Digital parts of the DACs */
	SND_SOC_DAPM_SUPPLY("DAC Enable", SUN4I_CODEC_DAC_DPC,
			    SUN4I_CODEC_DAC_DPC_EN_DA, 0,
			    NULL, 0),

	/* Analog parts of the DACs */
	SND_SOC_DAPM_DAC("Left DAC", "Codec Playback",
			 SUNIV_CODEC_OM_DACA_CTRL,
			 SUNIV_CODEC_OM_DACA_CTRL_DACALEN, 0),
	SND_SOC_DAPM_DAC("Right DAC", "Codec Playback",
			 SUNIV_CODEC_OM_DACA_CTRL,
			 SUNIV_CODEC_OM_DACA_CTRL_DACAREN, 0),

	/* Mixers */
	SOC_MIXER_ARRAY("Left Mixer", SUNIV_CODEC_OM_DACA_CTRL,
			SUNIV_CODEC_OM_DACA_CTRL_LMIXEN, 0,
			suniv_codec_dac_lmixer_controls),
	SOC_MIXER_ARRAY("Right Mixer", SUNIV_CODEC_OM_DACA_CTRL,
			SUNIV_CODEC_OM_DACA_CTRL_RMIXEN, 0,
			suniv_codec_dac_rmixer_controls),

	/* Headphone output path */
	SND_SOC_DAPM_MUX("Headphone Source Playback Route",
			 SND_SOC_NOPM, 0, 0, suniv_codec_hp_src),
	SND_SOC_DAPM_OUT_DRV("Headphone Amp", SUNIV_CODEC_OM_DACA_CTRL,
			     SUNIV_CODEC_OM_DACA_CTRL_HPPAEN, 0, NULL, 0),
	SND_SOC_DAPM_SUPPLY("HPCOM Protection", SUNIV_CODEC_OM_DACA_CTRL,
			    SUNIV_CODEC_OM_DACA_CTRL_COMPTEN, 0, NULL, 0),
	SND_SOC_DAPM_REG(snd_soc_dapm_supply, "HPCOM", SUNIV_CODEC_OM_DACA_CTRL,
			 SUNIV_CODEC_OM_DACA_CTRL_HPCOM_CTL, 0x3, 0x3, 0),
	SND_SOC_DAPM_OUTPUT("HP"),
};

static const struct snd_soc_dapm_route suniv_codec_codec_dapm_routes[] = {
	/* DAC Routes */
	{ "Left DAC", NULL, "DAC Enable" },
	{ "Right DAC", NULL, "DAC Enable" },

	/* Microphone Routes */
	{ "Mic Amplifier", NULL, "MIC"},

	/* Left Mixer Routes */
	{ "Left Mixer", "Right DAC Playback Switch", "Right DAC" },
	{ "Left Mixer", "Left DAC Playback Switch", "Left DAC" },
	{ "Left Mixer", "FM In Playback Switch", "FMINL" },
	{ "Left Mixer", "Line In Playback Switch", "LINEIN" },
	{ "Left Mixer", "Mic In Playback Switch", "Mic Amplifier" },

	/* Right Mixer Routes */
	{ "Right Mixer", "Left DAC Playback Switch", "Left DAC" },
	{ "Right Mixer", "Right DAC Playback Switch", "Right DAC" },
	{ "Right Mixer", "FM In Playback Switch", "FMINR" },
	{ "Right Mixer", "Line In Playback Switch", "LINEIN" },
	{ "Right Mixer", "Mic In Playback Switch", "Mic Amplifier" },

	/* ADC Mixer Routes */
	{ "ADC Mixer", "Right Out Capture Switch", "Right Mixer" },
	{ "ADC Mixer", "Left Out Capture Switch", "Left Mixer" },
	{ "ADC Mixer", "Line In Capture Switch", "LINEIN" },
	{ "ADC Mixer", "Right FM In Capture Switch", "FMINR" },
	{ "ADC Mixer", "Left FM In Capture Switch", "FMINL" },
	{ "ADC Mixer", "Mic Capture Switch", "Mic Amplifier" },

	/* Headphone Routes */
	{ "Headphone Source Playback Route", "DAC", "Left DAC" },
	{ "Headphone Source Playback Route", "DAC", "Right DAC" },
	{ "Headphone Source Playback Route", "Mixer", "Left Mixer" },
	{ "Headphone Source Playback Route", "Mixer", "Right Mixer" },
	{ "Headphone Amp", NULL, "Headphone Source Playback Route" },
	{ "HP", NULL, "Headphone Amp" },
	{ "HPCOM", NULL, "HPCOM Protection" },

	/* ADC Routes */
	{ "ADC", NULL, "ADC Mixer" },
	{ "ADC", NULL, "ADC Enable" },
};

static const struct snd_soc_component_driver suniv_codec_codec = {
	.controls		= suniv_codec_codec_widgets,
	.num_controls		= ARRAY_SIZE(suniv_codec_codec_widgets),
	.dapm_widgets		= suniv_codec_codec_dapm_widgets,
	.num_dapm_widgets	= ARRAY_SIZE(suniv_codec_codec_dapm_widgets),
	.dapm_routes		= suniv_codec_codec_dapm_routes,
	.num_dapm_routes	= ARRAY_SIZE(suniv_codec_codec_dapm_routes),
	.idle_bias_on		= 1,
	.use_pmdown_time	= 1,
	.endianness		= 1,
};

static const struct snd_soc_component_driver sun4i_codec_component = {
	.name			= "sun4i-codec",
	.legacy_dai_naming	= 1,
#ifdef CONFIG_DEBUG_FS
	.debugfs_prefix		= "cpu",
#endif
};

#define SUN4I_CODEC_FORMATS	(SNDRV_PCM_FMTBIT_S16_LE | \
				 SNDRV_PCM_FMTBIT_S32_LE)

static int sun4i_codec_dai_probe(struct snd_soc_dai *dai)
{
	struct snd_soc_card *card = snd_soc_dai_get_drvdata(dai);
	struct sun4i_codec *scodec = snd_soc_card_get_drvdata(card);

	snd_soc_dai_init_dma_data(dai, &scodec->playback_dma_data,
				  &scodec->capture_dma_data);

	return 0;
}

static const struct snd_soc_dai_ops dummy_dai_ops = {
	.probe	= sun4i_codec_dai_probe,
};

static struct snd_soc_dai_driver dummy_cpu_dai = {
	.name	= "sun4i-codec-cpu-dai",
	.playback = {
		.stream_name	= "Playback",
		.channels_min	= 1,
		.channels_max	= 2,
		.rates		= SUN4I_CODEC_RATES,
		.formats	= SUN4I_CODEC_FORMATS,
		.sig_bits	= 24,
	},
	.capture = {
		.stream_name	= "Capture",
		.channels_min	= 1,
		.channels_max	= 2,
		.rates 		= SUN4I_CODEC_RATES,
		.formats 	= SUN4I_CODEC_FORMATS,
		.sig_bits	= 24,
	 },
	.ops = &dummy_dai_ops,
};

static struct snd_soc_jack sun4i_headphone_jack;

static struct snd_soc_jack_pin sun4i_headphone_jack_pins[] = {
	{ .pin = "Headphone", .mask = SND_JACK_HEADPHONE },
};

static struct snd_soc_jack_gpio sun4i_headphone_jack_gpio = {
	.name = "hp-det",
	.report = SND_JACK_HEADPHONE,
	.debounce_time = 150,
};

static int sun4i_codec_machine_init(struct snd_soc_pcm_runtime *rtd)
{
	struct snd_soc_card *card = rtd->card;
	struct sun4i_codec *scodec = snd_soc_card_get_drvdata(card);
	int ret;

	if (scodec->gpio_hp) {
		ret = snd_soc_card_jack_new_pins(card, "Headphone Jack",
						 SND_JACK_HEADPHONE,
						 &sun4i_headphone_jack,
						 sun4i_headphone_jack_pins,
						 ARRAY_SIZE(sun4i_headphone_jack_pins));
		if (ret) {
			dev_err(rtd->dev,
				"Headphone jack creation failed: %d\n", ret);
			return ret;
		}

		sun4i_headphone_jack_gpio.desc = scodec->gpio_hp;
		ret = snd_soc_jack_add_gpios(&sun4i_headphone_jack, 1,
					     &sun4i_headphone_jack_gpio);

		if (ret) {
			dev_err(rtd->dev, "Headphone GPIO not added: %d\n", ret);
			return ret;
		}
	}

	return 0;
}

static struct snd_soc_dai_link *sun4i_codec_create_link(struct device *dev,
							int *num_links)
{
	struct snd_soc_dai_link *link = devm_kzalloc(dev, sizeof(*link),
						     GFP_KERNEL);
	struct snd_soc_dai_link_component *dlc = devm_kzalloc(dev,
						3 * sizeof(*dlc), GFP_KERNEL);
	if (!link || !dlc)
		return NULL;

	link->cpus	= &dlc[0];
	link->codecs	= &dlc[1];
	link->platforms	= &dlc[2];

	link->num_cpus		= 1;
	link->num_codecs	= 1;
	link->num_platforms	= 1;

	link->name		= "cdc";
	link->stream_name	= "CDC PCM";
	link->codecs->dai_name	= "Codec";
	link->cpus->dai_name	= dev_name(dev);
	link->codecs->name	= dev_name(dev);
	link->platforms->name	= dev_name(dev);
	link->dai_fmt		= SND_SOC_DAIFMT_I2S;
	link->init		= sun4i_codec_machine_init;

	*num_links = 1;

	return link;
};

static int sun4i_codec_spk_event(struct snd_soc_dapm_widget *w,
				 struct snd_kcontrol *k, int event)
{
	struct snd_soc_card *card = snd_soc_dapm_to_card(w->dapm);
	struct sun4i_codec *scodec = snd_soc_card_get_drvdata(card);

	gpiod_set_value_cansleep(scodec->gpio_pa,
				 !!SND_SOC_DAPM_EVENT_ON(event));

	if (SND_SOC_DAPM_EVENT_ON(event)) {
		/*
		 * Need a delay to wait for DAC to push the data. 700ms seems
		 * to be the best compromise not to feel this delay while
		 * playing a sound.
		 */
		msleep(700);
	}

	return 0;
}

static const struct snd_soc_dapm_widget sun4i_codec_card_dapm_widgets[] = {
	SND_SOC_DAPM_SPK("Speaker", sun4i_codec_spk_event),
};

static const struct snd_soc_dapm_route sun4i_codec_card_dapm_routes[] = {
	{ "Speaker", NULL, "HP Right" },
	{ "Speaker", NULL, "HP Left" },
};

static struct snd_soc_card *sun4i_codec_create_card(struct device *dev)
{
	struct snd_soc_card *card;

	card = devm_kzalloc(dev, sizeof(*card), GFP_KERNEL);
	if (!card)
		return ERR_PTR(-ENOMEM);

	card->dai_link = sun4i_codec_create_link(dev, &card->num_links);
	if (!card->dai_link)
		return ERR_PTR(-ENOMEM);

	card->dev		= dev;
	card->owner		= THIS_MODULE;
	card->name		= "sun4i-codec";
	card->dapm_widgets	= sun4i_codec_card_dapm_widgets;
	card->num_dapm_widgets	= ARRAY_SIZE(sun4i_codec_card_dapm_widgets);
	card->dapm_routes	= sun4i_codec_card_dapm_routes;
	card->num_dapm_routes	= ARRAY_SIZE(sun4i_codec_card_dapm_routes);

	return card;
};

static const struct snd_soc_dapm_widget sun6i_codec_card_dapm_widgets[] = {
	SND_SOC_DAPM_HP("Headphone", NULL),
	SND_SOC_DAPM_LINE("Line In", NULL),
	SND_SOC_DAPM_LINE("Line Out", NULL),
	SND_SOC_DAPM_MIC("Headset Mic", NULL),
	SND_SOC_DAPM_MIC("Mic", NULL),
	SND_SOC_DAPM_SPK("Speaker", sun4i_codec_spk_event),
};

static struct snd_soc_card *sun6i_codec_create_card(struct device *dev)
{
	struct snd_soc_card *card;
	int ret;

	card = devm_kzalloc(dev, sizeof(*card), GFP_KERNEL);
	if (!card)
		return ERR_PTR(-ENOMEM);

	card->dai_link = sun4i_codec_create_link(dev, &card->num_links);
	if (!card->dai_link)
		return ERR_PTR(-ENOMEM);

	card->dev		= dev;
	card->owner		= THIS_MODULE;
	card->name		= "A31 Audio Codec";
	card->dapm_widgets	= sun6i_codec_card_dapm_widgets;
	card->num_dapm_widgets	= ARRAY_SIZE(sun6i_codec_card_dapm_widgets);
	card->fully_routed	= true;

	ret = snd_soc_of_parse_audio_routing(card, "allwinner,audio-routing");
	if (ret)
		dev_warn(dev, "failed to parse audio-routing: %d\n", ret);

	return card;
};

/* Connect digital side enables to analog side widgets */
static const struct snd_soc_dapm_route sun8i_codec_card_routes[] = {
	/* ADC Routes */
	{ "Left ADC", NULL, "ADC Enable" },
	{ "Right ADC", NULL, "ADC Enable" },
	{ "Codec Capture", NULL, "Left ADC" },
	{ "Codec Capture", NULL, "Right ADC" },

	/* DAC Routes */
	{ "Left DAC", NULL, "DAC Enable" },
	{ "Right DAC", NULL, "DAC Enable" },
	{ "Left DAC", NULL, "Codec Playback" },
	{ "Right DAC", NULL, "Codec Playback" },
};

static struct snd_soc_aux_dev aux_dev = {
	.dlc = COMP_EMPTY(),
};

static struct snd_soc_card *sun8i_a23_codec_create_card(struct device *dev)
{
	struct snd_soc_card *card;
	int ret;

	card = devm_kzalloc(dev, sizeof(*card), GFP_KERNEL);
	if (!card)
		return ERR_PTR(-ENOMEM);

	aux_dev.dlc.of_node = of_parse_phandle(dev->of_node,
						 "allwinner,codec-analog-controls",
						 0);
	if (!aux_dev.dlc.of_node) {
		dev_err(dev, "Can't find analog controls for codec.\n");
		return ERR_PTR(-EINVAL);
	}

	card->dai_link = sun4i_codec_create_link(dev, &card->num_links);
	if (!card->dai_link)
		return ERR_PTR(-ENOMEM);

	card->dev		= dev;
	card->owner		= THIS_MODULE;
	card->name		= "A23 Audio Codec";
	card->dapm_widgets	= sun6i_codec_card_dapm_widgets;
	card->num_dapm_widgets	= ARRAY_SIZE(sun6i_codec_card_dapm_widgets);
	card->dapm_routes	= sun8i_codec_card_routes;
	card->num_dapm_routes	= ARRAY_SIZE(sun8i_codec_card_routes);
	card->aux_dev		= &aux_dev;
	card->num_aux_devs	= 1;
	card->fully_routed	= true;

	ret = snd_soc_of_parse_audio_routing(card, "allwinner,audio-routing");
	if (ret)
		dev_warn(dev, "failed to parse audio-routing: %d\n", ret);

	return card;
};

static struct snd_soc_card *sun8i_h3_codec_create_card(struct device *dev)
{
	struct snd_soc_card *card;
	int ret;

	card = devm_kzalloc(dev, sizeof(*card), GFP_KERNEL);
	if (!card)
		return ERR_PTR(-ENOMEM);

	aux_dev.dlc.of_node = of_parse_phandle(dev->of_node,
						 "allwinner,codec-analog-controls",
						 0);
	if (!aux_dev.dlc.of_node) {
		dev_err(dev, "Can't find analog controls for codec.\n");
		return ERR_PTR(-EINVAL);
	}

	card->dai_link = sun4i_codec_create_link(dev, &card->num_links);
	if (!card->dai_link)
		return ERR_PTR(-ENOMEM);

	card->dev		= dev;
	card->owner		= THIS_MODULE;
	card->name		= "H3 Audio Codec";
	card->dapm_widgets	= sun6i_codec_card_dapm_widgets;
	card->num_dapm_widgets	= ARRAY_SIZE(sun6i_codec_card_dapm_widgets);
	card->dapm_routes	= sun8i_codec_card_routes;
	card->num_dapm_routes	= ARRAY_SIZE(sun8i_codec_card_routes);
	card->aux_dev		= &aux_dev;
	card->num_aux_devs	= 1;
	card->fully_routed	= true;

	ret = snd_soc_of_parse_audio_routing(card, "allwinner,audio-routing");
	if (ret)
		dev_warn(dev, "failed to parse audio-routing: %d\n", ret);

	return card;
};

static struct snd_soc_card *sun8i_v3s_codec_create_card(struct device *dev)
{
	struct snd_soc_card *card;
	int ret;

	card = devm_kzalloc(dev, sizeof(*card), GFP_KERNEL);
	if (!card)
		return ERR_PTR(-ENOMEM);

	aux_dev.dlc.of_node = of_parse_phandle(dev->of_node,
						 "allwinner,codec-analog-controls",
						 0);
	if (!aux_dev.dlc.of_node) {
		dev_err(dev, "Can't find analog controls for codec.\n");
		return ERR_PTR(-EINVAL);
	}

	card->dai_link = sun4i_codec_create_link(dev, &card->num_links);
	if (!card->dai_link)
		return ERR_PTR(-ENOMEM);

	card->dev		= dev;
	card->owner		= THIS_MODULE;
	card->name		= "V3s Audio Codec";
	card->dapm_widgets	= sun6i_codec_card_dapm_widgets;
	card->num_dapm_widgets	= ARRAY_SIZE(sun6i_codec_card_dapm_widgets);
	card->dapm_routes	= sun8i_codec_card_routes;
	card->num_dapm_routes	= ARRAY_SIZE(sun8i_codec_card_routes);
	card->aux_dev		= &aux_dev;
	card->num_aux_devs	= 1;
	card->fully_routed	= true;

	ret = snd_soc_of_parse_audio_routing(card, "allwinner,audio-routing");
	if (ret)
		dev_warn(dev, "failed to parse audio-routing: %d\n", ret);

	return card;
};

static const struct snd_kcontrol_new sun50i_h616_codec_codec_controls[] = {
	SOC_SINGLE_TLV("DAC Playback Volume", SUN4I_CODEC_DAC_DPC,
		       SUN4I_CODEC_DAC_DPC_DVOL, 0x3f, 1,
		       sun6i_codec_dvol_scale),
	SOC_SINGLE_TLV("Line Out Playback Volume",
		       SUN50I_H616_DAC_AC_DAC_REG,
		       SUN50I_H616_LINEOUT_VOL, 0x1f, 0,
		       sun6i_codec_lineout_vol_scale),
	SOC_DOUBLE("Line Out Playback Switch",
		   SUN50I_H616_DAC_AC_DAC_REG,
		   SUN50I_H616_LINEOUTL_EN,
		   SUN50I_H616_LINEOUTR_EN, 1, 0),
};

static const struct snd_kcontrol_new sun50i_h616_codec_mixer_controls[] = {
	SOC_DAPM_DOUBLE("DAC Playback Switch",
			SUN50I_H616_DAC_AC_MIXER_REG,
			SUN50I_H616_LMIX_LDAC,
			SUN50I_H616_RMIX_RDAC, 1, 0),
	SOC_DAPM_DOUBLE("DAC Reversed Playback Switch",
			SUN50I_H616_DAC_AC_MIXER_REG,
			SUN50I_H616_LMIX_RDAC,
			SUN50I_H616_RMIX_LDAC, 1, 0),
};

static SOC_ENUM_DOUBLE_DECL(sun50i_h616_codec_lineout_src_enum,
			    SUN50I_H616_DAC_AC_DAC_REG,
			    SUN50I_H616_LINEOUTL_SEL,
			    SUN50I_H616_LINEOUTR_SEL,
			    sun6i_codec_lineout_src_enum_text);

static const struct snd_kcontrol_new sun50i_h616_codec_lineout_src[] = {
		SOC_DAPM_ENUM("Line Out Source Playback Route",
			      sun50i_h616_codec_lineout_src_enum),
};

static const struct snd_soc_dapm_widget sun50i_h616_codec_codec_widgets[] = {
	/* Digital parts of the DACs */
	SND_SOC_DAPM_SUPPLY("DAC Enable", SUN4I_CODEC_DAC_DPC,
			    SUN4I_CODEC_DAC_DPC_EN_DA, 0,
			    NULL, 0),

	/* Analog parts of the DACs */
	SND_SOC_DAPM_DAC("Left DAC", "Codec Playback",
			 SUN50I_H616_DAC_AC_DAC_REG,
			 SUN50I_H616_DAC_LEN, 0),
	SND_SOC_DAPM_DAC("Right DAC", "Codec Playback",
			 SUN50I_H616_DAC_AC_DAC_REG,
			 SUN50I_H616_DAC_REN, 0),

	/* Mixers */
	SOC_MIXER_ARRAY("Left Mixer", SUN50I_H616_DAC_AC_MIXER_REG,
			SUN50I_H616_LMIXEN, 0,
			sun50i_h616_codec_mixer_controls),
	SOC_MIXER_ARRAY("Right Mixer", SUN50I_H616_DAC_AC_MIXER_REG,
			SUN50I_H616_RMIXEN, 0,
			sun50i_h616_codec_mixer_controls),

	/* Line Out path */
	SND_SOC_DAPM_MUX("Line Out Source Playback Route",
			 SND_SOC_NOPM, 0, 0, sun50i_h616_codec_lineout_src),
	SND_SOC_DAPM_OUT_DRV("Line Out Ramp Controller",
			     SUN50I_H616_DAC_AC_RAMP_REG,
			     SUN50I_H616_RDEN, 0, NULL, 0),
	SND_SOC_DAPM_OUTPUT("LINEOUT"),
};

static const struct snd_soc_component_driver sun50i_h616_codec_codec = {
	.controls   = sun50i_h616_codec_codec_controls,
	.num_controls   = ARRAY_SIZE(sun50i_h616_codec_codec_controls),
	.dapm_widgets   = sun50i_h616_codec_codec_widgets,
	.num_dapm_widgets = ARRAY_SIZE(sun50i_h616_codec_codec_widgets),
	.idle_bias_on   = 1,
	.use_pmdown_time  = 1,
	.endianness   = 1,
};

static const struct snd_kcontrol_new sun50i_h616_card_controls[] = {
	SOC_DAPM_PIN_SWITCH("Speaker"),
};

static const struct snd_soc_dapm_widget sun50i_h616_codec_card_dapm_widgets[] = {
	SND_SOC_DAPM_HP("Headphone", NULL),
	SND_SOC_DAPM_LINE("Line Out", NULL),
	SND_SOC_DAPM_SPK("Speaker", sun4i_codec_spk_event),
};

/* Connect digital side enables to analog side widgets */
static const struct snd_soc_dapm_route sun50i_h616_codec_card_routes[] = {
	/* DAC Routes */
	{ "Left DAC", NULL, "DAC Enable" },
	{ "Right DAC", NULL, "DAC Enable" },

	/* Left Mixer Routes */
	{ "Left Mixer", "DAC Playback Switch", "Left DAC" },
	{ "Left Mixer", "DAC Reversed Playback Switch", "Right DAC" },

	/* Right Mixer Routes */
	{ "Right Mixer", "DAC Playback Switch", "Right DAC" },
	{ "Right Mixer", "DAC Reversed Playback Switch", "Left DAC" },

	/* Line Out Routes */
	{ "Line Out Source Playback Route", "Stereo", "Left Mixer" },
	{ "Line Out Source Playback Route", "Stereo", "Right Mixer" },
	{ "Line Out Source Playback Route", "Mono Differential", "Left Mixer" },
	{ "Line Out Source Playback Route", "Mono Differential", "Right Mixer" },
	{ "Line Out Ramp Controller", NULL, "Line Out Source Playback Route" },
	{ "LINEOUT", NULL, "Line Out Ramp Controller" },
};

static struct snd_soc_card *sun50i_h616_codec_create_card(struct device *dev)
{
	struct snd_soc_card *card;
	int ret;

	card = devm_kzalloc(dev, sizeof(*card), GFP_KERNEL);
	if (!card)
		return ERR_PTR(-ENOMEM);

	card->dai_link = sun4i_codec_create_link(dev, &card->num_links);
	if (!card->dai_link)
		return ERR_PTR(-ENOMEM);

	card->dai_link->playback_only = true;
	card->dai_link->capture_only = false;

	card->dev		= dev;
	card->owner		= THIS_MODULE;
	card->name		= "H616 Audio Codec";
	card->long_name		= "h616-audio-codec";
	card->driver_name	= "sun4i-codec";
	card->controls		= sun50i_h616_card_controls;
	card->num_controls	= ARRAY_SIZE(sun50i_h616_card_controls);
	card->dapm_widgets	= sun50i_h616_codec_card_dapm_widgets;
	card->num_dapm_widgets	= ARRAY_SIZE(sun50i_h616_codec_card_dapm_widgets);
	card->dapm_routes	= sun50i_h616_codec_card_routes;
	card->num_dapm_routes	= ARRAY_SIZE(sun50i_h616_codec_card_routes);
	card->fully_routed	= true;

	ret = snd_soc_of_parse_audio_routing(card, "allwinner,audio-routing");
	if (ret)
		dev_warn(dev, "failed to parse audio-routing: %d\n", ret);

	return card;
};

static const struct snd_soc_dapm_widget suniv_codec_card_dapm_widgets[] = {
	SND_SOC_DAPM_HP("Headphone", NULL),
	SND_SOC_DAPM_LINE("Line In", NULL),
	SND_SOC_DAPM_LINE("Right FM In", NULL),
	SND_SOC_DAPM_LINE("Left FM In", NULL),
	SND_SOC_DAPM_MIC("Mic", NULL),
	SND_SOC_DAPM_SPK("Speaker", sun4i_codec_spk_event),
};

/* Connect digital side enables to analog side widgets */
static const struct snd_soc_dapm_route suniv_codec_card_routes[] = {
	/* ADC Routes */
	{ "ADC", NULL, "ADC Enable" },
	{ "Codec Capture", NULL, "ADC" },

	/* DAC Routes */
	{ "Left DAC", NULL, "DAC Enable" },
	{ "Right DAC", NULL, "DAC Enable" },
	{ "Left DAC", NULL, "Codec Playback" },
	{ "Right DAC", NULL, "Codec Playback" },
};

static struct snd_soc_card *suniv_codec_create_card(struct device *dev)
{
	struct snd_soc_card *card;
	int ret;

	card = devm_kzalloc(dev, sizeof(*card), GFP_KERNEL);
	if (!card)
		return ERR_PTR(-ENOMEM);

	card->dai_link = sun4i_codec_create_link(dev, &card->num_links);
	if (!card->dai_link)
		return ERR_PTR(-ENOMEM);

	card->dev		= dev;
	card->name		= "F1C100s Audio Codec";
	card->dapm_widgets	= suniv_codec_card_dapm_widgets;
	card->num_dapm_widgets	= ARRAY_SIZE(suniv_codec_card_dapm_widgets);
	card->dapm_routes	= suniv_codec_card_routes;
	card->num_dapm_routes	= ARRAY_SIZE(suniv_codec_card_routes);
	card->fully_routed	= true;

	ret = snd_soc_of_parse_audio_routing(card, "allwinner,audio-routing");
	if (ret)
		dev_warn(dev, "failed to parse audio-routing: %d\n", ret);

	return card;
};

static const struct regmap_config sun4i_codec_regmap_config = {
	.reg_bits	= 32,
	.reg_stride	= 4,
	.val_bits	= 32,
	.max_register	= SUN4I_CODEC_ADC_RXCNT,
};

static const struct regmap_config sun6i_codec_regmap_config = {
	.reg_bits	= 32,
	.reg_stride	= 4,
	.val_bits	= 32,
	.max_register	= SUN6I_CODEC_HMIC_DATA,
};

static const struct regmap_config sun7i_codec_regmap_config = {
	.reg_bits	= 32,
	.reg_stride	= 4,
	.val_bits	= 32,
	.max_register	= SUN7I_CODEC_AC_MIC_PHONE_CAL,
};

static const struct regmap_config sun8i_a23_codec_regmap_config = {
	.reg_bits	= 32,
	.reg_stride	= 4,
	.val_bits	= 32,
	.max_register	= SUN8I_A23_CODEC_ADC_RXCNT,
};

static const struct regmap_config sun8i_h3_codec_regmap_config = {
	.reg_bits	= 32,
	.reg_stride	= 4,
	.val_bits	= 32,
	.max_register	= SUN8I_H3_CODEC_ADC_DBG,
};

static const struct regmap_config sun8i_v3s_codec_regmap_config = {
	.reg_bits	= 32,
	.reg_stride	= 4,
	.val_bits	= 32,
	.max_register	= SUN8I_H3_CODEC_ADC_DBG,
};

static const struct regmap_config sun50i_h616_codec_regmap_config = {
	.reg_bits	= 32,
	.reg_stride	= 4,
	.val_bits	= 32,
	.max_register	= SUN50I_H616_DAC_AC_RAMP_REG,
	.cache_type	= REGCACHE_NONE,
};

static const struct regmap_config suniv_codec_regmap_config = {
	.reg_bits	= 32,
	.reg_stride	= 4,
	.val_bits	= 32,
	.max_register	= SUNIV_CODEC_ADC_DBG,
};

static int a133_dac_event(struct snd_soc_dapm_widget *w,
			 struct snd_kcontrol *k, int event)
{
	if (event == SND_SOC_DAPM_POST_PMU)
		msleep(30);
	return 0;
}

static int a133_headphone_event(struct snd_soc_dapm_widget *w,
			       struct snd_kcontrol *k, int event)
{
	struct snd_soc_component *c = snd_soc_dapm_to_component(w->dapm);
	const unsigned int up[] = { BIT(11), BIT(10), BIT(15) };
	unsigned int i;
	int ret;

	/* Vendor orders input, output, PA on; reverse on power-down. */
	for (i = 0; i < ARRAY_SIZE(up); i++) {
		unsigned int bit = up[event == SND_SOC_DAPM_POST_PMU ? i : 2 - i];

		ret = snd_soc_component_update_bits(c, SUN50I_A133_HEADPHONE, bit,
			   event == SND_SOC_DAPM_POST_PMU ? bit : 0);
		if (ret < 0)
			return ret;
	}
	return 0;
}

/*
 * Coefficients from the shipped A133 codec initialization. Keep the ordered
 * writes: stock overwrites the peak attack values and writes EPS high twice,
 * without touching EPS low. Do not substitute a different SoC's register map.
 * Processing stays disabled until the associated DAPM supplies are powered.
 */
static int a133_adc_dsp_init(struct snd_soc_component *c)
{
	static const struct { unsigned int reg, value; } coefficients[] = {
		{ 0x20c, 0x000b },
		{ 0x210, 0x77f0 },
		{ 0x214, 0x000b },
		{ 0x218, 0x77f0 },
		{ 0x21c, 0x00ff },
		{ 0x220, 0xe1f8 },
		{ 0x224, 0x00ff },
		{ 0x228, 0xe1f8 },
		{ 0x20c, 0x0001 },
		{ 0x210, 0x2bb0 },
		{ 0x214, 0x0001 },
		{ 0x218, 0x2bb0 },
		{ 0x264, 0xff64 },
		{ 0x268, 0x1741 },
		{ 0x24c, 0xfc03 },
		{ 0x250, 0x80f3 },
		{ 0x27c, 0xf608 },
		{ 0x280, 0xc25f },
		{ 0x254, 0x0352 },
		{ 0x258, 0x69e0 },
		{ 0x23c, 0x06b3 },
		{ 0x240, 0x002c },
		{ 0x26c, 0x0a13 },
		{ 0x270, 0x9682 },
		{ 0x25c, 0x0022 },
		{ 0x260, 0x2222 },
		{ 0x244, 0x0100 },
		{ 0x248, 0x0000 },
		{ 0x284, 0x01c5 },
		{ 0x288, 0x3ef0 },
		{ 0x274, 0x0423 },
		{ 0x278, 0x4f68 },
		{ 0x28c, 0x0001 },
		{ 0x290, 0x7665 },
		{ 0x294, 0x0000 },
		{ 0x298, 0x0f04 },
		{ 0x29c, 0x69e0 },
		{ 0x2a0, 0xf95b },
		{ 0x2a4, 0xf95b },
		{ 0x2a8, 0x2c3f },
		{ 0x2ac, 0x0002 },
		{ 0x2ac, 0x5600 },
		{ SUN50I_A133_ADC_HPF_H, 0x00ff },
		{ SUN50I_A133_ADC_HPF_L, 0xfac1 },
	};
	unsigned int i;
	int ret;

	ret = snd_soc_component_update_bits(c, SUN50I_A133_ADC_DRC_CTRL, 0xfb, 0xfb);
	if (ret < 0)
		return ret;
	for (i = 0; i < ARRAY_SIZE(coefficients); i++) {
		ret = snd_soc_component_write(c, coefficients[i].reg, coefficients[i].value);
		if (ret < 0)
			return ret;
	}
	return 0;
}

/* Shipped speaker DRC coefficients; the headphone-only path bypasses DRC. */
static int a133_dac_dsp_init(struct snd_soc_component *c)
{
	static const struct { unsigned int reg, value; } coefficients[] = {
		{ 0x10c, 0x000b },
		{ 0x110, 0x77bf },
		{ 0x114, 0x000b },
		{ 0x118, 0x77f0 },
		{ 0x11c, 0x00ff },
		{ 0x120, 0xe1f8 },
		{ 0x124, 0x00ff },
		{ 0x128, 0xe1f8 },
		{ 0x12c, 0x0001 },
		{ 0x130, 0x2bb0 },
		{ 0x134, 0x0001 },
		{ 0x138, 0x2bb0 },
		{ 0x18c, 0x0001 },
		{ 0x190, 0x7665 },
		{ 0x194, 0x0000 },
		{ 0x198, 0x0f04 },
		{ 0x164, 0xfe56 },
		{ 0x168, 0xcb10 },
		{ 0x14c, 0xfb04 },
		{ 0x150, 0x612f },
		{ 0x17c, 0xf608 },
		{ 0x180, 0xc25f },
		{ 0x154, 0x0352 },
		{ 0x158, 0x69e0 },
		{ 0x13c, 0x06b3 },
		{ 0x140, 0x002c },
		{ 0x16c, 0x0a13 },
		{ 0x170, 0x9682 },
		{ 0x15c, 0x0040 },
		{ 0x160, 0x0000 },
		{ 0x144, 0x00fb },
		{ 0x148, 0xcda5 },
		{ 0x184, 0x0179 },
		{ 0x188, 0xb472 },
		{ 0x174, 0x0423 },
		{ 0x178, 0x4f68 },
		{ 0x19c, 0x0352 },
		{ 0x1a0, 0x69e0 },
		{ 0x1a4, 0xf95b },
		{ 0x1a8, 0x2c3f },
		{ 0x1ac, 0x0002 },
		{ 0x1b0, 0x5600 },
	};
	unsigned int i;
	int ret;

	/* Stock disables DAC HPF; do not program or enable that filter. */
	ret = snd_soc_component_update_bits(c, SUN50I_A133_DAC_DAP,
					    BIT(31) | BIT(29) | BIT(28), 0);
	if (ret < 0)
		return ret;
	ret = snd_soc_component_update_bits(c, SUN50I_A133_DAC_DRC_CTRL, 0xfb, 0);
	if (ret < 0)
		return ret;
	for (i = 0; i < ARRAY_SIZE(coefficients); i++) {
		ret = snd_soc_component_write(c, coefficients[i].reg, coefficients[i].value);
		if (ret < 0)
			return ret;
	}
	return 0;
}

/* Low initial digital level; userspace deliberately raises volume for test. */
static int a133_component_probe(struct snd_soc_component *c)
{
	struct sun4i_codec *scodec = snd_soc_component_get_drvdata(c);
	static const struct { unsigned int reg, mask, value; } init[] = {
		/* Match DAPM's initial off state, including inherited analog state. */
		/* Leave capture and both microphone paths off until DAPM/trigger. */
		{ SUN50I_A133_ADC_FIFOC, BIT(28) | BIT(3) | GENMASK(27, 25), GENMASK(27, 25) },
		{ SUN50I_A133_ADC_VOL, BIT(17) | GENMASK(15, 0), BIT(17) | 0xa0a0 },
		{ SUN50I_A133_ADCL, BIT(31) | BIT(30) | GENMASK(12, 8) | GENMASK(7, 0), 0x55 },
		{ SUN50I_A133_ADCR, BIT(31) | BIT(30) | GENMASK(12, 8) | GENMASK(7, 0), 0x55 },
		{ SUN50I_A133_MICBIAS, BIT(7), 0 },
		/* Disable shared processing before writing any coefficients. */
		{ SUN50I_A133_ADC_DAP, BIT(31) | BIT(29) | BIT(28), 0 },
		{ SUN50I_A133_HEADPHONE, BIT(15), 0 },
		{ SUN50I_A133_HEADPHONE, BIT(10), 0 },
		{ SUN50I_A133_HEADPHONE, BIT(11), 0 },
		{ SUN50I_A133_DAC_DPC, BIT(31) | GENMASK(17, 12), 48 << 12 },
		{ SUN50I_A133_DAC_ANALOG, GENMASK(15, 12), 0 },
		/* Start muted; ASoC restores the requested gains after DAPM startup. */
		{ SUN50I_A133_DAC_VOL, GENMASK(16, 0), 0 },
		{ SUN50I_A133_DAC_ANALOG, GENMASK(30, 28), 0 },
		{ SUN50I_A133_DAC_ANALOG, GENMASK(4, 0) | BIT(13) | BIT(12), 0 },
		{ SUN50I_A133_DAC_DEBUG, BIT(6), 0 }, /* Preserve PCM channel order. */
		{ SUN50I_A133_HEADPHONE, GENMASK(7, 6), 3 << 6 },
		{ SUN50I_A133_HEADPHONE, BIT(3), BIT(3) },
		{ SUN50I_A133_HEADPHONE, GENMASK(9, 8), 3 << 8 },
		{ SUN50I_A133_DAC_ANALOG, GENMASK(25, 24), 1 << 24 },
	};
	unsigned int i;
	int ret;

	for (i = 0; i < ARRAY_SIZE(init); i++) {
		ret = snd_soc_component_update_bits(c, init[i].reg,
						    init[i].mask, init[i].value);
		if (ret < 0)
			return ret;
	}
	/* A component reprobe must reflect the freshly muted register state. */
	mutex_lock(&scodec->a133_volume_lock);
	scodec->a133_dac_muted = true;
	mutex_unlock(&scodec->a133_volume_lock);
	ret = a133_adc_dsp_init(c);
	if (ret)
		return ret;
	return a133_dac_dsp_init(c);
}

static int a133_adc_event(struct snd_soc_dapm_widget *w,
			  struct snd_kcontrol *k, int event)
{
	/* Settle the powered analog input before trigger enables conversion. */
	if (event == SND_SOC_DAPM_POST_PMU)
		msleep(80);
	return 0;
}

static int a133_dac_volume_get(struct snd_kcontrol *kcontrol,
			       struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *c = snd_kcontrol_chip(kcontrol);
	struct sun4i_codec *scodec = snd_soc_component_get_drvdata(c);

	mutex_lock(&scodec->a133_volume_lock);
	ucontrol->value.integer.value[0] = scodec->a133_dac_volume >> 8;
	ucontrol->value.integer.value[1] = scodec->a133_dac_volume & 0xff;
	mutex_unlock(&scodec->a133_volume_lock);
	return 0;
}

static int a133_dac_volume_put(struct snd_kcontrol *kcontrol,
			       struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *c = snd_kcontrol_chip(kcontrol);
	struct sun4i_codec *scodec = snd_soc_component_get_drvdata(c);
	long left = ucontrol->value.integer.value[0];
	long right = ucontrol->value.integer.value[1];
	unsigned int volume;
	int ret = 0;

	if (left < 0 || left > 255 || right < 0 || right > 255)
		return -EINVAL;
	volume = (left << 8) | right;
	mutex_lock(&scodec->a133_volume_lock);
	if (volume == scodec->a133_dac_volume)
		goto out;
	/* Mixer changes while muted apply on the next actual unmute. */
	if (!scodec->a133_dac_muted) {
		ret = regmap_update_bits(scodec->regmap, SUN50I_A133_DAC_VOL,
					GENMASK(15, 0), volume);
		if (ret)
			goto out;
	}
	scodec->a133_dac_volume = volume;
	ret = 1;
out:
	mutex_unlock(&scodec->a133_volume_lock);
	return ret;
}

static const DECLARE_TLV_DB_SCALE(a133_digital_tlv, -7308, 116, 0);
static const struct snd_kcontrol_new a133_controls[] = {
	SOC_DOUBLE("ADC Capture Volume", SUN50I_A133_ADC_VOL, 8, 0, 255, 0),
	SOC_SINGLE("MIC1 Capture Gain", SUN50I_A133_ADCL, 8, 31, 0),
	SOC_SINGLE("MIC2 Capture Gain", SUN50I_A133_ADCR, 8, 31, 0),
	/* Independent channel gains; preserve DAC_VOL_SEL at bit 16. */
	SOC_DOUBLE_EXT("DAC Digital Playback Volume", SUN50I_A133_DAC_VOL,
		       8, 0, 255, 0, a133_dac_volume_get, a133_dac_volume_put),
	SOC_SINGLE_TLV("DAC Playback Volume", SUN50I_A133_DAC_DPC, 12, 63, 1, a133_digital_tlv),
	/* Analog TLV deliberately omitted until vendor gain curves audited. */
	SOC_SINGLE("Headphone Playback Volume", SUN50I_A133_DAC_ANALOG, 28, 7, 0),
};
static const struct snd_soc_dapm_widget a133_widgets[] = {
	SND_SOC_DAPM_SUPPLY("ADC DAP", SUN50I_A133_ADC_DAP, 31, 0, NULL, 0),
	SND_SOC_DAPM_SUPPLY("ADC HPF", SUN50I_A133_ADC_DAP, 28, 0, NULL, 0),
	SND_SOC_DAPM_SUPPLY("ADC DRC", SUN50I_A133_ADC_DAP, 29, 0, NULL, 0),
	SND_SOC_DAPM_ADC_E("Left ADC", "Codec Capture", SUN50I_A133_ADCL, 31, 0,
			   a133_adc_event, SND_SOC_DAPM_POST_PMU),
	SND_SOC_DAPM_ADC_E("Right ADC", "Codec Capture", SUN50I_A133_ADCR, 31, 0,
			   a133_adc_event, SND_SOC_DAPM_POST_PMU),
	SND_SOC_DAPM_PGA("MIC1 Amplifier", SUN50I_A133_ADCL, 30, 0, NULL, 0),
	SND_SOC_DAPM_PGA("MIC2 Amplifier", SUN50I_A133_ADCR, 30, 0, NULL, 0),
	SND_SOC_DAPM_SUPPLY("Main Mic Bias", SUN50I_A133_MICBIAS, 7, 0, NULL, 0),
	SND_SOC_DAPM_INPUT("MIC1"),
	SND_SOC_DAPM_INPUT("MIC2"),
	SND_SOC_DAPM_SUPPLY("DAC Enable", SUN50I_A133_DAC_DPC, 31, 0, a133_dac_event,
			   SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_PRE_PMD),
	SND_SOC_DAPM_DAC("Left DAC", "Codec Playback", SUN50I_A133_DAC_ANALOG, 15, 0),
	SND_SOC_DAPM_DAC("Right DAC", "Codec Playback", SUN50I_A133_DAC_ANALOG, 14, 0),
	SND_SOC_DAPM_PGA_E("Headphone Driver", SND_SOC_NOPM, 0, 0, NULL, 0,
			  a133_headphone_event,
			  SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_PRE_PMD),
	SND_SOC_DAPM_OUTPUT("HPOUTL"),
	SND_SOC_DAPM_OUTPUT("HPOUTR"),
};
static const struct snd_soc_dapm_route a133_routes[] = {
	/* Stock enables HPF for capture, and DRC for the MIC2 amplifier path. */
	{ "ADC HPF", NULL, "ADC DAP" },
	{ "ADC DRC", NULL, "ADC DAP" },
	{ "Left ADC", NULL, "ADC HPF" },
	{ "Right ADC", NULL, "ADC HPF" },
	{ "MIC2 Amplifier", NULL, "ADC DRC" },
	{ "Left ADC", NULL, "MIC1 Amplifier" },
	{ "Right ADC", NULL, "MIC2 Amplifier" },
	{ "MIC1 Amplifier", NULL, "MIC1" },
	{ "MIC2 Amplifier", NULL, "MIC2" },
	{ "Left DAC", NULL, "DAC Enable" },
	{ "Right DAC", NULL, "DAC Enable" },
	{ "Headphone Driver", NULL, "Left DAC" },
	{ "Headphone Driver", NULL, "Right DAC" },
	{ "HPOUTL", NULL, "Headphone Driver" },
	{ "HPOUTR", NULL, "Headphone Driver" },
};
static const struct snd_soc_component_driver sun50i_a133_codec_component = {
	.probe = a133_component_probe,
	.controls = a133_controls,
	.num_controls = ARRAY_SIZE(a133_controls),
	.dapm_widgets = a133_widgets,
	.num_dapm_widgets = ARRAY_SIZE(a133_widgets),
	.dapm_routes = a133_routes,
	.num_dapm_routes = ARRAY_SIZE(a133_routes),
	.use_pmdown_time = 1,
	.endianness = 1,
};

/*
 * DRC is the only enabled DAC processor. The single Speaker widget owns its
 * common gate, even when both headphone outputs feed the external amplifier.
 * Headphone-only playback never acquires this gate. ADC processing is separate.
 */
static int a133_speaker_drc_disable(struct sun4i_codec *scodec)
{
	int ret, err;

	ret = regmap_update_bits(scodec->regmap, SUN50I_A133_DAC_DAP,
				 BIT(31) | BIT(29), 0);
	/* Attempt both shutdown writes even when one fails. */
	err = regmap_update_bits(scodec->regmap, SUN50I_A133_DAC_DRC_CTRL, 0xfb, 0);
	return ret ?: err;
}

static int a133_speaker_drc_enable(struct sun4i_codec *scodec)
{
	int ret;

	ret = regmap_update_bits(scodec->regmap, SUN50I_A133_DAC_DRC_CTRL, 0xfb, 0xfb);
	if (ret)
		goto disable;
	ret = regmap_update_bits(scodec->regmap, SUN50I_A133_DAC_DAP,
				 BIT(31) | BIT(29), BIT(31) | BIT(29));
	if (!ret)
		return 0;

disable:
	a133_speaker_drc_disable(scodec);
	return ret;
}

static int sun50i_a133_speaker_event(struct snd_soc_dapm_widget *w,
				   struct snd_kcontrol *k, int event)
{
	struct snd_soc_card *card = snd_soc_dapm_to_card(w->dapm);
	struct sun4i_codec *scodec = snd_soc_card_get_drvdata(card);
	int ret;

	if (event == SND_SOC_DAPM_POST_PMU) {
		if (!scodec->gpio_pa)
			return -ENODEV;
		/* DAPM logs event errors but does not abort the graph: keep PA off. */
		ret = a133_speaker_drc_enable(scodec);
		if (ret)
			return ret;
		gpiod_set_value_cansleep(scodec->gpio_pa, 1);
		msleep(scodec->pa_delay_ms);
	} else {
		gpiod_set_value_cansleep(scodec->gpio_pa, 0);
		return a133_speaker_drc_disable(scodec);
	}
	return 0;
}

static int sun50i_a133_card_init(struct snd_soc_pcm_runtime *rtd)
{
	struct snd_soc_dapm_context *dapm = snd_soc_card_to_dapm(rtd->card);
	int ret;

	ret = sun4i_codec_machine_init(rtd);
	if (ret)
		return ret;
	/* The external amplifier requires an explicit userspace opt-in. */
	ret = snd_soc_dapm_disable_pin(dapm, "Speaker");
	if (ret)
		return ret;
	/* Recording is opt-in; board routing must identify the wired ADC input. */
	ret = snd_soc_dapm_disable_pin(dapm, "Onboard Mic");
	if (ret)
		return ret;
	return snd_soc_dapm_sync(dapm);
}

static const struct snd_kcontrol_new sun50i_a133_card_controls[] = {
	SOC_DAPM_PIN_SWITCH("Onboard Mic"),
	SOC_DAPM_PIN_SWITCH("Headphone"),
	SOC_DAPM_PIN_SWITCH("Speaker"),
};

static const struct snd_soc_dapm_widget sun50i_a133_card_widgets[] = {
	SND_SOC_DAPM_MIC("Onboard Mic", NULL),
	SND_SOC_DAPM_HP("Headphone", NULL),
	SND_SOC_DAPM_SPK("Speaker", sun50i_a133_speaker_event),
};

static struct snd_soc_card *sun50i_a133_codec_create_card(struct device *dev)
{
	struct snd_soc_card *card;
	int ret;

	card = devm_kzalloc(dev, sizeof(*card), GFP_KERNEL);
	if (!card)
		return ERR_PTR(-ENOMEM);
	card->dai_link = sun4i_codec_create_link(dev, &card->num_links);
	if (!card->dai_link)
		return ERR_PTR(-ENOMEM);
	/* DAI shutdown releases clocks; power down the analog path first. */
	card->dai_link->ignore_pmdown_time = true;
	card->dai_link->init = sun50i_a133_card_init;
	card->dev = dev;
	card->owner = THIS_MODULE;
	card->name = "A133 Audio Codec";
	card->driver_name = "sun4i-codec";
	card->controls = sun50i_a133_card_controls;
	card->num_controls = ARRAY_SIZE(sun50i_a133_card_controls);
	card->dapm_widgets = sun50i_a133_card_widgets;
	card->num_dapm_widgets = ARRAY_SIZE(sun50i_a133_card_widgets);
	card->fully_routed = true;
	ret = snd_soc_of_parse_audio_routing(card, "allwinner,audio-routing");
	if (ret)
		return ERR_PTR(ret);
	return card;
}

#define SUN50I_A133_RATES (SNDRV_PCM_RATE_8000 | SNDRV_PCM_RATE_11025 | \
	SNDRV_PCM_RATE_12000 | SNDRV_PCM_RATE_16000 | SNDRV_PCM_RATE_22050 | \
	SNDRV_PCM_RATE_24000 | SNDRV_PCM_RATE_32000 | SNDRV_PCM_RATE_44100 | \
	SNDRV_PCM_RATE_48000 | SNDRV_PCM_RATE_96000 | SNDRV_PCM_RATE_192000)

static struct snd_soc_dai_driver sun50i_a133_codec_dai = {
	.name = "Codec",
	.ops = &a133_codec_dai_ops,
	.playback = {
		.stream_name = "Codec Playback",
		.channels_min = 2,
		.channels_max = 2,
		.rates = SUN50I_A133_RATES,
		.rate_min = 8000,
		.rate_max = 192000,
		.formats = SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S24_LE,
		.sig_bits = 24,
	},
	.capture = {
		.stream_name = "Codec Capture",
		.channels_min = 1,
		.channels_max = 2,
		.rates = SUN50I_A133_RATES & ~(SNDRV_PCM_RATE_96000 | SNDRV_PCM_RATE_192000),
		.rate_min = 8000,
		.rate_max = 48000,
		.formats = SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S24_LE,
		.sig_bits = 24,
	},
};

static struct snd_soc_dai_driver sun50i_a133_cpu_dai = {
	.name = "sun4i-codec-cpu-dai",
	.playback = {
		.stream_name = "Playback",
		.channels_min = 2,
		.channels_max = 2,
		.rates = SUN50I_A133_RATES,
		.formats = SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S24_LE,
		.sig_bits = 24,
	},
	.ops = &dummy_dai_ops,
	.capture = {
		.stream_name = "Capture",
		.channels_min = 1,
		.channels_max = 2,
		.rates = SUN50I_A133_RATES & ~(SNDRV_PCM_RATE_96000 | SNDRV_PCM_RATE_192000),
		.rate_min = 8000,
		.rate_max = 48000,
		.formats = SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S24_LE,
		.sig_bits = 24,
	},
};

static const struct regmap_config sun50i_a133_regmap_config = {
	.reg_bits = 32,
	.reg_stride = 4,
	.val_bits = 32,
	.max_register = 0x32c,
};

static void a133_codec_apb_release(void *data)
{
	struct sun4i_codec *scodec = data;

	if (scodec->a133_apb_on) {
		clk_disable_unprepare(scodec->clk_apb);
		scodec->a133_apb_on = false;
	}
}

static int a133_codec_apb_enable(struct sun4i_codec *scodec)
{
	int ret;

	if (scodec->a133_apb_on)
		return 0;
	ret = clk_prepare_enable(scodec->clk_apb);
	if (!ret)
		scodec->a133_apb_on = true;
	return ret;
}

/* Explicit ownership permits balanced suspend/resume and partial-failure
 * cleanup. Keep shared voltages unchanged, exactly as the original probe.
 */
static void a133_codec_supplies_release(void *data)
{
	struct sun4i_codec *scodec = data;
	int i;

	for (i = ARRAY_SIZE(scodec->a133_supply) - 1; i >= 0; i--) {
		if (scodec->a133_supply_on[i]) {
			regulator_disable(scodec->a133_supply[i]);
			scodec->a133_supply_on[i] = false;
		}
	}
}

static int a133_codec_supply_enable(struct sun4i_codec *scodec, unsigned int i)
{
	int ret;

	if (scodec->a133_supply_on[i])
		return 0;
	ret = regulator_enable(scodec->a133_supply[i]);
	if (ret)
		return ret;
	scodec->a133_supply_on[i] = true;
	ret = regulator_get_voltage(scodec->a133_supply[i]);
	return ret < 0 ? ret : ret == 1800000 ? 0 : -EINVAL;
}

static int sun50i_a133_codec_resources(struct sun4i_codec *scodec)
{
	struct device *dev = scodec->dev;
	int ret;

	/* Do not accept dummy supplies or change a shared regulator voltage. */
	if (!of_property_present(dev->of_node, "avcc-supply") ||
	    !of_property_present(dev->of_node, "cpvin-supply"))
		return -EINVAL;
	scodec->a133_supply[0] = devm_regulator_get(dev, "avcc");
	if (IS_ERR(scodec->a133_supply[0]))
		return PTR_ERR(scodec->a133_supply[0]);
	scodec->a133_supply[1] = devm_regulator_get(dev, "cpvin");
	if (IS_ERR(scodec->a133_supply[1]))
		return PTR_ERR(scodec->a133_supply[1]);
	ret = devm_add_action_or_reset(dev, a133_codec_supplies_release, scodec);
	if (ret)
		return ret;
	ret = a133_codec_supply_enable(scodec, 0);
	if (ret)
		return ret;
	ret = a133_codec_supply_enable(scodec, 1);
	if (ret)
		return ret;

	scodec->a133_pll_audio = devm_clk_get(dev, "pll-audio");
	if (IS_ERR(scodec->a133_pll_audio))
		return PTR_ERR(scodec->a133_pll_audio);
	scodec->a133_pll_com_audio = devm_clk_get(dev, "pll-com-audio");
	if (IS_ERR(scodec->a133_pll_com_audio))
		return PTR_ERR(scodec->a133_pll_com_audio);

	mutex_init(&scodec->a133_clock_lock);
	mutex_init(&scodec->a133_volume_lock);
	scodec->a133_dac_volume = 0xa0a0;
	scodec->a133_dac_muted = true;
	scodec->a133_stream[SNDRV_PCM_STREAM_PLAYBACK].module = scodec->clk_module;
	scodec->a133_stream[SNDRV_PCM_STREAM_CAPTURE].module = devm_clk_get(dev, "adc");
	if (IS_ERR(scodec->a133_stream[SNDRV_PCM_STREAM_CAPTURE].module))
		return PTR_ERR(scodec->a133_stream[SNDRV_PCM_STREAM_CAPTURE].module);

	/* No PLL retuning until the PCM stream chooses its rate family. */
	return devm_add_action_or_reset(dev, sun50i_a133_release_stream_clocks,
				       scodec);
}

struct sun4i_codec_quirks {
	const struct regmap_config *regmap_config;
	const struct snd_soc_component_driver *codec;
	struct snd_soc_card * (*create_card)(struct device *dev);
	struct reg_field reg_adc_fifoc;	/* used for regmap_field */
	struct reg_field reg_dac_fifoc;	/* used for regmap_field */
	unsigned int reg_dac_txdata;	/* TX FIFO offset for DMA config */
	unsigned int reg_adc_rxdata;	/* RX FIFO offset for DMA config */
	bool has_reset;
	bool playback_only;
	u32 dma_max_burst;
};

static const struct sun4i_codec_quirks sun4i_codec_quirks = {
	.regmap_config	= &sun4i_codec_regmap_config,
	.codec		= &sun4i_codec_codec,
	.create_card	= sun4i_codec_create_card,
	.reg_adc_fifoc	= REG_FIELD(SUN4I_CODEC_ADC_FIFOC, 0, 31),
	.reg_dac_fifoc	= REG_FIELD(SUN4I_CODEC_DAC_FIFOC, 0, 31),
	.reg_dac_txdata	= SUN4I_CODEC_DAC_TXDATA,
	.reg_adc_rxdata	= SUN4I_CODEC_ADC_RXDATA,
	.dma_max_burst	= SUN4I_DMA_MAX_BURST,
};

static const struct sun4i_codec_quirks sun6i_a31_codec_quirks = {
	.regmap_config	= &sun6i_codec_regmap_config,
	.codec		= &sun6i_codec_codec,
	.create_card	= sun6i_codec_create_card,
	.reg_adc_fifoc	= REG_FIELD(SUN6I_CODEC_ADC_FIFOC, 0, 31),
	.reg_dac_fifoc	= REG_FIELD(SUN4I_CODEC_DAC_FIFOC, 0, 31),
	.reg_dac_txdata	= SUN4I_CODEC_DAC_TXDATA,
	.reg_adc_rxdata	= SUN6I_CODEC_ADC_RXDATA,
	.has_reset	= true,
	.dma_max_burst	= SUN4I_DMA_MAX_BURST,
};

static const struct sun4i_codec_quirks sun7i_codec_quirks = {
	.regmap_config	= &sun7i_codec_regmap_config,
	.codec		= &sun7i_codec_codec,
	.create_card	= sun4i_codec_create_card,
	.reg_adc_fifoc	= REG_FIELD(SUN4I_CODEC_ADC_FIFOC, 0, 31),
	.reg_dac_fifoc	= REG_FIELD(SUN4I_CODEC_DAC_FIFOC, 0, 31),
	.reg_dac_txdata	= SUN4I_CODEC_DAC_TXDATA,
	.reg_adc_rxdata	= SUN4I_CODEC_ADC_RXDATA,
	.dma_max_burst	= SUN4I_DMA_MAX_BURST,
};

static const struct sun4i_codec_quirks sun8i_a23_codec_quirks = {
	.regmap_config	= &sun8i_a23_codec_regmap_config,
	.codec		= &sun8i_a23_codec_codec,
	.create_card	= sun8i_a23_codec_create_card,
	.reg_adc_fifoc	= REG_FIELD(SUN6I_CODEC_ADC_FIFOC, 0, 31),
	.reg_dac_fifoc	= REG_FIELD(SUN4I_CODEC_DAC_FIFOC, 0, 31),
	.reg_dac_txdata	= SUN4I_CODEC_DAC_TXDATA,
	.reg_adc_rxdata	= SUN6I_CODEC_ADC_RXDATA,
	.has_reset	= true,
	.dma_max_burst	= SUN4I_DMA_MAX_BURST,
};

static const struct sun4i_codec_quirks sun8i_h3_codec_quirks = {
	.regmap_config	= &sun8i_h3_codec_regmap_config,
	/*
	 * TODO Share the codec structure with A23 for now.
	 * This should be split out when adding digital audio
	 * processing support for the H3.
	 */
	.codec		= &sun8i_a23_codec_codec,
	.create_card	= sun8i_h3_codec_create_card,
	.reg_adc_fifoc	= REG_FIELD(SUN6I_CODEC_ADC_FIFOC, 0, 31),
	.reg_dac_fifoc	= REG_FIELD(SUN4I_CODEC_DAC_FIFOC, 0, 31),
	.reg_dac_txdata	= SUN8I_H3_CODEC_DAC_TXDATA,
	.reg_adc_rxdata	= SUN6I_CODEC_ADC_RXDATA,
	.has_reset	= true,
	.dma_max_burst	= SUN4I_DMA_MAX_BURST,
};

static const struct sun4i_codec_quirks sun8i_v3s_codec_quirks = {
	.regmap_config	= &sun8i_v3s_codec_regmap_config,
	/*
	 * TODO The codec structure should be split out, like
	 * H3, when adding digital audio processing support.
	 */
	.codec		= &sun8i_a23_codec_codec,
	.create_card	= sun8i_v3s_codec_create_card,
	.reg_adc_fifoc	= REG_FIELD(SUN6I_CODEC_ADC_FIFOC, 0, 31),
	.reg_dac_fifoc	= REG_FIELD(SUN4I_CODEC_DAC_FIFOC, 0, 31),
	.reg_dac_txdata	= SUN8I_H3_CODEC_DAC_TXDATA,
	.reg_adc_rxdata	= SUN6I_CODEC_ADC_RXDATA,
	.has_reset	= true,
	.dma_max_burst	= SUN4I_DMA_MAX_BURST,
};

static const struct sun4i_codec_quirks sun50i_a133_codec_quirks = {
	.regmap_config = &sun50i_a133_regmap_config,
	.codec = &sun50i_a133_codec_component,
	.create_card = sun50i_a133_codec_create_card,
	.reg_dac_fifoc = REG_FIELD(0x10, 0, 31),
	.reg_dac_txdata = 0x20,
	.has_reset = true,
	.reg_adc_fifoc = REG_FIELD(SUN50I_A133_ADC_FIFOC, 0, 31),
	.reg_adc_rxdata = 0x40,
	.dma_max_burst = 4,
};

static const struct sun4i_codec_quirks sun50i_h616_codec_quirks = {
	.regmap_config	= &sun50i_h616_codec_regmap_config,
	.codec		= &sun50i_h616_codec_codec,
	.create_card	= sun50i_h616_codec_create_card,
	.reg_dac_fifoc	= REG_FIELD(SUN50I_H616_CODEC_DAC_FIFOC, 0, 31),
	.reg_dac_txdata	= SUN8I_H3_CODEC_DAC_TXDATA,
	.has_reset	= true,
	.playback_only	= true,
	.dma_max_burst	= SUN4I_DMA_MAX_BURST,
};

static const struct sun4i_codec_quirks suniv_f1c100s_codec_quirks = {
	.regmap_config	= &suniv_codec_regmap_config,
	.codec		= &suniv_codec_codec,
	.create_card	= suniv_codec_create_card,
	.reg_adc_fifoc	= REG_FIELD(SUNIV_CODEC_ADC_FIFOC, 0, 31),
	.reg_dac_fifoc	= REG_FIELD(SUN4I_CODEC_DAC_FIFOC, 0, 31),
	.reg_dac_txdata	= SUN4I_CODEC_DAC_TXDATA,
	.reg_adc_rxdata	= SUNIV_CODEC_ADC_RXDATA,
	.has_reset	= true,
	.dma_max_burst	= SUNIV_DMA_MAX_BURST,
};

static const struct of_device_id sun4i_codec_of_match[] = {
	{
		.compatible = "allwinner,sun50i-a133-codec",
		.data = &sun50i_a133_codec_quirks,
	},
	{
		.compatible = "allwinner,sun4i-a10-codec",
		.data = &sun4i_codec_quirks,
	},
	{
		.compatible = "allwinner,sun6i-a31-codec",
		.data = &sun6i_a31_codec_quirks,
	},
	{
		.compatible = "allwinner,sun7i-a20-codec",
		.data = &sun7i_codec_quirks,
	},
	{
		.compatible = "allwinner,sun8i-a23-codec",
		.data = &sun8i_a23_codec_quirks,
	},
	{
		.compatible = "allwinner,sun8i-h3-codec",
		.data = &sun8i_h3_codec_quirks,
	},
	{
		.compatible = "allwinner,sun8i-v3s-codec",
		.data = &sun8i_v3s_codec_quirks,
	},
	{
		.compatible = "allwinner,sun50i-h616-codec",
		.data = &sun50i_h616_codec_quirks,
	},
	{
		.compatible = "allwinner,suniv-f1c100s-codec",
		.data = &suniv_f1c100s_codec_quirks,
	},
	{}
};
MODULE_DEVICE_TABLE(of, sun4i_codec_of_match);

#define A133_HMIC_BIAS 0x318
#define A133_HMIC_CTRL 0x328
#define A133_HMIC_STATUS 0x32c
#define A133_JACK_EVENTS (BIT(3) | BIT(4))
#define A133_JACK_IRQS (BIT(1) | BIT(2))
#define A133_DISCARD_CONFIG GENMASK(14, 13)
#define A133_DETECTOR_CONFIG (BIT(23) | BIT(18) | BIT(17))
#define A133_DEBOUNCE_CONFIG GENMASK(9, 6)

static void a133_jack_work(struct work_struct *work)
{
	struct sun4i_codec *scodec = container_of(to_delayed_work(work),
					       struct sun4i_codec, jack_work);
	unsigned int events;
	int ret;

	mutex_lock(&scodec->jack_lock);
	if (!scodec->jack_active || scodec->jack_fault) {
		mutex_unlock(&scodec->jack_lock);
		return;
	}
	events = scodec->jack_pending;
	scodec->jack_pending = 0;
	mutex_unlock(&scodec->jack_lock);

	/* Coalesced opposite edges cannot establish the final physical state. */
	if (events != BIT(3) && events != BIT(4))
		return;

	/* Do not expose a fictitious absent state before initial detection.
	 * Work is serialized, and remove cancels it before card teardown.
	 * No DAPM pins: headphone reporting does not select the speaker route.
	 */
	if (!scodec->jack_registered) {
		ret = snd_soc_card_jack_new(scodec->jack_card, "Headphone",
					    SND_JACK_HEADPHONE,
					    &scodec->headphone_jack);
		if (ret) {
			dev_err(scodec->dev, "Cannot register headphone jack: %d\n", ret);
			return;
		}
		scodec->jack_registered = true;
	}
	snd_soc_jack_report(&scodec->headphone_jack,
			    events == BIT(3) ? SND_JACK_HEADPHONE : 0,
			    SND_JACK_HEADPHONE);
}

static irqreturn_t a133_jack_irq(int irq, void *data)
{
	struct sun4i_codec *scodec = data;
	unsigned int status, events;
	int ret;

	mutex_lock(&scodec->jack_lock);
	if (!scodec->jack_active || scodec->jack_fault)
		goto out;
	ret = regmap_read(scodec->regmap, A133_HMIC_STATUS, &status);
	if (ret)
		goto fault;
	events = status & A133_JACK_EVENTS;
	if (!events) {
		/* No guessed source acknowledgment on this dedicated IRQ. */
		ret = -EIO;
		goto fault;
	}
	/* Preserve mixed-register configuration; do not ACK unrelated events. */
	ret = regmap_write(scodec->regmap, A133_HMIC_STATUS,
			   (status & A133_DISCARD_CONFIG) | events);
	if (ret)
		goto fault;
	scodec->jack_pending |= events;
	mod_delayed_work(system_wq, &scodec->jack_work, msecs_to_jiffies(250));
	mutex_unlock(&scodec->jack_lock);
	return IRQ_HANDLED;

fault:
	/* Avoid an interrupt storm if the observed source is not understood. */
	regmap_update_bits(scodec->regmap, A133_HMIC_CTRL, A133_JACK_IRQS, 0);
	scodec->jack_fault = true;
	disable_irq_nosync(irq);
	dev_err(scodec->dev, "headphone detection stopped on unexpected/read error %d\n", ret);
out:
	mutex_unlock(&scodec->jack_lock);
	return IRQ_HANDLED;
}

static void a133_jack_stop(void *data)
{
	struct sun4i_codec *scodec = data;
	bool fault;

	mutex_lock(&scodec->jack_lock);
	if (!scodec->jack_active) {
		mutex_unlock(&scodec->jack_lock);
		return;
	}
	scodec->jack_active = false;
	fault = scodec->jack_fault;
	regmap_update_bits(scodec->regmap, A133_HMIC_CTRL, A133_JACK_IRQS, 0);
	mutex_unlock(&scodec->jack_lock);
	if (!fault)
		disable_irq(scodec->jack_irq);
	else
		synchronize_irq(scodec->jack_irq);
	cancel_delayed_work_sync(&scodec->jack_work);
	/* Restore only owned configuration, never replay pending W1C bits. */
	regmap_update_bits(scodec->regmap, A133_HMIC_BIAS,
			   A133_DETECTOR_CONFIG, scodec->jack_saved_bias);
	regmap_update_bits(scodec->regmap, A133_HMIC_CTRL,
			   A133_DEBOUNCE_CONFIG | A133_JACK_IRQS,
			   scodec->jack_saved_ctrl);
}

static int a133_jack_start(struct platform_device *pdev,
				   struct sun4i_codec *scodec)
{
	unsigned int bias, ctrl;
	int ret;

	mutex_init(&scodec->jack_lock);
	INIT_DELAYED_WORK(&scodec->jack_work, a133_jack_work);
	ret = regmap_read(scodec->regmap, A133_HMIC_BIAS, &bias);
	if (ret)
		return ret;
	ret = regmap_read(scodec->regmap, A133_HMIC_CTRL, &ctrl);
	if (ret)
		return ret;
	/* Do not take over another detector or microphone-button owner. */
	if ((bias & BIT(23)) || (ctrl & (A133_JACK_IRQS | BIT(0))))
		return -EBUSY;
	/* Stock and vendor agree on these analog/filter defaults. */
	if ((bias & GENMASK(31, 24)) != 0x40000000)
		return -EOPNOTSUPP;
	scodec->jack_saved_bias = bias & A133_DETECTOR_CONFIG;
	scodec->jack_saved_ctrl = ctrl & (A133_DEBOUNCE_CONFIG | A133_JACK_IRQS);
	scodec->jack_irq = platform_get_irq(pdev, 0);
	if (scodec->jack_irq < 0)
		return scodec->jack_irq;
	scodec->jack_active = true;
	ret = devm_request_threaded_irq(&pdev->dev, scodec->jack_irq, NULL,
				      a133_jack_irq, IRQF_ONESHOT,
				      "a133-headphone", scodec);
	if (ret) {
		scodec->jack_active = false;
		return ret;
	}
	ret = devm_add_action_or_reset(&pdev->dev, a133_jack_stop, scodec);
	if (ret)
		return ret;
	/* The IRQ handler cannot run setup concurrently. Sources are still off. */
	mutex_lock(&scodec->jack_lock);
	/* A pending source may have faulted immediately after IRQ request. */
	if (scodec->jack_fault) {
		ret = -EIO;
		goto setup_done;
	}
	ret = regmap_read(scodec->regmap, A133_HMIC_STATUS, &ctrl);
	if (ret)
		goto setup_done;
	if ((ctrl & A133_DISCARD_CONFIG) != A133_DISCARD_CONFIG) {
		ret = -EOPNOTSUPP;
		goto setup_done;
	}
	ret = regmap_write(scodec->regmap, A133_HMIC_STATUS,
			   A133_DISCARD_CONFIG | (ctrl & A133_JACK_EVENTS));
	if (ret)
		goto setup_done;
	/* Vendor HMIC_N=1: 125 ms * (1 + 1), with microphone IRQ untouched. */
	ret = regmap_update_bits(scodec->regmap, A133_HMIC_CTRL,
				A133_DEBOUNCE_CONFIG, BIT(6));
	if (ret)
		goto setup_done;
	/* Stock overrides DT: DETMODE=1, AUTOPLEN=0; leave mic circuitry alone. */
	ret = regmap_update_bits(scodec->regmap, A133_HMIC_BIAS,
				A133_DETECTOR_CONFIG, BIT(23) | BIT(18));
	if (ret)
		goto setup_done;
	ret = regmap_update_bits(scodec->regmap, A133_HMIC_CTRL,
				A133_JACK_IRQS, A133_JACK_IRQS);
setup_done:
	mutex_unlock(&scodec->jack_lock);
	if (ret) {
		a133_jack_stop(scodec);
		return ret;
	}
	return 0;
}


static int sun4i_codec_probe(struct platform_device *pdev)
{
	struct snd_soc_card *card;
	struct sun4i_codec *scodec;
	const struct sun4i_codec_quirks *quirks;
	struct resource *res;
	void __iomem *base;
	int ret;

	scodec = devm_kzalloc(&pdev->dev, sizeof(*scodec), GFP_KERNEL);
	if (!scodec)
		return -ENOMEM;

	scodec->dev = &pdev->dev;

	base = devm_platform_get_and_ioremap_resource(pdev, 0, &res);
	if (IS_ERR(base))
		return PTR_ERR(base);

	quirks = of_device_get_match_data(&pdev->dev);
	if (quirks == NULL) {
		dev_err(&pdev->dev, "Failed to determine the quirks to use\n");
		return -ENODEV;
	}

	scodec->regmap = devm_regmap_init_mmio(&pdev->dev, base,
					       quirks->regmap_config);
	if (IS_ERR(scodec->regmap))
		return dev_err_probe(&pdev->dev, PTR_ERR(scodec->regmap),
				     "Failed to create our regmap\n");

	scodec->a133 = quirks == &sun50i_a133_codec_quirks;
	/* Track the A133 APB vote explicitly; legacy ownership is unchanged. */
	scodec->clk_apb = scodec->a133 ? devm_clk_get(&pdev->dev, "apb") :
		devm_clk_get_enabled(&pdev->dev, "apb");
	if (IS_ERR(scodec->clk_apb))
		return dev_err_probe(&pdev->dev, PTR_ERR(scodec->clk_apb),
				     "Failed to get the APB clock\n");

	scodec->clk_module = devm_clk_get(&pdev->dev, "codec");
	if (IS_ERR(scodec->clk_module))
		return dev_err_probe(&pdev->dev, PTR_ERR(scodec->clk_module),
				     "Failed to get the module clock\n");

	if (scodec->a133) {
		ret = devm_add_action_or_reset(&pdev->dev, a133_codec_apb_release, scodec);
		if (ret)
			return ret;
		ret = a133_codec_apb_enable(scodec);
		if (ret)
			return ret;
		ret = sun50i_a133_codec_resources(scodec);
		if (ret)
			return dev_err_probe(&pdev->dev, ret, "Audio resources unavailable\n");
	}

	if (quirks->has_reset) {
		scodec->rst = devm_reset_control_get_exclusive_deasserted(&pdev->dev, NULL);
		if (IS_ERR(scodec->rst))
			return dev_err_probe(&pdev->dev, PTR_ERR(scodec->rst),
					     "Failed to get reset control\n");
	}

	scodec->gpio_pa = devm_gpiod_get_optional(&pdev->dev, "allwinner,pa",
						  GPIOD_OUT_LOW);
	if (IS_ERR(scodec->gpio_pa))
		return dev_err_probe(&pdev->dev, PTR_ERR(scodec->gpio_pa),
				     "Failed to get pa gpio\n");
	if (scodec->a133) {
		of_property_read_u32(pdev->dev.of_node, "allwinner,pa-delay-ms",
				     &scodec->pa_delay_ms);
		if (scodec->pa_delay_ms > 1000)
			return -EINVAL;
	}


	scodec->gpio_hp = devm_gpiod_get_optional(&pdev->dev, "hp-det", GPIOD_IN);
	if (IS_ERR(scodec->gpio_hp))
		return dev_err_probe(&pdev->dev, PTR_ERR(scodec->gpio_hp),
				     "Failed to get hp-det gpio\n");

	/* reg_field setup */
	scodec->reg_adc_fifoc = devm_regmap_field_alloc(&pdev->dev,
							scodec->regmap,
							quirks->reg_adc_fifoc);
	if (IS_ERR(scodec->reg_adc_fifoc))
		return dev_err_probe(&pdev->dev, PTR_ERR(scodec->reg_adc_fifoc),
				     "Failed to create regmap fields\n");

	scodec->reg_dac_fifoc = devm_regmap_field_alloc(&pdev->dev,
							scodec->regmap,
							quirks->reg_dac_fifoc);
	if (IS_ERR(scodec->reg_dac_fifoc))
		return dev_err_probe(&pdev->dev, PTR_ERR(scodec->reg_dac_fifoc),
				     "Failed to create regmap fields\n");

	/* DMA configuration for TX FIFO */
	scodec->playback_dma_data.addr = res->start + quirks->reg_dac_txdata;
	scodec->playback_dma_data.maxburst = quirks->dma_max_burst;
	scodec->playback_dma_data.addr_width = DMA_SLAVE_BUSWIDTH_2_BYTES;

	if (!quirks->playback_only) {
		/* DMA configuration for RX FIFO */
		scodec->capture_dma_data.addr = res->start +
						quirks->reg_adc_rxdata;
		scodec->capture_dma_data.maxburst = quirks->dma_max_burst;
		scodec->capture_dma_data.addr_width = DMA_SLAVE_BUSWIDTH_2_BYTES;
	}

	ret = devm_snd_soc_register_component(&pdev->dev, quirks->codec,
				     scodec->a133 ? &sun50i_a133_codec_dai :
				     &sun4i_codec_dai, 1);
	if (ret)
		return ret;

	ret = devm_snd_soc_register_component(&pdev->dev,
					      &sun4i_codec_component,
					      scodec->a133 ? &sun50i_a133_cpu_dai :
					      &dummy_cpu_dai, 1);
	if (ret)
		return ret;

	ret = devm_snd_dmaengine_pcm_register(&pdev->dev, NULL, 0);
	if (ret)
		return ret;

	card = quirks->create_card(&pdev->dev);
	if (IS_ERR(card))
		return PTR_ERR(card);

	snd_soc_card_set_drvdata(card, scodec);

	ret = snd_soc_register_card(card);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "Failed to register our card\n");

	if (scodec->a133) {
		/* Deferred ASoC binding has no live ALSA card for jack reporting. */
		if (!card->instantiated) {
			snd_soc_unregister_card(card);
			return -EPROBE_DEFER;
		}
		scodec->jack_card = card;
		ret = a133_jack_start(pdev, scodec);
		if (ret) {
			snd_soc_unregister_card(card);
			return dev_err_probe(&pdev->dev, ret,
					     "Headphone detection unavailable\n");
		}
	}
	return 0;
}

static void sun4i_codec_remove(struct platform_device *pdev)
{
	struct snd_soc_card *card = platform_get_drvdata(pdev);
	struct sun4i_codec *scodec = snd_soc_card_get_drvdata(card);

	if (scodec->a133)
		a133_jack_stop(scodec);
	snd_soc_unregister_card(card);
}

static struct platform_driver sun4i_codec_driver = {
	.driver = {
		.name = "sun4i-codec",
		.of_match_table = sun4i_codec_of_match,
	},
	.probe = sun4i_codec_probe,
	.remove = sun4i_codec_remove,
};
module_platform_driver(sun4i_codec_driver);

MODULE_DESCRIPTION("Allwinner A10 codec driver");
MODULE_AUTHOR("Emilio López <emilio@elopez.com.ar>");
MODULE_AUTHOR("Jon Smirl <jonsmirl@gmail.com>");
MODULE_AUTHOR("Maxime Ripard <maxime.ripard@free-electrons.com>");
MODULE_AUTHOR("Chen-Yu Tsai <wens@csie.org>");
MODULE_AUTHOR("Ryan Walklin <ryan@testtoast.com");
MODULE_AUTHOR("Mesih Kilinc <mesikilinc@gmail.com>");
MODULE_LICENSE("GPL");
