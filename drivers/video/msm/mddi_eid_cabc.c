/* drivers/video/msm_fb/mddi_eid_cabc.c
 *
 * Support for Samsung S6D05A0 CABC function
 *
 * Copyright (C) 2007 HTC Incorporated
 * Author: Jay Tu (jay_tu@htc.com)
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/platform_device.h>
#include <linux/delay.h>
#include <mach/msm_fb.h>
#include <linux/leds.h>
#include <linux/wakelock.h>
#include <linux/ls_alg.h> /* for light sensor alg */
#include <mach/htc_battery.h>
#include <mach/htc_pwrsink.h>
#include <linux/sched.h>

#define SLPIN		0x10
#define SLPOUT 		0x11
#define CASET		0x2a
#define PASET		0x2b
#define RAMWR		0x2c
#define TEON		0x35
#define MADCTL		0x36
#define COLMOD		0x3a
#define WRDISBV 	0x51
#define RDDISBV		0x52
#define WRCTRLD		0x53
#define WRCABC		0x55
#define WRCABCMB	0x5e
#define MIECTL1		0xca
#define BCMODE 		0xcb
#define MIECTL2		0xcc
#define MIECTL3		0xcd
#define DCON		0xef
#define DISCTL 		0xf2
#define PWRCTL 		0xf3
#define VCMCTL 		0xf4
#define SRCCTL 		0xf5
#define GAMCTL1		0xf7
#define GAMCTL2		0xf8
#define GATECTL		0xfd
#define MDDICTL		0xe0
#define DCON		0xef

enum bc_mode {
	BC_OFF = 0,
	BC_MANUAL,
	BC_AUTO,
	BC_MERGED,
	BC_UNDEF,
};

enum cabc_mode {
	CABC_OFF = 0,
	CABC_UI,
	CABC_STILL,
	CABC_MOVING,
	CABC_UNDEF,
};

#if 0
#define B(s...) printk(s)
#else
#define B(s...) do {} while(0)
#endif

#define DEBUG 0
#define DEFAULT_BRIGHTNESS 100
static char *str_bc_mode[] = {"BC_OFF", "BC_MANUAL", "BC_AUTO", "BC_MERGED"};
static char *str_cabc_mode[] = {"CABC_OFF", "CABC_USER_INTERFACE", 
				"CABC_STILL_IMAGE", "CABC_MOVING"};

static ssize_t 
samsung_show(struct device *dev, struct device_attribute *attr, char *buf);
static ssize_t
samsung_store(struct device *dev, struct device_attribute *attr,
		const char *buf, size_t count);

struct cabc_platform_data {
	int panel;
	int shrink;
	struct msm_mddi_client_data *client;
};

struct cabc {
	struct cabc_platform_data *cabc_config;
	struct led_classdev lcd_backlight;	/* user */
	struct led_classdev light_sensor_dev;	/* light sensor */
	struct led_classdev lcd_backlight_gate; /* other driver */
	struct ls_alg_classdev alg_cdev;	/* light sensor alg */
	struct mutex lock;
	struct work_struct resume_work;
	struct work_struct lcd_changed_work;	/* lcd backlight */
	struct work_struct ls_changed_work;	/* light sensor */
	struct wake_lock wakelock;
	wait_queue_head_t wq;
#ifdef CONFIG_HAS_EARLYSUSPEND
	struct early_suspend early_suspend;
#endif
	uint32_t status;
	enum cabc_mode mode_cabc;
	enum bc_mode mode_bc;
};

enum {
	LIGHT_SENSOR_ON = 1U << 0,
	GATE_ON		= 1U << 1,
	ENFORCE_ON	= 1U << 2,
	SUSPEND		= 1U << 3,
};

#define to_cabc(p, m) container_of(p, struct cabc, m)

struct complete_data {
	int done;
};

static inline
struct msm_mddi_client_data *cabc_get_client(struct cabc* cabc)
{
	struct cabc_platform_data *data = cabc->cabc_config;
	return data->client;
}

static inline int cabc_shrink(struct cabc *cabc, int brightness)
{
	struct cabc_platform_data *config = cabc->cabc_config;

	if (config->shrink)
		return (int)(brightness * 83 / 100);
	return brightness;
}

