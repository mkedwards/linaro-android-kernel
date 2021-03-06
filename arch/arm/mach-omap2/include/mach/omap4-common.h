/*
 * omap4-common.h: OMAP4 specific common header file
 *
 * Copyright (C) 2010 Texas Instruments, Inc.
 *
 * Author:
 *	Santosh Shilimkar <santosh.shilimkar@ti.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#ifndef OMAP_ARCH_OMAP4_COMMON_H
#define OMAP_ARCH_OMAP4_COMMON_H

/* Used to implement memory barrier on DRAM path */
#define OMAP4_DRAM_BARRIER_VA		0xfe600000

#ifndef __ASSEMBLER__

#ifdef CONFIG_CACHE_L2X0
extern void __iomem *omap4_get_l2cache_base(void);
#endif

#ifdef CONFIG_SMP
extern void __iomem *omap4_get_scu_base(void);
#else
static inline void __iomem *omap4_get_scu_base(void)
{
	return NULL;
}
#endif

extern void __init gic_init_irq(void);
extern void omap_smc1(u32 fn, u32 arg);
extern void omap_bus_sync(void);
extern unsigned long omap_get_dram_barrier_base(void);
extern void omap_do_wfi(void);
extern void __iomem *omap4_get_sar_ram_base(void);

#ifdef CONFIG_SMP
/* Needed for secondary core boot */
extern void omap_secondary_startup(void);
extern u32 omap_modify_auxcoreboot0(u32 set_mask, u32 clear_mask);
extern void omap_auxcoreboot_addr(u32 cpu_addr);
extern u32 omap_read_auxcoreboot0(void);
#endif

#if defined(CONFIG_SMP) && defined(CONFIG_PM)
extern int omap4_mpuss_init(void);
extern int omap4_enter_lowpower(unsigned int cpu, unsigned int power_state);
extern int omap4_finish_suspend(unsigned long cpu_state);
extern void omap4_cpu_resume(void);
extern int omap4_hotplug_cpu(unsigned int cpu, unsigned int power_state);
extern u32 omap4_mpuss_read_prev_context_state(void);
#else
static inline int omap4_enter_lowpower(unsigned int cpu,
					unsigned int power_state)
{
	omap_do_wfi();
	return 0;
}

static inline int omap4_hotplug_cpu(unsigned int cpu, unsigned int power_state)
{
	omap_do_wfi();
	return 0;
}

static inline int omap4_mpuss_init(void)
{
	return 0;
}
static inline u32 omap4_mpuss_read_prev_context_state(void)
{
	return 0;
}
static inline int omap4_finish_suspend(unsigned long cpu_state)
{}
static inline void omap4_cpu_resume(void)
{}
static inline u32 omap4_mpuss_read_prev_context_state(void)
{
	return 0;
}
#endif

#endif /* __ASSEMBLER__ */
#endif /* OMAP_ARCH_OMAP4_COMMON_H */
