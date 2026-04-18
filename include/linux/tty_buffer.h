/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_TTY_BUFFER_H
#define _LINUX_TTY_BUFFER_H

#include <linux/atomic.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/workqueue.h>

struct tty_buffer {
	struct tty_buffer *next;
	char *char_buf_ptr;
	unsigned char *flag_buf_ptr;
	int used;
	int size;
	int commit;
	int read;
	/* Data points here */
	unsigned long data[0];
};

struct tty_bufhead {
	struct delayed_work work;
	spinlock_t lock;
	struct tty_buffer *head;	/* Queue head */
	struct tty_buffer *tail;	/* Active buffer */
	struct tty_buffer *free;	/* Free queue head */
	int memory_used;		/* Buffer space used excluding
								free queue */
};
/*
 * When a break, frame error, or parity error happens, these codes are
 * stuffed into the flags buffer.
 */
#define TTY_NORMAL	0
#define TTY_BREAK	1
#define TTY_FRAME	2
#define TTY_PARITY	3
#define TTY_OVERRUN	4

#endif
