/*
 * Contains CPU specific errata definitions
 *
 * Copyright (C) 2014 ARM Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/types.h>
#include <asm/cpu.h>
#include <asm/cputype.h>
#include <asm/cpufeature.h>

static bool __maybe_unused
is_affected_midr_range(const struct arm64_cpu_capabilities *entry)
{
	return MIDR_IS_CPU_MODEL_RANGE(read_cpuid_id(), entry->midr_model,
				       entry->midr_range_min,
				       entry->midr_range_max);
}

static bool __maybe_unused
is_kryo_midr(const struct arm64_cpu_capabilities *entry)
{
	u32 model;

	model = read_cpuid_id();
	model &= MIDR_IMPLEMENTOR_MASK | (0xf00 << MIDR_PARTNUM_SHIFT) |
		MIDR_ARCHITECTURE_MASK;

	return model == entry->midr_model;
}

#ifdef CONFIG_HARDEN_BRANCH_PREDICTOR
#include <asm/mmu_context.h>
#include <asm/cacheflush.h>

DEFINE_PER_CPU_READ_MOSTLY(struct bp_hardening_data, bp_hardening_data);

#ifdef CONFIG_KVM
extern char __psci_hyp_bp_inval_start[], __psci_hyp_bp_inval_end[];
extern char __qcom_hyp_sanitize_link_stack_start[];
extern char __qcom_hyp_sanitize_link_stack_end[];

static void __copy_hyp_vect_bpi(int slot, const char *hyp_vecs_start,
				const char *hyp_vecs_end)
{
	void *dst = lm_alias(__bp_harden_hyp_vecs_start + slot * SZ_2K);
	int i;

	for (i = 0; i < SZ_2K; i += 0x80)
		memcpy(dst + i, hyp_vecs_start, hyp_vecs_end - hyp_vecs_start);

	flush_icache_range((uintptr_t)dst, (uintptr_t)dst + SZ_2K);
}

static void __install_bp_hardening_cb(bp_hardening_cb_t fn,
				      const char *hyp_vecs_start,
				      const char *hyp_vecs_end)
{
	static int last_slot = -1;
	static DEFINE_SPINLOCK(bp_lock);
	int cpu, slot = -1;

	spin_lock(&bp_lock);
	for_each_possible_cpu(cpu) {
		if (per_cpu(bp_hardening_data.fn, cpu) == fn) {
			slot = per_cpu(bp_hardening_data.hyp_vectors_slot, cpu);
			break;
		}
	}

	if (slot == -1) {
		last_slot++;
		BUG_ON(((__bp_harden_hyp_vecs_end - __bp_harden_hyp_vecs_start)
			/ SZ_2K) <= last_slot);
		slot = last_slot;
		__copy_hyp_vect_bpi(slot, hyp_vecs_start, hyp_vecs_end);
	}

	__this_cpu_write(bp_hardening_data.hyp_vectors_slot, slot);
	__this_cpu_write(bp_hardening_data.fn, fn);
	spin_unlock(&bp_lock);
}
#else
#define __psci_hyp_bp_inval_start		NULL
#define __psci_hyp_bp_inval_end			NULL
#define __qcom_hyp_sanitize_link_stack_start	NULL
#define __qcom_hyp_sanitize_link_stack_end	NULL

static void __maybe_unused __install_bp_hardening_cb(bp_hardening_cb_t fn,
				      const char *hyp_vecs_start,
				      const char *hyp_vecs_end)
{
	__this_cpu_write(bp_hardening_data.fn, fn);
}
#endif	/* CONFIG_KVM */

static void __maybe_unused install_bp_hardening_cb(
				const struct arm64_cpu_capabilities *entry,
				bp_hardening_cb_t fn,
				const char *hyp_vecs_start,
				const char *hyp_vecs_end)
{
	u64 pfr0;

	if (!entry->matches(entry))
		return;

	pfr0 = read_cpuid(SYS_ID_AA64PFR0_EL1);
	if (cpuid_feature_extract_unsigned_field(pfr0, ID_AA64PFR0_CSV2_SHIFT))
		return;

	__install_bp_hardening_cb(fn, hyp_vecs_start, hyp_vecs_end);
}

#include <linux/psci.h>

static int enable_psci_bp_hardening(void *data)
{
	const struct arm64_cpu_capabilities *entry = data;

	if (psci_ops.get_version)
		install_bp_hardening_cb(entry,
				       (bp_hardening_cb_t)psci_ops.get_version,
				       __psci_hyp_bp_inval_start,
				       __psci_hyp_bp_inval_end);

	return 0;
}

static void __maybe_unused qcom_link_stack_sanitization(void)
{
	u64 tmp;

	asm volatile("mov	%0, x30		\n"
		     ".rept	16		\n"
		     "bl	. + 4		\n"
		     ".endr			\n"
		     "mov	x30, %0		\n"
		     : "=&r" (tmp));
}

static void __maybe_unused qcom_bp_hardening(void)
{
	qcom_link_stack_sanitization();
	if (psci_ops.get_version)
		psci_ops.get_version();
}

static int __maybe_unused enable_qcom_bp_hardening(void *data)
{
	const struct arm64_cpu_capabilities *entry = data;

	install_bp_hardening_cb(entry,
				(bp_hardening_cb_t)qcom_bp_hardening,
				__psci_hyp_bp_inval_start,
				__psci_hyp_bp_inval_end);
	return 0;
}

