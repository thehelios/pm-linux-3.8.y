/*
 * OMAP4PLUS CPU idle Routines
 *
 * Copyright (C) 2011-2013 Texas Instruments, Inc.
 * Santosh Shilimkar <santosh.shilimkar@ti.com>
 * Rajendra Nayak <rnayak@ti.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/sched.h>
#include <linux/cpuidle.h>
#include <linux/cpu_pm.h>
#include <linux/export.h>
#include <linux/clockchips.h>

#include <asm/proc-fns.h>

#include "common.h"
#include "pm.h"
#include "prm.h"
#include "clockdomain.h"
#include "soc.h"

/* Machine specific information */
struct idle_statedata {
	u8 cpu_pwrst;
	u8 mpu_pwrst;
	u32 mpu_state_vote;
};

static struct idle_statedata omap4_idle_data[] = {
	{
		.cpu_pwrst = PWRDM_FUNC_PWRST_ON,
		.mpu_pwrst = PWRDM_FUNC_PWRST_ON,
	},
	{
		.cpu_pwrst = PWRDM_FUNC_PWRST_OFF,
		.mpu_pwrst = PWRDM_FUNC_PWRST_CSWR,
	},
	{
		.cpu_pwrst = PWRDM_FUNC_PWRST_OFF,
		.mpu_pwrst = PWRDM_FUNC_PWRST_OSWR,
	},
};

static struct idle_statedata omap5_idle_data[] = {
	{
		.cpu_pwrst = PWRDM_FUNC_PWRST_ON,
		.mpu_pwrst = PWRDM_FUNC_PWRST_ON,
	},
	{
		.cpu_pwrst = PWRDM_FUNC_PWRST_CSWR,
		.mpu_pwrst = PWRDM_FUNC_PWRST_CSWR,
	},
	{
		.cpu_pwrst = PWRDM_FUNC_PWRST_OFF,
		.mpu_pwrst = PWRDM_FUNC_PWRST_OSWR,
	},
};

static struct powerdomain *mpu_pd, *cpu_pd[NR_CPUS];
static struct clockdomain *cpu_clkdm[NR_CPUS];

static atomic_t abort_barrier;
static bool cpu_done[NR_CPUS];
static struct idle_statedata *state_ptr;
static DEFINE_RAW_SPINLOCK(mpu_lock);

/* Private functions */

/**
 * omap_enter_idle_[simple/smp/coupled] - OMAP4PLUS cpuidle entry functions
 * @dev: cpuidle device
 * @drv: cpuidle driver
 * @index: the index of state to be entered
 *
 * Called from the CPUidle framework to program the device to the
 * specified low power state selected by the governor.
 * Returns the amount of time spent in the low power state.
 */
static int omap_enter_idle_simple(struct cpuidle_device *dev,
			struct cpuidle_driver *drv,
			int index)
{
	omap_do_wfi();
	return index;
}