static inline int samsung_wait(struct cabc *cabc)
{
	int rc = 0;

	mutex_lock(&cabc->lock);
	if (cabc->status & ENFORCE_ON)
		goto end;

	rc = wait_event_interruptible_timeout(cabc->wq,
			cabc->status & GATE_ON,
			HZ);
end:
	mutex_unlock(&cabc->lock);

	if (rc <= 0)
		printk(KERN_WARNING "%s: wait failed (%d), mask = 0x%x\n",
				__func__, rc, cabc->status);
	return rc;
}

static uint32_t cabc_check_mask(struct cabc *cabc, uint32_t mask)
{
	uint32_t ret;

	mutex_lock(&cabc->lock);
	ret = cabc->status & mask;
	mutex_unlock(&cabc->lock);
	return ret;
}

static inline void
cabc_update_mask(struct cabc *cabc, uint32_t val, uint32_t mask)
{
	if ((cabc->status & mask) != val)
		cabc->status ^= mask;
}

static void 
samsung_send_cmd(struct msm_mddi_client_data *client_data, unsigned cmd,
	     uint8_t *prm, int size, uint8_t attrs, ...)
{
	int i;
	uint8_t tmp;
	va_list attr_list;

	if (size <= 0)
		return;

	prm[0] = attrs;

	va_start(attr_list, attrs);

	for (i = 1; i < size; i++) {
		tmp = (uint8_t)va_arg(attr_list, int);
		prm[i] = tmp;
	}

	va_end(attr_list);
#if DEBUG
	printk(KERN_DEBUG "0x%x ", cmd);
	for (i = 0; i < size; i++)
		printk("0x%x ", prm[i]);
	printk("\n");
#endif
	if (client_data)
		client_data->remote_write_vals(client_data, prm, cmd, size);
}

static int 
samsung_change_cabcmode(struct cabc *cabc, enum cabc_mode mode, uint8_t dimming)
{
	uint8_t prm[20];
	struct msm_mddi_client_data *client_data = cabc_get_client(cabc);
#if DEBUG
	printk(KERN_DEBUG "%s\n", __func__);
#endif
	samsung_send_cmd(client_data, MIECTL1, prm, 4, 0x80, 0x80, 0x10, 0x00);
	samsung_send_cmd(client_data, MIECTL2, prm, 8, 0x80, 0x07, 0x7c, 0x01,
			0x3f, 0x00, 0x00, 0x00);
	samsung_send_cmd(client_data, MIECTL3, prm, 4, 0x7f, dimming,
			0x00, 0x00);
	samsung_send_cmd(client_data, WRCABC, prm, 4, (uint8_t)mode, 0x00, 0x00,
			0x00);
	samsung_send_cmd(client_data, DCON, prm, 4, 0x07, 0x00, 0x00, 0x00);
	return 0;
}

static int
samsung_set_dimming(struct ls_alg_classdev *alg)
{
	struct cabc *cabc = to_cabc(alg, alg_cdev);
	return samsung_change_cabcmode(cabc, cabc->mode_cabc, 0x05);
}

static int
__set_brightness(struct cabc *cabc, int brightness)
{
	struct msm_mddi_client_data *client_data = cabc_get_client(cabc);
	uint8_t prm[20];
	unsigned percent;
	int shrink_br = brightness;

	/* no need to check brightness > LED_FULL, the led class
	 * already does */
	printk(KERN_INFO "brightness = %d, %s ls-(%s)\n",
			brightness,
			str_bc_mode[cabc->mode_bc],
			(cabc->status & LIGHT_SENSOR_ON) ? "on":"off");

	mutex_lock(&cabc->lock);
	shrink_br = cabc_shrink(cabc, brightness);
	percent = (shrink_br * 100) / 255;
	htc_pwrsink_set(PWRSINK_BACKLIGHT, percent);

	samsung_send_cmd(client_data, BCMODE, prm, 4,
			(uint8_t)cabc->mode_bc, 0x00, 0x00, 0x00);
	samsung_send_cmd(client_data, WRDISBV, prm, 4,
			(uint8_t)shrink_br, 0x00, 0x00, 0x00);
	samsung_send_cmd(client_data, WRCABC, prm, 4,
			(uint8_t)cabc->mode_cabc, 0x00, 0x00, 0x00);
	samsung_send_cmd(client_data, WRCTRLD, prm, 4, 0x2c, 0x00, 0x00, 0x00);
	samsung_send_cmd(client_data, DCON, prm, 4, 0x07, 0x00, 0x00, 0x00);

	mutex_unlock(&cabc->lock);
	return 0;
}

