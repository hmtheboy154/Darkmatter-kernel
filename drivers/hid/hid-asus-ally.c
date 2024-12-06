// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *  HID driver for Asus ROG laptops and Ally
 *
 *  Copyright (c) 2023 Luke Jones <luke@ljones.dev>
 */

#include "linux/device.h"
#include "linux/err.h"
#include "linux/kstrtox.h"
#include "linux/pm.h"
#include "linux/slab.h"
#include "linux/stddef.h"
#include <linux/hid.h>
#include <linux/types.h>
#include <linux/usb.h>
#include <linux/leds.h>
#include <linux/led-class-multicolor.h>

#include "hid-ids.h"
#include "hid-asus-ally.h"

#define READY_MAX_TRIES 3
#define FEATURE_REPORT_ID 0x0d
#define FEATURE_ROG_ALLY_REPORT_ID 0x5a
#define FEATURE_ROG_ALLY_CODE_PAGE 0xD1
#define FEATURE_ROG_ALLY_REPORT_SIZE 64
#define ALLY_X_INPUT_REPORT_USB 0x0B
#define ALLY_X_INPUT_REPORT_USB_SIZE 16

#define ALLY_CFG_INTF_IN_ADDRESS 0x83
#define ALLY_CFG_INTF_OUT_ADDRESS 0x04
#define ALLY_X_INTERFACE_ADDRESS 0x87

#define FEATURE_KBD_LED_REPORT_ID1 0x5d
#define FEATURE_KBD_LED_REPORT_ID2 0x5e

#define ALLY_MIN_BIOS 319
#define ALLY_X_MIN_BIOS 313

