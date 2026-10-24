/*
 *  Shared Transport Line discipline driver Core
 *	Init Manager module responsible for GPIO control
 *	and firmware download
 *  Copyright (C) 2009-2010 Texas Instruments
 *  Author: Pavan Savoy <pavan_savoy@ti.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */

#define pr_fmt(fmt) "(stk) :" fmt
#include <linux/platform_device.h>
#include <linux/jiffies.h>
#include <linux/firmware.h>
#include <linux/delay.h>
#include <linux/wait.h>
#include <linux/gpio.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/sched.h>
#include <linux/sysfs.h>
#include <linux/tty.h>

#include <linux/skbuff.h>
#include <linux/ti_wilink_st.h>
#include <linux/module.h>

#define pr_warn pr_warning
#ifndef reinit_completion
#define reinit_completion(x) INIT_COMPLETION(*(x))
#endif

#define MAX_ST_DEVICES	3	/* Imagine 1 on each UART for now */
static struct platform_device *st_kim_devices[MAX_ST_DEVICES];
static DEFINE_MUTEX(kim_debugfs_open_lock);

/**********************************************************************/
/* internal functions */

/*
 * st_get_plat_device -
 *	function which returns the reference to the platform device
 *	requested by id. As of now only 1 such device exists (id=0)
 *	the context requesting for reference can get the id to be
 *	requested by a. The protocol driver which is registering or
 *	b. the tty device which is opened.
 */
static struct platform_device *st_get_plat_device(int id)
{
	return st_kim_devices[id];
}

/* All response and parser state is serialized by response_lock. */
static void kim_clear_response(struct kim_data_s *kim)
{
	unsigned long flags;

	spin_lock_irqsave(&kim->response_lock, flags);
	if (kim->response_pending) {
		kim->response_result = -ESHUTDOWN;
		complete_all(&kim->kim_rcvd);
	}
	kim->response_pending = false;
	kim->expected_len = 0;
	kim->read_version = false;
	kfree_skb(kim->rx_skb);
	kim->rx_skb = NULL;
	kim->rx_count = 0;
	kim->rx_state = ST_W4_PACKET_TYPE;
	spin_unlock_irqrestore(&kim->response_lock, flags);
}

static void kim_arm_response(struct kim_data_s *kim, const u8 *event,
		unsigned int len, u16 opcode, bool version)
{
	unsigned long flags;

	spin_lock_irqsave(&kim->response_lock, flags);
	reinit_completion(&kim->kim_rcvd);
	memcpy(kim->expected_event, event, len);
	kim->expected_len = len;
	kim->expected_opcode = opcode;
	kim->read_version = version;
	kim->response_result = -EINPROGRESS;
	kim->response_pending = true;
	spin_unlock_irqrestore(&kim->response_lock, flags);
}

static long kim_wait_response(struct kim_data_s *kim)
{
	unsigned long flags;
	long ret;

	ret = wait_for_completion_interruptible_timeout(&kim->kim_rcvd,
			msecs_to_jiffies(CMD_RESP_TIME));
	spin_lock_irqsave(&kim->response_lock, flags);
	ret = ret > 0 ? kim->response_result : (ret ? ret : -ETIMEDOUT);
	kim->response_pending = false;
	kim->expected_len = 0;
	kim->read_version = false;
	spin_unlock_irqrestore(&kim->response_lock, flags);
	kim_clear_response(kim);
	return ret;
}

/* Called with response_lock held; skb->data excludes the HCI type byte. */
static void validate_firmware_response(struct kim_data_s *kim)
{
	struct sk_buff *skb = kim->rx_skb;
	const u8 *data = skb->data;
	unsigned int i, status = 0, credits = 0;
	int result = 0;

	if (!kim->response_pending || skb->len < 2 ||
	    skb->len != data[1] + 2 || data[0] != kim->expected_event[0])
		goto out;

	switch (data[0]) {
	case 0x0e: /* Command Complete: ncmd, opcode, return parameters */
		if (skb->len < 6 ||
		    (data[3] | (data[4] << 8)) != kim->expected_opcode)
			goto out;
		status = 5;
		credits = 2;
		break;
	case 0x0f: /* Command Status: status, ncmd, opcode */
		if (skb->len != 6 ||
		    (data[4] | (data[5] << 8)) != kim->expected_opcode)
			goto out;
		status = 2;
		credits = 3;
		break;
	default:
		/* Vendor events have no generic opcode/status layout. */
		break;
	}
	/* A matching command failure terminates the transaction explicitly. */
	if (status && data[status]) {
		result = -EIO;
	} else {
		if (skb->len != kim->expected_len)
			goto out;
		for (i = 0; i < skb->len; i++) {
			/* Command credits are controller flow control, not a match. */
			if (credits && i == credits)
				continue;
			if (kim->read_version && i >= 6)
				continue;
			if (data[i] != kim->expected_event[i])
				goto out;
		}
		if (kim->read_version)
			memcpy(kim->resp_buffer, data, skb->len);
	}
	kim->response_result = result;
	kim->response_pending = false;
	complete_all(&kim->kim_rcvd);
out:
	kfree_skb(skb);
}

/*
 * BTS wait data is a complete H4 event, including packet type 0x04.
 * Validate both length declarations before inspecting any event bytes.
 */
