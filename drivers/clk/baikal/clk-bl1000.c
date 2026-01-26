// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2025 Baikal Electronics, JSC
 */

#include <linux/acpi.h>
#include <linux/clk-provider.h>
#include <linux/clk.h>
#include <linux/clkdev.h>
#include <linux/delay.h>
#include <linux/firmware/baikal/baikal-smc.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/spinlock.h>

#define CLK_TYPE_PLL	1
#define CLK_TYPE_CH	2
#define CLK_TYPE_DIV	3
#define CLK_TYPE_MUX	4
#define CLK_TYPE_PVT	5

struct baikal_clk {
	struct clk_hw	hw;
	u64		base;
	unsigned int	index;
};

#define to_baikal_clk(_hw) container_of(_hw, struct baikal_clk, hw)

static int baikal_clk_enable(struct clk_hw *hw)
{
	struct baikal_clk *clk = to_baikal_clk(hw);
	struct arm_smccc_res res;

	arm_smccc_smc(BAIKAL_SMC_CLK_ENABLE, clk->base, clk->index, 0, 0, 0, 0, 0, &res);
	return res.a0;
}

static void baikal_clk_disable(struct clk_hw *hw)
{
	struct baikal_clk *clk = to_baikal_clk(hw);
	struct arm_smccc_res res;

	arm_smccc_smc(BAIKAL_SMC_CLK_DISABLE, clk->base, clk->index, 0, 0, 0, 0, 0, &res);
}

static int baikal_clk_is_enabled(struct clk_hw *hw)
{
	struct baikal_clk *clk = to_baikal_clk(hw);
	struct arm_smccc_res res;

	arm_smccc_smc(BAIKAL_SMC_CLK_IS_ENABLED, clk->base, clk->index, 0, 0, 0, 0, 0, &res);
	return res.a0;
}

static unsigned long baikal_clk_recalc_rate(struct clk_hw *hw,
					    unsigned long parent_rate)
{
	struct baikal_clk *clk = to_baikal_clk(hw);
	struct arm_smccc_res res;

	arm_smccc_smc(BAIKAL_SMC_CLK_GET, clk->base, clk->index, 0, 0, 0, 0, 0, &res);
	return res.a0;
}

static int baikal_clk_set_rate(struct clk_hw *hw, unsigned long rate,
			       unsigned long parent_rate)
{
	struct baikal_clk *clk = to_baikal_clk(hw);
	struct arm_smccc_res res;

	arm_smccc_smc(BAIKAL_SMC_CLK_SET, clk->base, clk->index, rate, 0, 0, 0, 0, &res);
	return res.a0;
}

static long baikal_clk_round_rate(struct clk_hw *hw, unsigned long rate,
				  unsigned long *prate)
{
	struct baikal_clk *clk = to_baikal_clk(hw);
	struct arm_smccc_res res;

	arm_smccc_smc(BAIKAL_SMC_CLK_ROUND, clk->base, clk->index, rate, 0, 0, 0, 0, &res);
	return res.a0;
}

static int baikal_clk_set_parent(struct clk_hw *hw, u8 index)
{
	struct baikal_clk *clk = to_baikal_clk(hw);
	struct arm_smccc_res res;

	arm_smccc_smc(BAIKAL_SMC_CLK_SET_PARENT, clk->base, clk->index, index, 0, 0, 0, 0, &res);
	return res.a0;
}

static u8 baikal_clk_get_parent(struct clk_hw *hw)
{
	struct baikal_clk *clk = to_baikal_clk(hw);
	struct arm_smccc_res res;

	arm_smccc_smc(BAIKAL_SMC_CLK_GET_PARENT, clk->base, clk->index, 0, 0, 0, 0, 0, &res);
	return res.a0;
}

static int baikal_clk_determine_rate(struct clk_hw *hw,
				     struct clk_rate_request *req)
{
	return clk_mux_determine_rate_flags(hw, req, CLK_MUX_ROUND_CLOSEST);
}

