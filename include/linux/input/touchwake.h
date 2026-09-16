/*
 * Shared touchscreen gesture infrastructure.
 */
#ifndef _LINUX_INPUT_TOUCHWAKE_H
#define _LINUX_INPUT_TOUCHWAKE_H

#include <linux/list.h>
#include <linux/types.h>

struct touchwake_client {
	struct list_head node;
	void (*position)(int x, int y, int x_min, int x_max,
			 int y_min, int y_max);
	void (*release)(void);
	void (*multitouch)(void);
	void (*invalid)(void);
	void (*display)(bool suspended);
	void (*disconnect)(void);
};

int touchwake_register_client(struct touchwake_client *client);
void touchwake_unregister_client(struct touchwake_client *client);
void touchwake_queue_power_key(void);

#endif /* _LINUX_INPUT_TOUCHWAKE_H */
