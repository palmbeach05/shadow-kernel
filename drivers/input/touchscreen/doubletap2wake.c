/* Doubletap2wake gesture recognition and controls. */
#include <linux/device.h>
#include <linux/init.h>
#include <linux/input/doubletap2wake.h>
#include <linux/input/touchwake.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/kobject.h>
#include <linux/module.h>
#include <linux/sysfs.h>

#define LOGTAG "[doubletap2wake]: "

#define DT2W_DEFAULT 1
#define DT2W_MAX_TAP_DURATION_MS 250
#define DT2W_MIN_INTERVAL_MS 50
#define DT2W_MAX_INTERVAL_MS 500
#define DT2W_MAX_MOVEMENT_PERCENT 5
#define DT2W_MAX_PAIR_DISTANCE_PERCENT 15

enum dt2w_state {
	DT2W_IDLE,
	DT2W_FIRST_TAP_DOWN,
	DT2W_WAIT_SECOND_TAP,
	DT2W_SECOND_TAP_DOWN,
	DT2W_BLOCKED,
};

extern struct kobject *android_touch_kobj;

int dt2w_switch = DT2W_DEFAULT;

static enum dt2w_state dt2w_state = DT2W_IDLE;
static bool display_suspended;
static int tap_start_x, tap_start_y;
static int tap_last_x, tap_last_y;
static int first_tap_x, first_tap_y;
static int touch_x_min, touch_x_max, touch_y_min, touch_y_max;
static unsigned long tap_press_time, first_tap_release_time;

static void dt2w_reset(void)
{
	dt2w_state = DT2W_IDLE;
}

static void dt2w_block(const char *reason)
{
	if (dt2w_state == DT2W_BLOCKED)
		return;
	pr_info(LOGTAG "%s\n", reason);
	dt2w_state = DT2W_BLOCKED;
}

static bool dt2w_movement_too_large(int x, int y)
{
	int x_percent = abs(x - tap_start_x) * 100 /
		(touch_x_max - touch_x_min);
	int y_percent = abs(y - tap_start_y) * 100 /
		(touch_y_max - touch_y_min);

	return x_percent * x_percent + y_percent * y_percent >
		DT2W_MAX_MOVEMENT_PERCENT * DT2W_MAX_MOVEMENT_PERCENT;
}

static bool dt2w_pair_too_distant(int x, int y)
{
	int x_percent = abs(x - first_tap_x) * 100 /
		(touch_x_max - touch_x_min);
	int y_percent = abs(y - first_tap_y) * 100 /
		(touch_y_max - touch_y_min);

	return x_percent * x_percent + y_percent * y_percent >
		DT2W_MAX_PAIR_DISTANCE_PERCENT *
		DT2W_MAX_PAIR_DISTANCE_PERCENT;
}

static bool dt2w_tap_too_long(unsigned long now)
{
	return time_after(now, tap_press_time +
		msecs_to_jiffies(DT2W_MAX_TAP_DURATION_MS));
}

static void dt2w_start_tap(int x, int y, enum dt2w_state state)
{
	tap_start_x = x;
	tap_start_y = y;
	tap_last_x = x;
	tap_last_y = y;
	tap_press_time = jiffies;
	dt2w_state = state;
}

static void dt2w_position(int x, int y, int x_min, int x_max,
		int y_min, int y_max)
{
	unsigned long interval;

	touch_x_min = x_min;
	touch_x_max = x_max;
	touch_y_min = y_min;
	touch_y_max = y_max;

	if (!dt2w_switch) {
		if (dt2w_state != DT2W_BLOCKED)
			dt2w_block("disabled");
		return;
	}
	if (!display_suspended) {
		if (dt2w_state != DT2W_BLOCKED)
			dt2w_block("display active");
		return;
	}

	switch (dt2w_state) {
	case DT2W_IDLE:
		dt2w_start_tap(x, y, DT2W_FIRST_TAP_DOWN);
		break;
	case DT2W_FIRST_TAP_DOWN:
	case DT2W_SECOND_TAP_DOWN:
		tap_last_x = x;
		tap_last_y = y;
		if (dt2w_movement_too_large(x, y))
			dt2w_block("tap movement too large");
		else if (dt2w_state == DT2W_SECOND_TAP_DOWN &&
			 dt2w_pair_too_distant(x, y))
			dt2w_block("tap-pair distance too large");
		break;
	case DT2W_WAIT_SECOND_TAP:
		interval = jiffies - first_tap_release_time;
		if (interval < msecs_to_jiffies(DT2W_MIN_INTERVAL_MS)) {
			dt2w_block("interval too short");
		} else if (interval >
			   msecs_to_jiffies(DT2W_MAX_INTERVAL_MS)) {
			dt2w_block("interval timeout");
		} else if (dt2w_pair_too_distant(x, y)) {
			dt2w_block("tap-pair distance too large");
		} else {
			dt2w_start_tap(x, y, DT2W_SECOND_TAP_DOWN);
		}
		break;
	case DT2W_BLOCKED:
		break;
	}
}

