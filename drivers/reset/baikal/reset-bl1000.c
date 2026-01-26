// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2021-2025 Baikal Electronics, JSC
 *
 * Baikal BE-L1000 reset driver
 *
 * This file is licensed under the terms of the GNU General Public
 * License version 2. This program is licensed "as is" without any
 * warranty of any kind, whether express or implied.
 */

#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/reset-controller.h>
#include <linux/types.h>
#include <linux/arm-smccc.h>
#include <linux/firmware/baikal/baikal-smc.h>

#define to_baikal_reset_priv(p)	container_of((p), struct baikal_reset_priv, rcdev)

struct baikal_reset_priv {
	struct reset_controller_dev rcdev;
	u64 base;
	u32 smc_assert;
	u32 smc_deassert;
};

static int baikal_reset_assert(struct reset_controller_dev *rcdev, unsigned long id)
{
	struct baikal_reset_priv *priv = to_baikal_reset_priv(rcdev);
	struct arm_smccc_res res;

	arm_smccc_smc(priv->smc_assert, priv->base, id, 0, 0, 0, 0, 0, &res);
	return res.a0;
}

static int baikal_reset_deassert(struct reset_controller_dev *rcdev, unsigned long id)
{
	struct baikal_reset_priv *priv = to_baikal_reset_priv(rcdev);
	struct arm_smccc_res res;

	arm_smccc_smc(priv->smc_deassert, priv->base, id, 0, 0, 0, 0, 0, &res);
	return res.a0;
}

static const struct reset_control_ops baikal_reset_ops = {
	.assert 	= baikal_reset_assert,
	.deassert 	= baikal_reset_deassert,
};

static int baikal_reset_probe(struct platform_device *pdev)
{
	struct device_node *node = pdev->dev.of_node;
	struct baikal_reset_priv *priv;

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv) {
		return -ENOMEM;
	}

	if (of_property_read_u64(node, "reg", &priv->base)) {
		return -1;
	}

	priv->rcdev.owner = THIS_MODULE;
	priv->rcdev.ops = &baikal_reset_ops;
	priv->rcdev.of_node = pdev->dev.of_node;
	priv->rcdev.of_reset_n_cells = 1;
	priv->rcdev.nr_resets = 32;
	if (of_find_property(node, "active-low", NULL)) {
		priv->smc_assert = BAIKAL_SMC_RST_CLR;
		priv->smc_deassert = BAIKAL_SMC_RST_SET;
	} else {
		priv->smc_assert = BAIKAL_SMC_RST_SET;
		priv->smc_deassert = BAIKAL_SMC_RST_CLR;
	}

	return reset_controller_register(&priv->rcdev);
}

static const struct of_device_id baikal_reset_dt_match[] = {
	{ .compatible = "baikal,bl1000-reset" },
	{ },
};
MODULE_DEVICE_TABLE(of, baikal_reset_dt_match);

static struct platform_driver baikal_reset_driver = {
	.probe	= baikal_reset_probe,
	.driver	= {
		.name = "bl1000-reset",
		.of_match_table = baikal_reset_dt_match,
	},
};
module_platform_driver(baikal_reset_driver);

MODULE_DESCRIPTION("Baikal BE-L1000 reset controller");
MODULE_LICENSE("GPL");
