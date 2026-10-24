#!/usr/bin/env python3
"""Compile the actual KIM lifecycle functions against scripted hardware stubs."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'drivers/misc/ti-st/st_kim.c').read_text()
start = source.index('long st_kim_start(void *kim_data)')
end = source.index('/**********************************************************************/', start)
functions = source[start:end]
harness = r'''
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#define ERESTARTSYS 512
#define POR_RETRY_COUNT 5
#define LDISC_TIME 1000
#define GPIO_LOW 0
#define GPIO_HIGH 1
#define INIT_COMPLETION(x) ((x) = 0)
#define pr_info(...) ((void)0)
#define pr_err(...) ((void)0)
struct tty_struct { int unused; };
struct kim_data_s;
struct ti_st_plat_data { int (*chip_enable)(struct kim_data_s *); int (*chip_disable)(struct kim_data_s *); };
struct platform_device { struct { void *platform_data; int kobj; } dev; };
struct st_data_s { struct tty_struct *tty; };
struct kim_data_s {
 struct platform_device *kim_pdev;
 struct st_data_s *core_data;
 int nshutdown, ldisc_installed, ldisc_install;
};
static long install_result, uninstall_result, firmware_result;
static int installs, uninstalls, downloads, disables, gpio_value;
static int enables, gpio_calls, notifications, flushes;
static int enable_result, disable_result, callbacks;
static int succeed_on;
static struct kim_data_s *active;
static int enable(struct kim_data_s *data) {
 assert(data == active); enables++; return enable_result;
}
static int disable(struct kim_data_s *data) {
 assert(data == active); disables++; return disable_result;
}
static void gpio_set_value_cansleep(int gpio, int value) { (void)gpio; gpio_calls++; gpio_value = value; }
static void mdelay(int delay) { (void)delay; }
static long msecs_to_jiffies(long value) { return value; }
static void sysfs_notify(void *obj, void *unused, const char *name) {
 (void)obj; (void)unused; (void)name; notifications++;
}
static void tty_ldisc_flush(struct tty_struct *tty) { (void)tty; flushes++; }
static void tty_driver_flush_buffer(struct tty_struct *tty) { (void)tty; flushes++; }
static long wait_for_completion_interruptible_timeout(int *completion, long timeout) {
 (void)completion; (void)timeout;
 if (active->ldisc_install) { installs++; return install_result; }
 uninstalls++; return uninstall_result;
}
static long download_firmware(struct kim_data_s *data) {
 assert(data == active); downloads++;
 return succeed_on && downloads == succeed_on ? 0 : firmware_result;
}
static void kim_clear_response(struct kim_data_s *kim) { (void)kim; }
long st_kim_stop(void *);
'''
cases = r'''
static void check(long install, long firmware, long uninstall, int success_attempt,
                  long expected, int attempts, int fw_calls, int cleanup_calls) {
 struct ti_st_plat_data pdata = {
  .chip_enable = callbacks ? enable : NULL,
  .chip_disable = callbacks ? disable : NULL
 };
 struct platform_device pdev = { .dev.platform_data = &pdata };
 struct tty_struct tty;
 struct st_data_s core = { .tty = &tty };
 struct kim_data_s kim = { .kim_pdev = &pdev, .core_data = &core };
 active = &kim; install_result = install; firmware_result = firmware;
 uninstall_result = uninstall; succeed_on = success_attempt;
 installs = uninstalls = downloads = disables = 0;
 enables = gpio_calls = notifications = flushes = 0;
 assert(st_kim_start(&kim) == expected);
 assert(installs == attempts && downloads == fw_calls);
 assert(uninstalls == cleanup_calls);
 assert(disables == (callbacks ? cleanup_calls : 0));
 assert(enables == (callbacks ? (enable_result ? 1 : attempts) : 0));
 assert(notifications == attempts + cleanup_calls);
 assert(gpio_calls == 2 * attempts + 3 * cleanup_calls);
 assert(flushes == 2 * cleanup_calls);
 if (attempts && cleanup_calls == attempts) assert(gpio_value == GPIO_LOW);
}
int main(void) {
 for (callbacks = 0; callbacks <= 1; callbacks++) {
  check(1, 0, 1, 0, 0, 1, 1, 0);
  check(0, 0, 1, 0, -ETIMEDOUT, 6, 0, 6);
  check(-ERESTARTSYS, 0, 1, 0, -ERESTARTSYS, 1, 0, 1);
  check(-EINTR, 0, 0, 0, -EINTR, 1, 0, 1);
  check(1, -ERESTARTSYS, 1, 0, -ERESTARTSYS, 1, 1, 1);
  check(1, -EINTR, -ERESTARTSYS, 0, -EINTR, 1, 1, 1);
  check(1, -EIO, 1, 0, -EIO, 6, 6, 6);
  check(1, -EINVAL, 0, 0, -EINVAL, 6, 6, 6);
  check(1, -ETIMEDOUT, -ERESTARTSYS, 0, -ETIMEDOUT, 1, 1, 1);
  check(1, -ETIMEDOUT, -EINTR, 0, -ETIMEDOUT, 1, 1, 1);
  check(1, -EIO, 1, 3, 0, 3, 3, 2);
 }
 /* Failed enable must return without reset, notification, wait or cleanup. */
 callbacks = 1; enable_result = -EIO;
 check(1, 0, 1, 0, -EIO, 0, 0, 0);
 enable_result = 0;
 /* Cleanup disable failures must never replace the startup error. */
 disable_result = -EACCES;
 check(1, -EINVAL, 1, 0, -EINVAL, 6, 6, 6);
 check(1, -EINVAL, 0, 0, -EINVAL, 6, 6, 6);
 check(-EINTR, 0, 1, 0, -EINTR, 1, 0, 1);
 check(1, -EINVAL, -ERESTARTSYS, 0, -EINVAL, 1, 1, 1);
 disable_result = -EINTR;
 check(1, -EINVAL, 1, 0, -EINVAL, 1, 1, 1);
 /* Exercise stop independently for each completion result. */
 {
  struct ti_st_plat_data pdata = { .chip_disable = disable };
  struct platform_device pdev = { .dev.platform_data = &pdata };
  struct st_data_s core = { .tty = NULL };
  struct kim_data_s kim = { .kim_pdev = &pdev, .core_data = &core };
  long results[] = { 10, 0, -ERESTARTSYS, -EINTR };
  long expected[] = { 0, -ETIMEDOUT, -ERESTARTSYS, -EINTR };
  unsigned int i;
  int failure;
  active = &kim;
  for (failure = 0; failure <= 1; failure++) {
   disable_result = failure ? -EIO : 0;
   for (i = 0; i < sizeof(results) / sizeof(results[0]); i++) {
    uninstall_result = results[i]; disables = 0;
    assert(st_kim_stop(&kim) == (expected[i] ? expected[i] : disable_result));
    assert(disables == 1 && gpio_value == GPIO_LOW && !kim.ldisc_install);
   }
  }
 }
 puts("KIM lifecycle checks passed");
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='kim-lifecycle-') as directory:
    test = Path(directory) / 'test.c'
    binary = Path(directory) / 'test'
    test.write_text(harness + functions + cases)
    subprocess.run(['cc', '-std=gnu89', '-Wall', '-Wextra', '-Werror',
                    str(test), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
