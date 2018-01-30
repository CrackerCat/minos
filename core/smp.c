#include <core/core.h>
#include <config/mvisor_config.h>
#include <core/percpu.h>

DEFINE_PER_CPU(uint64_t, cpu_id);

uint64_t smp_holding_pen[CONFIG_NUM_OF_CPUS] = {
	[0] = 0,
	[1 ... (CONFIG_NUM_OF_CPUS - 1)] = 0xffff,
};

uint32_t smp_get_cpuid(uint64_t mpidr_id)
{
	uint32_t smp_id;
	uint8_t aff0, aff1, aff2, aff3;

	mpidr_id &= MPIDR_ID_MASK;
	aff0 = (uint8_t)(mpidr_id);
	aff1 = (uint8_t)(mpidr_id >> 8);
	aff2 = (uint8_t)(mpidr_id >> 16);
	aff3 = (uint8_t)(mpidr_id >> 32);

	/*
	 * now assume there are only one cluster
	 * this is different on each platform, on
	 * fvp there is one cluster, so the aff0 value
	 * is the cpuid
	 */
	return (uint32_t)aff0;
}

int smp_cpu_up(uint64_t mpidr_id)
{
	uint32_t smp_id;

	smp_id = smp_get_cpuid(mpidr_id);
	if (smp_id > CONFIG_NUM_OF_CPUS)
		return -EINVAL;

	smp_holding_pen[smp_id] = mpidr_id;
	return 0;
}

void smp_cpus_up(void)
{
	int i;

	for (i = 1; i < CONFIG_NUM_OF_CPUS; i++)
		smp_cpu_up((i << 0) | (0 << 8) | (0 << 16) | (0ul << 32));
}

void smp_init(void)
{
	int i;

	for (i = 0; i < CONFIG_NUM_OF_CPUS; i++)
		get_per_cpu(cpu_id, i) = 0xffff;

	get_per_cpu(cpu_id, 0) = 0;
}
