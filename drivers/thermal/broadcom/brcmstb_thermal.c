// SPDX-License-Identifier: GPL-2.0-only
/*
 * Broadcom STB AVS TMON thermal sensor driver
 *
 * Copyright (c) 2015-2017 Broadcom
 */

#define DRV_NAME	"brcmstb_thermal"

#define pr_fmt(fmt)	DRV_NAME ": " fmt

#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/irqreturn.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of_device.h>
#include <linux/thermal.h>

#define AVS_TMON_STATUS			0x00

#define AVS_TMON_EN_OVERTEMP_RESET	0x04
 #define AVS_TMON_EN_OVERTEMP_RESET_msk	BIT(0)

#define AVS_TMON_RESET_THRESH		0x08
 #define AVS_TMON_RESET_THRESH_msk	GENMASK(10, 1)
 #define AVS_TMON_RESET_THRESH_shift	1

#define AVS_TMON_INT_IDLE_TIME		0x10

#define AVS_TMON_EN_TEMP_INT_SRCS	0x14
 #define AVS_TMON_EN_TEMP_INT_SRCS_high	BIT(1)
 #define AVS_TMON_EN_TEMP_INT_SRCS_low	BIT(0)

#define AVS_TMON_INT_THRESH		0x18
 #define AVS_TMON_INT_THRESH_high_msk	GENMASK(26, 17)
 #define AVS_TMON_INT_THRESH_high_shift	17
 #define AVS_TMON_INT_THRESH_low_msk	GENMASK(10, 1)
 #define AVS_TMON_INT_THRESH_low_shift	1

#define AVS_TMON_TEMP_INT_CODE		0x1c
#define AVS_TMON_TP_TEST_ENABLE		0x20

/* Default coefficients */
#define AVS_TMON_TEMP_SLOPE		487
#define AVS_TMON_TEMP_OFFSET		410040

/* HW related temperature constants */
#define AVS_TMON_TEMP_MAX		0x3ff
#define AVS_TMON_TEMP_MIN		-88161
#define AVS_TMON_TEMP_MASK		AVS_TMON_TEMP_MAX

enum avs_tmon_trip_type {
	TMON_TRIP_TYPE_LOW = 0,
	TMON_TRIP_TYPE_HIGH,
	TMON_TRIP_TYPE_RESET,
	TMON_TRIP_TYPE_MAX,
};

struct avs_tmon_trip {
	/* HW bit to enable the trip */
	u32 enable_offs;
	u32 enable_mask;

	/* HW field to read the trip temperature */
	u32 reg_offs;
	u32 reg_msk;
	int reg_shift;
};

static struct avs_tmon_trip avs_tmon_trips[] = {
	/* Trips when temperature is below threshold */
	[TMON_TRIP_TYPE_LOW] = {
		.enable_offs	= AVS_TMON_EN_TEMP_INT_SRCS,
		.enable_mask	= AVS_TMON_EN_TEMP_INT_SRCS_low,
		.reg_offs	= AVS_TMON_INT_THRESH,
		.reg_msk	= AVS_TMON_INT_THRESH_low_msk,
		.reg_shift	= AVS_TMON_INT_THRESH_low_shift,
	},
	/* Trips when temperature is above threshold */
	[TMON_TRIP_TYPE_HIGH] = {
		.enable_offs	= AVS_TMON_EN_TEMP_INT_SRCS,
		.enable_mask	= AVS_TMON_EN_TEMP_INT_SRCS_high,
		.reg_offs	= AVS_TMON_INT_THRESH,
		.reg_msk	= AVS_TMON_INT_THRESH_high_msk,
		.reg_shift	= AVS_TMON_INT_THRESH_high_shift,
	},
	/* Automatically resets chip when above threshold */
	[TMON_TRIP_TYPE_RESET] = {
		.enable_offs	= AVS_TMON_EN_OVERTEMP_RESET,
		.enable_mask	= AVS_TMON_EN_OVERTEMP_RESET_msk,
		.reg_offs	= AVS_TMON_RESET_THRESH,
		.reg_msk	= AVS_TMON_RESET_THRESH_msk,
		.reg_shift	= AVS_TMON_RESET_THRESH_shift,
	},
};

struct brcmstb_thermal_of_data {
	const struct thermal_zone_of_device_ops *of_ops;
	u32 status_valid_mask;
	u32 status_data_mask;
	u32 status_data_shift;
};

struct brcmstb_thermal_priv {
	void __iomem *tmon_base;
	struct device *dev;
	struct thermal_zone_device *thermal;
	struct clk *clk;
	const struct brcmstb_thermal_of_data *socdata;
};

/* Convert a HW code to a temperature reading (millidegree celsius) */
static inline int avs_tmon_code_to_temp(struct thermal_zone_device *tz,
					u32 code)
{
	return (AVS_TMON_TEMP_OFFSET -
		(int)((code & AVS_TMON_TEMP_MAX) * AVS_TMON_TEMP_SLOPE));
}