static int kim_validate_wait(const struct bts_action *action, u16 opcode,
		const u8 **event, unsigned int *len)
{
	const struct bts_action_wait *wait;
	const u8 *data;

	if (action->size < sizeof(*wait))
		return -EINVAL;
	wait = (const void *)action->data;
	if (wait->size < 3 || wait->size > 258 ||
	    wait->size != action->size - sizeof(*wait))
		return -EINVAL;
	data = wait->data;
	if (data[0] != 0x04 || data[2] != wait->size - 3)
		return -EINVAL;
	if (data[1] == 0x0e) {
		if (wait->size < 7 ||
		    (data[4] | (data[5] << 8)) != opcode || data[6])
			return -EINVAL;
	} else if (data[1] == 0x0f) {
		if (wait->size != 7 ||
		    (data[5] | (data[6] << 8)) != opcode || data[3])
			return -EINVAL;
	}
	*event = data + 1;
	*len = wait->size - 1;
	return 0;
}

/* Find the paired wait before transmitting, including across delays/remarks. */
static int kim_prepare_command(struct kim_data_s *kim, const u8 *ptr,
		size_t len, u16 opcode)
{
	const struct bts_action *action;
	const u8 *event;
	unsigned int event_len;
	int err;

	while (len >= sizeof(*action)) {
		action = (const void *)ptr;
		if (action->size > len - sizeof(*action))
			return -EINVAL;
		if (action->type == ACTION_WAIT_EVENT) {
			err = kim_validate_wait(action, opcode, &event, &event_len);
			if (!err)
				kim_arm_response(kim, event, event_len, opcode, false);
			return err;
		}
		if (action->type != ACTION_DELAY && action->type != ACTION_REMARKS)
			return -EINVAL;
		len -= sizeof(*action) + action->size;
		ptr += sizeof(*action) + action->size;
	}
	return -EINVAL;
}

/*
 * check for data len received inside kim_int_recv
 * most often hit the last case to update state to waiting for data
 */
static inline int kim_check_data_len(struct kim_data_s *kim_gdata, int len)
{
	register int room = skb_tailroom(kim_gdata->rx_skb);

	pr_debug("len %d room %d", len, room);

	if (!len) {
		validate_firmware_response(kim_gdata);
	} else if (len > room) {
		/*
		 * Received packet's payload length is larger.
		 * We can't accommodate it in created skb.
		 */
		pr_err("Data length is too large len %d room %d", len,
			   room);
		kfree_skb(kim_gdata->rx_skb);
	} else {
		/*
		 * Packet header has non-zero payload length and
		 * we have enough space in created skb. Lets read
		 * payload data */
		kim_gdata->rx_state = ST_W4_DATA;
		kim_gdata->rx_count = len;
		return len;
	}

	/*
	 * Change ST LL state to continue to process next
	 * packet
	 */
	kim_gdata->rx_state = ST_W4_PACKET_TYPE;
	kim_gdata->rx_skb = NULL;
	kim_gdata->rx_count = 0;

	return 0;
}

/*
 * kim_int_recv - receive function called during firmware download
 *	firmware download responses on different UART drivers
 *	have been observed to come in bursts of different
 *	tty_receive and hence the logic
 */
static void kim_int_recv(struct kim_data_s *kim_gdata, const u8 *ptr,
			 const u8 *fp, size_t count)
{
	int len = 0;
	unsigned char *plen;

	pr_debug("%s", __func__);
	/* Decode received bytes here */
	while (count) {
		if (kim_gdata->rx_count) {
			len = min_t(unsigned int, kim_gdata->rx_count, count);
			memcpy(skb_put(kim_gdata->rx_skb, len), ptr, len);
			kim_gdata->rx_count -= len;
			count -= len;
			ptr += len;

			if (kim_gdata->rx_count)
				continue;

			/* Check ST RX state machine , where are we? */
			switch (kim_gdata->rx_state) {
				/* Waiting for complete packet ? */
			case ST_W4_DATA:
				pr_debug("Complete pkt received");
				validate_firmware_response(kim_gdata);
				kim_gdata->rx_state = ST_W4_PACKET_TYPE;
				kim_gdata->rx_skb = NULL;
				continue;
				/* Waiting for Bluetooth event header ? */
			case ST_W4_HEADER:
				plen =
				(unsigned char *)&kim_gdata->rx_skb->data[1];
				pr_debug("event hdr: plen 0x%02x\n", *plen);
				kim_check_data_len(kim_gdata, *plen);
				continue;
			}	/* end of switch */
		}		/* end of if rx_state */
		switch (*ptr) {
			/* Bluetooth event packet? */
		case 0x04:
			kim_gdata->rx_state = ST_W4_HEADER;
			kim_gdata->rx_count = 2;
			break;
		default:
			pr_info("unknown packet");
			ptr++;
			count--;
			continue;
		}
		ptr++;
		count--;
		kim_gdata->rx_skb =
			alloc_skb(1024+8, GFP_ATOMIC);
		if (!kim_gdata->rx_skb) {
			pr_err("can't allocate mem for new packet");
			kim_gdata->rx_state = ST_W4_PACKET_TYPE;
			kim_gdata->rx_count = 0;
			return;
		}
		skb_reserve(kim_gdata->rx_skb, 8);
		kim_gdata->rx_skb->cb[0] = 4;
		kim_gdata->rx_skb->cb[1] = 0;

	}
	return;
}

