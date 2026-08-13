// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 BAIKAL ELECTRONICS, JSC
 *
 * Baikal SoC's SMC based power domains driver.
 */

#include <linux/firmware/baikal/baikal-smc.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>

struct baikal_pd {
	struct generic_pm_domain genpd;
	u32 index;
};

#define to_baikal_pd(gpd) container_of(gpd, struct baikal_pd, genpd)

static int baikal_pd_on(struct generic_pm_domain *domain)
{
	struct baikal_pd *bpd = to_baikal_pd(domain);
	struct arm_smccc_res res;

	arm_smccc_smc(BAIKAL_SMC_POWER_DOMAIN_ON, bpd->index, 0, 0, 0, 0, 0, 0, &res);
	return (int)res.a0;
}

static int baikal_pd_off(struct generic_pm_domain *domain)
{
	struct baikal_pd *bpd = to_baikal_pd(domain);
	struct arm_smccc_res res;

	arm_smccc_smc(BAIKAL_SMC_POWER_DOMAIN_OFF, bpd->index, 0, 0, 0, 0, 0, 0, &res);
	return (int)res.a0;
}

static int baikal_pd_state(struct baikal_pd *bpd)
{
	struct arm_smccc_res res;

	arm_smccc_smc(BAIKAL_SMC_POWER_DOMAIN_STATE, bpd->index, 0, 0, 0, 0, 0, 0, &res);
	return (int)res.a0;
}

static int baikal_pd_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct baikal_pd *domains, *bpd;
	struct genpd_onecell_data *bpd_data;
	int domain_index_cnt, domain_name_cnt, domain_index_max, domain_index;
	const char *domain_name;
	int ret, num_domains, i;

	domain_index_cnt = of_property_count_u32_elems(np, "domain-indices");
	domain_name_cnt = of_property_count_strings(np, "domain-names");
	num_domains = domain_index_cnt > domain_name_cnt ? domain_index_cnt : domain_name_cnt;
	if (num_domains < 1)
		num_domains = 1;

	domain_index_max = num_domains - 1;
	of_property_for_each_u32(np, "domain-indices", domain_index) {
		if (domain_index_max < domain_index)
			domain_index_max = domain_index;
	}

	domains = devm_kcalloc(dev, domain_index_max + 1, sizeof(*domains), GFP_KERNEL);
	if (!domains)
		return -ENOMEM;

	bpd_data = devm_kzalloc(dev, sizeof(*bpd_data), GFP_KERNEL);
	if (!bpd_data)
		return -ENOMEM;

	bpd_data->domains = devm_kcalloc(dev, domain_index_max + 1, sizeof(*bpd_data->domains), GFP_KERNEL);
	if (!bpd_data->domains)
		return -ENOMEM;
	bpd_data->num_domains = domain_index_max + 1;

	for (i = 0; i < num_domains; i++) {
		ret = of_property_read_u32_index(np, "domain-indices", i, &domain_index);
		if (ret)
			domain_index = i;
		bpd = &domains[domain_index];
		bpd->index = domain_index;
		ret = of_property_read_string_index(np, "domain-names", i, &domain_name);
		if (ret) {
			bpd->genpd.name = devm_kasprintf(dev, GFP_KERNEL,
							 "%pOFn.%d", np, domain_index);
		} else {
			bpd->genpd.name = devm_kasprintf(dev, GFP_KERNEL, "%s", domain_name);
		}
		if (!bpd->genpd.name) {
			dev_err(dev, "Failed to allocate genpd%d name\n", domain_index);
			continue;
		}
		bpd->genpd.power_off = baikal_pd_off;
		bpd->genpd.power_on = baikal_pd_on;
		bpd->genpd.flags = GENPD_FLAG_ACTIVE_WAKEUP;

		pm_genpd_init(&bpd->genpd, NULL, baikal_pd_state(bpd) != 1);

		bpd_data->domains[domain_index] = &bpd->genpd;
	}

	ret = of_genpd_add_provider_onecell(np, bpd_data);
	if (ret)
		goto err;

	dev_set_drvdata(dev, bpd_data);

	return 0;

err:
	for (i = 0; i < bpd_data->num_domains; i++)
		if (bpd_data->domains[i])
			pm_genpd_remove(bpd_data->domains[i]);
	return ret;
}

static void baikal_pd_remove(struct platform_device *pdev)
{
	struct genpd_onecell_data *bpd_data;
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	int i;

	of_genpd_del_provider(np);

	bpd_data = dev_get_drvdata(dev);
	for (i = 0; i < bpd_data->num_domains; i++)
		if (bpd_data->domains[i])
			pm_genpd_remove(bpd_data->domains[i]);
}

static const struct of_device_id baikal_pd_ids[] = {
	{ .compatible = "baikal,smc-power-domains", },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, baikal_pd_ids);

static struct platform_driver baikal_pd_driver = {
	.driver	= {
		.name = "baikal_smc_power_domains",
		.of_match_table = baikal_pd_ids,
	},
	.probe = baikal_pd_probe,
	.remove = baikal_pd_remove,
};
module_platform_driver(baikal_pd_driver);

MODULE_DESCRIPTION("Baikal SoC's SMC power domains driver");
MODULE_LICENSE("GPL v2");
