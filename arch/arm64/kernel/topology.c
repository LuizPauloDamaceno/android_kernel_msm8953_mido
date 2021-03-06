/*
 * arch/arm64/kernel/topology.c
 *
 * Copyright (C) 2011,2013,2014 Linaro Limited.
 *
 * Based on the arm32 version written by Vincent Guittot in turn based on
 * arch/sh/kernel/topology.c
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 */

#include <linux/cpu.h>
#include <linux/cpumask.h>
#include <linux/init.h>
#include <linux/percpu.h>
#include <linux/node.h>
#include <linux/nodemask.h>
#include <linux/of.h>
#include <linux/sched.h>
#include <linux/sched_energy.h>

#include <asm/cputype.h>
#include <asm/topology.h>

static DEFINE_PER_CPU(unsigned long, cpu_efficiency) = SCHED_CAPACITY_SCALE;

unsigned long arch_get_cpu_efficiency(int cpu)
{
	return per_cpu(cpu_efficiency, cpu);
}

static DEFINE_PER_CPU(unsigned long, cpu_scale) = SCHED_CAPACITY_SCALE;

unsigned long scale_cpu_capacity(struct sched_domain *sd, int cpu)
{
	return per_cpu(cpu_scale, cpu);
}

static void set_capacity_scale(unsigned int cpu, unsigned long capacity)
{
	per_cpu(cpu_scale, cpu) = capacity;
}

static int __init get_cpu_for_node(struct device_node *node)
{
	struct device_node *cpu_node;
	int cpu;

	cpu_node = of_parse_phandle(node, "cpu", 0);
	if (!cpu_node)
		return -1;

	for_each_possible_cpu(cpu) {
		if (of_get_cpu_node(cpu, NULL) == cpu_node) {
			of_node_put(cpu_node);
			return cpu;
		}
	}

	pr_crit("Unable to find CPU node for %s\n", cpu_node->full_name);

	of_node_put(cpu_node);
	return -1;
}

static int __init parse_core(struct device_node *core, int cluster_id,
			     int core_id)
{
	char name[10];
	bool leaf = true;
	int i = 0;
	int cpu;
	struct device_node *t;

	do {
		snprintf(name, sizeof(name), "thread%d", i);
		t = of_get_child_by_name(core, name);
		if (t) {
			leaf = false;
			cpu = get_cpu_for_node(t);
			if (cpu >= 0) {
				cpu_topology[cpu].cluster_id = cluster_id;
				cpu_topology[cpu].core_id = core_id;
				cpu_topology[cpu].thread_id = i;
			} else {
				pr_err("%s: Can't get CPU for thread\n",
				       t->full_name);
				of_node_put(t);
				return -EINVAL;
			}
			of_node_put(t);
		}
		i++;
	} while (t);

	cpu = get_cpu_for_node(core);
	if (cpu >= 0) {
		if (!leaf) {
			pr_err("%s: Core has both threads and CPU\n",
			       core->full_name);
			return -EINVAL;
		}

		cpu_topology[cpu].cluster_id = cluster_id;
		cpu_topology[cpu].core_id = core_id;
	} else if (leaf) {
		pr_err("%s: Can't get CPU for leaf core\n", core->full_name);
		return -EINVAL;
	}

	return 0;
}

static int __init parse_cluster(struct device_node *cluster, int depth)
{
	char name[10];
	bool leaf = true;
	bool has_cores = false;
	struct device_node *c;
	static int cluster_id __initdata;
	int core_id = 0;
	int i, ret;

	/*
	 * First check for child clusters; we currently ignore any
	 * information about the nesting of clusters and present the
	 * scheduler with a flat list of them.
	 */
	i = 0;
	do {
		snprintf(name, sizeof(name), "cluster%d", i);
		c = of_get_child_by_name(cluster, name);
		if (c) {
			leaf = false;
			ret = parse_cluster(c, depth + 1);
			of_node_put(c);
			if (ret != 0)
				return ret;
		}
		i++;
	} while (c);

	/* Now check for cores */
	i = 0;
	do {
		snprintf(name, sizeof(name), "core%d", i);
		c = of_get_child_by_name(cluster, name);
		if (c) {
			has_cores = true;

			if (depth == 0) {
				pr_err("%s: cpu-map children should be clusters\n",
				       c->full_name);
				of_node_put(c);
				return -EINVAL;
			}

			if (leaf) {
				ret = parse_core(c, cluster_id, core_id++);
			} else {
				pr_err("%s: Non-leaf cluster with core %s\n",
				       cluster->full_name, name);
				ret = -EINVAL;
			}

			of_node_put(c);
			if (ret != 0)
				return ret;
		}
		i++;
	} while (c);

	if (leaf && !has_cores)
		pr_warn("%s: empty cluster\n", cluster->full_name);

	if (leaf)
		cluster_id++;

	return 0;
}

