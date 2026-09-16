/*
 * Shared touchscreen gesture infrastructure.
 */
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/input.h>
#include <linux/input/touchwake.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#ifndef CONFIG_HAS_EARLYSUSPEND
#include <linux/lcd_notify.h>
#else
#include <linux/earlysuspend.h>
#endif

#define TOUCHWAKE_PWRKEY_DUR 60

static LIST_HEAD(touchwake_clients);
static DEFINE_RWLOCK(touchwake_clients_lock);
static DEFINE_MUTEX(touchwake_power_lock);
static struct input_dev *touchwake_power_dev;
static bool display_suspended;
static int x_min, x_max, y_min, y_max;

static int touch_x, touch_y, packet_x, packet_y;
static bool touch_x_valid, touch_y_valid;
static bool touch_x_called, touch_y_called;
static bool contact_called, contact_active, empty_contact_report;
static int touch_major;
static bool touch_major_called;
static int tracking_id, active_tracking_id;
static bool tracking_id_called, active_tracking_id_valid;
static unsigned int report_contacts;
static int report_x, report_y;
static bool report_contact_valid;

#ifndef CONFIG_HAS_EARLYSUSPEND
static struct notifier_block touchwake_lcd_notif;
#endif

static void touchwake_notify_position(int x, int y)
{
	struct touchwake_client *client;
	unsigned long flags;

	read_lock_irqsave(&touchwake_clients_lock, flags);
	list_for_each_entry(client, &touchwake_clients, node)
		if (client->position)
			client->position(x, y, x_min, x_max, y_min, y_max);
	read_unlock_irqrestore(&touchwake_clients_lock, flags);
}

static void touchwake_notify_release(void)
{
	struct touchwake_client *client;
	unsigned long flags;

	read_lock_irqsave(&touchwake_clients_lock, flags);
	list_for_each_entry(client, &touchwake_clients, node)
		if (client->release)
			client->release();
	read_unlock_irqrestore(&touchwake_clients_lock, flags);
}

static void touchwake_notify_multitouch(void)
{
	struct touchwake_client *client;
	unsigned long flags;

	read_lock_irqsave(&touchwake_clients_lock, flags);
	list_for_each_entry(client, &touchwake_clients, node)
		if (client->multitouch)
			client->multitouch();
	read_unlock_irqrestore(&touchwake_clients_lock, flags);
}

static void touchwake_notify_invalid(void)
{
	struct touchwake_client *client;
	unsigned long flags;

	read_lock_irqsave(&touchwake_clients_lock, flags);
	list_for_each_entry(client, &touchwake_clients, node)
		if (client->invalid)
			client->invalid();
	read_unlock_irqrestore(&touchwake_clients_lock, flags);
}

static void touchwake_notify_display(void)
{
	struct touchwake_client *client;
	unsigned long flags;

	read_lock_irqsave(&touchwake_clients_lock, flags);
	list_for_each_entry(client, &touchwake_clients, node)
		if (client->display)
			client->display(display_suspended);
	read_unlock_irqrestore(&touchwake_clients_lock, flags);
}

static void touchwake_notify_disconnect(void)
{
	struct touchwake_client *client;
	unsigned long flags;

	read_lock_irqsave(&touchwake_clients_lock, flags);
	list_for_each_entry(client, &touchwake_clients, node)
		if (client->disconnect)
			client->disconnect();
	read_unlock_irqrestore(&touchwake_clients_lock, flags);
}

int touchwake_register_client(struct touchwake_client *client)
{
	unsigned long flags;

	if (!client)
		return -EINVAL;

	write_lock_irqsave(&touchwake_clients_lock, flags);
	INIT_LIST_HEAD(&client->node);
	list_add_tail(&client->node, &touchwake_clients);
	write_unlock_irqrestore(&touchwake_clients_lock, flags);
	if (client->display)
		client->display(display_suspended);
	return 0;
}
EXPORT_SYMBOL_GPL(touchwake_register_client);

