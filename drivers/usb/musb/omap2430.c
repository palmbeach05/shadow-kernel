/*
 * Copyright (C) 2005-2007 by Texas Instruments
 * Some code has been taken from tusb6010.c
 * Copyrights for that are attributable to:
 * Copyright (C) 2006 Nokia Corporation
 * Tony Lindgren <tony@atomide.com>
 *
 * This file is part of the Inventra Controller Driver for Linux.
 *
 * The Inventra Controller Driver for Linux is free software; you
 * can redistribute it and/or modify it under the terms of the GNU
 * General Public License version 2 as published by the Free Software
 * Foundation.
 *
 * The Inventra Controller Driver for Linux is distributed in
 * the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public
 * License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with The Inventra Controller Driver for Linux ; if not,
 * write to the Free Software Foundation, Inc., 59 Temple Place,
 * Suite 330, Boston, MA  02111-1307  USA
 *
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/list.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/io.h>

#include <asm/mach-types.h>
#include <mach/hardware.h>
#include <plat/mux.h>

#include "musb_core.h"
#include "omap2430.h"

#ifdef CONFIG_ARCH_OMAP3430
#define	get_cpu_rev()	2
#endif


static struct timer_list musb_idle_timer;

static void musb_do_idle(unsigned long _musb)
{
	struct musb	*musb = (void *)_musb;
	unsigned long	flags;
#ifdef CONFIG_USB_MUSB_HDRC_HCD
	u8	power;
#endif
	u8	devctl;

	spin_lock_irqsave(&musb->lock, flags);

	devctl = musb_readb(musb->mregs, MUSB_DEVCTL);

	switch (musb->xceiv->state) {
	case OTG_STATE_A_WAIT_BCON:
		devctl &= ~MUSB_DEVCTL_SESSION;
		musb_writeb(musb->mregs, MUSB_DEVCTL, devctl);

		devctl = musb_readb(musb->mregs, MUSB_DEVCTL);
		if (devctl & MUSB_DEVCTL_BDEVICE) {
			musb->xceiv->state = OTG_STATE_B_IDLE;
			MUSB_DEV_MODE(musb);
		} else {
			musb->xceiv->state = OTG_STATE_A_IDLE;
			MUSB_HST_MODE(musb);
		}
		break;
#ifdef CONFIG_USB_MUSB_HDRC_HCD
	case OTG_STATE_A_SUSPEND:
		/* finish RESUME signaling? */
		if (musb->port1_status & MUSB_PORT_STAT_RESUME) {
			power = musb_readb(musb->mregs, MUSB_POWER);
			power &= ~MUSB_POWER_RESUME;
			DBG(1, "root port resume stopped, power %02x\n", power);
			musb_writeb(musb->mregs, MUSB_POWER, power);
			musb->is_active = 1;
			musb->port1_status &= ~(USB_PORT_STAT_SUSPEND
						| MUSB_PORT_STAT_RESUME);
			musb->port1_status |= USB_PORT_STAT_C_SUSPEND << 16;
			usb_hcd_poll_rh_status(musb_to_hcd(musb));
			/* NOTE: it might really be A_WAIT_BCON ... */
			musb->xceiv->state = OTG_STATE_A_HOST;
		}
		break;
#endif
#ifdef CONFIG_USB_MUSB_HDRC_HCD
	case OTG_STATE_A_HOST:
		devctl = musb_readb(musb->mregs, MUSB_DEVCTL);
		if (devctl &  MUSB_DEVCTL_BDEVICE)
			musb->xceiv->state = OTG_STATE_B_IDLE;
		else
			musb->xceiv->state = OTG_STATE_A_WAIT_BCON;
#endif
	default:
		break;
	}
	spin_unlock_irqrestore(&musb->lock, flags);
}