static const u8 EC_INIT_STRING[] = { 0x5A, 'A', 'S', 'U', 'S', ' ', 'T', 'e','c', 'h', '.', 'I', 'n', 'c', '.', '\0' };
static const u8 EC_MODE_LED_APPLY[] = { 0x5A, 0xB4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const u8 EC_MODE_LED_SET[] = { 0x5A, 0xB5, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const u8 FORCE_FEEDBACK_OFF[] = { 0x0D, 0x0F, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x00, 0xEB };

static const struct hid_device_id rog_ally_devices[] = {
	{ HID_USB_DEVICE(USB_VENDOR_ID_ASUSTEK, USB_DEVICE_ID_ASUSTEK_ROG_NKEY_ALLY) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_ASUSTEK, USB_DEVICE_ID_ASUSTEK_ROG_NKEY_ALLY_X) },
	{}
};

/* The hatswitch outputs integers, we use them to index this X|Y pair */
static const int hat_values[][2] = {
	{ 0, 0 }, { 0, -1 }, { 1, -1 }, { 1, 0 },   { 1, 1 },
	{ 0, 1 }, { -1, 1 }, { -1, 0 }, { -1, -1 },
};

/* rumble packet structure */
struct ff_data {
	u8 enable;
	u8 magnitude_left;
	u8 magnitude_right;
	u8 magnitude_strong;
	u8 magnitude_weak;
	u8 pulse_sustain_10ms;
	u8 pulse_release_10ms;
	u8 loop_count;
} __packed;

struct ff_report {
	u8 report_id;
	struct ff_data ff;
} __packed;

struct ally_x_input_report {
	uint16_t x, y;
	uint16_t rx, ry;
	uint16_t z, rz;
	uint8_t buttons[4];
} __packed;

struct ally_x_device {
	struct input_dev *input;
	struct hid_device *hdev;
	spinlock_t lock;

	struct ff_report *ff_packet;
	struct work_struct output_worker;
	bool output_worker_initialized;
	/* Prevent multiple queued event due to the enforced delay in worker */
	bool update_qam_btn;
	/* Set if the QAM and AC buttons emit Xbox and Xbox+A */
	bool qam_btns_steam_mode;
	bool update_ff;
};

struct ally_rgb_dev {
	struct hid_device *hdev;
	struct led_classdev_mc led_rgb_dev;
	struct work_struct work;
	bool output_worker_initialized;
	spinlock_t lock;

	bool removed;
	bool update_rgb;
	uint8_t red[4];
	uint8_t green[4];
	uint8_t blue[4];
};

struct ally_rgb_data {
	uint8_t brightness;
	uint8_t red[4];
	uint8_t green[4];
	uint8_t blue[4];
	bool initialized;
};

static struct ally_drvdata {
	struct hid_device *hdev;
	struct ally_x_device *ally_x;
	struct ally_rgb_dev *led_rgb_dev;
	struct ally_rgb_data led_rgb_data;
} drvdata;

static int asus_dev_get_report(struct hid_device *hdev, u8 *out_buf, size_t out_buf_size)
{
	return hid_hw_raw_request(hdev, FEATURE_REPORT_ID, out_buf, out_buf_size,
				  HID_FEATURE_REPORT, HID_REQ_GET_REPORT);
}

/**
 * asus_dev_set_report - send set report request to device.
 *
 * @hdev: hid device
 * @buf: in/out data to transfer
 * @len: length of buf
 *
 * Return: count of data transferred, negative if error
 *
 * Same behavior as hid_hw_raw_request. Note that the input buffer is duplicated.
 */
static int asus_dev_set_report(struct hid_device *hdev, const u8 *buf, size_t len)
{
	unsigned char *dmabuf;
	int ret;

	dmabuf = kmemdup(buf, len, GFP_KERNEL);
	if (!dmabuf)
		return -ENOMEM;

	ret = hid_hw_raw_request(hdev, buf[0], dmabuf, len, HID_FEATURE_REPORT,
				 HID_REQ_SET_REPORT);
	kfree(dmabuf);

	return ret;
}

static u8 get_endpoint_address(struct hid_device *hdev)
{
	struct usb_interface *intf;
	struct usb_host_endpoint *ep;

	intf = to_usb_interface(hdev->dev.parent);

	if (intf) {
		ep = intf->cur_altsetting->endpoint;
		if (ep) {
			return ep->desc.bEndpointAddress;
		}
	}

	return -ENODEV;
}

/**************************************************************************************************/
/* ROG Ally gamepad i/o and force-feedback                                                        */
/**************************************************************************************************/
static int ally_x_raw_event(struct ally_x_device *ally_x, struct hid_report *report, u8 *data,
			    int size)
{
	struct ally_x_input_report *in_report;
	unsigned long flags;
	u8 byte;

	if (data[0] == 0x0B) {
		in_report = (struct ally_x_input_report *)&data[1];

		input_report_abs(ally_x->input, ABS_X, in_report->x);
		input_report_abs(ally_x->input, ABS_Y, in_report->y);
		input_report_abs(ally_x->input, ABS_RX, in_report->rx);
		input_report_abs(ally_x->input, ABS_RY, in_report->ry);
		input_report_abs(ally_x->input, ABS_Z, in_report->z);
		input_report_abs(ally_x->input, ABS_RZ, in_report->rz);

		byte = in_report->buttons[0];
		input_report_key(ally_x->input, BTN_A, byte & BIT(0));
		input_report_key(ally_x->input, BTN_B, byte & BIT(1));
		input_report_key(ally_x->input, BTN_X, byte & BIT(2));
		input_report_key(ally_x->input, BTN_Y, byte & BIT(3));
		input_report_key(ally_x->input, BTN_TL, byte & BIT(4));
		input_report_key(ally_x->input, BTN_TR, byte & BIT(5));
		input_report_key(ally_x->input, BTN_SELECT, byte & BIT(6));
		input_report_key(ally_x->input, BTN_START, byte & BIT(7));

		byte = in_report->buttons[1];
		input_report_key(ally_x->input, BTN_THUMBL, byte & BIT(0));
		input_report_key(ally_x->input, BTN_THUMBR, byte & BIT(1));
		input_report_key(ally_x->input, BTN_MODE, byte & BIT(2));

		byte = in_report->buttons[2];
		input_report_abs(ally_x->input, ABS_HAT0X, hat_values[byte][0]);
		input_report_abs(ally_x->input, ABS_HAT0Y, hat_values[byte][1]);
	}
	/*
	 * The MCU used on Ally provides many devices: gamepad, keyboord, mouse, other.
	 * The AC and QAM buttons route through another interface making it difficult to
	 * use the events unless we grab those and use them here. Only works for Ally X.
	 */
	else if (data[0] == 0x5A) {
		if (ally_x->qam_btns_steam_mode) {
			spin_lock_irqsave(&ally_x->lock, flags);
			if (data[1] == 0x38 && !ally_x->update_qam_btn) {
				ally_x->update_qam_btn = true;
				if (ally_x->output_worker_initialized)
					schedule_work(&ally_x->output_worker);
			}
			spin_unlock_irqrestore(&ally_x->lock, flags);
			/* Left/XBox button. Long press does ctrl+alt+del which we can't catch */
			input_report_key(ally_x->input, BTN_MODE, data[1] == 0xA6);
		} else {
			input_report_key(ally_x->input, KEY_F16, data[1] == 0xA6);
			input_report_key(ally_x->input, KEY_PROG1, data[1] == 0x38);
		}
		/* QAM long press */
		input_report_key(ally_x->input, KEY_F17, data[1] == 0xA7);
		/* QAM long press released */
		input_report_key(ally_x->input, KEY_F18, data[1] == 0xA8);
	}

	input_sync(ally_x->input);

	return 0;
}

static struct input_dev *ally_x_alloc_input_dev(struct hid_device *hdev,
						const char *name_suffix)
{
	struct input_dev *input_dev;

	input_dev = devm_input_allocate_device(&hdev->dev);
	if (!input_dev)
		return ERR_PTR(-ENOMEM);

	input_dev->id.bustype = hdev->bus;
	input_dev->id.vendor = hdev->vendor;
	input_dev->id.product = hdev->product;
	input_dev->id.version = hdev->version;
	input_dev->uniq = hdev->uniq;
	input_dev->name = "ASUS ROG Ally X Gamepad";

	input_set_drvdata(input_dev, hdev);

	return input_dev;
}

static int ally_x_play_effect(struct input_dev *idev, void *data, struct ff_effect *effect)
{
	struct ally_x_device *ally_x = drvdata.ally_x;
	unsigned long flags;

	if (effect->type != FF_RUMBLE)
		return 0;

	spin_lock_irqsave(&ally_x->lock, flags);
	ally_x->ff_packet->ff.magnitude_strong = effect->u.rumble.strong_magnitude / 512;
	ally_x->ff_packet->ff.magnitude_weak = effect->u.rumble.weak_magnitude / 512;
	ally_x->update_ff = true;
	spin_unlock_irqrestore(&ally_x->lock, flags);

	if (ally_x->output_worker_initialized)
		schedule_work(&ally_x->output_worker);

	return 0;
}

static void ally_x_work(struct work_struct *work)
{
	struct ally_x_device *ally_x = container_of(work, struct ally_x_device, output_worker);
	struct ff_report *ff_report = NULL;
	bool update_qam = false;
	bool update_ff = false;
	unsigned long flags;

	spin_lock_irqsave(&ally_x->lock, flags);
	update_ff = ally_x->update_ff;
	if (ally_x->update_ff) {
		ff_report = kmemdup(ally_x->ff_packet, sizeof(*ally_x->ff_packet), GFP_KERNEL);
		ally_x->update_ff = false;
	}
	update_qam = ally_x->update_qam_btn;
	spin_unlock_irqrestore(&ally_x->lock, flags);

	if (update_ff && ff_report) {
		ff_report->ff.magnitude_left = ff_report->ff.magnitude_strong;
		ff_report->ff.magnitude_right = ff_report->ff.magnitude_weak;
		asus_dev_set_report(ally_x->hdev, (u8 *)ff_report, sizeof(*ff_report));
	}
	kfree(ff_report);

	if (update_qam) {
		/*
		 * The sleeps here are required to allow steam to register the button combo.
		 */
		usleep_range(1000, 2000);
		input_report_key(ally_x->input, BTN_MODE, 1);
		input_sync(ally_x->input);

		msleep(80);
		input_report_key(ally_x->input, BTN_A, 1);
		input_sync(ally_x->input);

		msleep(80);
		input_report_key(ally_x->input, BTN_A, 0);
		input_sync(ally_x->input);

		msleep(80);
		input_report_key(ally_x->input, BTN_MODE, 0);
		input_sync(ally_x->input);

		spin_lock_irqsave(&ally_x->lock, flags);
		ally_x->update_qam_btn = false;
		spin_unlock_irqrestore(&ally_x->lock, flags);
	}
}

static struct input_dev *ally_x_setup_input(struct hid_device *hdev)
{
	int ret, abs_min = 0, js_abs_max = 65535, tr_abs_max = 1023;
	struct input_dev *input;

	input = ally_x_alloc_input_dev(hdev, NULL);
	if (IS_ERR(input))
		return ERR_CAST(input);

	input_set_abs_params(input, ABS_X, abs_min, js_abs_max, 0, 0);
	input_set_abs_params(input, ABS_Y, abs_min, js_abs_max, 0, 0);
	input_set_abs_params(input, ABS_RX, abs_min, js_abs_max, 0, 0);
	input_set_abs_params(input, ABS_RY, abs_min, js_abs_max, 0, 0);
	input_set_abs_params(input, ABS_Z, abs_min, tr_abs_max, 0, 0);
	input_set_abs_params(input, ABS_RZ, abs_min, tr_abs_max, 0, 0);
	input_set_abs_params(input, ABS_HAT0X, -1, 1, 0, 0);
	input_set_abs_params(input, ABS_HAT0Y, -1, 1, 0, 0);
	input_set_capability(input, EV_KEY, BTN_A);
	input_set_capability(input, EV_KEY, BTN_B);
	input_set_capability(input, EV_KEY, BTN_X);
	input_set_capability(input, EV_KEY, BTN_Y);
	input_set_capability(input, EV_KEY, BTN_TL);
	input_set_capability(input, EV_KEY, BTN_TR);
	input_set_capability(input, EV_KEY, BTN_SELECT);
	input_set_capability(input, EV_KEY, BTN_START);
	input_set_capability(input, EV_KEY, BTN_MODE);
	input_set_capability(input, EV_KEY, BTN_THUMBL);
	input_set_capability(input, EV_KEY, BTN_THUMBR);

	input_set_capability(input, EV_KEY, KEY_PROG1);
	input_set_capability(input, EV_KEY, KEY_F16);
	input_set_capability(input, EV_KEY, KEY_F17);
	input_set_capability(input, EV_KEY, KEY_F18);

	input_set_capability(input, EV_FF, FF_RUMBLE);
	input_ff_create_memless(input, NULL, ally_x_play_effect);

	ret = input_register_device(input);
	if (ret)
		return ERR_PTR(ret);

	return input;
}

static ssize_t ally_x_qam_mode_show(struct device *dev, struct device_attribute *attr,
				    char *buf)
{
	struct ally_x_device *ally_x = drvdata.ally_x;

	return sysfs_emit(buf, "%d\n", ally_x->qam_btns_steam_mode);
}

static ssize_t ally_x_qam_mode_store(struct device *dev, struct device_attribute *attr,
				     const char *buf, size_t count)
{
	struct ally_x_device *ally_x = drvdata.ally_x;
	bool val;
	int ret;

	ret = kstrtobool(buf, &val);
	if (ret < 0)
		return ret;

	ally_x->qam_btns_steam_mode = val;

	return count;
}
ALLY_DEVICE_ATTR_RW(ally_x_qam_mode, qam_mode);

static struct ally_x_device *ally_x_create(struct hid_device *hdev)
{
	uint8_t max_output_report_size;
	struct ally_x_device *ally_x;
	struct ff_report *report;
	int ret;

	ally_x = devm_kzalloc(&hdev->dev, sizeof(*ally_x), GFP_KERNEL);
	if (!ally_x)
		return ERR_PTR(-ENOMEM);

	ally_x->hdev = hdev;
	INIT_WORK(&ally_x->output_worker, ally_x_work);
	spin_lock_init(&ally_x->lock);
	ally_x->output_worker_initialized = true;
	ally_x->qam_btns_steam_mode =
		true; /* Always default to steam mode, it can be changed by userspace attr */

	max_output_report_size = sizeof(struct ally_x_input_report);
	report = devm_kzalloc(&hdev->dev, sizeof(*report), GFP_KERNEL);
	if (!report) {
		ret = -ENOMEM;
		goto free_ally_x;
	}

	/* None of these bytes will change for the FF command for now */
	report->report_id = 0x0D;
	report->ff.enable = 0x0F; /* Enable all by default */
	report->ff.pulse_sustain_10ms = 0xFF; /* Duration */
	report->ff.pulse_release_10ms = 0x00; /* Start Delay */
	report->ff.loop_count = 0xEB; /* Loop Count */
	ally_x->ff_packet = report;

	ally_x->input = ally_x_setup_input(hdev);
	if (IS_ERR(ally_x->input)) {
		ret = PTR_ERR(ally_x->input);
		goto free_ff_packet;
	}

	if (sysfs_create_file(&hdev->dev.kobj, &dev_attr_ally_x_qam_mode.attr)) {
		ret = -ENODEV;
		goto unregister_input;
	}

	ally_x->update_ff = true;
	if (ally_x->output_worker_initialized)
		schedule_work(&ally_x->output_worker);

	hid_info(hdev, "Registered Ally X controller using %s\n",
		 dev_name(&ally_x->input->dev));
	return ally_x;

unregister_input:
	input_unregister_device(ally_x->input);
free_ff_packet:
	kfree(ally_x->ff_packet);
free_ally_x:
	kfree(ally_x);
	return ERR_PTR(ret);
}

static void ally_x_remove(struct hid_device *hdev)
{
	struct ally_x_device *ally_x = drvdata.ally_x;
	unsigned long flags;

	spin_lock_irqsave(&ally_x->lock, flags);
	ally_x->output_worker_initialized = false;
	spin_unlock_irqrestore(&ally_x->lock, flags);
	cancel_work_sync(&ally_x->output_worker);
	sysfs_remove_file(&hdev->dev.kobj, &dev_attr_ally_x_qam_mode.attr);
}

/**************************************************************************************************/
/* ROG Ally LED control                                                                           */
/**************************************************************************************************/
static void ally_rgb_schedule_work(struct ally_rgb_dev *led)
{
	unsigned long flags;

	spin_lock_irqsave(&led->lock, flags);
	if (!led->removed)
		schedule_work(&led->work);
	spin_unlock_irqrestore(&led->lock, flags);
}

/*
 * The RGB still has the basic 0-3 level brightness. Since the multicolour
 * brightness is being used in place, set this to max
 */
static int ally_rgb_set_bright_base_max(struct hid_device *hdev)
{
	u8 buf[] = { FEATURE_KBD_LED_REPORT_ID1, 0xba, 0xc5, 0xc4, 0x02 };

	return asus_dev_set_report(hdev, buf, sizeof(buf));
}

static void ally_rgb_do_work(struct work_struct *work)
{
	struct ally_rgb_dev *led = container_of(work, struct ally_rgb_dev, work);
	int ret;
	unsigned long flags;

	u8 buf[16] = { [0] = FEATURE_ROG_ALLY_REPORT_ID,
		       [1] = FEATURE_ROG_ALLY_CODE_PAGE,
		       [2] = xpad_cmd_set_leds,
		       [3] = xpad_cmd_len_leds };

	spin_lock_irqsave(&led->lock, flags);
	if (!led->update_rgb) {
		spin_unlock_irqrestore(&led->lock, flags);
		return;
	}

	for (int i = 0; i < 4; i++) {
		buf[5 + i * 3] = drvdata.led_rgb_dev->green[i];
		buf[6 + i * 3] = drvdata.led_rgb_dev->blue[i];
		buf[4 + i * 3] = drvdata.led_rgb_dev->red[i];
	}
	led->update_rgb = false;

	spin_unlock_irqrestore(&led->lock, flags);

	ret = asus_dev_set_report(led->hdev, buf, sizeof(buf));
	if (ret < 0)
		hid_err(led->hdev, "Ally failed to set gamepad backlight: %d\n", ret);
}

static void ally_rgb_set(struct led_classdev *cdev, enum led_brightness brightness)
{
	struct led_classdev_mc *mc_cdev = lcdev_to_mccdev(cdev);
	struct ally_rgb_dev *led = container_of(mc_cdev, struct ally_rgb_dev, led_rgb_dev);
	int intensity, bright;
	unsigned long flags;

	led_mc_calc_color_components(mc_cdev, brightness);
	spin_lock_irqsave(&led->lock, flags);
	led->update_rgb = true;
	bright = mc_cdev->led_cdev.brightness;
	for (int i = 0; i < 4; i++) {
		intensity = mc_cdev->subled_info[i].intensity;
		drvdata.led_rgb_dev->red[i] = (((intensity >> 16) & 0xFF) * bright) / 255;
		drvdata.led_rgb_dev->green[i] = (((intensity >> 8) & 0xFF) * bright) / 255;
		drvdata.led_rgb_dev->blue[i] = ((intensity & 0xFF) * bright) / 255;
	}
	spin_unlock_irqrestore(&led->lock, flags);
	drvdata.led_rgb_data.initialized = true;

	ally_rgb_schedule_work(led);
}

static int ally_rgb_set_static_from_multi(struct hid_device *hdev)
{
	u8 buf[17] = {FEATURE_KBD_LED_REPORT_ID1, 0xb3};
	int ret;

	/*
	 * Set single zone single colour based on the first LED of EC software mode.
	 * buf[2] = zone, buf[3] = mode
	 */
	buf[4] = drvdata.led_rgb_data.red[0];
	buf[5] = drvdata.led_rgb_data.green[0];
	buf[6] = drvdata.led_rgb_data.blue[0];

	ret = asus_dev_set_report(hdev, buf, sizeof(buf));
	if (ret < 0)
		return ret;

	ret = asus_dev_set_report(hdev, EC_MODE_LED_APPLY, sizeof(EC_MODE_LED_APPLY));
	if (ret < 0)
		return ret;

	return asus_dev_set_report(hdev, EC_MODE_LED_SET, sizeof(EC_MODE_LED_SET));
}

/*
 * Store the RGB values for restoring on resume, and set the static mode to the first LED colour
*/
static void ally_rgb_store_settings(void)
{
	int arr_size = sizeof(drvdata.led_rgb_data.red);

	struct ally_rgb_dev *led_rgb = drvdata.led_rgb_dev;

	drvdata.led_rgb_data.brightness = led_rgb->led_rgb_dev.led_cdev.brightness;

	memcpy(drvdata.led_rgb_data.red, led_rgb->red, arr_size);
	memcpy(drvdata.led_rgb_data.green, led_rgb->green, arr_size);
	memcpy(drvdata.led_rgb_data.blue, led_rgb->blue, arr_size);

	ally_rgb_set_static_from_multi(led_rgb->hdev);
}

static void ally_rgb_restore_settings(struct ally_rgb_dev *led_rgb, struct led_classdev *led_cdev,
				      struct mc_subled *mc_led_info)
{
	int arr_size = sizeof(drvdata.led_rgb_data.red);

	memcpy(led_rgb->red, drvdata.led_rgb_data.red, arr_size);
	memcpy(led_rgb->green, drvdata.led_rgb_data.green, arr_size);
	memcpy(led_rgb->blue, drvdata.led_rgb_data.blue, arr_size);
	for (int i = 0; i < 4; i++) {
		mc_led_info[i].intensity = (drvdata.led_rgb_data.red[i] << 16) |
					   (drvdata.led_rgb_data.green[i] << 8) |
					   drvdata.led_rgb_data.blue[i];
	}
	led_cdev->brightness = drvdata.led_rgb_data.brightness;
}

/* Set LEDs. Call after any setup. */
static void ally_rgb_resume(void)
{
	struct ally_rgb_dev *led_rgb = drvdata.led_rgb_dev;
	struct led_classdev *led_cdev;
	struct mc_subled *mc_led_info;

	if (!led_rgb)
		return;

	led_cdev = &led_rgb->led_rgb_dev.led_cdev;
	mc_led_info = led_rgb->led_rgb_dev.subled_info;

	if (drvdata.led_rgb_data.initialized) {
		ally_rgb_restore_settings(led_rgb, led_cdev, mc_led_info);
		led_rgb->update_rgb = true;
		ally_rgb_schedule_work(led_rgb);
		ally_rgb_set_bright_base_max(led_rgb->hdev);
	}
}

static int ally_rgb_register(struct hid_device *hdev, struct ally_rgb_dev *led_rgb)
{
	struct mc_subled *mc_led_info;
	struct led_classdev *led_cdev;

	mc_led_info =
		devm_kmalloc_array(&hdev->dev, 12, sizeof(*mc_led_info), GFP_KERNEL | __GFP_ZERO);
	if (!mc_led_info)
		return -ENOMEM;

	mc_led_info[0].color_index = LED_COLOR_ID_RGB;
	mc_led_info[1].color_index = LED_COLOR_ID_RGB;
	mc_led_info[2].color_index = LED_COLOR_ID_RGB;
	mc_led_info[3].color_index = LED_COLOR_ID_RGB;

	led_rgb->led_rgb_dev.subled_info = mc_led_info;
	led_rgb->led_rgb_dev.num_colors = 4;

	led_cdev = &led_rgb->led_rgb_dev.led_cdev;
	led_cdev->brightness = 128;
	led_cdev->name = "ally:rgb:joystick_rings";
	led_cdev->max_brightness = 255;
	led_cdev->brightness_set = ally_rgb_set;

	if (drvdata.led_rgb_data.initialized) {
		ally_rgb_restore_settings(led_rgb, led_cdev, mc_led_info);
	}

	return devm_led_classdev_multicolor_register(&hdev->dev, &led_rgb->led_rgb_dev);
}

static struct ally_rgb_dev *ally_rgb_create(struct hid_device *hdev)
{
	struct ally_rgb_dev *led_rgb;
	int ret;

	led_rgb = devm_kzalloc(&hdev->dev, sizeof(struct ally_rgb_dev), GFP_KERNEL);
	if (!led_rgb)
		return ERR_PTR(-ENOMEM);

	ret = ally_rgb_register(hdev, led_rgb);
	if (ret < 0) {
		cancel_work_sync(&led_rgb->work);
		devm_kfree(&hdev->dev, led_rgb);
		return ERR_PTR(ret);
	}

	led_rgb->hdev = hdev;
	led_rgb->removed = false;

	INIT_WORK(&led_rgb->work, ally_rgb_do_work);
	led_rgb->output_worker_initialized = true;
	spin_lock_init(&led_rgb->lock);

	ally_rgb_set_bright_base_max(hdev);

	/* Not marked as initialized unless ally_rgb_set() is called */
	if (drvdata.led_rgb_data.initialized) {
		msleep(1500);
		led_rgb->update_rgb = true;
		ally_rgb_schedule_work(led_rgb);
	}

	return led_rgb;
}

static void ally_rgb_remove(struct hid_device *hdev)
{
	struct ally_rgb_dev *led_rgb = drvdata.led_rgb_dev;
	unsigned long flags;
	int ep;

	ep = get_endpoint_address(hdev);
	if (ep != ALLY_CFG_INTF_IN_ADDRESS)
		return;

	if (!drvdata.led_rgb_dev || led_rgb->removed)
		return;

	spin_lock_irqsave(&led_rgb->lock, flags);
	led_rgb->removed = true;
	led_rgb->output_worker_initialized = false;
	spin_unlock_irqrestore(&led_rgb->lock, flags);
	cancel_work_sync(&led_rgb->work);
	devm_led_classdev_multicolor_unregister(&hdev->dev, &led_rgb->led_rgb_dev);

	hid_info(hdev, "Removed Ally RGB interface");
}

/**************************************************************************************************/
/* ROG Ally driver init                                                                           */
/**************************************************************************************************/

static int ally_raw_event(struct hid_device *hdev, struct hid_report *report, u8 *data,
			  int size)
{
	// struct ally_gamepad_cfg *cfg = drvdata.gamepad_cfg;
	struct ally_x_device *ally_x = drvdata.ally_x;

	if (ally_x) {
		if ((hdev->bus == BUS_USB && report->id == ALLY_X_INPUT_REPORT_USB &&
		     size == ALLY_X_INPUT_REPORT_USB_SIZE) ||
		    (data[0] == 0x5A)) {
			ally_x_raw_event(ally_x, report, data, size);
		} else {
			return -1;
		}
	}
	// if (cfg && !ally_x) {
	// 	input_report_key(cfg->input, KEY_PROG1, data[1] == 0x38);
	// 	input_report_key(cfg->input, KEY_F16, data[1] == 0xA6);
	// 	input_report_key(cfg->input, KEY_F17, data[1] == 0xA7);
	// 	input_report_key(cfg->input, KEY_F18, data[1] == 0xA8);
	// 	input_sync(cfg->input);
	// }

	return 0;
}

/*
 * Very simple parse. We don't care about any other part of the string except the version section.
 * Example strings: FGA80100.RC72LA.312_T01, FGA80100.RC71LS.318_T01
 */
static int mcu_parse_version_string(const u8 *response, size_t response_size)
{
	int dot_count = 0;
	size_t i;

	// Look for the second '.' to identify the start of the version
	for (i = 0; i < response_size; i++) {
		if (response[i] == '.') {
			dot_count++;
			if (dot_count == 2) {
				int version =
					simple_strtol((const char *)&response[i + 1], NULL, 10);
				return (version >= 0) ? version : -EINVAL;
			}
		}
	}

	return -EINVAL;
}

static int mcu_request_version(struct hid_device *hdev)
{
	const u8 request[] = { 0x5a, 0x05, 0x03, 0x31, 0x00, 0x20 };
	size_t response_size = FEATURE_ROG_ALLY_REPORT_SIZE;
	u8 *response;
	int ret;

	response = kzalloc(response_size, GFP_KERNEL);
	if (!response) {
		kfree(request);
		return -ENOMEM;
	}

	ret = asus_dev_set_report(hdev, request, sizeof(request));
	if (ret < 0)
		goto error;

	ret = asus_dev_get_report(hdev, response, response_size);
	if (ret < 0)
		goto error;

	ret = mcu_parse_version_string(response, response_size);
	if (ret < 0)
		goto error;

	goto cleanup;

error:
	hid_err(hdev, "Failed to get MCU version: %d\n", ret);
cleanup:
	kfree(response);

	return ret;
}

static void mcu_maybe_warn_version(struct hid_device *hdev, int idProduct)
{
	int min_version, version;

	min_version = 0;
	version = mcu_request_version(hdev);
	if (version) {
		switch (idProduct) {
		case USB_DEVICE_ID_ASUSTEK_ROG_NKEY_ALLY:
			min_version = ALLY_MIN_BIOS;
			break;
		case USB_DEVICE_ID_ASUSTEK_ROG_NKEY_ALLY_X:
			min_version = ALLY_X_MIN_BIOS;
			break;
		}
	}

	hid_info(hdev, "Ally device MCU version: %d\n", version);
	if (version <= min_version) {
		hid_warn(hdev,
			 "The MCU version must be %d or greater\n"
			 "Please update your MCU with official ASUS firmware release "
			 "which has bug fixes to make the Linux experience better\n",
			 min_version);
	}
}

static int ally_hid_init(struct hid_device *hdev)
{
	int ret;

	ret = asus_dev_set_report(hdev, EC_INIT_STRING, sizeof(EC_INIT_STRING));
	if (ret < 0) {
		hid_err(hdev, "Ally failed to send init command: %d\n", ret);
		return ret;
	}

	ret = asus_dev_set_report(hdev, FORCE_FEEDBACK_OFF, sizeof(FORCE_FEEDBACK_OFF));
	if (ret < 0)
		hid_err(hdev, "Ally failed to send init command: %d\n", ret);

	return ret;
}

static int ally_hid_probe(struct hid_device *hdev, const struct hid_device_id *_id)
{
	struct usb_interface *intf = to_usb_interface(hdev->dev.parent);
	struct usb_device *udev = interface_to_usbdev(intf);
	u16 idProduct = le16_to_cpu(udev->descriptor.idProduct);
	int ret, ep;

	ep = get_endpoint_address(hdev);
	if (ep < 0)
		return ep;

	if (ep != ALLY_CFG_INTF_IN_ADDRESS ||
	    ep != ALLY_X_INTERFACE_ADDRESS)
		return -ENODEV;

	ret = hid_parse(hdev);
	if (ret) {
		hid_err(hdev, "Parse failed\n");
		return ret;
	}

	ret = hid_hw_start(hdev, HID_CONNECT_HIDRAW);
	if (ret) {
		hid_err(hdev, "Failed to start HID device\n");
		return ret;
	}

	ret = hid_hw_open(hdev);
	if (ret) {
		hid_err(hdev, "Failed to open HID device\n");
		goto err_stop;
	}

	/* Initialize MCU even before alloc */
	ret = ally_hid_init(hdev);
	if (ret < 0)
		return ret;

	drvdata.hdev = hdev;
	hid_set_drvdata(hdev, &drvdata);

	/* This should almost always exist */
	if (ep == ALLY_CFG_INTF_IN_ADDRESS) {
		mcu_maybe_warn_version(hdev, idProduct);

		drvdata.led_rgb_dev = ally_rgb_create(hdev);
		if (IS_ERR(drvdata.led_rgb_dev))
			hid_err(hdev, "Failed to create Ally gamepad LEDs.\n");
		else
			hid_info(hdev, "Created Ally RGB LED controls.\n");

		if (IS_ERR(drvdata.led_rgb_dev))
			goto err_close;
	}

	/* May or may not exist */
	if (ep == ALLY_X_INTERFACE_ADDRESS) {
		drvdata.ally_x = ally_x_create(hdev);
		if (IS_ERR(drvdata.ally_x)) {
			hid_err(hdev, "Failed to create Ally X gamepad.\n");
			drvdata.ally_x = NULL;
			goto err_close;
		}
		hid_info(hdev, "Created Ally X controller.\n");

		// Not required since we send this inputs ep through the gamepad input dev
		// if (drvdata.gamepad_cfg && drvdata.gamepad_cfg->input) {
		// 	input_unregister_device(drvdata.gamepad_cfg->input);
		// 	hid_info(hdev, "Ally X removed unrequired input dev.\n");
		// }
	}

	return 0;

err_close:
	hid_hw_close(hdev);
err_stop:
	hid_hw_stop(hdev);
	return ret;
}

static void ally_hid_remove(struct hid_device *hdev)
{
	if (drvdata.led_rgb_dev)
		ally_rgb_remove(hdev);

	if (drvdata.ally_x)
		ally_x_remove(hdev);

	hid_hw_close(hdev);
	hid_hw_stop(hdev);
}

static int ally_hid_resume(struct hid_device *hdev)
{
	ally_rgb_resume();

	return 0;
}

static int ally_hid_reset_resume(struct hid_device *hdev)
{
	int ep = get_endpoint_address(hdev);
	if (ep != ALLY_CFG_INTF_IN_ADDRESS)
		return 0;

	ally_hid_init(hdev);
	ally_rgb_resume();

	return 0;
}

static int ally_pm_thaw(struct device *dev)
{
	struct hid_device *hdev = to_hid_device(dev);

	return ally_hid_reset_resume(hdev);
}

static int ally_pm_suspend(struct device *dev)
{
	if (drvdata.led_rgb_dev) {
		ally_rgb_store_settings();
	}

	return 0;
}

static const struct dev_pm_ops ally_pm_ops = {
	.thaw = ally_pm_thaw,
	.suspend = ally_pm_suspend,
	.poweroff = ally_pm_suspend,
};

MODULE_DEVICE_TABLE(hid, rog_ally_devices);

static struct hid_driver
	rog_ally_cfg = { .name = "asus_rog_ally",
			 .id_table = rog_ally_devices,
			 .probe = ally_hid_probe,
			 .remove = ally_hid_remove,
			 .raw_event = ally_raw_event,
			 /* HID is the better place for resume functions, not pm_ops */
			 .resume = ally_hid_resume,
			 .reset_resume = ally_hid_reset_resume,
			 .driver = {
				 .pm = &ally_pm_ops,
			 } };

static int __init rog_ally_init(void)
{
	return hid_register_driver(&rog_ally_cfg);
}

static void __exit rog_ally_exit(void)
{
	hid_unregister_driver(&rog_ally_cfg);
}

module_init(rog_ally_init);
module_exit(rog_ally_exit);

MODULE_AUTHOR("Luke D. Jones");
MODULE_DESCRIPTION("HID Driver for ASUS ROG Ally gamepad configuration.");
MODULE_LICENSE("GPL");
