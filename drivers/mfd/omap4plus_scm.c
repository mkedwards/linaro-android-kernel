/*
 * OMAP4 system control module driver file
 *
 * Copyright (C) 2011 Texas Instruments Incorporated - http://www.ti.com/
 * Author: J Keerthy <j-keerthy@ti.com>
 * Author: Moiz Sonasath <m-sonasath@ti.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301 USA
 *
 */

#include <linux/interrupt.h>
#include <linux/clk.h>
#include <linux/io.h>
#include <linux/slab.h>
#include <linux/gpio.h>
#include <linux/platform_device.h>
#include <plat/omap_device.h>
#include <linux/kernel.h>
#include <linux/jiffies.h>
#include <linux/err.h>
#include <linux/types.h>
#include <linux/mutex.h>
#include <linux/delay.h>
#include <linux/reboot.h>
#include <plat/scm.h>
#include <linux/mfd/omap4_scm.h>
#include <linux/thermal_framework.h>

u32 omap4plus_scm_readl(struct scm *scm_ptr, u32 reg)
{
	return __raw_readl(scm_ptr->base + reg);
}
EXPORT_SYMBOL_GPL(omap4plus_scm_readl);

void omap4plus_scm_writel(struct scm *scm_ptr, u32 val, u32 reg)
{
	__raw_writel(val, scm_ptr->base + reg);
}
EXPORT_SYMBOL_GPL(omap4plus_scm_writel);


void omap4plus_scm_client_register(struct temp_sensor_hwmon *tsh_ptr,
					  int i, const char *name)
{
	int ret;

	tsh_ptr->pdev = platform_device_alloc(name, i);
	if (tsh_ptr->pdev == NULL) {
		dev_err(tsh_ptr->scm_ptr->dev, "Failed to allocate %s\n", name);
		return;
	}

	tsh_ptr->pdev->dev.parent = tsh_ptr->scm_ptr->dev;
	platform_set_drvdata(tsh_ptr->pdev, tsh_ptr);
	ret = platform_device_add(tsh_ptr->pdev);
	if (ret != 0) {
		dev_err(tsh_ptr->scm_ptr->dev, "Failed to register %s: %d\n",
			name, ret);
		platform_device_put(tsh_ptr->pdev);
		tsh_ptr->pdev = NULL;
	}
}

static irqreturn_t talert_irq_handler(int irq, void *data)
{
	struct scm *scm_ptr;
	int t_hot = 0, t_cold, temp, i;
	struct omap4460plus_temp_sensor_registers *tsr;

	scm_ptr = data;
	/* Read the status of t_hot */
	for (i = 0; i < scm_ptr->cnt; i++) {
		tsr = scm_ptr->registers[i];
		t_hot = omap4plus_scm_readl(scm_ptr, tsr->bgap_status)
		    & tsr->status_hot_mask;

		/* Read the status of t_cold */
		t_cold = omap4plus_scm_readl(scm_ptr, tsr->bgap_status)
		    & tsr->status_cold_mask;

		temp = omap4plus_scm_readl(scm_ptr, tsr->bgap_mask_ctrl);
		/*
		 * One TALERT interrupt: Two sources
		 * If the interrupt is due to t_hot then mask t_hot and
		 * and unmask t_cold else mask t_cold and unmask t_hot
		 */
		if (t_hot) {
			temp &= ~tsr->mask_hot_mask;
			temp |= tsr->mask_cold_mask;
		} else if (t_cold) {
			temp &= ~tsr->mask_cold_mask;
			temp |= tsr->mask_hot_mask;
		}

		mutex_lock(&scm_ptr->scm_mutex);
		omap4plus_scm_writel(scm_ptr, temp, tsr->bgap_mask_ctrl);
		mutex_unlock(&scm_ptr->scm_mutex);

		/* read temperature */
		temp = omap4plus_scm_readl(scm_ptr, tsr->temp_sensor_ctrl);
		temp &= tsr->bgap_dtemp_mask;

		if (t_hot || t_cold) {
			/* kobject_uvent to user space threshold crossed */
			scm_ptr->therm_fw[i]->current_temp =
				adc_to_temp_conversion(scm_ptr, i, temp);
			thermal_sensor_set_temp(scm_ptr->therm_fw[i]);
			kobject_uevent(&scm_ptr->tsh_ptr[i].pdev->dev.kobj,
						KOBJ_CHANGE);
		}
	}

	return IRQ_HANDLED;
}

static irqreturn_t omap4460_tshut_irq_handler(int irq, void *data)
{
	kernel_restart(NULL);

	return IRQ_HANDLED;
}

