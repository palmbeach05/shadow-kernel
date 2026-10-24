#!/usr/bin/env python3
"""Compile KIM's response validation and fragmented receive parser with stubs."""
from pathlib import Path
import subprocess
import os
import shlex
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'drivers/misc/ti-st/st_kim.c').read_text()
functions = source[source.index('/* All response'):source.index('static long read_local_version')]
header = (root / 'include/linux/ti_wilink_st.h').read_text()
bts = header[header.index('struct bts_action {'):header.index('/**\n * struct hci_command')]
harness = r'''
#include <assert.h>
#include <stddef.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
#define ACTION_WAIT_EVENT 2
#define ACTION_DELAY 4
#define ACTION_REMARKS 6
#define ST_W4_PACKET_TYPE 0
#define ST_W4_HEADER 1
#define ST_W4_DATA 2
#define CMD_RESP_TIME 1000
#define GFP_ATOMIC 0
#define pr_debug(...) ((void)0)
#define pr_err(...) ((void)0)
#define pr_info(...) ((void)0)
#define min_t(type,a,b) ((type)(a) < (type)(b) ? (type)(a) : (type)(b))
#define spin_lock_irqsave(lock,flags) ((void)(lock), (flags) = 0)
#define spin_unlock_irqrestore(lock,flags) ((void)(lock), (void)(flags))
#define reinit_completion(c) (*(c) = 0)
#define complete_all(c) (*(c) = 1)
#define msecs_to_jiffies(t) (t)
struct sk_buff { u8 data[1032], cb[2]; unsigned int len; };
struct firmware { const u8 *data; size_t size; };
struct platform_device { int dev; };
struct hci_command { u8 prefix; u16 opcode; u8 plen; u32 speed; } __attribute__((packed));
struct bts_header { u8 data[32]; };
struct kim_data_s {
 void *core_data; struct platform_device *kim_pdev;
 const struct firmware *fw_entry;
 struct { unsigned int full, chip, maj_ver, min_ver; } version;
 int response_lock, kim_rcvd;
 u8 expected_event[257]; unsigned int expected_len; u16 expected_opcode;
 bool response_pending, read_version; int response_result;
 char resp_buffer[30];
 unsigned long rx_state, rx_count; struct sk_buff *rx_skb;
};
static long wait_result;
static long wait_for_completion_interruptible_timeout(int *c, long t) {
 (void)t; return *c ? 1 : wait_result;
}
static struct sk_buff *alloc_skb(int n, int flags) {
 (void)n; (void)flags; return calloc(1, sizeof(struct sk_buff));
}
static void kfree_skb(struct sk_buff *skb) { free(skb); }
static int skb_tailroom(struct sk_buff *skb) { return sizeof(skb->data) - skb->len; }
static void *skb_put(struct sk_buff *skb, int len) {
 void *p = skb->data + skb->len; skb->len += len; return p;
}
static void skb_reserve(struct sk_buff *skb, int n) { (void)skb; (void)n; }
'''
firmware_functions = source[source.index('static long read_local_version'):source.index('/**********************************************************************/', source.index('static long read_local_version'))]
firmware_stubs = r'''
#define MAKEWORD(a,b) ((u16)(u8)(a) | ((u16)(u8)(b) << 8))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define unlikely(x) (x)
#define pr_warn(...) ((void)0)
#define CMD_WR_TIME 100
#define ACTION_SEND_COMMAND 1
static unsigned long jiffies;
#define time_before(a,b) ((long)((a)-(b)) < 0)
#define time_after_eq(a,b) (!time_before(a,b))
static void mdelay(unsigned int msec) { jiffies += msec; }
static struct kim_data_s *active;
static int version_mode, requests;
static char requested[2][40];
static const u8 empty_script[32];
static const struct firmware fw = { empty_script, sizeof(empty_script) };
static int request_firmware(const struct firmware **entry, const char *name, void *dev) {
 (void)dev; assert(requests < 2); strcpy(requested[requests++], name);
 if (requests == 1) return -ENOENT;
 *entry = &fw; return 0;
}
static void release_firmware(const struct firmware *entry) { assert(entry == &fw); }
static int st_get_uart_wr_room(void *core) { (void)core; return 1024; }
static int st_int_write(void *core, const char *data, int len) {
 u8 response[] = {4, 0x0e, 12, 1, 1, 0x10, 0, 0, 0, 0, 0, 0, 0, 15, 0x9a};
 (void)core; (void)data; assert(len == 4);
 if (version_mode == 1) response[5]++;
 if (version_mode == 2) response[6] = 1;
 if (version_mode == 3) response[1] = 0xff;
 kim_int_recv(active, response, NULL, sizeof(response));
 return len;
}
'''
cases = r'''
static const u8 good[] = {4, 0x0e, 4, 1, 1, 0x10, 0};
static void arm(struct kim_data_s *kim) {
 kim_arm_response(kim, good + 1, sizeof(good) - 1, 0x1001, false);
}
static void feed(struct kim_data_s *kim, const u8 *p, size_t len) {
 kim_int_recv(kim, p, NULL, len);
}
int main(void) {
 struct kim_data_s kim = {0};
 u8 event[32], action_bytes[32] = {0};
 struct bts_action *action = (void *)action_bytes;
 struct bts_action_wait *wait = (void *)action->data;
 const u8 *expected; unsigned int len, i;
 action->type = ACTION_WAIT_EVENT;
 action->size = sizeof(*wait) + sizeof(good);
 wait->size = sizeof(good); memcpy(wait->data, good, sizeof(good));
 assert(!kim_validate_wait(action, 0x1001, &expected, &len));
 assert(len == 6 && expected[0] == 0x0e);
 assert(!kim_prepare_command(&kim, action_bytes, sizeof(*action) + action->size, 0x1001));
 feed(&kim, good, sizeof(good)); /* response before wait */
 assert(!kim_wait_response(&kim));
 for (i = 0; i < sizeof(*wait); i++) {
  action->size = i; assert(kim_validate_wait(action, 0x1001, &expected, &len) == -EINVAL);
 }
 action->size = sizeof(*wait) + sizeof(good);
 wait->size = 1000; assert(kim_validate_wait(action, 0x1001, &expected, &len) == -EINVAL);
 wait->size = sizeof(good); wait->data[0] = 0;
 assert(kim_validate_wait(action, 0x1001, &expected, &len) == -EINVAL);
 wait->data[0] = 4;
 assert(kim_validate_wait(action, 0x1002, &expected, &len) == -EINVAL);
 for (i = 1; i < sizeof(good); i++) {
  arm(&kim); feed(&kim, good, i); assert(!kim.kim_rcvd);
  feed(&kim, good + i, sizeof(good) - i); assert(!kim_wait_response(&kim));
 }
 arm(&kim); memcpy(event, good, sizeof(good)); event[4]++;
 memcpy(event + sizeof(good), good, sizeof(good));
 feed(&kim, event, 2 * sizeof(good)); assert(!kim_wait_response(&kim));
 arm(&kim); memcpy(event, good, sizeof(good)); event[1] = 0xff;
 feed(&kim, event, sizeof(good)); assert(!kim.kim_rcvd);
 feed(&kim, good, sizeof(good)); assert(!kim_wait_response(&kim));
 /* Complete but too short Command Complete cannot satisfy the wait. */
 arm(&kim); event[0] = 4; event[1] = 0x0e; event[2] = 1; event[3] = 1;
 feed(&kim, event, 4); assert(!kim.kim_rcvd);
 feed(&kim, good, sizeof(good)); assert(!kim_wait_response(&kim));
 arm(&kim); kim_clear_response(&kim);
 assert(kim_wait_response(&kim) == -ESHUTDOWN);
 arm(&kim); memcpy(event, good, sizeof(good)); event[6] = 1;
 feed(&kim, event, sizeof(good)); assert(kim_wait_response(&kim) == -EIO);
 {
  const u8 status[] = {4, 0x0f, 4, 0, 1, 1, 0x10};
  kim_arm_response(&kim, status + 1, 6, 0x1001, false);
  memcpy(event, status, sizeof(status)); event[3] = 2;
  feed(&kim, event, sizeof(status)); assert(kim_wait_response(&kim) == -EIO);
  kim_arm_response(&kim, status + 1, 6, 0x1001, false);
  feed(&kim, status, sizeof(status)); assert(!kim_wait_response(&kim));
 }
 {
  const u8 vendor[] = {4, 0xff, 3, 0x71, 0x99, 0x88};
  kim_arm_response(&kim, vendor + 1, 5, 0x1001, false);
  feed(&kim, vendor, sizeof(vendor)); assert(!kim_wait_response(&kim));
 }
 for (i = 0; i < 2; i++) {
  arm(&kim); feed(&kim, good, 3); wait_result = i ? -EINTR : 0;
  assert(kim_wait_response(&kim) == (i ? -EINTR : -ETIMEDOUT));
  assert(!kim.response_pending && !kim.rx_skb && !kim.expected_len);
  feed(&kim, good, sizeof(good)); arm(&kim);
  feed(&kim, good, sizeof(good)); assert(!kim_wait_response(&kim));
 }
 {
  const u8 version[14] = {0x0e, 12, 1, 1, 0x10, 0};
  kim_arm_response(&kim, version, 14, 0x1001, true);
  feed(&kim, good, sizeof(good)); assert(!kim.kim_rcvd);
  event[0] = 4; memcpy(event + 1, version, 14); event[14] = 0x42;
  feed(&kim, event, 15); assert(!kim_wait_response(&kim));
  assert(kim.resp_buffer[13] == 0x42);
 }
 /* Real TI BTS wait record: H4 type and zero command credits.
  * https://git.ti.com/cgit/ti-bt/service-packs/tree/initscripts/TIInit_6.12.26.bts
  */
 {
  const u8 record[] = {2,0,15,0,0x88,0x13,0,0,7,0,0,0,4,0x0e,4,0,5,0xff,0};
  assert(!kim_prepare_command(&kim, record, sizeof(record), 0xff05));
  memcpy(event, record + 12, 7); event[3] = 1;
  feed(&kim, event, 7); assert(!kim_wait_response(&kim));
 }
 {
  struct platform_device pdev = {0};
  char name[40];
  active = &kim; kim.kim_pdev = &pdev;
  wait_result = 0;
  for (version_mode = 1; version_mode <= 3; version_mode++) {
   memset(kim.resp_buffer, 0x55, sizeof(kim.resp_buffer));
   assert(read_local_version(&kim, name) == (version_mode == 2 ? -EIO : -ETIMEDOUT));
   assert(kim.resp_buffer[13] == 0x55);
  }
  version_mode = 0; requests = 0;
  assert(!download_firmware(&kim));
  assert(requests == 2);
  assert(!strcmp(requested[0], "ti-connectivity/TIInit_6.12.15.bts"));
  assert(!strcmp(requested[1], "TIInit_6.12.15.bts"));
  assert(!kim.fw_entry && !kim.expected_len);
 }
 kim_clear_response(&kim);
 puts("KIM response checks passed");
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='kim-response-') as directory:
    test = Path(directory) / 'test.c'
    binary = Path(directory) / 'test'
    test.write_text(harness + bts + functions + firmware_stubs + firmware_functions + cases)
    subprocess.run(['cc', '-std=gnu89', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-parameter', '-Wno-sign-compare',
                    '-Wno-pointer-sign'] + shlex.split(os.environ.get('CFLAGS', '')) +
                   [str(test), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