static long read_local_version(struct kim_data_s *kim_gdata, char *bts_scr_name)
{
	unsigned short version = 0, chip = 0, min_ver = 0, maj_ver = 0;
	static const char read_ver_cmd[] = { 0x01, 0x01, 0x10, 0x00 };
	long time_left;
	static const u8 version_event[14] = { 0x0e, 12, 1, 1, 0x10, 0 };

	pr_debug("%s", __func__);

	kim_arm_response(kim_gdata, version_event, sizeof(version_event),
			0x1001, true);
	if (4 != st_int_write(kim_gdata->core_data, read_ver_cmd, 4)) {
		pr_err("kim: couldn't write 4 bytes");
		kim_clear_response(kim_gdata);
		return -EIO;
	}

	time_left = kim_wait_response(kim_gdata);
	if (time_left)
		return time_left;
	/*
	 * the positions 12 & 13 in the response buffer provide with the
	 * chip, major & minor numbers
	 */

	version =
		MAKEWORD(kim_gdata->resp_buffer[12],
				kim_gdata->resp_buffer[13]);
	chip = (version & 0x7C00) >> 10;
	min_ver = (version & 0x007F);
	maj_ver = (version & 0x0380) >> 7;

	if (version & 0x8000)
		maj_ver |= 0x0008;

	sprintf(bts_scr_name, "ti-connectivity/TIInit_%d.%d.%d.bts",
		chip, maj_ver, min_ver);

	/* to be accessed later via sysfs entry */
	kim_gdata->version.full = version;
	kim_gdata->version.chip = chip;
	kim_gdata->version.maj_ver = maj_ver;
	kim_gdata->version.min_ver = min_ver;

	pr_info("%s", bts_scr_name);
	return 0;
}

static int skip_change_remote_baud(unsigned char **ptr, size_t *len)
{
	struct bts_action *action;
	unsigned char *nxt_action, *cur_action;
	size_t action_len;

	cur_action = *ptr;
	action = (struct bts_action *)cur_action;
	action_len = sizeof(*action) + action->size;
	nxt_action = cur_action + action_len;

	if (*len - action_len < sizeof(*action)) {
		pr_err("malformed firmware: missing wait event after change remote baud command");
		return -EINVAL;
	}

	action = (struct bts_action *)nxt_action;
	if (action->size > *len - action_len - sizeof(*action)) {
		pr_err("malformed firmware: truncated wait event after change remote baud command");
		return -EINVAL;
	}

	if (action->type != ACTION_WAIT_EVENT) {
		pr_err("invalid action after change remote baud command");
		return -EINVAL;
	} else {
		*ptr = nxt_action;
		*len -= action_len;
		/* warn user on not commenting these in firmware */
		pr_warn("skipping the wait event of change remote baud");
	}

	return 0;
}

/*
 * download_firmware -
 *	internal function which parses through the .bts firmware
 *	script file intreprets SEND, DELAY actions only as of now
 */