/*
 * Convert a temperature value (millidegree celsius) to a HW code
 *
 * @temp: temperature to convert
 * @low: if true, round toward the low side
 */
static inline u32 avs_tmon_temp_to_code(struct thermal_zone_device *tz,
					int temp, bool low)
{
	if (temp < AVS_TMON_TEMP_MIN)
		return AVS_TMON_TEMP_MAX;	/* Maximum code value */

	if (temp >= AVS_TMON_TEMP_OFFSET)
		return 0;	/* Minimum code value */

	if (low)
		return (u32)(DIV_ROUND_UP(AVS_TMON_TEMP_OFFSET - temp,
					  AVS_TMON_TEMP_SLOPE));
	else
		return (u32)((AVS_TMON_TEMP_OFFSET - temp) /
			      AVS_TMON_TEMP_SLOPE);
}

static int brcmstb_get_temp(void *data, int *temp)
{
	struct brcmstb_thermal_priv *priv = data;
	const struct brcmstb_thermal_of_data *socdata = priv->socdata;
	u32 val;
	long t;

	val = __raw_readl(priv->tmon_base + AVS_TMON_STATUS);

	if (!(val & socdata->status_valid_mask)) {
		dev_err(priv->dev, "reading not valid\n");
		return -EIO;
	}

	val = (val & socdata->status_data_mask) >> socdata->status_data_shift;

	t = avs_tmon_code_to_temp(priv->thermal, val);
	if (t < 0)
		*temp = 0;
	else
		*temp = t;

	return 0;
}

static void avs_tmon_trip_enable(struct brcmstb_thermal_priv *priv,
				 enum avs_tmon_trip_type type, int en)
{
	struct avs_tmon_trip *trip = &avs_tmon_trips[type];
	u32 val = __raw_readl(priv->tmon_base + trip->enable_offs);

	dev_dbg(priv->dev, "%sable trip, type %d\n", en ? "en" : "dis", type);

	if (en)
		val |= trip->enable_mask;
	else
		val &= ~trip->enable_mask;

	__raw_writel(val, priv->tmon_base + trip->enable_offs);
}

static int avs_tmon_get_trip_temp(struct brcmstb_thermal_priv *priv,
				  enum avs_tmon_trip_type type)
{
	struct avs_tmon_trip *trip = &avs_tmon_trips[type];
	u32 val = __raw_readl(priv->tmon_base + trip->reg_offs);

	val &= trip->reg_msk;
	val >>= trip->reg_shift;

	return avs_tmon_code_to_temp(priv->thermal, val);
}

static void avs_tmon_set_trip_temp(struct brcmstb_thermal_priv *priv,
				   enum avs_tmon_trip_type type,
				   int temp)
{
	struct avs_tmon_trip *trip = &avs_tmon_trips[type];
	u32 val, orig;

	dev_dbg(priv->dev, "set temp %d to %d\n", type, temp);

	/* round toward low temp for the low interrupt */
	val = avs_tmon_temp_to_code(priv->thermal, temp,
				    type == TMON_TRIP_TYPE_LOW);

	val <<= trip->reg_shift;
	val &= trip->reg_msk;

	orig = __raw_readl(priv->tmon_base + trip->reg_offs);
	orig &= ~trip->reg_msk;
	orig |= val;
	__raw_writel(orig, priv->tmon_base + trip->reg_offs);
}

static int avs_tmon_get_intr_temp(struct brcmstb_thermal_priv *priv)
{
	u32 val;

	val = __raw_readl(priv->tmon_base + AVS_TMON_TEMP_INT_CODE);
	return avs_tmon_code_to_temp(priv->thermal, val);
}

static irqreturn_t brcmstb_tmon_irq_thread(int irq, void *data)
{
	struct brcmstb_thermal_priv *priv = data;
	int low, high, intr;

	low = avs_tmon_get_trip_temp(priv, TMON_TRIP_TYPE_LOW);
	high = avs_tmon_get_trip_temp(priv, TMON_TRIP_TYPE_HIGH);
	intr = avs_tmon_get_intr_temp(priv);

	dev_dbg(priv->dev, "low/intr/high: %d/%d/%d\n",
			low, intr, high);

	/* Disable high-temp until next threshold shift */
	if (intr >= high)
		avs_tmon_trip_enable(priv, TMON_TRIP_TYPE_HIGH, 0);
	/* Disable low-temp until next threshold shift */
	if (intr <= low)
		avs_tmon_trip_enable(priv, TMON_TRIP_TYPE_LOW, 0);

	/*
	 * Notify using the interrupt temperature, in case the temperature
	 * changes before it can next be read out
	 */
	thermal_zone_device_update(priv->thermal, intr);

	return IRQ_HANDLED;
}