static void cabc_lcd_work(struct work_struct *work)
{
	struct cabc *cabc = to_cabc(work, lcd_changed_work);
	struct led_classdev *led_cdev = &cabc->lcd_backlight;
	char event_string[30];
	char *envp[] = { event_string, NULL };

	__set_brightness(cabc, led_cdev->brightness);
	sprintf(event_string, "CABC_BRIGHTNESS=%d", led_cdev->brightness);
	kobject_uevent_env(&led_cdev->dev->kobj, KOBJ_CHANGE, envp);

	wake_unlock(&cabc->wakelock);
}

/* user */
static void
samsung_set_brightness(struct led_classdev *led_cdev,
		       enum led_brightness brightness)
{
	struct cabc *cabc = to_cabc(led_cdev, lcd_backlight);
	int rc;

	/* did not accept user input while light sensor is on */
	rc = cabc_check_mask(cabc, LIGHT_SENSOR_ON | SUSPEND);
	if (rc)
		return;

	wake_lock(&cabc->wakelock);

	rc = samsung_wait(cabc);
	if (rc > 0)
		schedule_work(&cabc->lcd_changed_work);
	else
		wake_unlock(&cabc->wakelock);
}

static enum led_brightness
samsung_get_brightness(struct led_classdev *led_cdev)
{
	struct cabc *cabc = to_cabc(led_cdev, lcd_backlight);
	struct msm_mddi_client_data *client_data = cabc_get_client(cabc);
	uint32_t val = 0;

	wake_lock(&cabc->wakelock);

	val = client_data->remote_read(client_data, RDDISBV);
	val &= 0x000000ff;
	B(KERN_DEBUG "%s: brightness = %d\n", __func__,  val);

	wake_unlock(&cabc->wakelock);
	return val;
}

static int ls_on(struct cabc *cabc, int on)
{
	struct led_classdev *led_cdev = &cabc->light_sensor_dev;
	struct complete_data data;
	int rc;

	if (!led_cdev->trigger ||
	    !led_cdev->trigger->activate ||
	    !led_cdev->trigger->deactivate) {
		printk(KERN_ERR "%s: invalid trigger!\n", __func__);
		return -EIO;
	}

	if ((cabc->status & LIGHT_SENSOR_ON) == on)
		return 0;

	data.done = 0;
	led_cdev->trigger_data = &data;
	if (on)
		led_cdev->trigger->activate(led_cdev);
	else 
		led_cdev->trigger->deactivate(led_cdev);

	rc = wait_event_timeout(cabc->wq, data.done, HZ);
	if (rc == 0) {
		printk(KERN_WARNING "%s: turn on/off light sensor failed!\n",
				__func__);
		/* should we wait more? */
		rc = -EIO;
	}
	return rc;
}

static void cabc_ls_work(struct work_struct *work)
{
	struct cabc *cabc = to_cabc(work, ls_changed_work);
	struct led_classdev *lcd_cdev = &cabc->lcd_backlight;
	struct ls_alg_classdev *alg_cdev = &cabc->alg_cdev;
	char event_string[30];
	char *envp[] = { event_string, NULL };
	int rc;

	rc = samsung_wait(cabc);
	if (rc > 0) {
		__set_brightness(cabc, alg_cdev->brightness);
		sprintf(event_string, "CABC_BRIGHTNESS=%d",
				alg_cdev->brightness);
		kobject_uevent_env(&lcd_cdev->dev->kobj, KOBJ_CHANGE, envp);
	}

	wake_unlock(&cabc->wakelock);
}