static int __init parse_dt_topology(void)
{
	struct device_node *cn, *map;
	int ret = 0;
	int cpu;
	u32 efficiency;

	cn = of_find_node_by_path("/cpus");
	if (!cn) {
		pr_err("No CPU information found in DT\n");
		return 0;
	}

	/*
	 * When topology is provided cpu-map is essentially a root
	 * cluster with restricted subnodes.
	 */
	map = of_get_child_by_name(cn, "cpu-map");
	if (!map)
		goto out;

	ret = parse_cluster(map, 0);
	if (ret != 0)
		goto out_map;

	/*
	 * Check that all cores are in the topology; the SMP code will
	 * only mark cores described in the DT as possible.
	 */
	for_each_possible_cpu(cpu) {
		if (cpu_topology[cpu].cluster_id == -1)
			ret = -EINVAL;

		/* The CPU efficiency value passed from the device tree */
		cn = of_get_cpu_node(cpu, NULL);
		if (of_property_read_u32(cn, "efficiency", &efficiency) < 0) {
			WARN_ON(1);
			continue;
		}
		per_cpu(cpu_efficiency, cpu) = efficiency;
	}

out_map:
	of_node_put(map);
out:
	of_node_put(cn);
	return ret;
}

/*
 * cpu topology table
 */
struct cpu_topology cpu_topology[NR_CPUS];
EXPORT_SYMBOL_GPL(cpu_topology);

/* sd energy functions */
static inline
const struct sched_group_energy * const cpu_cluster_energy(int cpu)
{
	struct sched_group_energy *sge = sge_array[cpu][SD_LEVEL1];

	if (!sge) {
		pr_warn("Invalid sched_group_energy for Cluster%d\n", cpu);
		return NULL;
	}

	return sge;
}

static inline
const struct sched_group_energy * const cpu_core_energy(int cpu)
{
	struct sched_group_energy *sge = sge_array[cpu][SD_LEVEL0];

	if (!sge) {
		pr_warn("Invalid sched_group_energy for CPU%d\n", cpu);
		return NULL;
	}

	return sge;
}

const struct cpumask *cpu_coregroup_mask(int cpu)
{
	return &cpu_topology[cpu].core_sibling;
}

static int cpu_cpu_flags(void)
{
	return SD_ASYM_CPUCAPACITY;
}

static inline int cpu_corepower_flags(void)
{
	return SD_SHARE_PKG_RESOURCES  | SD_SHARE_POWERDOMAIN | \
	       SD_SHARE_CAP_STATES;
}

static struct sched_domain_topology_level arm64_topology[] = {
#ifdef CONFIG_SCHED_MC
	{ cpu_coregroup_mask, cpu_corepower_flags, cpu_core_energy, SD_INIT_NAME(MC) },
#endif
	{ cpu_cpu_mask, cpu_cpu_flags, cpu_cluster_energy, SD_INIT_NAME(DIE) },
	{ NULL, },
};

#include <linux/sched.h>
 
/* Protected by sched_domains_mutex: */
static cpumask_var_t sched_domains_tmpmask;
static cpumask_var_t sched_domains_tmpmask2;
 
#ifdef CONFIG_SCHED_DEBUG
 
static int __init sched_debug_setup(char *str)
{
         sched_debug_enabled = true;
 
         return 0;
}
early_param("sched_debug", sched_debug_setup);
 
static inline bool sched_debug(void)
{
         return sched_debug_enabled;
}
 