void touchwake_unregister_client(struct touchwake_client *client)
{
	unsigned long flags;

	if (!client)
		return;

	write_lock_irqsave(&touchwake_clients_lock, flags);
	if (!list_empty(&client->node))
		list_del_init(&client->node);
	write_unlock_irqrestore(&touchwake_clients_lock, flags);
}
EXPORT_SYMBOL_GPL(touchwake_unregister_client);

static void touchwake_press_power(struct work_struct *work)
{
	if (!mutex_trylock(&touchwake_power_lock))
		return;
	input_event(touchwake_power_dev, EV_KEY, KEY_POWER, 1);
	input_sync(touchwake_power_dev);
	msleep(TOUCHWAKE_PWRKEY_DUR);
	input_event(touchwake_power_dev, EV_KEY, KEY_POWER, 0);
	input_sync(touchwake_power_dev);
	msleep(TOUCHWAKE_PWRKEY_DUR);
	mutex_unlock(&touchwake_power_lock);
}
static DECLARE_WORK(touchwake_power_work, touchwake_press_power);

void touchwake_queue_power_key(void)
{
	schedule_work(&touchwake_power_work);
}
EXPORT_SYMBOL_GPL(touchwake_queue_power_key);

static void touchwake_reset_contact_state(void)
{
	touch_x_valid = false;
	touch_y_valid = false;
	contact_active = false;
	active_tracking_id_valid = false;
}

static void touchwake_reset_packet(void)
{
	touch_x_called = false;
	touch_y_called = false;
	contact_called = false;
	touch_major_called = false;
	tracking_id_called = false;
}

static void touchwake_reset_frame(void)
{
	report_contacts = 0;
	report_contact_valid = false;
	empty_contact_report = false;
	touchwake_reset_packet();
}

static void touchwake_record_cached_contact(void)
{
	report_contacts++;
	if (report_contacts == 1 && touch_x_valid && touch_y_valid) {
		report_x = touch_x;
		report_y = touch_y;
		report_contact_valid = true;
	}
}

static void touchwake_record_contact(bool has_tracking_id)
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
	touchwake_record_cached_contact();
}

static void touchwake_input_event(struct input_handle *handle,
		unsigned int type, unsigned int code, int value)
{
	bool has_tracking_id;
	bool packet_is_contact;

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
				touchwake_record_cached_contact();
				empty_contact_report = false;
			}
			touchwake_record_contact(has_tracking_id);
		} else if (contact_active) {
			empty_contact_report = true;
		}
		touchwake_reset_packet();
	} else if (code == SYN_REPORT) {
		if (report_contacts == 1) {
			contact_active = true;
			if (report_contact_valid &&
			    report_x >= x_min && report_x <= x_max &&
			    report_y >= y_min && report_y <= y_max)
				touchwake_notify_position(report_x, report_y);
			else
				touchwake_notify_invalid();
		} else if (report_contacts > 1) {
			contact_active = true;
			touchwake_notify_multitouch();
		} else {
			touchwake_notify_release();
			touchwake_reset_contact_state();
		}
		touchwake_reset_frame();
	}
}

static int touchwake_input_connect(struct input_handler *handler,
		struct input_dev *dev, const struct input_device_id *id)
{
	struct input_handle *handle;
	int error;

	if (!dev->name || strcmp(dev->name, "qtouch-touchscreen"))
		return -ENODEV;
	x_min = dev->absmin[ABS_MT_POSITION_X];
	x_max = dev->absmax[ABS_MT_POSITION_X];
	y_min = dev->absmin[ABS_MT_POSITION_Y];
	y_max = dev->absmax[ABS_MT_POSITION_Y];
	if (x_max <= x_min || y_max <= y_min) {
		touchwake_notify_invalid();
		return -ENODEV;
	}