static long download_firmware(struct kim_data_s *kim_gdata)
{
	long err = 0;
	size_t len = 0;
	unsigned char *ptr = NULL;
	unsigned char *action_ptr = NULL;
	struct bts_action *action;
	char bts_scr_name[40] = { 0 };	/* 40 char long bts scr name? */
	const char *fw_names[2];
	int wr_room_space;
	int cmd_size;
	unsigned int i;
	unsigned long timeout;
	bool awaiting_response = false;

	err = read_local_version(kim_gdata, bts_scr_name);
	if (err != 0) {
		pr_err("kim: failed to read local ver");
		return err;
	}

	fw_names[0] = bts_scr_name;
	fw_names[1] = bts_scr_name + sizeof("ti-connectivity/") - 1;

	for (i = 0; i < ARRAY_SIZE(fw_names); i++) {
		pr_info("kim: requesting firmware %s", fw_names[i]);
		err = request_firmware(&kim_gdata->fw_entry, fw_names[i],
			&kim_gdata->kim_pdev->dev);
		if (!err) {
			pr_info("kim: selected firmware %s", fw_names[i]);
			break;
		}

		if (i + 1 < ARRAY_SIZE(fw_names))
			pr_info("kim: firmware %s unavailable (%ld), trying %s",
				fw_names[i], err, fw_names[i + 1]);
	}

	if (unlikely((err != 0) || (kim_gdata->fw_entry == NULL))) {
		pr_err("kim: failed to load firmware %s (errno %ld)",
			fw_names[ARRAY_SIZE(fw_names) - 1], err);
		return -EINVAL;
	}

	ptr = (void *)kim_gdata->fw_entry->data;
	len = kim_gdata->fw_entry->size;
	/*
	 * bts_header to remove out magic number and
	 * version
	 */
	if (len < sizeof(struct bts_header)) {
		pr_err("malformed firmware: truncated BTS header (%u bytes)",
			(unsigned int)len);
		err = -EINVAL;
		goto release_firmware;
	}
	ptr += sizeof(struct bts_header);
	len -= sizeof(struct bts_header);

	while (len > 0) {
		if (len < sizeof(*action)) {
			pr_err("malformed firmware: truncated BTS action header (%u bytes remain)",
				(unsigned int)len);
			err = -EINVAL;
			goto release_firmware;
		}

		action = (struct bts_action *)ptr;
		if (action->size > len - sizeof(*action)) {
			pr_err("malformed firmware: action type %u payload size %u exceeds %u remaining bytes",
				(unsigned int)action->type,
				(unsigned int)action->size,
				(unsigned int)(len - sizeof(*action)));
			err = -EINVAL;
			goto release_firmware;
		}

		pr_debug(" action size %d, type %d ",
			   action->size, action->type);

		switch (action->type) {
		case ACTION_SEND_COMMAND:	/* action send */
			if (awaiting_response) {
				err = -EINVAL;
				goto release_firmware;
			}
			pr_debug("S");
			if (action->size < offsetof(struct hci_command, speed)) {
				pr_err("malformed firmware: send command payload is too short (%u bytes)",
					(unsigned int)action->size);
				err = -EINVAL;
				goto release_firmware;
			}
			action_ptr = &action->data[0];
			if (((struct hci_command *)action_ptr)->prefix != 0x01 ||
			    ((struct hci_command *)action_ptr)->plen !=
			    action->size - offsetof(struct hci_command, speed)) {
				pr_err("malformed firmware: send command payload length does not match plen");
				err = -EINVAL;
				goto release_firmware;
			}
			if (unlikely
			    (((struct hci_command *)action_ptr)->opcode ==
			     0xFF36)) {
				/*
				 * ignore remote change
				 * baud rate HCI VS command
				 */
				pr_warn("change remote baud"
				    " rate command in firmware");
				err = skip_change_remote_baud(&ptr, &len);
				if (err)
					goto release_firmware;
				action = (struct bts_action *)ptr;
				break;
			}
			/*
			 * Make sure we have enough free space in uart
			 * tx buffer to write current firmware command
			 */
			cmd_size = action->size;
			timeout = jiffies + msecs_to_jiffies(CMD_WR_TIME);
			do {
				wr_room_space =
					st_get_uart_wr_room(kim_gdata->core_data);
				if (wr_room_space < 0) {
					pr_err("Unable to get free "
							"space info from uart tx buffer");
					err = wr_room_space;
					goto release_firmware;
				}
				mdelay(1); /* wait 1ms before checking room */
			} while ((wr_room_space < cmd_size) &&
					time_before(jiffies, timeout));

			/* Timeout happened ? */
			if (time_after_eq(jiffies, timeout)) {
				pr_err("Timeout while waiting for free "
						"free space in uart tx buffer");
				err = -ETIMEDOUT;
				goto release_firmware;
			}
			/* Arm the paired BTS wait before a response can arrive. */
			err = kim_prepare_command(kim_gdata,
				ptr + sizeof(*action) + action->size,
				len - sizeof(*action) - action->size,
				((struct hci_command *)action_ptr)->opcode);
			if (err)
				goto release_firmware;

			/*
			 * Free space found in uart buffer, call st_int_write
			 * to send current firmware command to the uart tx
			 * buffer.
			 */
			awaiting_response = true;
			err = st_int_write(kim_gdata->core_data,
			((struct bts_action_send *)action_ptr)->data,
					   action->size);
			if (unlikely(err < 0)) {
				goto release_firmware;
			}
			/*
			 * Check number of bytes written to the uart tx buffer
			 * and requested command write size
			 */
			if (err != cmd_size) {
				pr_err("Number of bytes written to uart "
						"tx buffer are not matching with "
						"requested cmd write size");
				err = -EIO;
				goto release_firmware;
			}
			break;
		case ACTION_WAIT_EVENT:  /* wait */
			pr_debug("W");
			if (!awaiting_response) {
				err = -EINVAL;
				goto release_firmware;
			}
			awaiting_response = false;
			err = kim_wait_response(kim_gdata);
			if (err)
				goto release_firmware;
			break;
		case ACTION_DELAY:	/* sleep */
			pr_info("sleep command in scr");
			if (action->size < sizeof(struct bts_action_delay)) {
				pr_err("malformed firmware: delay payload is too short (%u bytes)",
					(unsigned int)action->size);
				err = -EINVAL;
				goto release_firmware;
			}
			action_ptr = &action->data[0];
			mdelay(((struct bts_action_delay *)action_ptr)->msec);
			break;
		}
		len -= sizeof(*action) + action->size;
		ptr += sizeof(*action) + action->size;
	}
	/* fw download complete */
	err = 0;

release_firmware:
	kim_clear_response(kim_gdata);
	release_firmware(kim_gdata->fw_entry);
	kim_gdata->fw_entry = NULL;
	return err;
}

/**********************************************************************/
/* functions called from ST core */
/* called from ST Core, when REG_IN_PROGRESS (registration in progress)
 * can be because of
 * 1. response to read local version
 * 2. during send/recv's of firmware download
 */
void st_kim_recv(void *disc_data, const u8 *data, const u8 *fp, size_t count)
{
	struct st_data_s	*st_gdata = (struct st_data_s *)disc_data;
	struct kim_data_s	*kim_gdata = st_gdata->kim_data;
	unsigned long flags;

	/*
	 * proceed to gather all data and distinguish read fw version response
	 * from other fw responses when data gathering is complete
	 */
	spin_lock_irqsave(&kim_gdata->response_lock, flags);
	kim_int_recv(kim_gdata, data, fp, count);
	spin_unlock_irqrestore(&kim_gdata->response_lock, flags);
	return;
}