static int
ls_update_brightness(struct ls_alg_classdev *alg, int brightness, int increase)
{
	struct cabc *cabc = to_cabc(alg, alg_cdev);

	B(KERN_DEBUG "%s: enter, val = %d\n", __func__, brightness);
	if (increase)
		samsung_change_cabcmode(cabc, cabc->mode_cabc, 0x25);
	wake_lock(&cabc->wakelock);
	schedule_work(&cabc->ls_changed_work);
	return 0;
}

/* this should pass from board */
static uint8_t pwm_data[10] = {8, 16, 34, 61, 96, 138, 167, 195, 227, 255};
static void
ls_set_brightness(struct led_classdev *led_cdev, enum led_brightness level)
{
	struct cabc *cabc = to_cabc(led_cdev, light_sensor_dev);
	struct ls_alg_classdev *alg_cdev = &cabc->alg_cdev;
	int brightness = pwm_data[level];

	if (cabc->status & SUSPEND == 0)
		ls_alg_process(alg_cdev, level, brightness);
}

static enum led_brightness
ls_get_brightness(struct led_classdev *led_cdev)
{
	int level = led_cdev->brightness;
	return pwm_data[level];
}

static void cabc_resume_work(struct work_struct *work)
{
	struct cabc *cabc = to_cabc(work, resume_work);
	struct led_classdev *led_cdev = &cabc->lcd_backlight_gate;
	struct led_classdev *lcd_cdev = &cabc->lcd_backlight;

	if (led_cdev->brightness != LED_FULL) {
		mutex_lock(&cabc->lock);
		cabc->status &= ~GATE_ON;
		mutex_unlock(&cabc->lock);
	} else {
		samsung_change_cabcmode(cabc, cabc->mode_cabc, 0x25);
		if (lcd_cdev->brightness > 0)
			__set_brightness(cabc, lcd_cdev->brightness);
		else
			__set_brightness(cabc, DEFAULT_BRIGHTNESS);

		mutex_lock(&cabc->lock);
		cabc->status |= GATE_ON;
		mutex_unlock(&cabc->lock);
		wake_up_interruptible(&cabc->wq);
	}

	wake_unlock(&cabc->wakelock);
}

static void
blgate_set_brightness(struct led_classdev *led_cdev,
		       enum led_brightness brightness)
{
	struct cabc *cabc = to_cabc(led_cdev, lcd_backlight_gate);

	printk(KERN_DEBUG "turn %s backlight.\n", 
		brightness == LED_FULL ?  "on":"off");

	wake_lock(&cabc->wakelock);
	schedule_work(&cabc->resume_work);
}

static enum led_brightness
blgate_get_brightness(struct led_classdev *led_cdev)
{
	return led_cdev->brightness;
}

static int
samsung_auto_backlight(struct cabc *cabc, int on)
{
	int rc = -EIO;
	enum bc_mode bc_tmp = cabc->mode_bc;
	enum cabc_mode cabc_tmp = cabc->mode_cabc;

	/* for consistent with android UI,
	 * 1: turn on, LABC
	 * 0: turn off, Manual */
	printk(KERN_DEBUG "%s: %s\n", __func__, on ? "ON":"OFF");
	if (on) {
		/* bc_tmp = BC_MERGED; */
		cabc_tmp = CABC_OFF;
	} else {
		bc_tmp = BC_MANUAL;
		cabc_tmp = CABC_OFF;
	}

	rc = ls_on(cabc, on);
	if (rc >= 0) {
		cabc_update_mask(cabc, on, LIGHT_SENSOR_ON);
		/* update on success */
		cabc->mode_bc = bc_tmp;
		cabc->mode_cabc = cabc_tmp;
		if (on) {
			cabc->alg_cdev.brightness = 0;
			cabc->alg_cdev.level = 0;
		}
		rc = 0;
	} else 
		printk(KERN_ERR "set auto failed!\n");
	return rc;
}

#define CABC_ATTR(name) __ATTR(name, 0644, samsung_show, samsung_store)

enum {
	CABC_MODE = 0,
	BC_MODE,
	LIGHT_SENSOR,
	AUTO_BACKLIGHT,
};