static int brcmstb_set_trips(void *data, int low, int high)
{
	struct brcmstb_thermal_priv *priv = data;

	dev_dbg(priv->dev, "set trips %d <--> %d\n", low, high);

	/*
	 * Disable low-temp if "low" is too small. As per thermal framework
	 * API, we use -INT_MAX rather than INT_MIN.
	 */
	if (low <= -INT_MAX) {
		avs_tmon_trip_enable(priv, TMON_TRIP_TYPE_LOW, 0);
	} else {
		avs_tmon_set_trip_temp(priv, TMON_TRIP_TYPE_LOW, low);
		avs_tmon_trip_enable(priv, TMON_TRIP_TYPE_LOW, 1);
	}

	/* Disable high-temp if "high" is too big. */
	if (high == INT_MAX) {
		avs_tmon_trip_enable(priv, TMON_TRIP_TYPE_HIGH, 0);
	} else {
		avs_tmon_set_trip_temp(priv, TMON_TRIP_TYPE_HIGH, high);
		avs_tmon_trip_enable(priv, TMON_TRIP_TYPE_HIGH, 1);
	}

	return 0;
}

static const struct thermal_zone_of_device_ops bcm7445_thermal_of_ops = {
	.get_temp	= brcmstb_get_temp,
	.set_trips	= brcmstb_set_trips,
};

static const struct thermal_zone_of_device_ops bcm2711_thermal_of_ops = {
	.get_temp	= brcmstb_get_temp,
};

static const struct brcmstb_thermal_of_data bcm7445_thermal_of_data = {
	.of_ops = &bcm7445_thermal_of_ops,
	.status_valid_mask = BIT(11),
	.status_data_mask = GENMASK(10, 1),
	.status_data_shift = 1,
};

static const struct brcmstb_thermal_of_data bcm2711_thermal_of_data = {
	.of_ops = &bcm2711_thermal_of_ops,
	.status_valid_mask = BIT(10),
	.status_data_mask = GENMASK(9, 0),
	.status_data_shift = 0,
};

static const struct of_device_id brcmstb_thermal_id_table[] = {
	{ .compatible = "brcm,avs-tmon",
	  .data = &bcm7445_thermal_of_data },
	{ .compatible = "brcm,avs-tmon-bcm2711",
	  .data = &bcm2711_thermal_of_data },
	{},
};
MODULE_DEVICE_TABLE(of, brcmstb_thermal_id_table);

static int brcmstb_thermal_probe(struct platform_device *pdev)
{
	struct thermal_zone_device *thermal;
	struct brcmstb_thermal_priv *priv;
	struct resource *res;
	int irq, ret;

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	priv->tmon_base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(priv->tmon_base))
		return PTR_ERR(priv->tmon_base);

	priv->socdata = of_device_get_match_data(&pdev->dev);
	if (!priv->socdata) {
		dev_err(&pdev->dev, "no device match found\n");
		return -ENODEV;
	}

	priv->clk = devm_clk_get(&pdev->dev, NULL);
	if (IS_ERR(priv->clk) && PTR_ERR(priv->clk) == -EPROBE_DEFER)
		return -EPROBE_DEFER;

	if (!IS_ERR(priv->clk)) {
		ret = clk_prepare_enable(priv->clk);
		if (ret)
			return ret;
	}

	priv->dev = &pdev->dev;
	platform_set_drvdata(pdev, priv);

	thermal = devm_thermal_zone_of_sensor_register(&pdev->dev, 0, priv,
						       priv->socdata->of_ops);
	if (IS_ERR(thermal)) {
		ret = PTR_ERR(thermal);
		dev_err(&pdev->dev, "could not register sensor: %d\n", ret);
		return ret;
	}

	priv->thermal = thermal;

	irq = platform_get_irq(pdev, 0);
	if (irq < 0) {
		dev_err(&pdev->dev, "could not get IRQ\n");
		return irq;
	}
	ret = devm_request_threaded_irq(&pdev->dev, irq, NULL,
					brcmstb_tmon_irq_thread, IRQF_ONESHOT,
					DRV_NAME, priv);
	if (ret < 0) {
		dev_err(&pdev->dev, "could not request IRQ: %d\n", ret);
		return ret;
	}

	dev_info(&pdev->dev, "registered AVS TMON of-sensor driver\n");

	return 0;
}

static struct platform_driver brcmstb_thermal_driver = {
	.probe = brcmstb_thermal_probe,
	.driver = {
		.name = DRV_NAME,
		.of_match_table = brcmstb_thermal_id_table,
	},
};
module_platform_driver(brcmstb_thermal_driver);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Brian Norris");
MODULE_DESCRIPTION("Broadcom STB AVS TMON thermal driver");