#endif	/* CONFIG_HARDEN_BRANCH_PREDICTOR */

#define MIDR_RANGE(model, min, max) \
	.matches = is_affected_midr_range, \
	.midr_model = model, \
	.midr_range_min = min, \
	.midr_range_max = max

#define MIDR_ALL_VERSIONS(model) \
	.matches = is_affected_midr_range, \
	.midr_model = model, \
	.midr_range_min = 0, \
	.midr_range_max = (MIDR_VARIANT_MASK | MIDR_REVISION_MASK)

const struct arm64_cpu_capabilities arm64_errata[] = {
#if	defined(CONFIG_ARM64_ERRATUM_826319) || \
	defined(CONFIG_ARM64_ERRATUM_827319) || \
	defined(CONFIG_ARM64_ERRATUM_824069)
	{
	/* Cortex-A53 r0p[012] */
		.desc = "ARM errata 826319, 827319, 824069",
		.capability = ARM64_WORKAROUND_CLEAN_CACHE,
		MIDR_RANGE(MIDR_CORTEX_A53, 0x00, 0x02),
	},
#endif
#ifdef CONFIG_ARM64_ERRATUM_819472
	{
	/* Cortex-A53 r0p[01] */
		.desc = "ARM errata 819472",
		.capability = ARM64_WORKAROUND_CLEAN_CACHE,
		MIDR_RANGE(MIDR_CORTEX_A53, 0x00, 0x01),
	},
#endif
#ifdef CONFIG_ARM64_ERRATUM_832075
	{
	/* Cortex-A57 r0p0 - r1p2 */
		.desc = "ARM erratum 832075",
		.capability = ARM64_WORKAROUND_DEVICE_LOAD_ACQUIRE,
		MIDR_RANGE(MIDR_CORTEX_A57,
			   MIDR_CPU_VAR_REV(0, 0),
			   MIDR_CPU_VAR_REV(1, 2)),
	},
#endif
#ifdef CONFIG_ARM64_ERRATUM_834220
	{
	/* Cortex-A57 r0p0 - r1p2 */
		.desc = "ARM erratum 834220",
		.capability = ARM64_WORKAROUND_834220,
		MIDR_RANGE(MIDR_CORTEX_A57,
			   MIDR_CPU_VAR_REV(0, 0),
			   MIDR_CPU_VAR_REV(1, 2)),
	},
#endif
#ifdef CONFIG_ARM64_ERRATUM_845719
	{
	/* Cortex-A53 r0p[01234] */
		.desc = "ARM erratum 845719",
		.capability = ARM64_WORKAROUND_845719,
		MIDR_RANGE(MIDR_CORTEX_A53, 0x00, 0x04),
	},
	{
	/* Kryo2xx Silver rAp4 */
		.desc = "Kryo2xx Silver erratum 845719",
		.capability = ARM64_WORKAROUND_845719,
		MIDR_RANGE(MIDR_KRYO2XX_SILVER, 0xA00004, 0xA00004),
	},
#endif
#ifdef CONFIG_CAVIUM_ERRATUM_23154
	{
	/* Cavium ThunderX, pass 1.x */
		.desc = "Cavium erratum 23154",
		.capability = ARM64_WORKAROUND_CAVIUM_23154,
		MIDR_RANGE(MIDR_THUNDERX, 0x00, 0x01),
	},
#endif
#ifdef CONFIG_CAVIUM_ERRATUM_27456
	{
	/* Cavium ThunderX, T88 pass 1.x - 2.1 */
		.desc = "Cavium erratum 27456",
		.capability = ARM64_WORKAROUND_CAVIUM_27456,
		MIDR_RANGE(MIDR_THUNDERX,
			   MIDR_CPU_VAR_REV(0, 0),
			   MIDR_CPU_VAR_REV(1, 1)),
	},
#endif
#ifdef CONFIG_HARDEN_BRANCH_PREDICTOR
	{
		.capability = ARM64_HARDEN_BRANCH_PREDICTOR,
		MIDR_ALL_VERSIONS(MIDR_CORTEX_A57),
		.enable = enable_psci_bp_hardening,
	},
	{
		.capability = ARM64_HARDEN_BRANCH_PREDICTOR,
		MIDR_ALL_VERSIONS(MIDR_CORTEX_A72),
		.enable = enable_psci_bp_hardening,
	},
	{
		.capability = ARM64_HARDEN_BRANCH_PREDICTOR,
		MIDR_ALL_VERSIONS(MIDR_CORTEX_A73),
		.enable = enable_psci_bp_hardening,
	},
	{
		.capability = ARM64_HARDEN_BRANCH_PREDICTOR,
		MIDR_ALL_VERSIONS(MIDR_CORTEX_A75),
		.enable = enable_psci_bp_hardening,
	},
	{
		.capability = ARM64_HARDEN_BRANCH_PREDICTOR,
		MIDR_ALL_VERSIONS(MIDR_KRYO2XX_GOLD),
		.enable = enable_psci_bp_hardening,
	},
	{
		.capability = ARM64_HARDEN_BRANCH_PREDICTOR,
		.midr_model = MIDR_QCOM_KRYO,
		.matches = is_kryo_midr,
		.enable = enable_qcom_bp_hardening,
	},
#endif
	{
	}
};

void check_local_cpu_errata(void)
{
	update_cpu_capabilities(arm64_errata, "enabling workaround for");
}

void __init enable_errata_workarounds(void)
{
	enable_cpu_capabilities(arm64_errata);
}