/*
 * to signal completion of line discipline installation
 * called from ST Core, upon tty_open
 */
void st_kim_complete(void *kim_data)
{
	struct kim_data_s	*kim_gdata = (struct kim_data_s *)kim_data;
	complete(&kim_gdata->ldisc_installed);
}

/*
 * st_kim_start - called from ST Core upon 1st registration
 *	This involves toggling the chip enable gpio, reading
 *	the firmware version from chip, forming the fw file name
 *	based on the chip version, requesting the fw, parsing it
 *	and perform download(send/recv).
 */
long st_kim_start(void *kim_data)
{
	long err, stop_err;
	long retry = POR_RETRY_COUNT;
	struct ti_st_plat_data	*pdata;
	struct kim_data_s	*kim_gdata = (struct kim_data_s *)kim_data;

	pr_info(" %s", __func__);
	pdata = kim_gdata->kim_pdev->dev.platform_data;

	do {
		/* platform specific enabling code here */
		if (pdata->chip_enable) {
			err = pdata->chip_enable(kim_gdata);
			if (err) {
				pr_err("chip enable failed: %ld", err);
				return err;
			}
		}

		/* Configure BT nShutdown to HIGH state */
		gpio_set_value_cansleep(kim_gdata->nshutdown, GPIO_LOW);
		mdelay(5);	/* FIXME: a proper toggle */
		gpio_set_value_cansleep(kim_gdata->nshutdown, GPIO_HIGH);
		mdelay(100);
		/* re-initialize the completion */
		INIT_COMPLETION(kim_gdata->ldisc_installed);
		/* send notification to UIM */
		kim_gdata->ldisc_install = 1;
		pr_info("ldisc_install = 1");
		sysfs_notify(&kim_gdata->kim_pdev->dev.kobj,
				NULL, "install");
		/* wait for ldisc to be installed */
		err = wait_for_completion_interruptible_timeout(
			&kim_gdata->ldisc_installed, msecs_to_jiffies(LDISC_TIME));
		if (err > 0) {
			pr_info("line discipline installed");
			err = download_firmware(kim_gdata);
			if (!err)
				return 0;
			pr_err("download firmware failed: %ld", err);
		} else if (!err) {
			pr_err("ldisc installation timeout");
			err = -ETIMEDOUT;
		}

		/* Flush UART and power down without losing the startup error. */
		stop_err = st_kim_stop(kim_gdata);
		if (stop_err)
			pr_err("startup cleanup failed: %ld", stop_err);

		/* A signaled wait must not start another power-on attempt. */
		if (err == -ERESTARTSYS || err == -EINTR ||
		    stop_err == -ERESTARTSYS || stop_err == -EINTR)
			return err;
	} while (retry--);
	return err;
}

/*
 * st_kim_stop - stop communication with chip.
 *	This can be called from ST Core/KIM, on the-
 *	(a) last un-register when chip need not be powered there-after,
 *	(b) upon failure to either install ldisc or download firmware.
 *	The function is responsible to (a) notify UIM about un-installation,
 *	(b) flush UART if the ldisc was installed.
 *	(c) reset BT_EN - pull down nshutdown at the end.
 *	(d) invoke platform's chip disabling routine.
 */
long st_kim_stop(void *kim_data)
{
	long err = 0;
	int disable_err;
	struct kim_data_s	*kim_gdata = (struct kim_data_s *)kim_data;
	struct ti_st_plat_data	*pdata =
		kim_gdata->kim_pdev->dev.platform_data;
	struct tty_struct	*tty = kim_gdata->core_data->tty;

	kim_clear_response(kim_gdata);
	INIT_COMPLETION(kim_gdata->ldisc_installed);

	if (tty) {	/* can be called before ldisc is installed */
		/* Flush any pending characters in the driver and discipline. */
		tty_ldisc_flush(tty);
		tty_driver_flush_buffer(tty);
	}

	/* send uninstall notification to UIM */
	pr_info("ldisc_install = 0");
	kim_gdata->ldisc_install = 0;
	sysfs_notify(&kim_gdata->kim_pdev->dev.kobj, NULL, "install");

	/* wait for ldisc to be un-installed */
	err = wait_for_completion_interruptible_timeout(
		&kim_gdata->ldisc_installed, msecs_to_jiffies(LDISC_TIME));
	if (!err) {		/* timeout */
		pr_err(" timed out waiting for ldisc to be un-installed");
		err = -ETIMEDOUT;
	} else if (err > 0) {
		err = 0;
	}

	/* By default configure BT nShutdown to LOW state */
	gpio_set_value_cansleep(kim_gdata->nshutdown, GPIO_LOW);
	mdelay(1);
	gpio_set_value_cansleep(kim_gdata->nshutdown, GPIO_HIGH);
	mdelay(1);
	gpio_set_value_cansleep(kim_gdata->nshutdown, GPIO_LOW);

	/* platform specific disable */
	if (pdata->chip_disable) {
		disable_err = pdata->chip_disable(kim_gdata);
		if (disable_err) {
			pr_err("chip disable failed: %d", disable_err);
			if (!err)
				err = disable_err;
		}
	}
	return err;
}

