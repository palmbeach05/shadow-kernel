/*
 * OMAP3 clock function prototypes and shared declarations
 *
 * Copyright (C) 2007-2009 Texas Instruments, Inc.
 * Copyright (C) 2007-2009 Nokia Corporation
 */
#ifndef __ARCH_ARM_MACH_OMAP2_CLOCK34XX_H
#define __ARCH_ARM_MACH_OMAP2_CLOCK34XX_H

#include <plat/clock.h>

#define DPLL_LOW_POWER_STOP		0x1
#define DPLL_LOW_POWER_BYPASS		0x5
#define DPLL_LOCKED			0x7

extern const struct clkops clkops_noncore_dpll_ops;
extern const struct clkops clkops_omap3430es2_ssi_wait;
extern const struct clkops clkops_omap3430es2_hsotgusb_wait;
extern const struct clkops clkops_omap3430es2_dss_usbhost_wait;
extern const struct clkops clkops_omap3630_pwrdn_bug;
extern struct clk_functions omap2_clk_functions;
extern unsigned long long cpu_hz;

unsigned long omap3_dpll_recalc(struct clk *clk);
unsigned long omap3_clkoutx2_recalc(struct clk *clk);
void omap3_dpll_allow_idle(struct clk *clk);
void omap3_dpll_deny_idle(struct clk *clk);
u32 omap3_dpll_autoidle_read(struct clk *clk);
int omap3_noncore_dpll_set_rate(struct clk *clk, unsigned long rate);
int omap3_dpll4_set_rate(struct clk *clk, unsigned long rate);
int omap3_core_dpll_m2_set_rate(struct clk *clk, unsigned long rate);
void omap3_clk_lock_dpll5(void);

extern struct clk *sdrc_ick_p;
extern struct clk *arm_fck_p;

#endif
