// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2026 Baikal Electronics, JSC
 */
#include <linux/bits.h>
#include <linux/gpio/driver.h>
#include <linux/module.h>
#include <linux/platform_device.h>

#define DRIVER_NAME "baikal-spi-cs"

struct baikal_spi_cs_ctrl {
	void __iomem *phys;
	u8 sel_shift;
	u8 reg_shift;
	u8 reg_width;
};

struct baikal_spi_cs_data {
	struct gpio_chip chip;
	void __iomem *ctrl_reg;
	u32 sel;
	u32 reg_mask;
	u8  reg_shift;
};

static inline void baikal_spi_cs_clrsetbits_32(void __iomem *addr, u32 clear, u32 set)
{
	writel((readl(addr) & ~clear) | set, addr);
}

static void baikal_spi_cs_set(struct gpio_chip *gc, unsigned int offset, int value)
{
	struct baikal_spi_cs_data *data = container_of(gc, struct baikal_spi_cs_data, chip);
	unsigned int shift = offset + data->reg_shift;

	baikal_spi_cs_clrsetbits_32(data->ctrl_reg,
				    (1 << shift) & data->reg_mask,
				    value << shift);
}

static int baikal_spi_cs_probe(struct platform_device *pdev)
{
	struct baikal_spi_cs_data *data;
	struct resource *iores;
	int ret;
	const struct baikal_spi_cs_ctrl *cs_ctrl;

	data = devm_kzalloc(&pdev->dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->ctrl_reg = devm_platform_get_and_ioremap_resource(pdev, 0, &iores);
	if (IS_ERR(data->ctrl_reg))
		return PTR_ERR(data->ctrl_reg);

	data->chip.label = DRIVER_NAME;
	data->chip.parent = &pdev->dev;
	data->chip.owner = THIS_MODULE;
	data->chip.set = baikal_spi_cs_set;
	data->chip.base = -1;
	data->chip.can_sleep = true;

	cs_ctrl = device_get_match_data(&pdev->dev);
	if (!cs_ctrl)
		return -EINVAL;

	while (cs_ctrl->phys) {
		if (cs_ctrl->phys == (void __iomem *) iores->start)
			break;
		++cs_ctrl;
	}

	if (!cs_ctrl->phys)
		return -EINVAL;

	data->sel = BIT(cs_ctrl->sel_shift);
	data->reg_shift = cs_ctrl->reg_shift;
	data->reg_mask = GENMASK(cs_ctrl->reg_shift + (cs_ctrl->reg_width - 1), cs_ctrl->reg_shift);

	platform_set_drvdata(pdev, (void *) data);

	ret = devm_gpiochip_add_data(&pdev->dev, &data->chip, NULL);
	if (ret)
		return ret;

	/* Set manual Chip Select */
	baikal_spi_cs_clrsetbits_32(data->ctrl_reg, data->sel, data->sel);

	return 0;
}

static void baikal_spi_cs_remove(struct platform_device *pdev)
{
	struct baikal_spi_cs_data *data = platform_get_drvdata(pdev);

	/* Set native Chip Select */
	baikal_spi_cs_clrsetbits_32(data->ctrl_reg, data->sel, 0);
}

static const struct baikal_spi_cs_ctrl bl1000_qspi_cs[] = {
	{ .phys = (void __iomem *)  0x840020, .sel_shift = 12, .reg_shift = 13, .reg_width = 3, },
	{ .phys = (void __iomem *)0x288400a8, .sel_shift = 10, .reg_shift = 11, .reg_width = 3, },
	{ /* sentinel */ }
};

static const struct of_device_id baikal_spi_cs_of_match[] = {
	{ .compatible = "baikal,bl1000-qspi-cs", .data = bl1000_qspi_cs },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, baikal_spi_cs_of_match);

static struct platform_driver baikal_spi_cs_driver = {
	.probe = baikal_spi_cs_probe,
	.remove = baikal_spi_cs_remove,
	.driver = {
		.name = DRIVER_NAME,
		.of_match_table = baikal_spi_cs_of_match,
	},
};
module_platform_driver(baikal_spi_cs_driver);

MODULE_DESCRIPTION("Baikal manual SPI Chip Select");
MODULE_LICENSE("GPL v2");