static int omap_enter_idle_coupled(struct cpuidle_device *dev,
			struct cpuidle_driver *drv,
			int index)
{
	struct idle_statedata *cx = state_ptr + index;
	int cpu_id = smp_processor_id();

	/*
	 * CPU0 has to wait and stay ON until CPU1 is OFF state.
	 * This is necessary to honour hardware recommondation
	 * of triggeing all the possible low power modes once CPU1 is
	 * out of coherency and in OFF mode.
	 */
	if (dev->cpu == 0 && cpumask_test_cpu(1, cpu_online_mask)) {
		while (pwrdm_read_fpwrst(cpu_pd[1]) != PWRDM_FUNC_PWRST_OFF) {
			cpu_relax();

			/*
			 * CPU1 could have already entered & exited idle
			 * without hitting off because of a wakeup
			 * or a failed attempt to hit off mode.  Check for
			 * that here, otherwise we could spin forever
			 * waiting for CPU1 off.
			 */
			if (cpu_done[1])
			    goto fail;

		}
	}

	clockevents_notify(CLOCK_EVT_NOTIFY_BROADCAST_ENTER, &cpu_id);

	/*
	 * Call idle CPU PM enter notifier chain so that
	 * VFP and per CPU interrupt context is saved.
	 */
	cpu_pm_enter();

	if (dev->cpu == 0) {
		WARN_ON(pwrdm_set_next_fpwrst(mpu_pd, cx->mpu_pwrst));

		/*
		 * Call idle CPU cluster PM enter notifier chain
		 * to save GIC and wakeupgen context.
		 */
		if (cx->mpu_pwrst == PWRDM_FUNC_PWRST_OSWR)
			cpu_cluster_pm_enter();
	}

	omap4_mpuss_enter_lowpower(dev->cpu, cx->cpu_pwrst);
	cpu_done[dev->cpu] = true;

	/* Wakeup CPU1 only if it is not offlined */
	if (dev->cpu == 0 && cpumask_test_cpu(1, cpu_online_mask)) {
		/* Restore MPU PD state post idle */
		pwrdm_set_next_fpwrst(mpu_pd, PWRDM_FUNC_PWRST_ON);
		clkdm_wakeup(cpu_clkdm[1]);
		pwrdm_set_next_fpwrst(cpu_pd[1], PWRDM_FUNC_PWRST_ON);
		clkdm_allow_idle(cpu_clkdm[1]);
	}

	/*
	 * Call idle CPU PM exit notifier chain to restore
	 * VFP and per CPU IRQ context.
	 */
	cpu_pm_exit();

	/*
	 * Call idle CPU cluster PM exit notifier chain
	 * to restore GIC and wakeupgen context.
	 */
	if (cx->mpu_pwrst == PWRDM_FUNC_PWRST_OSWR)
		cpu_cluster_pm_exit();

	clockevents_notify(CLOCK_EVT_NOTIFY_BROADCAST_EXIT, &cpu_id);

fail:
	cpuidle_coupled_parallel_barrier(dev, &abort_barrier);
	cpu_done[dev->cpu] = false;

	return index;
}

static int omap_enter_idle_smp(struct cpuidle_device *dev,
			struct cpuidle_driver *drv,
			int index)
{
	struct idle_statedata *cx = state_ptr + index;
	int cpu_id = smp_processor_id();
	unsigned long flag;

	clockevents_notify(CLOCK_EVT_NOTIFY_BROADCAST_ENTER, &cpu_id);

	raw_spin_lock_irqsave(&mpu_lock, flag);
	cx->mpu_state_vote++;
	if (cx->mpu_state_vote == num_online_cpus())
		pwrdm_set_next_fpwrst(mpu_pd, cx->mpu_pwrst);

	raw_spin_unlock_irqrestore(&mpu_lock, flag);

	omap4_mpuss_enter_lowpower(dev->cpu, cx->cpu_pwrst);

	raw_spin_lock_irqsave(&mpu_lock, flag);
	if (cx->mpu_state_vote == num_online_cpus())
		pwrdm_set_next_fpwrst(mpu_pd, PWRDM_FUNC_PWRST_ON);

	cx->mpu_state_vote--;
	raw_spin_unlock_irqrestore(&mpu_lock, flag);

	clockevents_notify(CLOCK_EVT_NOTIFY_BROADCAST_EXIT, &cpu_id);

	return index;
}

/*
 * For each cpu, setup the broadcast timer because local timers
 * stops for the states above C1.
 */
static void omap_setup_broadcast_timer(void *arg)
{
	int cpu = smp_processor_id();
	clockevents_notify(CLOCK_EVT_NOTIFY_BROADCAST_ON, &cpu);
}

static DEFINE_PER_CPU(struct cpuidle_device, omap_idle_dev);

