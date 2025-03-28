/* Copyright 2020 IBM Corp.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * 	http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
 * implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <stdio.h>
#include <inttypes.h>

#include "hwunit.h"
#include "bitutils.h"
#include "operations.h"
#include "chip.h"
#include "debug.h"

/*
 * NOTE!
 * All timeouts and scom procedures in general through the file should be kept
 * in synch with skiboot (e.g., core/direct-controls.c) as far as possible.
 * If you fix a bug here, fix it in skiboot, and vice versa.
 */

#define P10_CORE_THREAD_STATE	0x28412
#define P10_THREAD_INFO		0x28413
#define P10_DIRECT_CONTROL	0x28449
#define P10_RAS_STATUS		0x28454

/* PCB Slave registers */
#define QME_SSH_FSP		0xE8824
#define  SPECIAL_WKUP_DONE	PPC_BIT(1)
#define QME_SPWU_FSP		0xE8834

#define RAS_STATUS_TIMEOUT	100 /* 100ms */
#define SPECIAL_WKUP_TIMEOUT	100 /* 100ms */

static int thread_read(struct thread *thread, uint64_t addr, uint64_t *data)
{
	struct pdbg_target *core = pdbg_target_require_parent("core", &thread->target);

	return pib_read(core, addr, data);
}

static uint64_t thread_write(struct thread *thread, uint64_t addr, uint64_t data)
{
	struct pdbg_target *chip = pdbg_target_require_parent("core", &thread->target);

	return pib_write(chip, addr, data);
}

struct thread_state p10_thread_state(struct thread *thread)
{
	struct thread_state thread_state;
	uint64_t value;
	bool maint_mode, thread_quiesced, ict_empty;
	uint8_t smt_mode;

	thread_read(thread, P10_RAS_STATUS, &value);

	maint_mode	= (value & PPC_BIT(0 + 8*thread->id));
	thread_quiesced	= (value & PPC_BIT(1 + 8*thread->id));
	ict_empty	= (value & PPC_BIT(2 + 8*thread->id));

	/*
	 * RAM mode (if implemented) additionally requires bit 3 (LSU quiesce)
	 * to be set. RAM mode has other requirements as well, so those could
	 * all be put into ram_setup.
	 */
	if (maint_mode && thread_quiesced && ict_empty)
		thread_state.quiesced = true;
	else
		thread_state.quiesced = false;

	thread_read(thread, P10_THREAD_INFO, &value);
	thread_state.active = !!(value & PPC_BIT(thread->id));

	smt_mode = GETFIELD(PPC_BITMASK(8,9), value);
	switch (smt_mode) {
	case 0:
		thread_state.smt_state = PDBG_SMT_1;
		break;

	case 2:
		thread_state.smt_state = PDBG_SMT_2;
		break;

	case 3:
		thread_state.smt_state = PDBG_SMT_4;
		break;

	default:
		thread_state.smt_state = PDBG_SMT_UNKNOWN;
		break;
	}

	thread_read(thread, P10_CORE_THREAD_STATE, &value);
	if (value & PPC_BIT(56 + thread->id))
		thread_state.sleep_state = PDBG_THREAD_STATE_STOP;
	else
		thread_state.sleep_state = PDBG_THREAD_STATE_RUN;

	thread_state.lpar_per_thread = !(value & PPC_BIT(62));
	thread_state.fused_core_mode = !!(value & PPC_BIT(63));

	return thread_state;
}

static int p10_thread_probe(struct pdbg_target *target)
{
	struct thread *thread = target_to_thread(target);

	thread->id = pdbg_target_index(target);
	thread->status = thread->state(thread);

	return 0;
}

static void p10_thread_release(struct pdbg_target *target)
{
	struct core *core = target_to_core(pdbg_target_require_parent("core", target));
	struct thread *thread = target_to_thread(target);

	if (thread->status.quiesced)
		/* This thread is still quiesced so don't release spwkup */
		core->release_spwkup = false;
}

static int p10_thread_start(struct thread *thread)
{
	if (!(thread->status.quiesced))
		return 1;

	if ((!(thread->status.active)) ||
	    (thread->status.sleep_state == PDBG_THREAD_STATE_STOP)) {
		/* Inactive or active and stopped: Clear Maint */
		thread_write(thread, P10_DIRECT_CONTROL, PPC_BIT(3 + 8*thread->id));
	} else {
		/* Active and not stopped: Start */
		thread_write(thread, P10_DIRECT_CONTROL, PPC_BIT(6 + 8*thread->id));
	}

	thread->status = thread->state(thread);

	return 0;
}