static void dt2w_release(void)
{
	unsigned long now = jiffies;

	switch (dt2w_state) {
	case DT2W_FIRST_TAP_DOWN:
		if (dt2w_tap_too_long(now)) {
			pr_info(LOGTAG "tap duration too long\n");
			dt2w_reset();
			break;
		}
		first_tap_x = tap_last_x;
		first_tap_y = tap_last_y;
		first_tap_release_time = now;
		dt2w_state = DT2W_WAIT_SECOND_TAP;
		pr_info(LOGTAG "first tap accepted\n");
		break;
	case DT2W_SECOND_TAP_DOWN:
		if (dt2w_tap_too_long(now)) {
			pr_info(LOGTAG "tap duration too long\n");
		} else if (dt2w_movement_too_large(tap_last_x, tap_last_y)) {
			pr_info(LOGTAG "tap movement too large\n");
		} else if (dt2w_pair_too_distant(tap_last_x, tap_last_y)) {
			pr_info(LOGTAG "tap-pair distance too large\n");
		} else {
			pr_info(LOGTAG "second tap accepted\n");
			touchwake_queue_power_key();
			pr_info(LOGTAG "power key queued\n");
		}
		dt2w_reset();
		break;
	case DT2W_BLOCKED:
		dt2w_reset();
		break;
	case DT2W_IDLE:
	case DT2W_WAIT_SECOND_TAP:
		break;
	}
}

static void dt2w_multitouch(void)
{
	dt2w_block("multi-touch");
}

static void dt2w_invalid(void)
{
	dt2w_block("invalid contact data");
}

static void dt2w_display(bool suspended)
{
	display_suspended = suspended;
	dt2w_reset();
	if (!suspended)
		pr_info(LOGTAG "display active\n");
}

static void dt2w_disconnect(void)
{
	dt2w_reset();
}

static struct touchwake_client dt2w_client = {
	.position = dt2w_position,
	.release = dt2w_release,
	.multitouch = dt2w_multitouch,
	.invalid = dt2w_invalid,
	.display = dt2w_display,
	.disconnect = dt2w_disconnect,
};

static ssize_t dt2w_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", dt2w_switch);
}

static ssize_t dt2w_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	unsigned long setting;

	if (strict_strtoul(buf, 10, &setting) || setting > 1)
		return -EINVAL;
	if (dt2w_switch != setting) {
		dt2w_switch = setting;
		dt2w_reset();
		if (!setting)
			pr_info(LOGTAG "disabled\n");
	}
	return count;
}
static DEVICE_ATTR(doubletap2wake, S_IWUSR | S_IRUGO,
		dt2w_show, dt2w_store);

static int __init doubletap2wake_init(void)
{
	int error;

	error = touchwake_register_client(&dt2w_client);
	if (error)
		return error;
	if (!android_touch_kobj) {
		error = -ENODEV;
		goto err_client;
	}
	error = sysfs_create_file(android_touch_kobj,
				  &dev_attr_doubletap2wake.attr);
	if (error)
		goto err_client;
	return 0;

err_client:
	touchwake_unregister_client(&dt2w_client);
	return error;
}

static void __exit doubletap2wake_exit(void)
{
	sysfs_remove_file(android_touch_kobj, &dev_attr_doubletap2wake.attr);
	touchwake_unregister_client(&dt2w_client);
}

module_init(doubletap2wake_init);
module_exit(doubletap2wake_exit);

MODULE_DESCRIPTION("Double tap to wake gesture");
MODULE_LICENSE("GPL v2");