void musb_platform_try_idle(struct musb *musb, unsigned long timeout)
{
	unsigned long		default_timeout = jiffies + msecs_to_jiffies(3);
	static unsigned long	last_timer;

	if (timeout == 0)
		timeout = default_timeout;

	/* Never idle if active, or when VBUS timeout is not set as host */
	if (musb->is_active || ((musb->a_wait_bcon == 0)
			&& (musb->xceiv->state == OTG_STATE_A_WAIT_BCON))) {
		DBG(4, "%s active, deleting timer\n", otg_state_string(musb));
		del_timer(&musb_idle_timer);
		last_timer = jiffies;
		return;
	}

	if (time_after(last_timer, timeout)) {
		if (!timer_pending(&musb_idle_timer))
			last_timer = timeout;
		else {
			DBG(4, "Longer idle timer already pending, ignoring\n");
			return;
		}
	}
	last_timer = timeout;

	DBG(4, "%s inactive, for idle timer for %lu ms\n",
		otg_state_string(musb),
		(unsigned long)jiffies_to_msecs(timeout - jiffies));
	mod_timer(&musb_idle_timer, timeout);
}

void musb_platform_enable(struct musb *musb)
{
}
void musb_platform_disable(struct musb *musb)
{
}
static void omap_vbus_power(struct musb *musb, int is_on, int sleeping)
{
}

static void omap_set_vbus(struct musb *musb, int is_on)
{
	u8		devctl;
	/* HDRC controls CPEN, but beware current surges during device
	 * connect.  They can trigger transient overcurrent conditions
	 * that must be ignored.
	 */

	devctl = musb_readb(musb->mregs, MUSB_DEVCTL);

	if (is_on) {
		musb->is_active = 1;
		musb->xceiv->default_a = 1;
		musb->xceiv->state = OTG_STATE_A_WAIT_VRISE;
		devctl |= MUSB_DEVCTL_SESSION;

		MUSB_HST_MODE(musb);
	} else {
		musb->is_active = 0;

		/* NOTE:  we're skipping A_WAIT_VFALL -> A_IDLE and
		 * jumping right to B_IDLE...
		 */

		musb->xceiv->default_a = 0;
		musb->xceiv->state = OTG_STATE_B_IDLE;
		devctl &= ~MUSB_DEVCTL_SESSION;

		MUSB_DEV_MODE(musb);
	}
	musb_writeb(musb->mregs, MUSB_DEVCTL, devctl);

	DBG(1, "VBUS %s, devctl %02x "
		/* otg %3x conf %08x prcm %08x */ "\n",
		otg_state_string(musb),
		musb_readb(musb->mregs, MUSB_DEVCTL));
}
static int omap_set_power(struct otg_transceiver *x, unsigned mA)
{
	return 0;
}

#if defined(CONFIG_PM) && defined(CONFIG_ARCH_OMAP3430)
static int omap_musb_pm_prepare(struct musb *musb);
static int omap_musb_pm_suspend(struct musb *musb);
static int omap_musb_pm_resume(struct musb *musb);
static void omap_musb_pm_complete(struct musb *musb);
#endif

int musb_platform_set_mode(struct musb *musb, u8 musb_mode)
{
	u8	devctl = musb_readb(musb->mregs, MUSB_DEVCTL);

	devctl |= MUSB_DEVCTL_SESSION;
	musb_writeb(musb->mregs, MUSB_DEVCTL, devctl);

	switch (musb_mode) {
#ifdef CONFIG_USB_MUSB_HDRC_HCD
	case MUSB_HOST:
		otg_set_host(musb->xceiv, musb->xceiv->host);
		break;
#endif
#ifdef CONFIG_USB_GADGET_MUSB_HDRC
	case MUSB_PERIPHERAL:
		otg_set_peripheral(musb->xceiv, musb->xceiv->gadget);
		break;
#endif
#ifdef CONFIG_USB_MUSB_OTG
	case MUSB_OTG:
		break;
#endif
	default:
		return -EINVAL;
	}
	return 0;
}