static int __devinit omap4plus_scm_probe(struct platform_device *pdev)
{
	struct omap4plus_scm_pdata *pdata = pdev->dev.platform_data;
	struct scm *scm_ptr;
	struct resource *mem;
	int ret = 0, i;

	if (!pdata) {
		dev_err(&pdev->dev, "platform data missing\n");
		return -EINVAL;
	}

	scm_ptr = kzalloc(sizeof(*scm_ptr), GFP_KERNEL);
	if (!scm_ptr) {
		dev_err(&pdev->dev, "Memory allocation failed\n");
		return -ENOMEM;
	}

	scm_ptr->tsh_ptr = kzalloc(sizeof(*scm_ptr->tsh_ptr) * pdata->cnt,
					GFP_KERNEL);
	if (!scm_ptr) {
		dev_err(&pdev->dev, "Memory allocation failed for tsh\n");
		kfree(scm_ptr);
		return -ENOMEM;
	}

	mutex_init(&scm_ptr->scm_mutex);
	mem = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!mem) {
		dev_err(&pdev->dev, "no mem resource\n");
		ret = -ENOMEM;
		goto plat_res_err;
	}

	scm_ptr->irq = platform_get_irq_byname(pdev, "thermal_alert");
	if (scm_ptr->irq < 0) {
		dev_err(&pdev->dev, "get_irq_byname failed\n");
		ret = scm_ptr->irq;
		goto plat_res_err;
	}

	scm_ptr->base = ioremap(mem->start, resource_size(mem));
	if (!scm_ptr->base) {
		dev_err(&pdev->dev, "ioremap failed\n");
		ret = -ENOMEM;
		goto plat_res_err;
	}
	scm_ptr->cnt = pdata->cnt;
	scm_ptr->rev = pdata->rev;
	scm_ptr->accurate = pdata->accurate;

	scm_ptr->dev = &pdev->dev;

	dev_set_drvdata(&pdev->dev, scm_ptr);
	scm_ptr->fclock = clk_get(&pdev->dev, "fck");
	scm_ptr->div_clk = clk_get(&pdev->dev, "div_ck");

/* Initialize temperature sensors */
	if (scm_ptr->rev == 1 && pdata->accurate) {
		ret = omap4460plus_temp_sensor_init(scm_ptr);
		if (ret)
			goto clken_err;

		ret = omap4460_tshut_init(scm_ptr);

		for (i = 0; i < pdata->cnt; i++) {
			scm_ptr->tsh_ptr[i].scm_ptr = scm_ptr;
			omap4plus_scm_client_register(&(scm_ptr->tsh_ptr[i]),
						i, "temp_sensor_hwmon");
		}

		ret = request_threaded_irq(scm_ptr->irq, NULL,
			   talert_irq_handler,
			   IRQF_TRIGGER_RISING | IRQF_ONESHOT, "TAlert",
			   scm_ptr);
		if (ret) {
			dev_err(&pdev->dev, "Request threaded irq failed.\n");
			goto req_irq_err;
		}

		ret = request_threaded_irq(gpio_to_irq(86), NULL,
						omap4460_tshut_irq_handler,
					IRQF_TRIGGER_RISING, "tshut", NULL);
		if (ret) {
			gpio_free(86);
			pr_err("request irq failed for TSHUT");
			return ret;
		}

		mutex_lock(&scm_ptr->scm_mutex);

		for (i = 0; i < pdata->cnt; i++)
			temp_sensor_unmask_interrupts(scm_ptr, i,
					scm_ptr->ts_data[i]->adc_end_val,
					scm_ptr->ts_data[i]->adc_start_val);

		mutex_unlock(&scm_ptr->scm_mutex);

		return ret;
req_irq_err:
		omap4460plus_temp_sensor_deinit(scm_ptr);
	}

clken_err:
	clk_put(scm_ptr->fclock);
	clk_put(scm_ptr->div_clk);
	iounmap(scm_ptr->base);
plat_res_err:
	dev_set_drvdata(&pdev->dev, NULL);
	mutex_destroy(&scm_ptr->scm_mutex);
	kfree(scm_ptr->tsh_ptr);
	kfree(scm_ptr);

	return ret;
}

static int __devexit omap4plus_scm_remove(struct platform_device *pdev)
{
	struct scm *scm_ptr = platform_get_drvdata(pdev);

	free_irq(scm_ptr->irq, scm_ptr);
	clk_disable(scm_ptr->fclock);
	clk_put(scm_ptr->fclock);
	clk_put(scm_ptr->div_clk);
	iounmap(scm_ptr->base);
	dev_set_drvdata(&pdev->dev, NULL);
	mutex_destroy(&scm_ptr->scm_mutex);
	kfree(scm_ptr);

	return 0;
}

static struct platform_driver omap4plus_scm_driver = {
	.probe = omap4plus_scm_probe,
	.remove = omap4plus_scm_remove,
	.driver = {
		   .name = "omap4plus_scm",
		   },
};

int __init omap4plus_scm_init(void)
{
	return platform_driver_register(&omap4plus_scm_driver);
}

module_init(omap4plus_scm_init);

static void __exit omap4plus_scm_exit(void)
{
	platform_driver_unregister(&omap4plus_scm_driver);
}

module_exit(omap4plus_scm_exit);

MODULE_DESCRIPTION("OMAP4 plus system control module Driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:" DRIVER_NAME);
MODULE_AUTHOR("J Keerthy <j-keerthy@ti.com>");
