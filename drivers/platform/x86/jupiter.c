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
#include <linux/regmap.h>

struct jupiter {
	struct acpi_device *adev;
	struct device *hwmon;
	void *regmap;
	long fan_target;
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
JUPITER_ATTR_WO(maximum_battery_charge_rate, "CHGR", U16_MAX);
JUPITER_ATTR_WO(recalculate, "SCHG", U16_MAX);

/*
 * FIXME: The following attributes should probably be moved out of
 * HWMON device since they don't reall belong to it
 */
JUPITER_ATTR_WO(led_brightness, "CHBV", U8_MAX);
JUPITER_ATTR_WO(content_adaptive_brightness, "CABC", U8_MAX);
JUPITER_ATTR_WO(gamma_set, "GAMA", U8_MAX);
JUPITER_ATTR_WO(display_brightness, "WDBV", U8_MAX);
JUPITER_ATTR_WO(ctrl_display, "WCDV", U8_MAX);
JUPITER_ATTR_WO(cabc_minimum_brightness, "WCMB", U8_MAX);
JUPITER_ATTR_WO(memory_data_access_control, "MDAC", U8_MAX);

#define JUPITER_ATTR_WO_NOARG(_name, _method)				\
	static ssize_t _name##_store(struct device *dev,		\
				     struct device_attribute *attr,	\
				     const char *buf, size_t count)	\
	{								\
		struct jupiter *fan = dev_get_drvdata(dev);		\
									\
		if (ACPI_FAILURE(acpi_evaluate_object(fan->adev->handle, \
						      _method, NULL, NULL))) \
			return -EIO;					\
									\
		return count;						\
	}								\
	static DEVICE_ATTR_WO(_name)

JUPITER_ATTR_WO_NOARG(power_cycle_display, "DPCY");
JUPITER_ATTR_WO_NOARG(display_normal_mode_on, "NORO");
JUPITER_ATTR_WO_NOARG(display_inversion_off, "INOF");
JUPITER_ATTR_WO_NOARG(display_inversion_on, "INON");
JUPITER_ATTR_WO_NOARG(idle_mode_on, "WRNE");

#define JUPITER_ATTR_RO(_name, _method)					\
	static ssize_t _name##_show(struct device *dev,			\
				    struct device_attribute *attr,	\
				    char *buf)				\
	{								\
		struct jupiter *jup = dev_get_drvdata(dev);		\
		unsigned long long val;					\
									\
		if (ACPI_FAILURE(acpi_evaluate_integer(			\
					 jup->adev->handle,		\
					 _method, NULL, &val)))		\
			return -EIO;					\
									\
		return sprintf(buf, "%llu\n", val);			\
	}								\
	static DEVICE_ATTR_RO(_name)

JUPITER_ATTR_RO(firmware_version, "PDFW");
JUPITER_ATTR_RO(board_id, "BOID");

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
	&dev_attr_maximum_battery_charge_rate.attr,
	&dev_attr_recalculate.attr,
	&dev_attr_power_cycle_display.attr,

	&dev_attr_led_brightness.attr,
	&dev_attr_content_adaptive_brightness.attr,
	&dev_attr_gamma_set.attr,
	&dev_attr_display_brightness.attr,
	&dev_attr_ctrl_display.attr,
	&dev_attr_cabc_minimum_brightness.attr,
	&dev_attr_memory_data_access_control.attr,

	&dev_attr_display_normal_mode_on.attr,
	&dev_attr_display_inversion_off.attr,
	&dev_attr_display_inversion_on.attr,
	&dev_attr_idle_mode_on.attr,

	&dev_attr_firmware_version.attr,
	&dev_attr_board_id.attr,

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

static int jupiter_read_fan_speed(struct jupiter *jup, long *speed)
{
	unsigned long long val;

	if (ACPI_FAILURE(acpi_evaluate_integer(jup->adev->handle,
					       "FANR", NULL, &val)))
		return -EIO;

	*speed = val;
	return 0;
}