/**********************************************************************/
/* functions called from subsystems */
/* called when debugfs entry is read from */

static int version_show(struct seq_file *s, void *unused)
{
	struct kim_data_s *kim_gdata = s->private;
	seq_printf(s, "%04X %d.%d.%d\n", kim_gdata->version.full,
			kim_gdata->version.chip, kim_gdata->version.maj_ver,
			kim_gdata->version.min_ver);
	return 0;
}

static int list_show(struct seq_file *s, void *unused)
{
	struct kim_data_s *kim_gdata = s->private;
	kim_st_list_protocols(kim_gdata->core_data, s);
	return 0;
}

static void kim_debugfs_ref_release(struct kref *ref)
{
	struct kim_data_s *kim_gdata = container_of(ref,
			struct kim_data_s, debugfs_ref);

	kfree(kim_gdata);
}

static int kim_debugfs_open(struct inode *inode, struct file *file,
		int (*show)(struct seq_file *, void *))
{
	struct kim_data_s *kim_gdata = NULL;
	struct platform_device *pdev;
	int device_id;
	int err;

	mutex_lock(&kim_debugfs_open_lock);
	for (device_id = 0; device_id < MAX_ST_DEVICES; device_id++) {
		pdev = st_kim_devices[device_id];
		if (!pdev)
			continue;
		kim_gdata = platform_get_drvdata(pdev);
		if (kim_gdata && (inode == kim_gdata->debugfs_version_inode ||
				inode == kim_gdata->debugfs_protocols_inode))
			break;
	}
	if (device_id == MAX_ST_DEVICES || kim_gdata->debugfs_removed) {
		mutex_unlock(&kim_debugfs_open_lock);
		return -ENODEV;
	}
	kref_get(&kim_gdata->debugfs_ref);
	mutex_unlock(&kim_debugfs_open_lock);

	err = single_open(file, show, kim_gdata);
	if (err)
		kref_put(&kim_gdata->debugfs_ref, kim_debugfs_ref_release);

	return err;
}

static ssize_t kim_debugfs_read(struct file *file, char __user *buf,
		size_t size, loff_t *pos)
{
	struct seq_file *seq = file->private_data;
	struct kim_data_s *kim_gdata = seq->private;
	ssize_t ret;

	mutex_lock(&kim_gdata->debugfs_lock);
	ret = kim_gdata->debugfs_removed ? -ENODEV :
		seq_read(file, buf, size, pos);
	mutex_unlock(&kim_gdata->debugfs_lock);
	return ret;
}

static loff_t kim_debugfs_lseek(struct file *file, loff_t offset, int origin)
{
	struct seq_file *seq = file->private_data;
	struct kim_data_s *kim_gdata = seq->private;
	loff_t ret;

	mutex_lock(&kim_gdata->debugfs_lock);
	ret = kim_gdata->debugfs_removed ? -ENODEV :
		seq_lseek(file, offset, origin);
	mutex_unlock(&kim_gdata->debugfs_lock);
	return ret;
}

static int version_open(struct inode *inode, struct file *file)
{
	return kim_debugfs_open(inode, file, version_show);
}

static int list_open(struct inode *inode, struct file *file)
{
	return kim_debugfs_open(inode, file, list_show);
}

static int kim_debugfs_release(struct inode *inode, struct file *file)
{
	struct seq_file *seq = file->private_data;
	struct kim_data_s *kim_gdata = seq->private;
	int err;

	err = single_release(inode, file);
	kref_put(&kim_gdata->debugfs_ref, kim_debugfs_ref_release);

	return err;
}

static const struct file_operations version_fops = {
	.owner		= THIS_MODULE,
	.open		= version_open,
	.read		= kim_debugfs_read,
	.llseek		= kim_debugfs_lseek,
	.release	= kim_debugfs_release,
};

static const struct file_operations list_fops = {
	.owner		= THIS_MODULE,
	.open		= list_open,
	.read		= kim_debugfs_read,
	.llseek		= kim_debugfs_lseek,
	.release	= kim_debugfs_release,
};

static void kim_debugfs_remove(struct kim_data_s *kim_gdata)
{
	mutex_lock(&kim_debugfs_open_lock);
	mutex_lock(&kim_gdata->debugfs_lock);
	kim_gdata->debugfs_removed = true;
	mutex_unlock(&kim_gdata->debugfs_lock);
	mutex_unlock(&kim_debugfs_open_lock);

	debugfs_remove_recursive(kim_gdata->debugfs_dir);
	kim_gdata->debugfs_dir = NULL;
}

static ssize_t show_install(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct kim_data_s *kim_data = dev_get_drvdata(dev);
	return sprintf(buf, "%d\n", kim_data->ldisc_install);
}

#ifdef DEBUG
static ssize_t store_dev_name(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct kim_data_s *kim_data = dev_get_drvdata(dev);
	pr_debug("storing dev name >%s<", buf);
	strncpy(kim_data->dev_name, buf, sizeof(kim_data->dev_name));
	pr_debug("stored dev name >%s<", kim_data->dev_name);
	return count;
}

