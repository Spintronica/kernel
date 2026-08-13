// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 BAIKAL ELECTRONICS, JSC
 *
 * Baikal SoC's SMC based eFuse driver.
 */

#include <linux/firmware/baikal/baikal-smc.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/nvmem-provider.h>
#include <linux/platform_device.h>

#define BAIKAL_EFUSE_N_CELLS	7

static const struct nvmem_cell_info baikal_efuse_cell_info[BAIKAL_EFUSE_N_CELLS] = {
	{
		.name = "Process",
		.offset = 0,
		.bytes = 5,
	},
	{
		.name = "ProjectName",
		.offset = 5,
		.bytes = 2,
	},
	{
		.name = "ProjectRevision",
		.offset = 7,
		.bytes = 1,
	},
	{
		.name = "LotID",
		.offset = 8,
		.bytes = 6,
	},
	{
		.name = "SerialNumber",
		.offset = 14,
		.bytes = 3,
	},
	{
		.name = "MacOffset",
		.offset = 17,
		.bytes = 3,
	},
	{
		.name = "BinMask",
		.offset = 20,
		.bytes = 4,
	},
};

#define BAIKAL_EFUSE_DATA_SIZE	24

struct baikal_efuse_dev {
	struct device *dev;
	u8 data[BAIKAL_EFUSE_DATA_SIZE];
};

static int baikal_efuse_reg_read(void *context, unsigned int offset,
				 void *val, size_t bytes)
{
	struct baikal_efuse_dev *edev = (struct baikal_efuse_dev *)context;

	if (offset + bytes > BAIKAL_EFUSE_DATA_SIZE)
		return -ERANGE;

	memcpy(val, edev->data + offset, bytes);

	return 0;
}

static int baikal_efuse_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct baikal_efuse_dev *edev;
	struct nvmem_config econfig = {};
	struct nvmem_device *nvmem;
	const unsigned long baikal_efuse_cell_smc_id[BAIKAL_EFUSE_N_CELLS] = {
		BAIKAL_SMC_EFUSE_GET_PROCESS, BAIKAL_SMC_EFUSE_GET_PROJECT,
		BAIKAL_SMC_EFUSE_GET_REVISION, BAIKAL_SMC_EFUSE_GET_LOT,
		BAIKAL_SMC_EFUSE_GET_SERIAL, BAIKAL_SMC_EFUSE_GET_MAC,
		BAIKAL_SMC_EFUSE_GET_BINNING
	};
	int i;

	edev = devm_kzalloc(dev, sizeof(*edev), GFP_KERNEL);
	if (!edev)
		return -ENOMEM;
	edev->dev = dev;
	dev_set_drvdata(dev, edev);

	econfig.dev = dev;
	econfig.name = "efuse";
	econfig.id = NVMEM_DEVID_AUTO;
	econfig.cells = baikal_efuse_cell_info;
	econfig.ncells = BAIKAL_EFUSE_N_CELLS;
	econfig.read_only = true;
	econfig.reg_read = baikal_efuse_reg_read;
	econfig.size = BAIKAL_EFUSE_DATA_SIZE;
	econfig.word_size = 1;
	econfig.stride = 1;
	econfig.priv = edev;

	nvmem = devm_nvmem_register(dev, &econfig);
	if (IS_ERR(nvmem))
		return PTR_ERR(nvmem);

	for (i = 0; i < BAIKAL_EFUSE_N_CELLS; i++) {
		struct arm_smccc_res res;

		arm_smccc_smc(baikal_efuse_cell_smc_id[i], 0, 0, 0, 0, 0, 0, 0, &res);
		if ((long)res.a0 < 0) {
			dev_err(dev, "Couldn't get eFuse %s data (%ld)\n",
				baikal_efuse_cell_info[i].name, (long)res.a0);
			return (long)res.a0;
		}
		memcpy(edev->data + baikal_efuse_cell_info[i].offset,
		      (u8 *)&res.a0, baikal_efuse_cell_info[i].bytes);
	}

	return 0;
}

static const struct of_device_id baikal_efuse_match[] = {
	{ .compatible = "baikal,efuse-smc", },
	{ }
};
MODULE_DEVICE_TABLE(of, baikal_efuse_match);

static struct platform_driver baikal_efuse_driver = {
	.probe = baikal_efuse_probe,
	.driver = {
		.name = "baikal-efuse-smc",
		.of_match_table = baikal_efuse_match,
	},
};

module_platform_driver(baikal_efuse_driver);

MODULE_DESCRIPTION("Baikal SoC's SMC eFuse driver");
MODULE_LICENSE("GPL v2");
