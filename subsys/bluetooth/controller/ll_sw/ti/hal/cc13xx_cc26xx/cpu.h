/*
 * Copyright (c) 2016-2021 Nordic Semiconductor ASA
 * Copyright (c) 2016 Vinayak Kariappa Chettimada
 *
 * SPDX-License-Identifier: Apache-2.0
 */

static inline void cpu_sleep(void)
{
#if defined(CONFIG_CPU_CORTEX_M0) || \
	defined(CONFIG_CPU_CORTEX_M4) || \
	defined(CONFIG_CPU_CORTEX_M33) || \
	defined(CONFIG_SOC_SERIES_BSIM_NRFXX)
	__WFE();
	/* __SEV(); */
	__WFE();
#endif
}

static inline void cpu_dmb(void)
{
	/* FIXME: Add necessary host machine required Data Memory Barrier
	 *        instruction alongwith the below defined compiler memory
	 *        clobber.
	 */
	__asm__ volatile ("" : : : "memory");
}