static const struct clk_ops baikal_clk_pll_ops = {
	.enable	     = baikal_clk_enable,
	.is_enabled  = baikal_clk_is_enabled,
	.recalc_rate = baikal_clk_recalc_rate,
	.set_rate    = baikal_clk_set_rate,
	.round_rate  = baikal_clk_round_rate,
};

static const struct clk_ops baikal_clk_ops = {
	.enable	     = baikal_clk_enable,
	.disable     = baikal_clk_disable,
	.is_enabled  = baikal_clk_is_enabled,
	.recalc_rate = baikal_clk_recalc_rate,
	.set_rate    = baikal_clk_set_rate,
	.round_rate  = baikal_clk_round_rate,
};

static const struct clk_ops baikal_clk_mux_ops = {
	.set_parent     = baikal_clk_set_parent,
	.get_parent     = baikal_clk_get_parent,
	.determine_rate = baikal_clk_determine_rate,
};

static int baikal_clk_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *node = dev_of_node(dev);
	struct clk_init_data init = { 0 };
	struct baikal_clk *cmu;
	struct clk_onecell_data *clk_data;
	const char *clk_name;
	int clk_index;
	int clk_index_max;
	int clk_index_cnt;
	int clk_name_cnt;
	int clk_cnt;
	int i;
	u64 base;
	u32 type;
	struct clk *clk;
	int ret;
	int multi;
	unsigned int num_parents;
	const char **parent_names = NULL;

	ret = of_property_read_u64(node, "reg", &base);
	if (ret)
		return ret;

	ret = of_property_read_u32(node, "type", &type);
	if (ret)
		type = 0;

	num_parents = of_clk_get_parent_count(node);
	if (num_parents > 0) {
		parent_names = kcalloc(num_parents, sizeof(char *), GFP_KERNEL);
		if (!parent_names)
			return -ENOMEM;
		of_clk_parent_fill(node, parent_names, num_parents);
	}

	clk_index_cnt = of_property_count_u32_elems(node, "clock-indices");
	clk_name_cnt = of_property_count_strings(node, "clock-output-names");
	clk_cnt = clk_index_cnt > clk_name_cnt ? clk_index_cnt : clk_name_cnt;
	if (clk_cnt < 1)
		clk_cnt = 1;

	clk_index_max = clk_cnt - 1;
	of_property_for_each_u32(node, "clock-indices", clk_index) {
		if (clk_index_max < clk_index)
			clk_index_max = clk_index;
	}

	multi = clk_index_max > 0;

	if (multi) {
		clk_data = devm_kzalloc(dev, sizeof(*clk_data), GFP_KERNEL);
		if (!clk_data) {
			ret = -ENOMEM;
			goto out_free;
		}
		clk_data->clks = devm_kcalloc(dev, clk_index_max + 1, sizeof(struct clk *), GFP_KERNEL);
		if (!clk_data->clks) {
			ret = -ENOMEM;
			goto out_free;
		}
		clk_data->clk_num = clk_index_max + 1;
	}

	init.flags = CLK_IGNORE_UNUSED;
	switch (type) {
	case CLK_TYPE_PLL:
		init.ops = &baikal_clk_pll_ops;
		init.num_parents = 1;
		break;
	case CLK_TYPE_MUX:
		init.flags |= CLK_SET_RATE_PARENT;
		init.ops = &baikal_clk_mux_ops;
		init.num_parents = num_parents / clk_cnt;
		break;
	default:
		init.ops = &baikal_clk_ops;
		init.num_parents = 1;
	}
	init.parent_names = parent_names;
	for (i = 0; i < clk_cnt; i++) {
		ret = of_property_read_u32_index(node, "clock-indices", i, &clk_index);
		if (ret)
			clk_index = i;

		ret = of_property_read_string_index(node, "clock-output-names", i, &clk_name);
		if (ret) {
			if (multi)
				init.name = kasprintf(GFP_KERNEL, "%s.%d", node->name, clk_index);
			else
				init.name = kasprintf(GFP_KERNEL, "%s",	   node->name);
		} else {
			init.name = kasprintf(GFP_KERNEL, "%s", clk_name);
		}

		cmu = devm_kmalloc(dev, sizeof(*cmu), GFP_KERNEL);
		if (!cmu) {
			dev_err(dev, "failed to register '%s' clock (%d)\n",
				init.name, -ENOMEM);
			kfree(init.name);
		}

		cmu->base = base;
		cmu->index = clk_index;
		cmu->hw.init = &init;
		clk = devm_clk_register(dev, &cmu->hw);
		if (!IS_ERR(clk)) {
			devm_clk_hw_register_clkdev(dev, &cmu->hw, init.name, NULL);
			if (multi)
				clk_data->clks[clk_index] = clk;
		}
		else {
			dev_err(dev, "failed to register '%s' clock (%ld)\n",
				init.name, PTR_ERR(clk));
			devm_kfree(dev, cmu);
		}
		kfree(init.name);
		if (type == CLK_TYPE_MUX)
			init.parent_names += init.num_parents;
	}

	if (multi)
		ret = of_clk_add_provider(pdev->dev.of_node, of_clk_src_onecell_get, clk_data);
	else
		ret = of_clk_add_provider(pdev->dev.of_node, of_clk_src_simple_get, clk);