int __init musb_platform_init(struct musb *musb)
{
	u32 l;

#if defined(CONFIG_ARCH_OMAP2430)
	omap_cfg_reg(AE5_2430_USB0HS_STP);
#endif

	/* Reset controller */
	if (musb->set_clock)
		musb->set_clock(musb->clock, 1);
	else
		clk_enable(musb->clock);
	l = omap_readl(OTG_SYSCONFIG);
	l |= SOFTRST;
	omap_writel(l, OTG_SYSCONFIG);
	while (!(RESETDONE & omap_readl(OTG_SYSSTATUS)))
		cpu_relax();

	/* We require some kind of external transceiver, hooked
	 * up through ULPI.  TWL4030-family PMICs include one,
	 * which needs a driver, drivers aren't always needed.
	 */
	musb->xceiv = otg_get_transceiver();
	if (!musb->xceiv) {
		pr_err("HS USB OTG: no transceiver configured\n");
		return -ENODEV;
	}

	/* The clock was enabled above; do not acquire it a second time. */
	otg_set_suspend(musb->xceiv, 0);

	l = omap_readl(OTG_SYSCONFIG);
	l &= ~ENABLEWAKEUP;	/* disable wakeup */
	l &= ~NOSTDBY;		/* remove possible nostdby */
	l &= ~SMARTSTDBY;	/* disable smart standby */
	l &= ~AUTOIDLE;		/* disable auto idle */
	l &= ~NOIDLE;		/* remove possible noidle */
	/*
	 * MUSB AUTOIDLE and SMARTIDLE don't work in 3430.
	 * Workaround by Richard Woodruff/TI
	 */
	if (!cpu_is_omap3430()) {
		l |= AUTOIDLE;		/* enable auto idle */
	}

	omap_writel(l, OTG_SYSCONFIG);

	l = omap_readl(OTG_INTERFSEL);
	l |= ULPI_12PIN;
	omap_writel(l, OTG_INTERFSEL);

	pr_debug("HS USB OTG: revision 0x%x, sysconfig 0x%02x, "
			"sysstatus 0x%x, intrfsel 0x%x, simenable  0x%x\n",
			omap_readl(OTG_REVISION), omap_readl(OTG_SYSCONFIG),
			omap_readl(OTG_SYSSTATUS), omap_readl(OTG_INTERFSEL),
			omap_readl(OTG_SIMENABLE));

	omap_vbus_power(musb, musb->board_mode == MUSB_HOST, 1);

	if (is_host_enabled(musb))
		musb->board_set_vbus = omap_set_vbus;
	if (!musb->xceiv->set_power && is_peripheral_enabled(musb))
		musb->xceiv->set_power = omap_set_power;

	setup_timer(&musb_idle_timer, musb_do_idle, (unsigned long) musb);

#if defined(CONFIG_PM) && defined(CONFIG_ARCH_OMAP3430)
	/* OMAP2430 retention and PHY sequencing need separate validation. */
	if (cpu_is_omap3430() || cpu_is_omap3630()) {
		mutex_init(&musb->omap_context.cpcap_lock);
		musb->pm_prepare = omap_musb_pm_prepare;
		musb->pm_suspend = omap_musb_pm_suspend;
		musb->pm_resume = omap_musb_pm_resume;
		musb->pm_complete = omap_musb_pm_complete;
	}
#endif

	return 0;
}

int musb_platform_suspend(struct musb *musb)
{
	u32 l;

	if (!musb->clock)
		return 0;

	/* in any role */
	l = omap_readl(OTG_FORCESTDBY);
	l |= ENABLEFORCE;	/* enable MSTANDBY */
	omap_writel(l, OTG_FORCESTDBY);

	l = omap_readl(OTG_SYSCONFIG);
	l |= ENABLEWAKEUP;	/* enable wakeup */
	omap_writel(l, OTG_SYSCONFIG);

	if (musb->xceiv->set_suspend)
		musb->xceiv->set_suspend(musb->xceiv, 1);

	if (musb->set_clock)
		musb->set_clock(musb->clock, 0);
	else
		clk_disable(musb->clock);

	return 0;
}