static int p10_thread_stop(struct thread *thread)
{
	int i = 0;

	thread_write(thread, P10_DIRECT_CONTROL, PPC_BIT(7 + 8*thread->id));
	do {
		usleep(1000);
		if (i++ > RAS_STATUS_TIMEOUT) {
			PR_ERROR("Unable to quiesce thread\n");
			return 1;
		}
		thread->status = thread->state(thread);
	} while (!thread->status.quiesced);

	return 0;
}

static int p10_thread_sreset(struct thread *thread)
{
	/* Can only sreset if a thread is quiesced */
	if (!(thread->status.quiesced))
		return 1;

	thread_write(thread, P10_DIRECT_CONTROL, PPC_BIT(4 + 8*thread->id));

	thread->status = thread->state(thread);

	return 0;
}

static struct thread p10_thread = {
	.target = {
		.name = "POWER Thread",
		.compatible = "ibm,power10-thread",
		.class = "thread",
		.probe = p10_thread_probe,
		.release = p10_thread_release,
	},
	.state = p10_thread_state,
	.start = p10_thread_start,
	.stop = p10_thread_stop,
	.sreset = p10_thread_sreset,
};
DECLARE_HW_UNIT(p10_thread);

static int p10_core_probe(struct pdbg_target *target)
{
	struct core *core = target_to_core(target);
	uint64_t value;
	int i = 0;

	/*
	 * BMC applications using libpdbg, do not need special wakeup
	 * asserted by default. Only when running pdbg tool or equivalent
	 * assert special wakeup.
	 */
	if (!pdbg_context_is_short()) {
		core->release_spwkup = false;
		return 0;
	}

	CHECK_ERR(pib_write(target, QME_SPWU_FSP, PPC_BIT(0)));
	do {
		usleep(1000);
		CHECK_ERR(pib_read(target, QME_SSH_FSP, &value));

		if (i++ > SPECIAL_WKUP_TIMEOUT) {
			PR_ERROR("Timeout waiting for special wakeup on %s\n",
				 pdbg_target_path(target));
			break;
		}
	} while (!(value & SPECIAL_WKUP_DONE));

	core->release_spwkup = true;

	return 0;
}

static void p10_core_release(struct pdbg_target *target)
{
	struct pdbg_target *child;
	struct core *core = target_to_core(target);
	enum pdbg_target_status status;

	/* Probe and release all threads to ensure release_spwkup is up to
	 * date */
	pdbg_for_each_target("thread", target, child) {
		status = pdbg_target_status(child);

		/* This thread has already been release so should have set
		 * release_spwkup to false if it was quiesced, */
		if (status == PDBG_TARGET_RELEASED)
			continue;

		status = pdbg_target_probe(child);
		if (status != PDBG_TARGET_ENABLED)
			continue;

		/* Release the thread to ensure release_spwkup is updated. */
		pdbg_target_release(child);
	}

	if (!core->release_spwkup)
		return;

	pib_write(target, QME_SPWU_FSP, 0);
}

#define NUM_CORES_PER_EQ 4
#define EQ0_CHIPLET_ID 0x20

static uint64_t p10_core_translate(struct core *c, uint64_t addr)
{
	int region = 0;
	int chip_unitnum = pdbg_target_index(&c->target);
	int chiplet_id = EQ0_CHIPLET_ID + chip_unitnum / NUM_CORES_PER_EQ;

	switch(chip_unitnum % NUM_CORES_PER_EQ) {
	case 0:
		region = 8;
		break;
	case 1:
		region = 4;
		break;
	case 2:
		region = 2;
		break;
	case 3:
		region = 1;
		break;
	}
	addr &= 0xFFFFFFFFC0FFFFFFULL;
	addr |= ((chiplet_id & 0x3F) << 24);

	addr &= 0xFFFFFFFFFFFF0FFFULL;
	addr |= ((region & 0xF) << 12);

	return addr;
}

static struct core p10_core = {
	.target = {
		.name = "POWER Core",
		.compatible = "ibm,power10-core",
		.class = "core",
		.probe = p10_core_probe,
		.release = p10_core_release,
		.translate = translate_cast(p10_core_translate),
	},
};
DECLARE_HW_UNIT(p10_core);

__attribute__((constructor))
static void register_p10chip(void)
{
	pdbg_hwunit_register(PDBG_DEFAULT_BACKEND, &p10_thread_hw_unit);
	pdbg_hwunit_register(PDBG_DEFAULT_BACKEND, &p10_core_hw_unit);
}