static struct device_attribute cabc_attrs[] = {
	CABC_ATTR(cabc_mode),
	CABC_ATTR(bc_mode),
	CABC_ATTR(light_sensor),
	CABC_ATTR(auto),
};

static ssize_t 
samsung_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	int i = 0, brightness;
	const ptrdiff_t off = attr - cabc_attrs;
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct cabc *cabc = to_cabc(led_cdev, lcd_backlight);
	struct led_classdev *lightsensor_cdev = &cabc->light_sensor_dev;

	mutex_lock(&cabc->lock);
	switch (off) {
	case CABC_MODE:
		i += scnprintf(buf + i, PAGE_SIZE - 1, "%s\n",
				str_cabc_mode[cabc->mode_cabc]);
		break;
	case BC_MODE:
		i += scnprintf(buf + i, PAGE_SIZE - 1, "%s\n",
				str_bc_mode[cabc->mode_bc]);
		break;
	case LIGHT_SENSOR:
		brightness = ls_get_brightness(lightsensor_cdev);
		i += scnprintf(buf + i, PAGE_SIZE - 1, "%d\n", brightness);
		break;
	case AUTO_BACKLIGHT:
		i += scnprintf(buf + i, PAGE_SIZE - 1, "%d\n",
				cabc->status & LIGHT_SENSOR_ON);
		break;
	default:
		i = -EINVAL;
		break;
	}
	mutex_unlock(&cabc->lock);
	return i;
}

static ssize_t
samsung_store(struct device *dev, struct device_attribute *attr,
	      const char *buf, size_t count)
{
	int rc;
	unsigned long res;
	const ptrdiff_t off = attr - cabc_attrs;
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct cabc *cabc = to_cabc(led_cdev, lcd_backlight);

	rc = strict_strtoul(buf, 10, &res);
	if (rc) {
		printk(KERN_ERR "invalid parameter, %s %d\n", buf, rc);
		count = -EINVAL;
		goto err_out;
	}

	mutex_lock(&cabc->lock);
	switch (off) {
	case CABC_MODE:
		if (res >= CABC_OFF && res < CABC_UNDEF) {
			cabc->mode_cabc = res;
			samsung_change_cabcmode(cabc, res, 0x25);
		}
		break;
	case BC_MODE:
		if (res >= BC_OFF && res < BC_UNDEF)
			cabc->mode_bc = res;
		break;
	case LIGHT_SENSOR:
		if (ls_on(cabc, !!res) >= 0)
			cabc_update_mask(cabc, !!res, LIGHT_SENSOR_ON);
		else
			count = -EIO;
		break;
	case AUTO_BACKLIGHT:
		if (samsung_auto_backlight(cabc, !!res))
			count = -EIO;
		break;
	default:
		count = -EINVAL;
		break;
	}
	mutex_unlock(&cabc->lock);
err_out:
	return count;
}

#define LED_DEV(ptr, member, _name)				\
{								\
	(ptr)->member.name = #_name;				\
	(ptr)->member.brightness_set = _name##_set_brightness;	\
	(ptr)->member.brightness_get = _name##_get_brightness;	\
}

static void
samsung_cabc_suspend(struct early_suspend *h)
{
	struct cabc *cabc = to_cabc(h, early_suspend);
	int res;

	B(KERN_DEBUG "%s\n", __func__);
	res = batt_notifier_call_chain(BATT_EVENT_SUSPEND, NULL);
	if (res != NOTIFY_STOP) {
		cabc->status |= SUSPEND;
		if (cabc_check_mask(cabc, LIGHT_SENSOR_ON))
			led_classdev_suspend(&cabc->light_sensor_dev);
	} else {
		mutex_lock(&cabc->lock);
		cabc->status |= ENFORCE_ON;
		mutex_unlock(&cabc->lock);
		__set_brightness(cabc, DEFAULT_BRIGHTNESS);
	}
}

static void
samsung_cabc_resume(struct early_suspend *h)
{
	struct cabc *cabc = to_cabc(h, early_suspend);

	B(KERN_DEBUG "%s\n", __func__);

	cabc->status &= ~(ENFORCE_ON | SUSPEND);
	if (cabc->status & LIGHT_SENSOR_ON)
		led_classdev_resume(&cabc->light_sensor_dev);
}

