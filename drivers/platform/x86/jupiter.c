// SPDX-License-Identifier: GPL-2.0+

/*
 * Jupiter ACPI platform driver
 *
 * Copyright (C) 2021 Valve Corporation
 *
 */
#include <linux/acpi.h>
#include <linux/hwmon.h>
#include <linux/platform_device.h>

struct jupiter {
	struct acpi_device *adev;
	struct device *hwmon;
};

static ssize_t
jupiter_simple_store(struct device *dev, const char *buf, size_t count,
			 const char *method,
			 unsigned long upper_limit)
{
	struct jupiter *fan = dev_get_drvdata(dev);
	unsigned long value;

	if (kstrtoul(buf, 10, &value) || value >= upper_limit)
		return -EINVAL;

	if (ACPI_FAILURE(acpi_execute_simple_method(fan->adev->handle,
						    (char *)method, value)))
		return -EIO;

	return count;
}

#define JUPITER_ATTR_WO(_name, _method, _upper_limit)			\
	static ssize_t _name##_store(struct device *dev,		\
				     struct device_attribute *attr,	\
				     const char *buf, size_t count)	\
	{								\
		return jupiter_simple_store(dev, buf, count,		\
					    _method,			\
					    _upper_limit);		\
	}								\
	static DEVICE_ATTR_WO(_name)

JUPITER_ATTR_WO(target_cpu_temp, "STCT", U8_MAX / 2);
JUPITER_ATTR_WO(gain, "SGAN", U16_MAX);
JUPITER_ATTR_WO(ramp_rate, "SFRR", U8_MAX);
JUPITER_ATTR_WO(hysteresis, "SHTS",  U16_MAX);
JUPITER_ATTR_WO(speed, "FANS", U16_MAX);
JUPITER_ATTR_WO(maximum_battery_charge_rate, "CHGR", U16_MAX);
JUPITER_ATTR_WO(recalculate, "SCHG", U16_MAX);

/*
 * FIXME: The following attributes should probably be moved out of
 * HWMON device since they don't reall belong to it
 */
static ssize_t
power_cycle_display_store(struct device *dev, struct device_attribute *attr,
			  const char *buf, size_t count)
{
	struct jupiter *fan = dev_get_drvdata(dev);

	if (ACPI_FAILURE(acpi_evaluate_object(fan->adev->handle,
					      "DPCY", NULL, NULL)))
		return -EIO;

	return count;
}

static DEVICE_ATTR_WO(power_cycle_display);

static umode_t
jupiter_is_visible(struct kobject *kobj, struct attribute *attr, int index)
{
	return attr->mode;
}

static struct attribute *jupiter_attributes[] = {
	&dev_attr_target_cpu_temp.attr,
	&dev_attr_gain.attr,
	&dev_attr_ramp_rate.attr,
	&dev_attr_hysteresis.attr,
	&dev_attr_speed.attr,
	&dev_attr_maximum_battery_charge_rate.attr,
	&dev_attr_recalculate.attr,
	&dev_attr_power_cycle_display.attr,
	NULL
};

static const struct attribute_group jupiter_group = {
	.attrs = jupiter_attributes,
	.is_visible = jupiter_is_visible,
};

static const struct attribute_group *jupiter_groups[] = {
	&jupiter_group,
	NULL
};

#define JUPITER_STA_OK				\
	(ACPI_STA_DEVICE_ENABLED |		\
	 ACPI_STA_DEVICE_PRESENT |		\
	 ACPI_STA_DEVICE_FUNCTIONING)

static int jupiter_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct jupiter *jup;
	acpi_status status;
	unsigned long long sta;

	jup = devm_kzalloc(dev, sizeof(*jup), GFP_KERNEL);
	if (!jup)
		return -ENOMEM;
	jup->adev = ACPI_COMPANION(&pdev->dev);
	platform_set_drvdata(pdev, jup);

	status = acpi_evaluate_integer(jup->adev->handle, "_STA",
				       NULL, &sta);
	if (ACPI_FAILURE(status)) {
		dev_err(dev, "Status check failed (0x%x)\n", status);
		return -EINVAL;
	}

	if ((sta & JUPITER_STA_OK) != JUPITER_STA_OK) {
		dev_err(dev, "Device is not ready\n");
		return -EINVAL;
	}

	jup->hwmon = devm_hwmon_device_register_with_groups(dev,
							    "jupiter",
							    jup,
							    jupiter_groups);
	if (IS_ERR(jup->hwmon)) {
		dev_err(dev, "Failed to register HWMON device");
		return PTR_ERR(jup->hwmon);
	}

	return 0;
}

static const struct acpi_device_id jupiter_device_ids[] = {
	{ "VLV0100", 0 },
	{ "", 0 },
};
MODULE_DEVICE_TABLE(acpi, jupiter_device_ids);

static struct platform_driver jupiter_driver = {
	.probe = jupiter_probe,
	.driver = {
		.name = "jupiter",
		.acpi_match_table = jupiter_device_ids,
	},
};
module_platform_driver(jupiter_driver);

MODULE_AUTHOR("Andrey Smirnov <andrew.smirnov@gmail.com>");
MODULE_DESCRIPTION("Jupiter ACPI platform driver");
MODULE_LICENSE("GPL");