#if defined(CONFIG_PM) && defined(CONFIG_ARCH_OMAP3430)
/* All four callbacks run in the regular (sleepable) system PM phase. */
static int omap_musb_pm_prepare(struct musb *musb)
{
	unsigned long flags;

	/* Wait for the whole cable callback, including unlocked givebacks. */
	mutex_lock(&musb->omap_context.cpcap_lock);
	spin_lock_irqsave(&musb->lock, flags);
	/* A failed resume must be retried before another context save. */
	if (musb->omap_context.quiesced) {
		spin_unlock_irqrestore(&musb->lock, flags);
		mutex_unlock(&musb->omap_context.cpcap_lock);
		return -EBUSY;
	}
	/* OMAP36xx supports only 12-pin SDR ULPI (SWPU177AA 22.1.5.1). */
	if (cpu_is_omap3630() &&
			(omap_readl(OTG_INTERFSEL) & 3) != ULPI_12PIN) {
		spin_unlock_irqrestore(&musb->lock, flags);
		mutex_unlock(&musb->omap_context.cpcap_lock);
		return -EINVAL;
	}
	musb->omap_context.quiesced = 1;
	spin_unlock_irqrestore(&musb->lock, flags);
	mutex_unlock(&musb->omap_context.cpcap_lock);
	/* Neither interrupts nor timers may touch an unclocked core. */
	disable_irq(musb->nIrq);
	musb->omap_context.dma_irq = 0;
#ifdef CONFIG_USB_INVENTRA_DMA
	if (musb->dma_controller) {
		int irq = platform_get_irq_byname(
			to_platform_device(musb->controller), "dma");

		if (irq > 0 && irq != musb->nIrq) {
			disable_irq(irq);
			musb->omap_context.dma_irq = irq;
		}
	}
#endif
#ifdef CONFIG_USB_MUSB_OTG
	musb->omap_context.otg_pending = del_timer_sync(&musb->otg_timer);
	musb->omap_context.otg_expires = musb->otg_timer.expires;
#endif
	musb->omap_context.idle_pending = del_timer_sync(&musb_idle_timer);
	musb->omap_context.idle_expires = musb_idle_timer.expires;
	return 0;
}

static void omap_musb_pm_complete(struct musb *musb)
{
	unsigned long flags;

	mutex_lock(&musb->omap_context.cpcap_lock);
	spin_lock_irqsave(&musb->lock, flags);
#ifdef CONFIG_USB_MUSB_OTG
	if (musb->omap_context.otg_pending) {
		mod_timer(&musb->otg_timer,
			time_after(musb->omap_context.otg_expires, jiffies) ?
			musb->omap_context.otg_expires : jiffies + 1);
		musb->omap_context.otg_pending = 0;
	}
#endif
	if (musb->omap_context.idle_pending) {
		mod_timer(&musb_idle_timer,
			time_after(musb->omap_context.idle_expires, jiffies) ?
			musb->omap_context.idle_expires : jiffies + 1);
		musb->omap_context.idle_pending = 0;
	}
	musb->omap_context.quiesced = 0;
#ifdef CONFIG_USB_GADGET_MUSB_HDRC
	musb_cpcap_resume(musb);
#endif
	spin_unlock_irqrestore(&musb->lock, flags);
	mutex_unlock(&musb->omap_context.cpcap_lock);
	if (musb->omap_context.dma_irq)
		enable_irq(musb->omap_context.dma_irq);
	enable_irq(musb->nIrq);
}

static int omap_musb_restore_wrapper(struct musb *musb)
{
	u32 sysconfig = musb->omap_context.sysconfig;
	u32 interfsel = omap_readl(OTG_INTERFSEL) & 3;

	/* CPCAP's callback cannot stop the external 60 MHz clock.  The
	 * supported OMAP3630 selection also matches its reset value.  Fail
	 * closed on a mismatch rather than switch interfaces with FCLK on.
	 */
	if (cpu_is_omap3630() && interfsel != musb->omap_context.interfsel)
		return -EIO;

	/* SPRUF98Y 23.1.5.4 / SWPU177AA 22.1.5.4: release force first.
	 * Do not enable AUTOIDLE in the same write as smart-idle.
	 * Replay each CPU's saved AUTOIDLE policy (3430 workaround included).
	 */
	omap_writel(0, OTG_FORCESTDBY);
	omap_writel(NOSTDBY | NOIDLE, OTG_SYSCONFIG);
	/* SWPU177AA 22.1.4.3.1 forbids changing PHYSEL with FCLK running.
	 * OMAP3630 resets to 12-pin ULPI already; leave a matching value alone.
	 */
	if (interfsel != musb->omap_context.interfsel)
		omap_writel(musb->omap_context.interfsel, OTG_INTERFSEL);
	if (cpu_is_omap3630())
		omap_writel(musb->omap_context.bigendian, OTG_BIGENDIAN);
	omap_writel(sysconfig & ~AUTOIDLE, OTG_SYSCONFIG);
	omap_writel(musb->omap_context.forcestandby, OTG_FORCESTDBY);
	omap_writel(sysconfig, OTG_SYSCONFIG);
	/* Flush posted wrapper writes before any Mentor core accesses. */
	omap_readl(OTG_SYSCONFIG);
	return 0;
}

