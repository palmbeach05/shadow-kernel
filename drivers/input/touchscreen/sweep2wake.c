/*
 * drivers/input/touchscreen/sweep2wake.c
 *
 *
 * Copyright (c) 2013, Dennis Rassmann <showp1984@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/err.h>
#include <linux/input/sweep2wake.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <linux/input.h>
#ifndef CONFIG_HAS_EARLYSUSPEND
#include <linux/lcd_notify.h>
#else
#include <linux/earlysuspend.h>
#endif
#include <linux/hrtimer.h>

/* uncomment since no touchscreen defines android touch, do that here */
//#define ANDROID_TOUCH_DECLARED

/* Version, author, desc, etc */
#define DRIVER_AUTHOR "Dennis Rassmann <showp1984@gmail.com>"
#define DRIVER_DESCRIPTION "Sweep2wake for almost any device"
#define DRIVER_VERSION "1.5"
#define LOGTAG "[sweep2wake]: "

MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESCRIPTION);
MODULE_VERSION(DRIVER_VERSION);
MODULE_LICENSE("GPLv2");

/* Tuneables */
#define S2W_DEBUG		0
#define S2W_DEFAULT		1
#define S2W_S2SONLY_DEFAULT	0
#define S2W_PWRKEY_DUR          60
#define S2W_MIN_DISPLACEMENT_PERCENT 60

/* Resources */
int s2w_switch = S2W_DEFAULT, s2w_s2sonly = S2W_S2SONLY_DEFAULT;
static int touch_x = 0, touch_y = 0;
static int packet_x, packet_y;
static bool touch_x_valid, touch_y_valid;
static bool touch_x_called = false, touch_y_called = false;
static bool contact_called, contact_active, empty_contact_report;
static int touch_major;
static bool touch_major_called;
static int tracking_id, active_tracking_id;
static bool tracking_id_called, active_tracking_id_valid;
static bool scr_suspended = false, exec_count = true;
static int x_min, x_max, y_min, y_max;
static int y_limit;
static int gesture_start_x;
static int gesture_max_progress;
static bool gesture_start_valid;
static bool gesture_blocked;
static unsigned int report_contacts;
static int report_x, report_y;
static bool report_contact_valid;
static unsigned int diagnostic_gesture_id;
static bool diagnostic_gesture_active;
static bool diagnostic_power_queued;
#ifndef CONFIG_HAS_EARLYSUSPEND
static struct notifier_block s2w_lcd_notif;
#endif
static struct input_dev * sweep2wake_pwrdev;
static DEFINE_MUTEX(pwrkeyworklock);

/* Read cmdline for s2w */
static int __init read_s2w_cmdline(char *s2w)
{
	if (strcmp(s2w, "1") == 0) {
		pr_info("[cmdline_s2w]: Sweep2Wake enabled. | s2w='%s'\n", s2w);
		s2w_switch = 1;
	} else if (strcmp(s2w, "0") == 0) {
		pr_info("[cmdline_s2w]: Sweep2Wake disabled. | s2w='%s'\n", s2w);
		s2w_switch = 0;
	} else {
		pr_info("[cmdline_s2w]: No valid input found. Going with default: | s2w='%u'\n", s2w_switch);
	}
	return 1;
}
__setup("s2w=", read_s2w_cmdline);

/* PowerKey work func */
static void sweep2wake_presspwr(struct work_struct * sweep2wake_presspwr_work) {
	if (!mutex_trylock(&pwrkeyworklock))
                return;
	pr_info(LOGTAG "diag id=%u power: emitting KEY_POWER\n",
		diagnostic_gesture_id);
	input_event(sweep2wake_pwrdev, EV_KEY, KEY_POWER, 1);
	input_event(sweep2wake_pwrdev, EV_SYN, 0, 0);
	msleep(S2W_PWRKEY_DUR);
	input_event(sweep2wake_pwrdev, EV_KEY, KEY_POWER, 0);
	input_event(sweep2wake_pwrdev, EV_SYN, 0, 0);
	msleep(S2W_PWRKEY_DUR);
        mutex_unlock(&pwrkeyworklock);
	return;
}
static DECLARE_WORK(sweep2wake_presspwr_work, sweep2wake_presspwr);