static int sched_domain_debug_one(struct sched_domain *sd, int cpu, int level,
                                   struct cpumask *groupmask)
{
         struct sched_group *group = sd->groups;
 
         cpumask_clear(groupmask);
 
         printk(KERN_DEBUG "%*s domain-%d: ", level, "", level);
 
         if (!(sd->flags & SD_LOAD_BALANCE)) {
                 printk("does not load-balance\n");
                 if (sd->parent)
                         printk(KERN_ERR "ERROR: !SD_LOAD_BALANCE domain has parent");
                 return -1;
         }
 
         printk(KERN_CONT "span=%*pbl level=%s\n",
                cpumask_pr_args(sched_domain_span(sd)), sd->name);
 
         if (!cpumask_test_cpu(cpu, sched_domain_span(sd))) {
                 printk(KERN_ERR "ERROR: domain->span does not contain CPU%d\n", cpu);
         }
         if (group && !cpumask_test_cpu(cpu, sched_group_span(group))) {
                 printk(KERN_ERR "ERROR: domain->groups does not contain CPU%d\n", cpu);
         }
 
         printk(KERN_DEBUG "%*s groups:", level + 1, "");
         do {
                 if (!group) {
                         printk("\n");
                         printk(KERN_ERR "ERROR: group is NULL\n");
                         break;
                 }
 
                 if (!cpumask_weight(sched_group_span(group))) {
                         printk(KERN_CONT "\n");
                         printk(KERN_ERR "ERROR: empty group\n");
                         break;
                 }
 
                 if (!(sd->flags & SD_OVERLAP) &&
                     cpumask_intersects(groupmask, sched_group_span(group))) {
                         printk(KERN_CONT "\n");
                         printk(KERN_ERR "ERROR: repeated CPUs\n");
                         break;
                 }
 
                 cpumask_or(groupmask, groupmask, sched_group_span(group));
 
                 printk(KERN_CONT " %d:{ span=%*pbl",
                                 group->sgc->id,
                                 cpumask_pr_args(sched_group_span(group)));
 
                 if ((sd->flags & SD_OVERLAP) &&
                     !cpumask_equal(group_balance_mask(group), sched_group_span(group))) {
                         printk(KERN_CONT " mask=%*pbl",
                                 cpumask_pr_args(group_balance_mask(group)));
                 }
 
                 if (group->sgc->capacity != SCHED_CAPACITY_SCALE)
                         printk(KERN_CONT " cap=%lu", group->sgc->capacity);
 
                 if (group == sd->groups && sd->child &&
                     !cpumask_equal(sched_domain_span(sd->child),
                                    sched_group_span(group))) {
                         printk(KERN_ERR "ERROR: domain->groups does not match domain->child\n");
                 }
 
                 printk(KERN_CONT " }");
 
                 group = group->next;
 
                 if (group != sd->groups)
                         printk(KERN_CONT ",");
 
         } while (group != sd->groups);
         printk(KERN_CONT "\n");
 
         if (!cpumask_equal(sched_domain_span(sd), groupmask))
                 printk(KERN_ERR "ERROR: groups don't span domain->span\n");
 
         if (sd->parent &&
             !cpumask_subset(groupmask, sched_domain_span(sd->parent)))
                 printk(KERN_ERR "ERROR: parent span is not a superset of domain->span\n");
         return 0;
}
 
static void sched_domain_debug(struct sched_domain *sd, int cpu)
{
         int level = 0;
 
         if (!sched_debug_enabled)
                 return;
 
         if (!sd) {
                 printk(KERN_DEBUG "CPU%d attaching NULL sched-domain.\n", cpu);
                 return;
         }
 
         printk(KERN_DEBUG "CPU%d attaching sched-domain(s):\n", cpu);
 
         for (;;) {
                 if (sched_domain_debug_one(sd, cpu, level, sched_domains_tmpmask))
                         break;
                 level++;
                 sd = sd->parent;
                 if (!sd)
                         break;
         }
}
#else /* !CONFIG_SCHED_DEBUG */
 