static int omap_musb_pm_resume_once(struct musb *musb)
{
	int ret, undo;

	if (musb->omap_context.clock_off) {
		if (musb->set_clock)
			ret = musb->set_clock(musb->clock, 1);
		else
			ret = clk_enable(musb->clock);
		if (ret)
			return ret;
		musb->omap_context.clock_off = 0;
	}

	ret = omap_musb_restore_wrapper(musb);
	if (ret)
		goto cleanup;
	/* TWL4030 restores power, PHY clock and ULPI mode over I2C.
	 * CPCAP retains its board-managed configuration: its callback is a
	 * no-op.  Do not reset it or duplicate cpcap-usb-det's VUSB ownership.
	 * The callback must finish before the core context is restored.
	 */
	ret = otg_set_suspend(musb->xceiv, 0);
cleanup:
	if (ret) {
		/* Leave the IRQ/timer quiesced and release our clock reference.
		 * A later retry must not enable an already enabled clock.
		 */
		omap_writel(ENABLEFORCE, OTG_FORCESTDBY);
		omap_readl(OTG_FORCESTDBY);
		if (musb->set_clock) {
			undo = musb->set_clock(musb->clock, 0);
			if (!undo)
				musb->omap_context.clock_off = 1;
			else
				dev_err(musb->controller,
					"resume clock cleanup failed: %d\n", undo);
		} else {
			clk_disable(musb->clock);
			musb->omap_context.clock_off = 1;
		}
	}
	return ret;
}

#define OMAP_MUSB_RESUME_ATTEMPTS 3
#define OMAP_MUSB_RESUME_DELAY_MS 20

static int omap_musb_pm_resume(struct musb *musb)
{
	int attempt, ret;

	if (!musb->omap_context.quiesced)
		return -EINVAL;

	/* Regular PM and detach hold dev->sem for the whole callback.
	 * Keep retries sleepable and leave core restore/completion to the caller.
	 */
	for (attempt = 0; attempt < OMAP_MUSB_RESUME_ATTEMPTS; attempt++) {
		if (attempt)
			msleep(OMAP_MUSB_RESUME_DELAY_MS);
		ret = omap_musb_pm_resume_once(musb);
		if (!ret)
			return 0;
	}

	dev_err(musb->controller,
		"resume failed after %d attempts: %d; controller remains quiesced\n",
		OMAP_MUSB_RESUME_ATTEMPTS, ret);
	return ret;
}

static int omap_musb_pm_suspend(struct musb *musb)
{
	int ret, undo;

	/* Core context has already been saved with the clock enabled. */
	musb->omap_context.sysconfig = omap_readl(OTG_SYSCONFIG) & ~SOFTRST;
	musb->omap_context.interfsel = omap_readl(OTG_INTERFSEL) & 3;
	musb->omap_context.forcestandby = omap_readl(OTG_FORCESTDBY) & ENABLEFORCE;
	if (cpu_is_omap3630())
		musb->omap_context.bigendian =
			omap_readl(OTG_BIGENDIAN) & BIGENDIAN;

	omap_writel(ENABLEFORCE, OTG_FORCESTDBY);
	omap_writel(musb->omap_context.sysconfig | ENABLEWAKEUP,
		OTG_SYSCONFIG);
	omap_readl(OTG_SYSCONFIG);
	ret = otg_set_suspend(musb->xceiv, 1);
	if (ret)
		goto undo_suspend;

	if (musb->set_clock) {
		ret = musb->set_clock(musb->clock, 0);
		if (ret)
			goto undo_suspend;
	} else {
		clk_disable(musb->clock);
	}
	musb->omap_context.clock_off = 1;
	return 0;

undo_suspend:
	undo = omap_musb_pm_resume(musb);
	if (!undo)
		omap_musb_pm_complete(musb);
	else
		dev_err(musb->controller, "suspend rollback failed: %d\n", undo);
	return ret;
}
#endif

int musb_platform_exit(struct musb *musb)
{
	del_timer_sync(&musb_idle_timer);

	omap_vbus_power(musb, 0 /*off*/, 1);

	musb_platform_suspend(musb);

	clk_put(musb->clock);
	musb->clock = 0;

	return 0;
}
