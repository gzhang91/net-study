/*
 * LSAPIC Interrupt Controller
 *
 * This takes care of interrupts that are generated by the CPU's
 * internal Streamlined Advanced Programmable Interrupt Controller
 * (LSAPIC), such as the ITC and IPI interrupts.
    *
 * Copyright (C) 1999 VA Linux Systems
 * Copyright (C) 1999 Walt Drummond <drummond@valinux.com>
 * Copyright (C) 2000 Hewlett-Packard Co
 * Copyright (C) 2000 David Mosberger-Tang <davidm@hpl.hp.com>
 */

#include <linux/sched.h>
#include <linux/irq.h>

static unsigned int
lsapic_noop_startup (unsigned int irq)
{
	return 0;
}

static void
lsapic_noop (unsigned int irq)
{
	/* nuthing to do... */
}

static int lsapic_retrigger(unsigned int irq)
{
	ia64_resend_irq(irq);

	return 1;
}

struct hw_interrupt_type irq_type_ia64_lsapic = {
	.name =		"LSAPIC",
	.startup =	lsapic_noop_startup,
	.shutdown =	lsapic_noop,
	.enable =	lsapic_noop,
	.disable =	lsapic_noop,
	.ack =		lsapic_noop,
	.end =		lsapic_noop,
	.retrigger =	lsapic_retrigger,
};