static ssize_t store_baud_rate(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct kim_data_s *kim_data = dev_get_drvdata(dev);
	pr_debug("storing baud rate >%s<", buf);
	sscanf(buf, "%ld", &kim_data->baud_rate);
	pr_debug("stored baud rate >%ld<", kim_data->baud_rate);
	return count;
}
#endif	/* if DEBUG */

static ssize_t show_dev_name(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct kim_data_s *kim_data = dev_get_drvdata(dev);
	return sprintf(buf, "%s\n", kim_data->dev_name);
}

static ssize_t show_baud_rate(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct kim_data_s *kim_data = dev_get_drvdata(dev);
	return sprintf(buf, "%d\n", kim_data->baud_rate);
}

static ssize_t show_flow_cntrl(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct kim_data_s *kim_data = dev_get_drvdata(dev);
	return sprintf(buf, "%d\n", kim_data->flow_cntrl);
}

/* structures specific for sysfs entries */
static struct kobj_attribute ldisc_install =
__ATTR(install, 0444, (void *)show_install, NULL);

static struct kobj_attribute uart_dev_name =
#ifdef DEBUG	/* TODO: move this to debug-fs if possible */
__ATTR(dev_name, 0644, (void *)show_dev_name, (void *)store_dev_name);
#else
__ATTR(dev_name, 0444, (void *)show_dev_name, NULL);
#endif

static struct kobj_attribute uart_baud_rate =
#ifdef DEBUG	/* TODO: move to debugfs */
__ATTR(baud_rate, 0644, (void *)show_baud_rate, (void *)store_baud_rate);
#else
__ATTR(baud_rate, 0444, (void *)show_baud_rate, NULL);
#endif

static struct kobj_attribute uart_flow_cntrl =
__ATTR(flow_cntrl, 0444, (void *)show_flow_cntrl, NULL);

static struct attribute *uim_attrs[] = {
	&ldisc_install.attr,
	&uart_dev_name.attr,
	&uart_baud_rate.attr,
	&uart_flow_cntrl.attr,
	NULL,
};

static const struct attribute_group uim_attr_grp = {
	.attrs = uim_attrs,
};

/*
 * st_kim_ref - reference the core's data
 *	This references the per-ST platform device in the arch/xx/
 *	board-xx.c file.
 *	This would enable multiple such platform devices to exist
 *	on a given platform
 */
void st_kim_ref(struct st_data_s **core_data, int id)
{
	struct platform_device	*pdev;
	struct kim_data_s	*kim_gdata;
	/* get kim_gdata reference from platform device */
	pdev = st_get_plat_device(id);
	if (!pdev)
		goto err;
	kim_gdata = platform_get_drvdata(pdev);
	if (!kim_gdata)
		goto err;

	*core_data = kim_gdata->core_data;
	return;
err:
	*core_data = NULL;
}

/**********************************************************************/
/* functions called from platform device driver subsystem
 * need to have a relevant platform device entry in the platform's
 * board-*.c file
 */