static struct cpuidle_driver omap4_idle_driver = {
	.name				= "omap4_idle",
	.owner				= THIS_MODULE,
	.en_core_tk_irqen		= 1,
	.states = {
		{
			/* C1 - CPU0 ON + CPU1 ON + MPU ON */
			.exit_latency = 2 + 2,
			.target_residency = 5,
			.flags = CPUIDLE_FLAG_TIME_VALID,
			.enter = omap_enter_idle_simple,
			.name = "C1",
			.desc = "MPUSS ON"
		},
		{
			/* C2 - CPU0 OFF + CPU1 OFF + MPU CSWR */
			.exit_latency = 328 + 440,
			.target_residency = 960,
			.flags = CPUIDLE_FLAG_TIME_VALID | CPUIDLE_FLAG_COUPLED,
			.enter = omap_enter_idle_coupled,
			.name = "C2",
			.desc = "MPUSS CSWR",
		},
		{
			/* C3 - CPU0 OFF + CPU1 OFF + MPU OSWR */
			.exit_latency = 460 + 518,
			.target_residency = 1100,
			.flags = CPUIDLE_FLAG_TIME_VALID | CPUIDLE_FLAG_COUPLED,
			.enter = omap_enter_idle_coupled,
			.name = "C3",
			.desc = "MPUSS OSWR",
		},
	},
	.state_count = ARRAY_SIZE(omap4_idle_data),
	.safe_state_index = 0,
};

static struct cpuidle_driver omap5_idle_driver = {
	.name				= "omap5_idle",
	.owner				= THIS_MODULE,
	.en_core_tk_irqen		= 1,
	.states = {
		{
			/* C1 - CPU0 ON + CPU1 ON + MPU ON */
			.exit_latency = 2 + 2,
			.target_residency = 5,
			.flags = CPUIDLE_FLAG_TIME_VALID,
			.enter = omap_enter_idle_simple,
			.name = "C1",
			.desc = "MPUSS ON"
		},
		{
			/* C2 - CPU0 CSWR + CPU1 CSWR + MPU CSWR */
			.exit_latency = 16 + 16,
			.target_residency = 40,
			.flags = CPUIDLE_FLAG_TIME_VALID,
			.enter = omap_enter_idle_smp,
			.name = "C2",
			.desc = "MPUSS CSWR",
		},
		{
			/* C3 - CPU0 OFF + CPU1 OFF + MPU OSWR */
			.exit_latency = 460 + 518,
			.target_residency = 1100,
			.flags = CPUIDLE_FLAG_TIME_VALID | CPUIDLE_FLAG_COUPLED,
			.enter = omap_enter_idle_coupled,
			.name = "C3",
			.desc = "MPUSS OSWR",
		},
	},
	.state_count = ARRAY_SIZE(omap5_idle_data),
	.safe_state_index = 0,
};

/* Public functions */

/**
 * omap4_idle_init - Init routine for OMAP4 idle
 *
 * Registers the OMAP4 specific cpuidle driver to the cpuidle
 * framework with the valid set of states.
 */
int __init omap4_idle_init(void)
{
	struct cpuidle_device *dev;
	unsigned cpu_id = 0;

	/* Configure the broadcast timer on each cpu */
	mpu_pd = pwrdm_lookup("mpu_pwrdm");
	cpu_pd[0] = pwrdm_lookup("cpu0_pwrdm");
	cpu_pd[1] = pwrdm_lookup("cpu1_pwrdm");
	if ((!mpu_pd) || (!cpu_pd[0]) || (!cpu_pd[1]))
		return -ENODEV;

	cpu_clkdm[0] = clkdm_lookup("mpu0_clkdm");
	cpu_clkdm[1] = clkdm_lookup("mpu1_clkdm");
	if (!cpu_clkdm[0] || !cpu_clkdm[1])
		return -ENODEV;

	/* Configure the broadcast timer on each cpu */
	on_each_cpu(omap_setup_broadcast_timer, NULL, 1);

	for_each_cpu(cpu_id, cpu_online_mask) {
		dev = &per_cpu(omap_idle_dev, cpu_id);
		dev->cpu = cpu_id;
#ifdef CONFIG_ARCH_NEEDS_CPU_IDLE_COUPLED
		dev->coupled_cpus = *cpu_online_mask;
#endif
		if (cpu_is_omap44xx()) {
			state_ptr = &omap4_idle_data[0];
			cpuidle_register_driver(&omap4_idle_driver);
		} else if (soc_is_omap54xx()) {
			state_ptr = &omap5_idle_data[0];
			cpuidle_register_driver(&omap5_idle_driver);
		}

		if (cpuidle_register_device(dev)) {
			pr_err("%s: CPUidle register failed\n", __func__);
			return -EIO;
		}
	}

	return 0;
}