out_free:
	kfree(parent_names);
	return ret;
}

static void baikal_clk_remove(struct platform_device *pdev)
{
	of_clk_del_provider(pdev->dev.of_node);
}

#ifdef CONFIG_ACPI
static const char *baikal_acpi_ref_clk_str[] = { "baikal_ref_clk" };

static struct clk *baikal_acpi_ref_clk;

struct baikal_acpi_clk_data {
	struct clk *cmu_clk;
	struct clk_lookup *cmu_clk_l;
	struct clk **clk;
	struct clk_lookup **clk_l;
	unsigned int clk_num;
};

static int baikal_acpi_clk_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct acpi_device *ref_dev, *adev = to_acpi_device_node(pdev->dev.fwnode);
	struct clk_init_data init, *init_ch;
	struct baikal_clk *cmu, *cmu_ch;
	struct baikal_acpi_clk_data *clk_data = NULL;
	union acpi_object *package, *element;
	acpi_status status;
	struct acpi_buffer buffer = { ACPI_ALLOCATE_BUFFER, NULL };
	int size, i, index, ret = 0;
	char *str, *str2;
	const char *cmu_name;

	cmu = devm_kzalloc(dev, sizeof(*cmu), GFP_KERNEL);
	if (!cmu)
		return -ENOMEM;

	status = acpi_evaluate_object_typed(adev->handle, "PROP", NULL, &buffer, ACPI_TYPE_PACKAGE);
	if (ACPI_FAILURE(status)) {
		dev_err(dev, "failed to get PROP data\n");
		return -ENODEV;
	}

	package = buffer.pointer;
	if (package->package.count != 2) {
		dev_err(dev, "invalid PROP data\n");
		ret = -EINVAL;
		goto ret;
	}

	element = &package->package.elements[0];
	if (element->type != ACPI_TYPE_INTEGER) {
		dev_err(dev, "failed to get CMU id\n");
		ret = -EINVAL;
		goto ret;
	}

	cmu->base = element->integer.value;

	element = &package->package.elements[1];
	if (element->type != ACPI_TYPE_STRING) {
		dev_err(dev, "failed to get CMU clock name\n");
		ret = -EINVAL;
		goto ret;
	}

	str = devm_kzalloc(dev, element->string.length + 1, GFP_KERNEL);
	if (!str) {
		ret = -ENOMEM;
		goto ret;
	}

	memcpy(str, element->string.pointer, element->string.length);
	cmu_name = str;

	acpi_os_free(buffer.pointer);
	buffer.length = ACPI_ALLOCATE_BUFFER;
	buffer.pointer = NULL;

	init.parent_names = baikal_acpi_ref_clk_str;
	init.num_parents = 1;
	init.name = cmu_name;
	init.ops = &baikal_clk_ops;
	init.flags = CLK_IGNORE_UNUSED;

	cmu->hw.init = &init;

	clk_data = devm_kzalloc(dev, sizeof(*clk_data), GFP_KERNEL);
	if (!clk_data)
		return -ENOMEM;

	clk_data->cmu_clk = clk_register(NULL, &cmu->hw);
	if (IS_ERR(clk_data->cmu_clk)) {
		dev_err(dev, "failed to register CMU clock\n");
		return PTR_ERR(clk_data->cmu_clk);
	}

	clk_data->cmu_clk_l = clkdev_create(clk_data->cmu_clk, cmu_name, NULL);
	if (!clk_data->cmu_clk_l) {
		dev_err(dev, "failed to register CMU clock lookup\n");
		clk_unregister(clk_data->cmu_clk);
		return -ENOMEM;
	}

	/*
	 * FIXME: Clock subsystem disables parent clock if all defined child
	 * clock channels disabled. PLL clock is enabled here to avoid this.
	 */
	clk_prepare_enable(clk_data->cmu_clk);

	platform_set_drvdata(pdev, clk_data);

	status = acpi_evaluate_object_typed(adev->handle, "CLKS", NULL, &buffer, ACPI_TYPE_PACKAGE);
	if (ACPI_FAILURE(status)) {
		buffer.pointer = NULL;
		goto ret;
	}

	package = buffer.pointer;
	if (!package->package.count || package->package.count % 4) {
		dev_err(dev, "invalid CLKS data\n");
		ret = -EINVAL;
		goto ret;
	}

	clk_data->clk_num = package->package.count >> 2;
	clk_data->clk = devm_kzalloc(dev, clk_data->clk_num * sizeof(struct clk *), GFP_KERNEL);
	if (!clk_data->clk) {
		ret = -ENOMEM;
		goto ret;
	}

	clk_data->clk_l = devm_kzalloc(dev, clk_data->clk_num * sizeof(struct clk_lookup *), GFP_KERNEL);
	if (!clk_data->clk_l) {
		ret = -ENOMEM;
		goto ret;
	}

	init_ch = devm_kzalloc(dev, clk_data->clk_num * sizeof(struct clk_init_data), GFP_KERNEL);
	if (!init_ch) {
		ret = -ENOMEM;
		goto ret;
	}

	cmu_ch = devm_kzalloc(dev, clk_data->clk_num * sizeof(struct baikal_clk), GFP_KERNEL);
	if (!cmu_ch) {
		ret = -ENOMEM;
		goto ret;
	}

	for (i = 0; i < clk_data->clk_num; ++i) {
		ref_dev = NULL;
		size = 0;

		element = &package->package.elements[4 * i];
		if (element->type == ACPI_TYPE_LOCAL_REFERENCE && element->reference.handle)
			ref_dev = acpi_fetch_acpi_dev(element->reference.handle);

		element = &package->package.elements[4 * i + 1];
		if (element->type == ACPI_TYPE_STRING) {
			if (ref_dev)
				size = strlen(dev_name(&ref_dev->dev)) + 1;

			str = devm_kzalloc(dev, size + element->string.length + 1, GFP_KERNEL);
			if (str) {
				if (ref_dev) {
					memcpy(str, dev_name(&ref_dev->dev), size - 1);
					str[size - 1] = '_';
					memcpy(str + size, element->string.pointer, element->string.length);
				} else {
					memcpy(str, element->string.pointer, element->string.length);
				}
			}
		} else {
			dev_err(dev, "failed to process clock device name #%i\n", i);
			continue;
		}

		element = &package->package.elements[4 * i + 2];
		if (element->type == ACPI_TYPE_INTEGER) {
			index = element->integer.value;
		} else {
			dev_err(dev, "failed to process clock device id #%i\n", i);
			continue;
		}

		element = &package->package.elements[4 * i + 3];
		if (element->type == ACPI_TYPE_STRING) {
			str2 = devm_kzalloc(dev, element->string.length + 1, GFP_KERNEL);
			if (str2)
				memcpy(str2, element->string.pointer, element->string.length);
		} else {
			str2 = NULL;
		}

		init_ch[i].parent_names = &cmu_name;
		init_ch[i].num_parents = 1;
		init_ch[i].name = str;
		init_ch[i].ops = &baikal_clk_ops;
		init_ch[i].flags = CLK_IGNORE_UNUSED;

		cmu_ch[i].base = cmu->base + 0x20;
		cmu_ch[i].index = index;
		cmu_ch[i].hw.init = &init_ch[i];

		clk_data->clk[i] = clk_register(ref_dev ? &ref_dev->dev : NULL, &cmu_ch[i].hw);
		if (IS_ERR(clk_data->clk[i])) {
			dev_err(dev, "failed to register CMU channel clock #%i\n", i);
			clk_data->clk[i] = NULL;
			continue;
		}

		if (ref_dev)
			clk_data->clk_l[i] = clkdev_create(clk_data->clk[i], str2, "%s", dev_name(&ref_dev->dev));
		else
			clk_data->clk_l[i] = clkdev_create(clk_data->clk[i], str2, NULL);

		if (!clk_data->clk_l[i]) {
			dev_err(dev, "failed to register CMU channel clock lookup #%i\n", i);
			clk_unregister(clk_data->clk[i]);
			clk_data->clk[i] = NULL;
			continue;
		}
	}

	clk_data = NULL;