/* PowerKey trigger */
static void sweep2wake_pwrtrigger(void) {
	pr_info(LOGTAG "diag id=%u power: queued\n",
		diagnostic_gesture_id);
	schedule_work(&sweep2wake_presspwr_work);
        return;
}

/* reset on finger release */
static void sweep2wake_reset(void) {
	exec_count = true;
	gesture_start_valid = false;
	gesture_max_progress = 0;
}

static void sweep2wake_invalidate(void)
{
	gesture_blocked = contact_active;
	sweep2wake_reset();
}

static void s2w_reset_contact_state(void)
{
	touch_x_valid = false;
	touch_y_valid = false;
	contact_active = false;
	active_tracking_id_valid = false;
}

static void s2w_reset_contact_packet(void)
{
	touch_x_called = false;
	touch_y_called = false;
	contact_called = false;
	touch_major_called = false;
	tracking_id_called = false;
}

static void s2w_record_cached_contact(void)
{
	report_contacts++;
	if (report_contacts == 1 && touch_x_valid && touch_y_valid) {
		report_x = touch_x;
		report_y = touch_y;
		report_contact_valid = true;
	}
}

static void s2w_record_contact(bool has_tracking_id)
{
	if (has_tracking_id && tracking_id_called) {
		if (!active_tracking_id_valid ||
		    tracking_id != active_tracking_id) {
			touch_x_valid = false;
			touch_y_valid = false;
		}
		active_tracking_id = tracking_id;
		active_tracking_id_valid = true;
	}

	if (touch_x_called) {
		touch_x = packet_x;
		touch_x_valid = true;
	}
	if (touch_y_called) {
		touch_y = packet_y;
		touch_y_valid = true;
	}

	s2w_record_cached_contact();
}

/* Sweep2wake main function */
static void detect_sweep2wake(int x, int y)
{
	int displacement;
	int minimum_displacement;

#if S2W_DEBUG
	pr_info(LOGTAG"x,y(%4d,%4d)\n", x, y);
#endif
	if (x < x_min || x > x_max || y < y_min || y > y_max) {
		pr_info(LOGTAG "diag id=%u reject: out-of-bounds x=%d y=%d "
			"x-range=%d..%d y-range=%d..%d\n",
			diagnostic_gesture_id, x, y, x_min, x_max, y_min, y_max);
		sweep2wake_invalidate();
		return;
	}

	if (!gesture_start_valid) {
		gesture_start_x = x;
		gesture_start_valid = true;
	}

	displacement = scr_suspended ? x - gesture_start_x :
		gesture_start_x - x;
	if (displacement > gesture_max_progress)
		gesture_max_progress = displacement;
	minimum_displacement = (x_max - x_min) *
		S2W_MIN_DISPLACEMENT_PERCENT / 100;

	pr_info(LOGTAG "diag id=%u progress: start=%d x=%d y=%d "
		"current=%d max=%d required=%d suspended=%d enabled=%d "
		"s2sonly=%d blocked=%d exec=%d\n",
		diagnostic_gesture_id, gesture_start_x, x, y, displacement,
		gesture_max_progress, minimum_displacement, scr_suspended,
		s2w_switch, s2w_s2sonly, gesture_blocked, exec_count);

	//left->right
	if (scr_suspended && s2w_switch > 0 && !s2w_s2sonly) {
		if (gesture_max_progress >= minimum_displacement && exec_count) {
			pr_info(LOGTAG"wake gesture: emitting KEY_POWER\n");
			diagnostic_power_queued = true;
			sweep2wake_pwrtrigger();
			exec_count = false;
		}
	//right->left
	} else if (!scr_suspended && s2w_switch > 0) {
		if (y <= y_limit) {
			pr_info(LOGTAG "diag id=%u reject: sleep-gesture y=%d "
				"y-limit=%d\n",
				diagnostic_gesture_id, y, y_limit);
			sweep2wake_invalidate();
			return;
		}
		if (gesture_max_progress >= minimum_displacement && exec_count) {
			pr_info(LOGTAG"sleep gesture: emitting KEY_POWER\n");
			diagnostic_power_queued = true;
			sweep2wake_pwrtrigger();
			exec_count = false;
		}
	} else {
		pr_info(LOGTAG "diag id=%u reject: unsupported-state "
			"suspended=%d enabled=%d s2sonly=%d\n",
			diagnostic_gesture_id, scr_suspended, s2w_switch,
			s2w_s2sonly);
		sweep2wake_invalidate();
	}
}

