/* Sweep2wake gesture recognition and controls. */
#include <linux/device.h>
#include <linux/init.h>
#include <linux/input/sweep2wake.h>
#include <linux/input/touchwake.h>
#include <linux/kernel.h>
#include <linux/kobject.h>
#include <linux/module.h>
#include <linux/sysfs.h>

#define DRIVER_AUTHOR "Dennis Rassmann <showp1984@gmail.com>"
#define DRIVER_DESCRIPTION "Sweep2wake for almost any device"
#define DRIVER_VERSION "1.5"
#define LOGTAG "[sweep2wake]: "
#define S2W_DEBUG 0
#define S2W_DEFAULT 1
#define S2W_S2SONLY_DEFAULT 0
#define S2W_MIN_DISPLACEMENT_PERCENT 60

MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESCRIPTION);
MODULE_VERSION(DRIVER_VERSION);
MODULE_LICENSE("GPL v2");

int s2w_switch = S2W_DEFAULT, s2w_s2sonly = S2W_S2SONLY_DEFAULT;
static bool scr_suspended, contact_active, gesture_start_valid;
static bool gesture_blocked;
static bool exec_count = true;
static int gesture_start_x, gesture_max_progress;

static int __init read_s2w_cmdline(char *s2w)
{
	if (!strcmp(s2w, "1")) {
		pr_info("[cmdline_s2w]: Sweep2Wake enabled. | s2w='%s'\n", s2w);
		s2w_switch = 1;
	} else if (!strcmp(s2w, "0")) {
		pr_info("[cmdline_s2w]: Sweep2Wake disabled. | s2w='%s'\n", s2w);
		s2w_switch = 0;
	} else {
		pr_info("[cmdline_s2w]: No valid input found. Going with default: | s2w='%u'\n",
			s2w_switch);
	}
	return 1;
}
__setup("s2w=", read_s2w_cmdline);

static void sweep2wake_reset(void)
{
	exec_count = true;
	gesture_start_valid = false;
	gesture_max_progress = 0;
}

static void sweep2wake_invalidate(void)
{
	gesture_blocked = contact_active;
	sweep2wake_reset();
}

static void detect_sweep2wake(int x, int y, int x_min, int x_max,
		int y_min, int y_max)
{
	int displacement, minimum_displacement;
	int y_limit;

#if S2W_DEBUG
	pr_info(LOGTAG "x,y(%4d,%4d)\n", x, y);
#endif
	if (x < x_min || x > x_max || y < y_min || y > y_max) {
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
	y_limit = y_max - ((y_max - y_min) * 13 / 100);

	if (scr_suspended && s2w_switch > 0 && !s2w_s2sonly) {
		if (gesture_max_progress >= minimum_displacement && exec_count) {
			touchwake_queue_power_key();
			exec_count = false;
		}
	} else if (!scr_suspended && s2w_switch > 0) {
		if (y <= y_limit) {
			sweep2wake_invalidate();
			return;
		}
		if (gesture_max_progress >= minimum_displacement && exec_count) {
			touchwake_queue_power_key();
			exec_count = false;
		}
	} else {
		sweep2wake_invalidate();
	}
}

static void sweep2wake_position(int x, int y, int x_min, int x_max,
		int y_min, int y_max)
{
	contact_active = true;
	if (!gesture_blocked)
		detect_sweep2wake(x, y, x_min, x_max, y_min, y_max);
}

static void sweep2wake_release(void)
{
	sweep2wake_reset();
	gesture_blocked = false;
	contact_active = false;
}

static void sweep2wake_reject(void)
{
	contact_active = true;
	sweep2wake_invalidate();
}

static void sweep2wake_display(bool suspended)
{
	scr_suspended = suspended;
	sweep2wake_invalidate();
}

static void sweep2wake_disconnect(void)
{
	sweep2wake_invalidate();
	gesture_blocked = false;
	contact_active = false;
}

static struct touchwake_client sweep2wake_client = {
	.position = sweep2wake_position,
	.release = sweep2wake_release,
	.multitouch = sweep2wake_reject,
	.invalid = sweep2wake_reject,
	.display = sweep2wake_display,
	.disconnect = sweep2wake_disconnect,
};

static ssize_t s2w_switch_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", s2w_switch);
}

static ssize_t s2w_switch_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	if (buf[0] >= '0' && buf[0] <= '1' && buf[1] == '\n' &&
	    s2w_switch != buf[0] - '0') {
		s2w_switch = buf[0] - '0';
		sweep2wake_invalidate();
	}
	return count;
}
static DEVICE_ATTR(sweep2wake, S_IWUSR | S_IRUGO,
		s2w_switch_show, s2w_switch_store);

static ssize_t s2w_s2sonly_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", s2w_s2sonly);
}

static ssize_t s2w_s2sonly_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	if (buf[0] >= '0' && buf[0] <= '1' && buf[1] == '\n' &&
	    s2w_s2sonly != buf[0] - '0') {
		s2w_s2sonly = buf[0] - '0';
		sweep2wake_invalidate();
	}
	return count;
}
static DEVICE_ATTR(s2w_s2sonly, S_IWUSR | S_IRUGO,
		s2w_s2sonly_show, s2w_s2sonly_store);

static ssize_t s2w_version_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%s\n", DRIVER_VERSION);
}
static ssize_t s2w_version_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	return count;
}
static DEVICE_ATTR(sweep2wake_version, S_IWUSR | S_IRUGO,
		s2w_version_show, s2w_version_store);

#ifdef ANDROID_TOUCH_DECLARED
extern struct kobject *android_touch_kobj;
#else
struct kobject *android_touch_kobj;
EXPORT_SYMBOL_GPL(android_touch_kobj);
#endif

static int __init sweep2wake_init(void)
{
	int error;

	error = touchwake_register_client(&sweep2wake_client);
	if (error)
		return error;
#ifndef ANDROID_TOUCH_DECLARED
	android_touch_kobj = kobject_create_and_add("android_touch", NULL);
	if (!android_touch_kobj) {
		error = -ENOMEM;
		goto err_client;
	}
#endif
	error = sysfs_create_file(android_touch_kobj, &dev_attr_sweep2wake.attr);
	if (error)
		goto err_kobject;
	error = sysfs_create_file(android_touch_kobj, &dev_attr_s2w_s2sonly.attr);
	if (error)
		goto err_sweep2wake;
	error = sysfs_create_file(android_touch_kobj,
				  &dev_attr_sweep2wake_version.attr);
	if (error)
		goto err_s2sonly;
	pr_info(LOGTAG "%s done\n", __func__);
	return 0;

err_s2sonly:
	sysfs_remove_file(android_touch_kobj, &dev_attr_s2w_s2sonly.attr);
err_sweep2wake:
	sysfs_remove_file(android_touch_kobj, &dev_attr_sweep2wake.attr);
err_kobject:
#ifndef ANDROID_TOUCH_DECLARED
	kobject_put(android_touch_kobj);
#endif
err_client:
	touchwake_unregister_client(&sweep2wake_client);
	return error;
}

static void __exit sweep2wake_exit(void)
{
	sysfs_remove_file(android_touch_kobj, &dev_attr_sweep2wake_version.attr);
	sysfs_remove_file(android_touch_kobj, &dev_attr_s2w_s2sonly.attr);
	sysfs_remove_file(android_touch_kobj, &dev_attr_sweep2wake.attr);
#ifndef ANDROID_TOUCH_DECLARED
	kobject_put(android_touch_kobj);
#endif
	touchwake_unregister_client(&sweep2wake_client);
}

module_init(sweep2wake_init);
module_exit(sweep2wake_exit);