static int samsung_cabc_probe(struct platform_device *pdev)
{
	int i, err;
	struct cabc *cabc;
	struct cabc_platform_data *data;

	cabc = kzalloc(sizeof(struct cabc), GFP_KERNEL);
	if (!cabc)
		return -ENOMEM;
	platform_set_drvdata(pdev, cabc);

	data = pdev->dev.platform_data;
	if (data == NULL || !data->client) {
		err = -EINVAL;
		goto err_client;
	}

	cabc->cabc_config = data;
	INIT_WORK(&cabc->resume_work, cabc_resume_work);
	INIT_WORK(&cabc->lcd_changed_work, cabc_lcd_work);
	INIT_WORK(&cabc->ls_changed_work, cabc_ls_work);
	mutex_init(&cabc->lock);
	init_waitqueue_head(&cabc->wq);
	wake_lock_init(&cabc->wakelock, WAKE_LOCK_IDLE, "cabc_present");

	cabc->lcd_backlight.name = "lcd-backlight";
	cabc->lcd_backlight.brightness_set = samsung_set_brightness;
	cabc->lcd_backlight.brightness_get = samsung_get_brightness;
	err = led_classdev_register(&pdev->dev, &cabc->lcd_backlight);
	if (err)
		goto err_register_lcd_bl;

	LED_DEV(cabc, light_sensor_dev, ls);
	cabc->light_sensor_dev.default_trigger = "light-sensor-trigger";
	err = led_classdev_register(&pdev->dev, &cabc->light_sensor_dev);
	if (err)
		goto err_register_ls;

	LED_DEV(cabc, lcd_backlight_gate, blgate);
	cabc->lcd_backlight_gate.default_trigger = "lcd-backlight-gate";
	err = led_classdev_register(&pdev->dev, &cabc->lcd_backlight_gate);
	if (err)
		goto err_register_bl_gate;

	for (i = 0; i < ARRAY_SIZE(cabc_attrs); i++) {
		err = device_create_file(cabc->lcd_backlight.dev,
					&cabc_attrs[i]);
		if (err)
			goto err_out;
	}

	/* default setting */
	cabc->mode_cabc = CABC_OFF;
	samsung_change_cabcmode(cabc, cabc->mode_cabc, 0x25);
	cabc->mode_bc = BC_MANUAL;
	samsung_set_brightness(&cabc->lcd_backlight, 255);

#ifdef CONFIG_HAS_EARLYSUSPEND
	cabc->early_suspend.suspend = samsung_cabc_suspend;
	cabc->early_suspend.resume = samsung_cabc_resume;
	cabc->early_suspend.level = EARLY_SUSPEND_LEVEL_BLANK_SCREEN;
	register_early_suspend(&cabc->early_suspend);
#endif
	cabc->alg_cdev.name = "cabc_ls_alg";
	cabc->alg_cdev.update = ls_update_brightness;
	cabc->alg_cdev.dimming_set = samsung_set_dimming;
	err = ls_alg_create(&pdev->dev, &cabc->alg_cdev);
	if (err)
		goto err_out;
	return 0;

err_out:
	while (i--)
		device_remove_file(&pdev->dev, &cabc_attrs[i]);
err_register_bl_gate:
	led_classdev_unregister(&cabc->lcd_backlight_gate);
err_register_ls:
	led_classdev_unregister(&cabc->light_sensor_dev);
err_register_lcd_bl:
	led_classdev_unregister(&cabc->lcd_backlight);
err_client:
	kfree(cabc);
	return err;
}

static struct platform_driver samsung_cabc_driver = {
	.probe = samsung_cabc_probe,
	.driver = { .name = "samsung_cabc" },
};

static int __init samsung_cabc_init(void)
{
	return platform_driver_register(&samsung_cabc_driver);
}

module_init(samsung_cabc_init);

MODULE_AUTHOR("Jay Tu <jay_tu@htc.com>");
MODULE_DESCRIPTION("samsung cabc driver");
MODULE_LICENSE("GPL");
