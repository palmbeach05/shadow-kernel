#!/usr/bin/env python3
"""Compile the actual OMAP resume callbacks against fault-injecting host stubs."""
import pathlib
import re
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]


def function(path, name):
    source = (ROOT / path).read_text()
    match = re.search(r"static int " + name + r"\([^;]*?\)\n\{", source)
    start = match.start()
    end = match.end()
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


omap = "drivers/usb/musb/omap2430.c"
source = (ROOT / omap).read_text()
constants = "\n".join(re.findall(r"^#define OMAP_MUSB_RESUME_.*$", source, re.M))
preamble = r'''
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#define ENABLEFORCE 1
#define OTG_FORCESTDBY 2
struct device { int unused; };
struct musb {
    struct { int quiesced, clock_off; } omap_context;
    int (*set_clock)(void *, int);
    void *clock, *xceiv;
    struct device *controller;
    int lock;
    int (*pm_resume)(struct musb *);
    void (*pm_complete)(struct musb *);
};
static int refs, enables, disables, wrappers, phys, cores, completes, sleeps;
static int clock_fail, wrapper_fail, phy_fail, cleanup_fail, logs;
static int fails(int *count) { if (*count) { --*count; return 1; } return 0; }
static int clock_set(void *clock, int on) {
    (void)clock;
    if (on) {
        enables++;
        if (fails(&clock_fail)) return -EIO;
        assert(refs == 0); refs++;
    } else {
        disables++;
        if (fails(&cleanup_fail)) return -EIO;
        assert(refs == 1); refs--;
    }
    return 0;
}
static int clk_enable(void *clock) { return clock_set(clock, 1); }
static void clk_disable(void *clock) { assert(!clock_set(clock, 0)); }
static int omap_musb_restore_wrapper(struct musb *m) {
    assert(m->omap_context.quiesced && refs == 1);
    wrappers++;
    return fails(&wrapper_fail) ? -EIO : 0;
}
static int otg_set_suspend(void *phy, int suspend) {
    (void)phy; assert(!suspend && refs == 1); phys++;
    return fails(&phy_fail) ? -EIO : 0;
}
static void omap_writel(int value, int reg) {
    (void)value; (void)reg; assert(refs == 1);
}
static void omap_readl(int reg) { (void)reg; assert(refs == 1); }
#define dev_err(dev, ...) do { (void)(dev); logs++; } while (0)
static void msleep(int ms) { assert(ms == 20); sleeps++; }
#define spin_lock_irqsave(lock, flags) do { (void)(lock); (flags) = 0; } while (0)
#define spin_unlock_irqrestore(lock, flags) do { (void)(lock); (void)(flags); } while (0)
static void musb_restore_context(struct musb *m) {
    assert(m->omap_context.quiesced && refs == 1 && phys); cores++;
}
static void complete(struct musb *m) {
    assert(cores == 1 && refs == 1 && m->omap_context.quiesced);
    completes++; m->omap_context.quiesced = 0;
}
static struct musb *dev_to_musb(struct device *dev) { return (struct musb *)dev; }
'''
main = r'''
static struct musb reset(int callback) {
    struct musb m;
    memset(&m, 0, sizeof(m));
    refs = enables = disables = wrappers = phys = cores = completes = sleeps = 0;
    clock_fail = wrapper_fail = phy_fail = cleanup_fail = logs = 0;
    m.clock = (void *)1;
    m.omap_context.quiesced = m.omap_context.clock_off = 1;
    m.set_clock = callback ? clock_set : 0;
    m.pm_resume = omap_musb_pm_resume;
    m.pm_complete = complete;
    return m;
}
static int resume(struct musb *m) { return musb_resume((struct device *)m); }
int main(void) {
    struct musb m;
    int callback;
    for (callback = 0; callback <= 1; callback++) {
        m = reset(callback);
        assert(!resume(&m));
        assert(enables == 1 && !sleeps && completes == 1 && refs == 1);
        m = reset(callback); clock_fail = 1;
        assert(!resume(&m));
        assert(enables == 2 && wrappers == 1 && sleeps == 1 && completes == 1);
        m = reset(callback); wrapper_fail = 1;
        assert(!resume(&m));
        assert(enables == 2 && disables == 1 && phys == 1 && refs == 1);
        m = reset(callback); phy_fail = 1;
        assert(!resume(&m));
        assert(enables == 2 && disables == 1 && phys == 2 && completes == 1);
        m = reset(callback); clock_fail = 99;
        assert(resume(&m) == -EIO);
        assert(enables == 3 && sleeps == 2 && !wrappers && !cores && !completes);
        assert(!refs && m.omap_context.quiesced && logs == 1);
        m = reset(callback); phy_fail = 99;
        assert(resume(&m) == -EIO);
        assert(enables == 3 && disables == 3 && sleeps == 2 && !cores && !completes);
        assert(!refs && m.omap_context.quiesced && m.omap_context.clock_off);
    }
    m = reset(1); phy_fail = 1; cleanup_fail = 1;
    assert(!resume(&m));
    assert(enables == 1 && disables == 1 && refs == 1 && completes == 1);
    /* A failed disable retains ownership; no duplicate enable on retry. */
    m = reset(1); m.omap_context.quiesced = 0;
    assert(resume(&m) == -EINVAL && !enables && !sleeps && !cores);
    puts("MUSB extracted-source resume fault checks passed");
    return 0;
}
'''
program = "\n".join([preamble, constants,
                      function(omap, "omap_musb_pm_resume_once"),
                      function(omap, "omap_musb_pm_resume"),
                      function("drivers/usb/musb/musb_core.c", "musb_resume"), main])
with tempfile.TemporaryDirectory(prefix="musb-resume-") as directory:
    c_file = pathlib.Path(directory) / "test.c"
    executable = pathlib.Path(directory) / "test"
    c_file.write_text(program)
    subprocess.run(["gcc", "-std=gnu89", "-Wall", "-Wextra", "-Werror",
                    str(c_file), "-o", str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