ret:
	if (buffer.pointer)
		acpi_os_free(buffer.pointer);

	if (clk_data) {
		clk_disable_unprepare(clk_data->cmu_clk);
		clkdev_drop(clk_data->cmu_clk_l);
		clk_unregister(clk_data->cmu_clk);
	}

	return ret;
}

static void baikal_acpi_clk_remove(struct platform_device *pdev)
{
	struct baikal_acpi_clk_data *clk_data = platform_get_drvdata(pdev);
	int i;

	if (clk_data) {
		clk_disable_unprepare(clk_data->cmu_clk);
		clkdev_drop(clk_data->cmu_clk_l);
		clk_unregister(clk_data->cmu_clk);

		for (i = 0; i < clk_data->clk_num; ++i) {
			if (clk_data->clk_l[i])
				clkdev_drop(clk_data->clk_l[i]);
			if (clk_data->clk[i])
				clk_unregister(clk_data->clk[i]);
		}
	}
}

static const struct acpi_device_id baikal_acpi_clk_device_ids[] = {
	{ "BKLE1001" },
	{ }
};

static struct platform_driver baikal_acpi_clk_driver = {
	.probe		= baikal_acpi_clk_probe,
	.remove		= baikal_acpi_clk_remove,
	.driver		= {
		.name	= "bl1000-cmu-acpi",
		.acpi_match_table = ACPI_PTR(baikal_acpi_clk_device_ids)
	}
};