static void s2w_input_event(struct input_handle *handle, unsigned int type,
				unsigned int code, int value) {
	bool has_tracking_id;
	bool packet_is_contact;

#if S2W_DEBUG
	pr_info("sweep2wake: code: %s|%u, val: %i\n",
		((code==ABS_MT_POSITION_X) ? "X" :
		(code==ABS_MT_POSITION_Y) ? "Y" :
		(code==ABS_MT_TRACKING_ID) ? "ID" :
		"undef"), code, value);
#endif
	if (type == EV_ABS) {
		contact_called = true;
		if (code == ABS_MT_POSITION_X) {
			packet_x = value;
			touch_x_called = true;
		} else if (code == ABS_MT_POSITION_Y) {
			packet_y = value;
			touch_y_called = true;
		} else if (code == ABS_MT_TOUCH_MAJOR) {
			touch_major = value;
			touch_major_called = true;
		} else if (code == ABS_MT_TRACKING_ID) {
			tracking_id = value;
			tracking_id_called = true;
		}
		return;
	}

	if (type != EV_SYN)
		return;

	if (code == SYN_MT_REPORT) {
		has_tracking_id = test_bit(ABS_MT_TRACKING_ID,
					   handle->dev->absbit);
		packet_is_contact = contact_called;
		if (touch_major_called && !touch_major)
			packet_is_contact = false;
		if (has_tracking_id && tracking_id_called && tracking_id < 0)
			packet_is_contact = false;

		if (packet_is_contact) {
			if (empty_contact_report) {
				s2w_record_cached_contact();
				empty_contact_report = false;
			}
			s2w_record_contact(has_tracking_id);
		} else if (contact_active) {
			empty_contact_report = true;
		}
		s2w_reset_contact_packet();
	} else if (code == SYN_REPORT) {
		if (report_contacts == 1 && !contact_active) {
			diagnostic_gesture_id++;
			diagnostic_gesture_active = true;
			diagnostic_power_queued = false;
			pr_info(LOGTAG "diag id=%u begin: valid=%d blocked=%d "
				"suspended=%d x=%d y=%d\n",
				diagnostic_gesture_id, report_contact_valid,
				gesture_blocked, scr_suspended, report_x, report_y);
		}

		if (report_contacts == 1) {
			contact_active = true;
			if (report_contact_valid && !gesture_blocked) {
				detect_sweep2wake(report_x, report_y);
			} else {
				pr_info(LOGTAG "diag id=%u skip: valid=%d blocked=%d "
					"suspended=%d x=%d y=%d\n",
					diagnostic_gesture_id, report_contact_valid,
					gesture_blocked, scr_suspended, report_x,
					report_y);
			}
		} else if (report_contacts > 1) {
			contact_active = true;
			pr_info(LOGTAG "diag id=%u reject: contacts=%u\n",
				diagnostic_gesture_id, report_contacts);
			sweep2wake_invalidate();
		} else {
			if (diagnostic_gesture_active) {
				pr_info(LOGTAG "diag id=%u end: start-valid=%d "
					"max-progress=%d blocked=%d suspended=%d "
					"power-queued=%d\n",
					diagnostic_gesture_id, gesture_start_valid,
					gesture_max_progress, gesture_blocked,
					scr_suspended, diagnostic_power_queued);
			}
			sweep2wake_reset();
			gesture_blocked = false;
			s2w_reset_contact_state();
			diagnostic_gesture_active = false;
		}

		report_contacts = 0;
		report_contact_valid = false;
		empty_contact_report = false;
		s2w_reset_contact_packet();
	}
}