static int kim_probe(struct platform_device *pdev)
{
	struct kim_data_s	*kim_gdata;
	struct ti_st_plat_data	*pdata = pdev->dev.platform_data;
	struct dentry		*debugfs_file;
	int device_id;
	int err;

	if ((pdev->id >= 0) && (pdev->id < MAX_ST_DEVICES)) {
		device_id = pdev->id;
	} else {
		/* platform's sure about existence of 1 device */
		device_id = 0;
	}

	kim_gdata = kzalloc(sizeof(struct kim_data_s), GFP_KERNEL);
	if (!kim_gdata) {
		pr_err("no mem to allocate");
		return -ENOMEM;
	}
	platform_set_drvdata(pdev, kim_gdata);

	err = st_core_init(&kim_gdata->core_data);
	if (err != 0) {
		pr_err("ST core init failed: %d", err);
		goto err_core_init;
	}
	/* refer to itself */
	kim_gdata->core_data->kim_data = kim_gdata;

	/* Claim the chip enable nShutdown gpio from the system */
	kim_gdata->nshutdown = pdata->nshutdown_gpio;
	err = gpio_request(kim_gdata->nshutdown, "kim");
	if (unlikely(err)) {
		pr_err("gpio %d request failed: %d", kim_gdata->nshutdown,
		       err);
		goto err_gpio_request;
	}

	/* Configure nShutdown GPIO as output=0 */
	err = gpio_direction_output(kim_gdata->nshutdown, 0);
	if (unlikely(err)) {
		pr_err("unable to configure gpio %d: %d",
		       kim_gdata->nshutdown, err);
		goto err_gpio_direction;
	}
	/* get reference of pdev for request_firmware */
	kim_gdata->kim_pdev = pdev;
	spin_lock_init(&kim_gdata->response_lock);
	init_completion(&kim_gdata->kim_rcvd);
	init_completion(&kim_gdata->ldisc_installed);

	err = sysfs_create_group(&pdev->dev.kobj, &uim_attr_grp);
	if (err) {
		pr_err("failed to create sysfs entries: %d", err);
		goto err_sysfs_group;
	}

	/* copying platform data */
	strncpy(kim_gdata->dev_name, pdata->dev_name,
			sizeof(kim_gdata->dev_name));
	kim_gdata->flow_cntrl = pdata->flow_cntrl;
	kim_gdata->baud_rate = pdata->baud_rate;
	pr_info("sysfs entries created\n");

	kref_init(&kim_gdata->debugfs_ref);
	mutex_init(&kim_gdata->debugfs_lock);

	kim_gdata->debugfs_dir = debugfs_create_dir(dev_name(&pdev->dev), NULL);
	if (IS_ERR_OR_NULL(kim_gdata->debugfs_dir)) {
		err = kim_gdata->debugfs_dir ?
			PTR_ERR(kim_gdata->debugfs_dir) : -ENOMEM;
		if (err == -ENODEV) {
			kim_gdata->debugfs_dir = NULL;
			goto debugfs_unavailable;
		}
		pr_err("failed to create debugfs directory: %d", err);
		kim_gdata->debugfs_dir = NULL;
		goto err_debugfs;
	}

	debugfs_file = debugfs_create_file("version", S_IRUGO,
				kim_gdata->debugfs_dir,
				kim_gdata, &version_fops);
	if (IS_ERR_OR_NULL(debugfs_file)) {
		err = debugfs_file ? PTR_ERR(debugfs_file) : -ENOMEM;
		pr_err("failed to create version debugfs entry: %d", err);
		goto err_debugfs;
	}
	kim_gdata->debugfs_version_inode = debugfs_file->d_inode;

	debugfs_file = debugfs_create_file("protocols", S_IRUGO,
				kim_gdata->debugfs_dir,
				kim_gdata, &list_fops);
	if (IS_ERR_OR_NULL(debugfs_file)) {
		err = debugfs_file ? PTR_ERR(debugfs_file) : -ENOMEM;
		pr_err("failed to create protocols debugfs entry: %d", err);
		goto err_debugfs;
	}
	kim_gdata->debugfs_protocols_inode = debugfs_file->d_inode;

debugfs_unavailable:
	mutex_lock(&kim_debugfs_open_lock);
	st_kim_devices[device_id] = pdev;
	mutex_unlock(&kim_debugfs_open_lock);
	return 0;

err_debugfs:
	kim_debugfs_remove(kim_gdata);
	sysfs_remove_group(&pdev->dev.kobj, &uim_attr_grp);
err_sysfs_group:
	kim_gdata->kim_pdev = NULL;
err_gpio_direction:
	gpio_free(kim_gdata->nshutdown);
err_gpio_request:
	kim_gdata->core_data->kim_data = NULL;
	st_core_exit(kim_gdata->core_data);
err_core_init:
	platform_set_drvdata(pdev, NULL);
	if (kim_gdata->debugfs_removed)
		kref_put(&kim_gdata->debugfs_ref, kim_debugfs_ref_release);
	else
		kfree(kim_gdata);

	return err;
}

static int kim_remove(struct platform_device *pdev)
{
	struct kim_data_s	*kim_gdata;
	int device_id;

	kim_gdata = platform_get_drvdata(pdev);
	if (!kim_gdata)
		return 0;

	if ((pdev->id >= 0) && (pdev->id < MAX_ST_DEVICES))
		device_id = pdev->id;
	else
		device_id = 0;
	mutex_lock(&kim_debugfs_open_lock);
	st_kim_devices[device_id] = NULL;
	mutex_unlock(&kim_debugfs_open_lock);
	kim_debugfs_remove(kim_gdata);
	sysfs_remove_group(&pdev->dev.kobj, &uim_attr_grp);
	pr_info("sysfs entries removed");

	kim_gdata->kim_pdev = NULL;
	gpio_free(kim_gdata->nshutdown);
	pr_info("nshutdown GPIO freed");

	kim_clear_response(kim_gdata);
	kim_gdata->core_data->kim_data = NULL;
	st_core_exit(kim_gdata->core_data);
	kim_gdata->core_data = NULL;

	platform_set_drvdata(pdev, NULL);
	kref_put(&kim_gdata->debugfs_ref, kim_debugfs_ref_release);
	return 0;
}

static int kim_suspend(struct platform_device *pdev, pm_message_t state)
{
	struct ti_st_plat_data	*pdata = pdev->dev.platform_data;

	if (pdata->suspend)
		return pdata->suspend(pdev, state);

	return 0;
}

static int kim_resume(struct platform_device *pdev)
{
	struct ti_st_plat_data	*pdata = pdev->dev.platform_data;

	if (pdata->resume)
		return pdata->resume(pdev);

	return 0;
}

/**********************************************************************/
/* entry point for ST KIM module, called in from ST Core */
static struct platform_driver kim_platform_driver = {
	.probe = kim_probe,
	.remove = kim_remove,
	.suspend = kim_suspend,
	.resume = kim_resume,
	.driver = {
		.name = "kim",
	},
};

static int __init st_kim_init(void)
{
	return platform_driver_register(&kim_platform_driver);
}

static void __exit st_kim_deinit(void)
{
	platform_driver_unregister(&kim_platform_driver);
}


module_init(st_kim_init);
module_exit(st_kim_deinit);
MODULE_AUTHOR("Pavan Savoy <pavan_savoy@ti.com>");
MODULE_DESCRIPTION("Shared Transport Driver for TI BT/FM/GPS combo chips ");
MODULE_LICENSE("GPL");