# define sched_debug_enabled 0
# define sched_domain_debug(sd, cpu) do { } while (0)
static inline bool sched_debug(void)
{
         return false;
}
#endif /* CONFIG_SCHED_DEBUG */
 
static int sd_degenerate(struct sched_domain *sd)
{
         if (cpumask_weight(sched_domain_span(sd)) == 1)
                 return 1;
 
         /* Following flags need at least 2 groups */
         if (sd->flags & (SD_LOAD_BALANCE |
                          SD_BALANCE_NEWIDLE |
                          SD_BALANCE_FORK |
                          SD_BALANCE_EXEC |
                          SD_SHARE_CPUCAPACITY |
                          SD_ASYM_CPUCAPACITY |
                          SD_SHARE_PKG_RESOURCES |
                          SD_SHARE_POWERDOMAIN)) {
                 if (sd->groups)
                         return 0;
         }
 
         /* Following flags don't use groups */
         if (sd->flags & (SD_WAKE_AFFINE))
                 return 0;
 
         return 1;
}
 
static int
sd_parent_degenerate(struct sched_domain *sd, struct sched_domain *parent)
{
         unsigned long cflags = sd->flags, pflags = parent->flags;
 
         if (sd_degenerate(parent))
                 return 1;
 
         if (!cpumask_equal(sched_domain_span(sd), sched_domain_span(parent)))
                 return 0;
 
         /* Flags needing groups don't count if only 1 group in parent */
         if (parent->groups) {
                 pflags &= ~(SD_LOAD_BALANCE |
                                 SD_BALANCE_NEWIDLE |
                                 SD_BALANCE_FORK |
                                 SD_BALANCE_EXEC |
                                 SD_ASYM_CPUCAPACITY |
                                 SD_SHARE_CPUCAPACITY |
                                 SD_SHARE_PKG_RESOURCES |
                                 SD_PREFER_SIBLING |
                                 SD_SHARE_POWERDOMAIN);
                 if (nr_node_ids == 1)
                         pflags &= ~SD_SERIALIZE;
         }
         if (~cflags & pflags)
                 return 0;
 
         return 1;
}

#if defined(CONFIG_ENERGY_MODEL) && defined(CONFIG_CPU_FREQ_GOV_SCHEDUTIL)
DEFINE_MUTEX(sched_energy_mutex);
bool sched_energy_update;
 
static void free_pd(struct perf_domain *pd)
{
         struct perf_domain *tmp;
 
         while (pd) {
                 tmp = pd->next;
                 kfree(pd);
                 pd = tmp;
         }
}
 
static struct perf_domain *find_pd(struct perf_domain *pd, int cpu)
{
         while (pd) {
                 if (cpumask_test_cpu(cpu, perf_domain_span(pd)))
                         return pd;
                 pd = pd->next;
         }
 
         return NULL;
}
 
static struct perf_domain *pd_init(int cpu)
{
         struct em_perf_domain *obj = em_cpu_get(cpu);
         struct perf_domain *pd;
 
         if (!obj) {
                 if (sched_debug())
                         pr_info("%s: no EM found for CPU%d\n", __func__, cpu);
                 return NULL;
         }
 
         pd = kzalloc(sizeof(*pd), GFP_KERNEL);
         if (!pd)
                 return NULL;
         pd->em_pd = obj;
 
         return pd;
}
 
static void perf_domain_debug(const struct cpumask *cpu_map,
                                                 struct perf_domain *pd)
{
         if (!sched_debug() || !pd)
                 return;
 
         printk(KERN_DEBUG "root_domain %*pbl: ", cpumask_pr_args(cpu_map));
 
         while (pd) {
                 printk(KERN_CONT " pd%d:{ cpus=%*pbl nr_cstate=%d }",
                                 cpumask_first(perf_domain_span(pd)),
                                 cpumask_pr_args(perf_domain_span(pd)),
                                 em_pd_nr_cap_states(pd->em_pd));
                 pd = pd->next;
         }
 
         printk(KERN_CONT "\n");
}
 
static void destroy_perf_domain_rcu(struct rcu_head *rp)
{
         struct perf_domain *pd;
 
         pd = container_of(rp, struct perf_domain, rcu);
         free_pd(pd);
}
 
