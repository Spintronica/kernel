// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025 BAIKAL ELECTRONICS, JSC
 *
 * Baikal BE-L1000 SoC Process, Voltage, Temperature sensor driver.
 */

#include <linux/acpi.h>
#include <linux/clk-provider.h>
#include <linux/clkdev.h>
#include <linux/firmware/baikal/baikal-smc.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/polynomial.h>

#include "baikal-pvt.h"

#define PVT_READ	0
#define PVT_WRITE	1

#define PVT_CLK_RATE	1190000
static const char *pvt_clk_name = "bl1000_pvt_clk";
static struct clk *pvt_clk;
static struct clk_lookup *pvt_clk_lookup;

/*
 * For the sake of the code simplification we created the sensors info table
 * with the sensor names, activation modes, threshold registers base address
 * and the thresholds bit fields.
 */
static const struct pvt_sensor_info pvt_info[] = {
	PVT_SENSOR_INFO(0, "Temperature", hwmon_temp, TEMP, TTHRES),
	PVT_SENSOR_INFO(0, "Voltage", hwmon_in, VOLT, VTHRES),
	PVT_SENSOR_INFO(1, "Low-Vt", hwmon_in, LVT, LTHRES),
	PVT_SENSOR_INFO(2, "Ultra-Low-Vt", hwmon_in, ULVT, UTHRES),
	PVT_SENSOR_INFO(3, "Standard-Vt", hwmon_in, SVT, STHRES),
};

static const struct hwmon_channel_info * const pvt_channel_info[] = {
	HWMON_CHANNEL_INFO(chip, HWMON_C_UPDATE_INTERVAL),
	HWMON_CHANNEL_INFO(temp,
#if defined(CONFIG_SENSORS_BAIKAL_PVT_ALARMS)
			   HWMON_T_MIN | HWMON_T_MIN_ALARM |
			   HWMON_T_MAX | HWMON_T_MAX_ALARM |
#endif
			   HWMON_T_INPUT | HWMON_T_TYPE | HWMON_T_LABEL),
	HWMON_CHANNEL_INFO(in,
#if defined(CONFIG_SENSORS_BAIKAL_PVT_ALARMS)
			   HWMON_I_MIN | HWMON_I_MIN_ALARM |
			   HWMON_I_MAX | HWMON_I_MAX_ALARM |
#endif
			   HWMON_I_INPUT | HWMON_I_LABEL,
#if defined(CONFIG_SENSORS_BAIKAL_PVT_ALARMS)
			   HWMON_I_MIN | HWMON_I_MIN_ALARM |
			   HWMON_I_MAX | HWMON_I_MAX_ALARM |
#endif
			   HWMON_I_INPUT | HWMON_I_LABEL,
#if defined(CONFIG_SENSORS_BAIKAL_PVT_ALARMS)
			   HWMON_I_MIN | HWMON_I_MIN_ALARM |
			   HWMON_I_MAX | HWMON_I_MAX_ALARM |
#endif
			   HWMON_I_INPUT | HWMON_I_LABEL,
#if defined(CONFIG_SENSORS_BAIKAL_PVT_ALARMS)
			   HWMON_I_MIN | HWMON_I_MIN_ALARM |
			   HWMON_I_MAX | HWMON_I_MAX_ALARM |
#endif
			   HWMON_I_INPUT | HWMON_I_LABEL),
	NULL
};

static const struct polynomial bl1000_poly_temp_to_N = {
	.total_divider = 10000,
	.terms = {
		{4,   60818, 10000, 10000},
		{3,    1287, 10000,    10},
		{2,   72244, 10000,    10},
		{1,   36484,  1000,     1},
		{0, 1619800,     1,     1}
	}
};

static const struct polynomial bl1000_poly_N_to_temp = {
	.total_divider = 1,
	.terms = {
		{4,  -18439, 1000, 1},
		{3,   80705, 1000, 1},
		{2, -185010, 1000, 1},
		{1,  328430, 1000, 1},
		{0,  -48690,    1, 1}
	}
};

static const struct polynomial bl1000_poly_volt_to_N = {
	.total_divider = 10,
	.terms = {
		{1, 13905, 1000, 1},
		{0, -5769,    1, 1}
	}
};

static const struct polynomial bl1000_poly_N_to_volt = {
	.total_divider = 10,
	.terms = {
		{1,   100000, 13905,     1},
		{0, 57685000,     1, 13905}
	}
};


