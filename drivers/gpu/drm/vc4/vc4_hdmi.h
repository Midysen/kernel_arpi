#ifndef _VC4_HDMI_H_
#define _VC4_HDMI_H_

#include <drm/drm_connector.h>
#include <media/cec.h>
#include <sound/dmaengine_pcm.h>
#include <sound/soc.h>

#include "vc4_drv.h"

/* VC4 HDMI encoder KMS struct */
struct vc4_hdmi_encoder {
	struct vc4_encoder base;
	bool hdmi_monitor;
	bool limited_rgb_range;
};

static inline struct vc4_hdmi_encoder *
to_vc4_hdmi_encoder(struct drm_encoder *encoder)
{
	return container_of(encoder, struct vc4_hdmi_encoder, base.base);
}

struct drm_display_mode;

struct vc4_hdmi;
struct vc4_hdmi_register;

enum vc4_hdmi_phy_channel {
	PHY_LANE_0 = 0,
	PHY_LANE_1,
	PHY_LANE_2,
	PHY_LANE_CK,
};

struct vc4_hdmi_variant {
	/* On devices that have multiple, different instances (like
	 * the BCM2711), which instance is that variant useful for.
	 */
	unsigned int id;

	/* Set to true when the audio support is available */
	bool audio_available;

	/* Set to true when the CEC support is available */
	bool cec_available;

	/* Maximum pixel clock supported by the controller (in Hz) */
	unsigned long long max_pixel_clock;

	/* List of the registers available on that variant */
	const struct vc4_hdmi_register *registers;

	/* Number of registers on that variant */
	unsigned int num_registers;

	/* BCM2711 Only.
	 * The variants don't map the lane in the same order in the
	 * PHY, so this is an array mapping the HDMI channel (index)
	 * to the PHY lane (value).
	 */
	enum vc4_hdmi_phy_channel phy_lane_mapping[4];

	/* Callback to get the resources (memory region, interrupts,
	 * clocks, etc) for that variant.
	 */
	int (*init_resources)(struct vc4_hdmi *vc4_hdmi);

	/* Callback to reset the HDMI block */
	void (*reset)(struct vc4_hdmi *vc4_hdmi);

	/* Callback to enable / disable the CSC */
	void (*csc_setup)(struct vc4_hdmi *vc4_hdmi, bool enable);

	/* Callback to configure the video timings in the HDMI block */
	void (*set_timings)(struct vc4_hdmi *vc4_hdmi,
			    struct drm_display_mode *mode);

	/* Callback to initialize the PHY according to the mode */
	void (*phy_init)(struct vc4_hdmi *vc4_hdmi,
			 struct drm_display_mode *mode);

	/* Callback to disable the PHY */
	void (*phy_disable)(struct vc4_hdmi *vc4_hdmi);

	/* Callback to enable the RNG in the PHY */
	void (*phy_rng_enable)(struct vc4_hdmi *vc4_hdmi);

	/* Callback to disable the RNG in the PHY */
	void (*phy_rng_disable)(struct vc4_hdmi *vc4_hdmi);

	/* Callback to get hsm clock */
	u32 (*get_hsm_clock)(struct vc4_hdmi *vc4_hdmi);

	/* Callback to get hsm clock */
	u32 (*calc_hsm_clock)(struct vc4_hdmi *vc4_hdmi, unsigned long pixel_rate);

	/* Callback to get channel map */
	u32 (*channel_map)(struct vc4_hdmi *vc4_hdmi, u32 channel_mask);
};

/* HDMI audio information */
struct vc4_hdmi_audio {
	struct snd_soc_card card;
	struct snd_soc_dai_link link;
	struct snd_soc_dai_link_component cpu;
	struct snd_soc_dai_link_component codec;
	struct snd_soc_dai_link_component platform;
	int samplerate;
	int channels;
	struct snd_dmaengine_dai_dma_data dma_data;
	struct snd_pcm_substream *substream;

	bool streaming;
};

/* General HDMI hardware state. */
struct vc4_hdmi {
	struct platform_device *pdev;
	const struct vc4_hdmi_variant *variant;

	struct vc4_hdmi_encoder encoder;
	struct drm_connector connector;

	struct vc4_hdmi_audio audio;

	struct i2c_adapter *ddc;
	void __iomem *hdmicore_regs;
	void __iomem *hd_regs;

	/* VC5 Only */
	void __iomem *cec_regs;
	/* VC5 Only */
	void __iomem *csc_regs;
	/* VC5 Only */
	void __iomem *dvp_regs;
	/* VC5 Only */
	void __iomem *phy_regs;
	/* VC5 Only */
	void __iomem *ram_regs;
	/* VC5 Only */
	void __iomem *rm_regs;

	int hpd_gpio;
	bool hpd_active_low;

	struct cec_adapter *cec_adap;
	struct cec_msg cec_rx_msg;
	bool cec_tx_ok;
	bool cec_irq_was_rx;

	struct clk *pixel_clock;
	struct clk *hsm_clock;

	struct reset_control *reset;

	struct debugfs_regset32 hdmi_regset;
	struct debugfs_regset32 hd_regset;
};

static inline struct vc4_hdmi *
connector_to_vc4_hdmi(struct drm_connector *connector)
{
	return container_of(connector, struct vc4_hdmi, connector);
}

static inline struct vc4_hdmi *
encoder_to_vc4_hdmi(struct drm_encoder *encoder)
{
	struct vc4_hdmi_encoder *_encoder = to_vc4_hdmi_encoder(encoder);

	return container_of(_encoder, struct vc4_hdmi, encoder);
}

void vc4_hdmi_phy_init(struct vc4_hdmi *vc4_hdmi,
		       struct drm_display_mode *mode);
void vc4_hdmi_phy_disable(struct vc4_hdmi *vc4_hdmi);
void vc4_hdmi_phy_rng_enable(struct vc4_hdmi *vc4_hdmi);
void vc4_hdmi_phy_rng_disable(struct vc4_hdmi *vc4_hdmi);

void vc5_hdmi_phy_init(struct vc4_hdmi *vc4_hdmi,
		       struct drm_display_mode *mode);
void vc5_hdmi_phy_rng_enable(struct vc4_hdmi *vc4_hdmi);
void vc5_hdmi_phy_rng_disable(struct vc4_hdmi *vc4_hdmi);

#endif /* _VC4_HDMI_H_ */