static int jupiter_hwmon_read(struct device *dev, enum hwmon_sensor_types type,
			      u32 attr, int channel, long *out)
{
	struct jupiter *jup = dev_get_drvdata(dev);
	unsigned long long val;

	switch (type) {
	case hwmon_temp:
		if (attr != hwmon_temp_input)
			return -EOPNOTSUPP;

		if (ACPI_FAILURE(acpi_evaluate_integer(jup->adev->handle,
						       "BATT", NULL, &val)))
			return -EIO;
		/*
		 * Assuming BATT returns deg C we need to mutiply it
		 * by 1000 to convert to mC
		 */
		*out = val * 1000;
		break;
	case hwmon_fan:
		switch (attr) {
		case hwmon_fan_input:
			return jupiter_read_fan_speed(jup, out);
		case hwmon_fan_target:
			*out = jup->fan_target;
			break;
		case hwmon_fan_fault:
			if (ACPI_FAILURE(acpi_evaluate_integer(
						 jup->adev->handle,
						 "FANC", NULL, &val)))
				return -EIO;
			/*
			 * FANC (Fan check):
			 * 0: Abnormal
			 * 1: Normal
			 */
			*out = !val;
			break;
		default:
			return -EOPNOTSUPP;
		}
		break;
	default:
		return -EOPNOTSUPP;
	}

	return 0;
}

static int
jupiter_hwmon_read_string(struct device *dev, enum hwmon_sensor_types type,
			  u32 attr, int channel, const char **str)
{
	switch (type) {
	case hwmon_temp:
		*str = "Battery Temp";
		break;
	case hwmon_fan:
		*str = "System Fan";
		break;
	default:
		return -EOPNOTSUPP;
	}

	return 0;
}

static int jupiter_hwmon_write(struct device *dev, enum hwmon_sensor_types type,
			       u32 attr, int channel, long val)
{
	struct jupiter *jup = dev_get_drvdata(dev);

	if (type != hwmon_fan ||
	    attr != hwmon_fan_target)
		return -EOPNOTSUPP;

	if (val > U16_MAX)
		return -EINVAL;

	jup->fan_target = val;

	if (ACPI_FAILURE(acpi_execute_simple_method(jup->adev->handle,
						    "FANS", val)))
		return -EIO;

	return 0;
}

static umode_t
jupiter_hwmon_is_visible(const void *data, enum hwmon_sensor_types type,
			 u32 attr, int channel)
{
	if (type == hwmon_fan &&
	    attr == hwmon_fan_target)
		return 0644;

	return 0444;
}

static const struct hwmon_channel_info *jupiter_info[] = {
	HWMON_CHANNEL_INFO(temp,
			   HWMON_T_INPUT | HWMON_T_LABEL),
	HWMON_CHANNEL_INFO(fan,
			   HWMON_F_INPUT | HWMON_F_LABEL |
			   HWMON_F_TARGET | HWMON_F_FAULT),
	NULL
};

static const struct hwmon_ops jupiter_hwmon_ops = {
	.is_visible = jupiter_hwmon_is_visible,
	.read = jupiter_hwmon_read,
	.read_string = jupiter_hwmon_read_string,
	.write = jupiter_hwmon_write,
};

static const struct hwmon_chip_info jupiter_chip_info = {
	.ops = &jupiter_hwmon_ops,
	.info = jupiter_info,
};

#define JUPITER_STA_OK				\
	(ACPI_STA_DEVICE_ENABLED |		\
	 ACPI_STA_DEVICE_PRESENT |		\
	 ACPI_STA_DEVICE_FUNCTIONING)

static int
jupiter_ddic_reg_read(void *context, unsigned int reg, unsigned int *val)
{
	union acpi_object obj = { .type = ACPI_TYPE_INTEGER };
	struct acpi_object_list arg_list = { .count = 1, .pointer = &obj, };
	struct jupiter *jup = context;
	unsigned long long _val;

	obj.integer.value = reg;

	if (ACPI_FAILURE(acpi_evaluate_integer(jup->adev->handle,
					       "RDDI", &arg_list, &_val)))
		return -EIO;

	*val = _val;
	return 0;
}

static int jupiter_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct jupiter *jup;
	acpi_status status;
	unsigned long long sta;

	static const struct regmap_config regmap_config = {
		.reg_bits = 8,
		.val_bits = 8,
		.max_register = 255,
		.cache_type = REGCACHE_NONE,
		.reg_read = jupiter_ddic_reg_read,
	};

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

	/*
	 * Our ACPI interface doesn't expose a method to read current
	 * fan target, so we use current fan speed as an
	 * approximation.
	 */
	if (jupiter_read_fan_speed(jup, &jup->fan_target))
		dev_warn(dev, "Failed to read fan speed");

	jup->hwmon = devm_hwmon_device_register_with_info(dev,
							  "jupiter",
							  jup,
							  &jupiter_chip_info,
							  jupiter_groups);
	if (IS_ERR(jup->hwmon)) {
		dev_err(dev, "Failed to register HWMON device");
		return PTR_ERR(jup->hwmon);
	}

	jup->regmap = devm_regmap_init(dev, NULL, jup, &regmap_config);
	if (IS_ERR(jup->regmap))
		dev_err(dev, "Failed to register REGMAP");

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