static const struct baikal_pvt_data {
	const struct polynomial *temp_to_N;
	const struct polynomial *N_to_temp;
	const struct polynomial *volt_to_N;
	const struct polynomial *N_to_volt;
} baikal_pvt = {
	.temp_to_N = &bl1000_poly_temp_to_N,
	.N_to_temp = &bl1000_poly_N_to_temp,
	.volt_to_N = &bl1000_poly_volt_to_N,
	.N_to_volt = &bl1000_poly_N_to_volt
};

static int baikal_init_pvt(struct pvt_hwmon *pvt)
{
	struct platform_device *pdev = to_platform_device(pvt->dev);
	struct resource *res;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -ENODEV;
	pvt->regs = (void __iomem *)res->start;

	if (!acpi_disabled)
		pvt->clks[PVT_CLOCK_REF].id = pvt_clk_name;

	return 0;
}

static u32 baikal_read_pvt(struct pvt_hwmon *pvt, u32 reg)
{
	struct arm_smccc_res res;

	arm_smccc_smc(BAIKAL_SMC_PVT_CMD, PVT_READ, (unsigned long)pvt->regs,
		      reg, 0, 0, 0, 0, &res);

	return res.a0;
}

static int baikal_write_pvt(struct pvt_hwmon *pvt, u32 reg, u32 data)
{
	struct arm_smccc_res res;

	arm_smccc_smc(BAIKAL_SMC_PVT_CMD, PVT_WRITE, (unsigned long)pvt->regs,
		      reg, data, 0, 0, 0, &res);
	return res.a0;
}

static u32 baikal_to_pvt(struct pvt_hwmon *pvt, enum pvt_sensor_type type,
			 long val)
{
	const struct polynomial *poly = (type == PVT_TEMP) ?
					baikal_pvt.temp_to_N :
					baikal_pvt.volt_to_N;

	return polynomial_calc(poly, val);
}

static long baikal_from_pvt(struct pvt_hwmon *pvt, enum pvt_sensor_type type,
			    u32 data)
{
	const struct polynomial *poly = (type == PVT_TEMP) ?
					baikal_pvt.N_to_temp :
					baikal_pvt.N_to_volt;

	return polynomial_calc(poly, data);
}

static struct pvt_ops baikal_pvt_ops = {
	.init     = baikal_init_pvt,
	.read     = baikal_read_pvt,
	.write    = baikal_write_pvt,
	.to_pvt   = baikal_to_pvt,
	.from_pvt = baikal_from_pvt,
};

static int pvt_probe(struct platform_device *pdev)
{
	return baikal_pvt_create(pdev, &baikal_pvt_ops, pvt_info, pvt_channel_info);
}

static const struct of_device_id pvt_of_match[] = {
	{ .compatible = "baikal,bl1000-pvt" },
	{ }
};
MODULE_DEVICE_TABLE(of, pvt_of_match);

static struct platform_driver pvt_driver = {
	.probe  = pvt_probe,
	.driver = {
		.name = "bl1000-pvt",
		.of_match_table = pvt_of_match
	}
};

static int __init pvt_driver_init(void)
{
	int ret;

	if (!acpi_disabled) {
		pvt_clk = clk_register_fixed_rate(NULL, pvt_clk_name, NULL, 0, PVT_CLK_RATE);
		if (IS_ERR(pvt_clk)) {
			pr_err("%s: Failed to register PVT reference clock\n", __func__);
			return PTR_ERR(pvt_clk);
		}

		pvt_clk_lookup = clkdev_create(pvt_clk, pvt_clk_name, NULL);
		if (!pvt_clk_lookup) {
			clk_unregister_fixed_rate(pvt_clk);
			pr_err("%s: Failed to register PVT reference clock lookup\n",
				__func__);
			return -ENOMEM;
		}
	}

	ret = platform_driver_register(&pvt_driver);
	if (ret) {
		if (!acpi_disabled) {
			clkdev_drop(pvt_clk_lookup);
			clk_unregister_fixed_rate(pvt_clk);
		}
		return ret;
	}

	return 0;
}
module_init(pvt_driver_init);

static void __exit pvt_driver_exit(void)
{
	platform_driver_unregister(&pvt_driver);
	if (!acpi_disabled) {
		clkdev_drop(pvt_clk_lookup);
		clk_unregister_fixed_rate(pvt_clk);
	}
}
module_exit(pvt_driver_exit);

MODULE_DESCRIPTION("Baikal BE-L1000 SoC PVT driver");
MODULE_LICENSE("GPL v2");