/*
 * EAS can be used on a root domain if it meets all the following conditions:
 *    1. an Energy Model (EM) is available;
 *    2. the SD_ASYM_CPUCAPACITY flag is set in the sched_domain hierarchy.
 *    3. the EM complexity is low enough to keep scheduling overheads low;
 *    4. schedutil is driving the frequency of all CPUs of the rd;
 *
 * The complexity of the Energy Model is defined as:
 *
 *              C = nr_pd * (nr_cpus + nr_cs)
 *
 * with parameters defined as:
 *  - nr_pd:    the number of performance domains
 *  - nr_cpus:  the number of CPUs
 *  - nr_cs:    the sum of the number of capacity states of all performance
 *              domains (for example, on a system with 2 performance domains,
 *              with 10 capacity states each, nr_cs = 2 * 10 = 20).
 *
 * It is generally not a good idea to use such a model in the wake-up path on
 * very complex platforms because of the associated scheduling overheads. The
 * arbitrary constraint below prevents that. It makes EAS usable up to 16 CPUs
 * with per-CPU DVFS and less than 8 capacity states each, for example.
 */
#define EM_MAX_COMPLEXITY 2048
 
extern struct cpufreq_governor schedutil_gov;
static void build_perf_domains(const struct cpumask *cpu_map)
{
         int i, nr_pd = 0, nr_cs = 0, nr_cpus = cpumask_weight(cpu_map);
         struct perf_domain *pd = NULL, *tmp;
         int cpu = cpumask_first(cpu_map);
         struct root_domain *rd = cpu_rq(cpu)->rd;
         struct cpufreq_policy *policy;
         struct cpufreq_governor *gov;
 
         /* EAS is enabled for asymmetric CPU capacity topologies. */
         if (!per_cpu(sd_asym_cpucapacity, cpu)) {
                 if (sched_debug()) {
                         pr_info("rd %*pbl: CPUs do not have asymmetric capacities\n",
                                         cpumask_pr_args(cpu_map));
                 }
                 goto free;
         }
 
         for_each_cpu(i, cpu_map) {
                 /* Skip already covered CPUs. */
                 if (find_pd(pd, i))
                         continue;
 
                 /* Do not attempt EAS if schedutil is not being used. */
                 policy = cpufreq_cpu_get(i);
                 if (!policy)
                         goto free;
                 gov = policy->governor;
                 cpufreq_cpu_put(policy);
                 if (gov != &schedutil_gov) {
                         if (rd->pd)
                                 pr_warn("rd %*pbl: Disabling EAS, schedutil is mandatory\n",
                                                 cpumask_pr_args(cpu_map));
                         goto free;
                 }
 
                 /* Create the new pd and add it to the local list. */
                 tmp = pd_init(i);
                 if (!tmp)
                         goto free;
                 tmp->next = pd;
                 pd = tmp;
 
                 /*
                  * Count performance domains and capacity states for the
                  * complexity check.
                  */
                 nr_pd++;
                 nr_cs += em_pd_nr_cap_states(pd->em_pd);
         }
 
         /* Bail out if the Energy Model complexity is too high. */
         if (nr_pd * (nr_cs + nr_cpus) > EM_MAX_COMPLEXITY) {
                 WARN(1, "rd %*pbl: Failed to start EAS, EM complexity is too high\n",
                                                 cpumask_pr_args(cpu_map));
                 goto free;
         }
 
         perf_domain_debug(cpu_map, pd);
 
         /* Attach the new list of performance domains to the root domain. */
         tmp = rd->pd;
         rcu_assign_pointer(rd->pd, pd);
         if (tmp)
                 call_rcu(&tmp->rcu, destroy_perf_domain_rcu);
 
         return;
 
 free:
         free_pd(pd);
         tmp = rd->pd;
         rcu_assign_pointer(rd->pd, NULL);
         if (tmp)
                 call_rcu(&tmp->rcu, destroy_perf_domain_rcu);
	
static void free_pd(struct perf_domain *pd) { }
}
#endif /* CONFIG_ENERGY_MODEL && CONFIG_CPU_FREQ_GOV_SCHEDUTIL*/