static int input_dev_filter(struct input_dev *dev) {
	return !dev->name || strcmp(dev->name, "qtouch-touchscreen");
}

static int s2w_input_connect(struct input_handler *handler,
				struct input_dev *dev, const struct input_device_id *id) {
	struct input_handle *handle;
	int error;

	if (input_dev_filter(dev))
		return -ENODEV;

	x_min = dev->absmin[ABS_MT_POSITION_X];
	x_max = dev->absmax[ABS_MT_POSITION_X];
	y_min = dev->absmin[ABS_MT_POSITION_Y];
	y_max = dev->absmax[ABS_MT_POSITION_Y];
	if (x_max <= x_min || y_max <= y_min)
		return -ENODEV;

	y_limit = y_max - ((y_max - y_min) * 13 / 100);
	sweep2wake_reset();
	gesture_blocked = false;
	s2w_reset_contact_state();
	s2w_reset_contact_packet();
	report_contacts = 0;
	report_contact_valid = false;
	empty_contact_report = false;

	handle = kzalloc(sizeof(struct input_handle), GFP_KERNEL);
	if (!handle)
		return -ENOMEM;

	handle->dev = dev;
	handle->handler = handler;
	handle->name = "s2w";

	error = input_register_handle(handle);
	if (error)
		goto err2;

	error = input_open_device(handle);
	if (error)
		goto err1;

	pr_info(LOGTAG"attached to %s\n", dev->name);
	return 0;
err1:
	input_unregister_handle(handle);
err2:
	kfree(handle);
	return error;
}

static void s2w_input_disconnect(struct input_handle *handle) {
	sweep2wake_invalidate();
	s2w_reset_contact_state();
	s2w_reset_contact_packet();
	report_contacts = 0;
	report_contact_valid = false;
	empty_contact_report = false;
	input_close_device(handle);
	input_unregister_handle(handle);
	kfree(handle);
}

static const struct input_device_id s2w_ids[] = {
	{ .driver_info = 1 },
	{ },
};

static struct input_handler s2w_input_handler = {
	.event		= s2w_input_event,
	.connect	= s2w_input_connect,
	.disconnect	= s2w_input_disconnect,
	.name		= "s2w_inputreq",
	.id_table	= s2w_ids,
};

#ifndef CONFIG_HAS_EARLYSUSPEND
static int lcd_notifier_callback(struct notifier_block *this,
				unsigned long event, void *data)
{
	switch (event) {
	case LCD_EVENT_ON_END:
		scr_suspended = false;
		sweep2wake_invalidate();
		break;
	case LCD_EVENT_OFF_END:
		scr_suspended = true;
		sweep2wake_invalidate();
		break;
	default:
		break;
	}

	return 0;
}
#else
static void s2w_early_suspend(struct early_suspend *h) {
	scr_suspended = true;
	sweep2wake_invalidate();
}

static void s2w_late_resume(struct early_suspend *h) {
	scr_suspended = false;
	
	/* The execution stopped, it should be save to reset the hole thing */
	sweep2wake_invalidate();
}

static struct early_suspend s2w_early_suspend_handler = {
	.level = EARLY_SUSPEND_LEVEL_BLANK_SCREEN,
	.suspend = s2w_early_suspend,
	.resume = s2w_late_resume,
};
#endif

/*
 * SYSFS stuff below here
 */
static ssize_t s2w_sweep2wake_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	size_t count = 0;

	count += sprintf(buf, "%d\n", s2w_switch);

	return count;
}

static ssize_t s2w_sweep2wake_dump(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	if (buf[0] >= '0' && buf[0] <= '1' && buf[1] == '\n' &&
	    s2w_switch != buf[0] - '0') {
		s2w_switch = buf[0] - '0';
		sweep2wake_invalidate();
	}

	return count;
}

static DEVICE_ATTR(sweep2wake, (S_IWUSR|S_IRUGO),
	s2w_sweep2wake_show, s2w_sweep2wake_dump);

static ssize_t s2w_s2w_s2sonly_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	size_t count = 0;

	count += sprintf(buf, "%d\n", s2w_s2sonly);

	return count;
}