	touchwake_reset_contact_state();
	touchwake_reset_frame();
	handle = kzalloc(sizeof(*handle), GFP_KERNEL);
	if (!handle)
		return -ENOMEM;
	handle->dev = dev;
	handle->handler = handler;
	handle->name = "touchwake";
	error = input_register_handle(handle);
	if (error)
		goto err_free;
	error = input_open_device(handle);
	if (error)
		goto err_unregister;
	return 0;

err_unregister:
	input_unregister_handle(handle);
err_free:
	kfree(handle);
	return error;
}

static void touchwake_input_disconnect(struct input_handle *handle)
{
	touchwake_notify_disconnect();
	touchwake_reset_contact_state();
	touchwake_reset_frame();
	input_close_device(handle);
	input_unregister_handle(handle);
	kfree(handle);
}

static const struct input_device_id touchwake_ids[] = {
	{ .driver_info = 1 },
	{ },
};

static struct input_handler touchwake_input_handler = {
	.event = touchwake_input_event,
	.connect = touchwake_input_connect,
	.disconnect = touchwake_input_disconnect,
	.name = "touchwake_input",
	.id_table = touchwake_ids,
};

#ifndef CONFIG_HAS_EARLYSUSPEND
static int touchwake_lcd_callback(struct notifier_block *this,
		unsigned long event, void *data)
{
	if (event == LCD_EVENT_ON_END)
		display_suspended = false;
	else if (event == LCD_EVENT_OFF_END)
		display_suspended = true;
	else
		return 0;
	touchwake_notify_display();
	return 0;
}
#else
static void touchwake_early_suspend(struct early_suspend *h)
{
	display_suspended = true;
	touchwake_notify_display();
}

static void touchwake_late_resume(struct early_suspend *h)
{
	display_suspended = false;
	touchwake_notify_display();
}

static struct early_suspend touchwake_early_suspend_handler = {
	.level = EARLY_SUSPEND_LEVEL_BLANK_SCREEN,
	.suspend = touchwake_early_suspend,
	.resume = touchwake_late_resume,
};
#endif

static int __init touchwake_init(void)
{
	int error;

	touchwake_power_dev = input_allocate_device();
	if (!touchwake_power_dev)
		return -ENOMEM;
	input_set_capability(touchwake_power_dev, EV_KEY, KEY_POWER);
	touchwake_power_dev->name = "s2w_pwrkey";
	touchwake_power_dev->phys = "s2w_pwrkey/input0";
	error = input_register_device(touchwake_power_dev);
	if (error)
		goto err_free_power;
	error = input_register_handler(&touchwake_input_handler);
	if (error)
		goto err_unregister_power;
#ifndef CONFIG_HAS_EARLYSUSPEND
	touchwake_lcd_notif.notifier_call = touchwake_lcd_callback;
	error = lcd_register_client(&touchwake_lcd_notif);
	if (error)
		goto err_unregister_handler;
#else
	register_early_suspend(&touchwake_early_suspend_handler);
#endif
	return 0;

#ifndef CONFIG_HAS_EARLYSUSPEND
err_unregister_handler:
	input_unregister_handler(&touchwake_input_handler);
#endif
err_unregister_power:
	input_unregister_device(touchwake_power_dev);
	return error;
err_free_power:
	input_free_device(touchwake_power_dev);
	return error;
}

static void __exit touchwake_exit(void)
{
#ifndef CONFIG_HAS_EARLYSUSPEND
	lcd_unregister_client(&touchwake_lcd_notif);
#else
	unregister_early_suspend(&touchwake_early_suspend_handler);
#endif
	input_unregister_handler(&touchwake_input_handler);
	cancel_work_sync(&touchwake_power_work);
	input_unregister_device(touchwake_power_dev);
}

module_init(touchwake_init);
module_exit(touchwake_exit);

MODULE_DESCRIPTION("Shared touchscreen wake gesture infrastructure");
MODULE_LICENSE("GPL v2");