static int __init baikal_acpi_clk_driver_init(void)
{
	if (!acpi_disabled) {
		struct clk_lookup *baikal_acpi_ref_clk_lookup;

		baikal_acpi_ref_clk = clk_register_fixed_rate(NULL, baikal_acpi_ref_clk_str[0], NULL, 0, 25000000);
		if (IS_ERR(baikal_acpi_ref_clk)) {
			pr_err("%s: failed to register reference clock\n", __func__);
			return PTR_ERR(baikal_acpi_ref_clk);
		}

		baikal_acpi_ref_clk_lookup = clkdev_create(baikal_acpi_ref_clk, NULL, "%s", baikal_acpi_ref_clk_str[0]);
		if (!baikal_acpi_ref_clk_lookup) {
			clk_unregister_fixed_rate(baikal_acpi_ref_clk);
			pr_err("%s: failed to register reference clock lookup\n", __func__);
			return -ENOMEM;
		}

		clk_prepare_enable(baikal_acpi_ref_clk);

		return platform_driver_register(&baikal_acpi_clk_driver);
	}

	return 0;
}

device_initcall(baikal_acpi_clk_driver_init);
#endif

static const struct of_device_id baikal_clk_of_match[] = {
	{ .compatible = "baikal,bl1000-cmu" },
	{ /* sentinel */ }
};

static struct platform_driver bl1000_cmu_driver = {
	.probe	= baikal_clk_probe,
	.remove	= baikal_clk_remove,
	.driver	= {
		.name = "bl1000-cmu",
		.of_match_table = baikal_clk_of_match
	}
};
module_platform_driver(bl1000_cmu_driver);

MODULE_DESCRIPTION("Baikal BE-L1000 clock driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:bl1000-cmu");