static ssize_t s2w_s2w_s2sonly_dump(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	if (buf[0] >= '0' && buf[0] <= '1' && buf[1] == '\n' &&
	    s2w_s2sonly != buf[0] - '0') {
		s2w_s2sonly = buf[0] - '0';
		sweep2wake_invalidate();
	}

	return count;
}

static DEVICE_ATTR(s2w_s2sonly, (S_IWUSR|S_IRUGO),
	s2w_s2w_s2sonly_show, s2w_s2w_s2sonly_dump);

static ssize_t s2w_version_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	size_t count = 0;

	count += sprintf(buf, "%s\n", DRIVER_VERSION);

	return count;
}

static ssize_t s2w_version_dump(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	return count;
}

static DEVICE_ATTR(sweep2wake_version, (S_IWUSR|S_IRUGO),
	s2w_version_show, s2w_version_dump);

/*
 * INIT / EXIT stuff below here
 */
#ifdef ANDROID_TOUCH_DECLARED
extern struct kobject *android_touch_kobj;
#else
struct kobject *android_touch_kobj;
EXPORT_SYMBOL_GPL(android_touch_kobj);
#endif
static int __init sweep2wake_init(void)
{
	int rc = 0;

	sweep2wake_pwrdev = input_allocate_device();
	if (!sweep2wake_pwrdev) {
		pr_err("Can't allocate suspend autotest power button\n");
		goto err_alloc_dev;
	}

	input_set_capability(sweep2wake_pwrdev, EV_KEY, KEY_POWER);
	sweep2wake_pwrdev->name = "s2w_pwrkey";
	sweep2wake_pwrdev->phys = "s2w_pwrkey/input0";

	rc = input_register_device(sweep2wake_pwrdev);
	if (rc) {
		pr_err("%s: input_register_device err=%d\n", __func__, rc);
		goto err_input_dev;
	}

	rc = input_register_handler(&s2w_input_handler);
	if (rc)
		pr_err("%s: Failed to register s2w_input_handler\n", __func__);

#ifndef CONFIG_HAS_EARLYSUSPEND
	s2w_lcd_notif.notifier_call = lcd_notifier_callback;
	if (lcd_register_client(&s2w_lcd_notif) != 0) {
		pr_err("%s: Failed to register lcd callback\n", __func__);
	}
#else
	register_early_suspend(&s2w_early_suspend_handler);
#endif

#ifndef ANDROID_TOUCH_DECLARED
	android_touch_kobj = kobject_create_and_add("android_touch", NULL) ;
	if (android_touch_kobj == NULL) {
		printk("%s: android_touch_kobj create_and_add failed\n", __func__);
	}
#endif
	rc = sysfs_create_file(android_touch_kobj, &dev_attr_sweep2wake.attr);
	if (rc) {
		printk("%s: sysfs_create_file failed for sweep2wake\n", __func__);
	}
	rc = sysfs_create_file(android_touch_kobj, &dev_attr_s2w_s2sonly.attr);
	if (rc) {
		printk("%s: sysfs_create_file failed for s2w_s2sonly\n", __func__);
	}
	rc = sysfs_create_file(android_touch_kobj, &dev_attr_sweep2wake_version.attr);
	if (rc) {
		printk("%s: sysfs_create_file failed for sweep2wake_version\n", __func__);
	}
	pr_info(LOGTAG"%s done\n", __func__);
	return 0;

err_input_dev:
	input_free_device(sweep2wake_pwrdev);
err_alloc_dev:
	return rc ? rc : -ENOMEM;
}

static void __exit sweep2wake_exit(void)
{
#ifndef ANDROID_TOUCH_DECLARED
	kobject_del(android_touch_kobj);
#endif
#ifndef CONFIG_HAS_EARLYSUSPEND
	lcd_unregister_client(&s2w_lcd_notif);
#endif
	input_unregister_handler(&s2w_input_handler);
	cancel_work_sync(&sweep2wake_presspwr_work);
	input_unregister_device(sweep2wake_pwrdev);
	return;
}

module_init(sweep2wake_init);
module_exit(sweep2wake_exit);