static void update_cpu_capacity(unsigned int cpu)
{
	unsigned long capacity = SCHED_CAPACITY_SCALE;

	if (cpu_core_energy(cpu)) {
		int max_cap_idx = cpu_core_energy(cpu)->nr_cap_states - 1;
		capacity = cpu_core_energy(cpu)->cap_states[max_cap_idx].cap;
	}

	set_capacity_scale(cpu, capacity);

	pr_info("CPU%d: update cpu_capacity %lu\n",
		cpu, arch_scale_cpu_capacity(NULL, cpu));
}

void update_cpu_power_capacity(int cpu)
{
	update_cpu_capacity(cpu);
}

static void update_siblings_masks(unsigned int cpuid)
{
	struct cpu_topology *cpu_topo, *cpuid_topo = &cpu_topology[cpuid];
	int cpu;

	/* update core and thread sibling masks */
	for_each_possible_cpu(cpu) {
		cpu_topo = &cpu_topology[cpu];

		if (cpuid_topo->cluster_id != cpu_topo->cluster_id)
			continue;

		cpumask_set_cpu(cpuid, &cpu_topo->core_sibling);
		if (cpu != cpuid)
			cpumask_set_cpu(cpu, &cpuid_topo->core_sibling);

		if (cpuid_topo->core_id != cpu_topo->core_id)
			continue;

		cpumask_set_cpu(cpuid, &cpu_topo->thread_sibling);
		if (cpu != cpuid)
			cpumask_set_cpu(cpu, &cpuid_topo->thread_sibling);
	}
}

void store_cpu_topology(unsigned int cpuid)
{
	struct cpu_topology *cpuid_topo = &cpu_topology[cpuid];
	u64 mpidr;

	if (cpuid_topo->cluster_id != -1)
		goto topology_populated;

	mpidr = read_cpuid_mpidr();

	/* Uniprocessor systems can rely on default topology values */
	if (mpidr & MPIDR_UP_BITMASK)
		return;

	/* Create cpu topology mapping based on MPIDR. */
	if (mpidr & MPIDR_MT_BITMASK) {
		/* Multiprocessor system : Multi-threads per core */
		cpuid_topo->thread_id  = MPIDR_AFFINITY_LEVEL(mpidr, 0);
		cpuid_topo->core_id    = MPIDR_AFFINITY_LEVEL(mpidr, 1);
		cpuid_topo->cluster_id = MPIDR_AFFINITY_LEVEL(mpidr, 2);
	} else {
		/* Multiprocessor system : Single-thread per core */
		cpuid_topo->thread_id  = -1;
		cpuid_topo->core_id    = MPIDR_AFFINITY_LEVEL(mpidr, 0);
		cpuid_topo->cluster_id = MPIDR_AFFINITY_LEVEL(mpidr, 1);
	}

	pr_debug("CPU%u: cluster %d core %d thread %d mpidr %#016llx\n",
		 cpuid, cpuid_topo->cluster_id, cpuid_topo->core_id,
		 cpuid_topo->thread_id, mpidr);

topology_populated:
	update_siblings_masks(cpuid);
}

static void __init reset_cpu_topology(void)
{
	unsigned int cpu;

	for_each_possible_cpu(cpu) {
		struct cpu_topology *cpu_topo = &cpu_topology[cpu];

		cpu_topo->thread_id = -1;
		cpu_topo->core_id = 0;
		cpu_topo->cluster_id = -1;

		cpumask_clear(&cpu_topo->core_sibling);
		cpumask_set_cpu(cpu, &cpu_topo->core_sibling);
		cpumask_clear(&cpu_topo->thread_sibling);
		cpumask_set_cpu(cpu, &cpu_topo->thread_sibling);
	}
}

void __init init_cpu_topology(void)
{
	int cpu;

	reset_cpu_topology();

	/*
	 * Discard anything that was parsed if we hit an error so we
	 * don't use partial information.
	 */
	if (parse_dt_topology()) {
		reset_cpu_topology();
	} else {
		for_each_possible_cpu(cpu)
			update_siblings_masks(cpu);

		set_sched_topology(arm64_topology);
	}

	init_sched_energy_costs();
}
