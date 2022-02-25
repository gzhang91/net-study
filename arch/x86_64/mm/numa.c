/* 
 * Generic VM initialization for x86-64 NUMA setups.
 * Copyright 2002,2003 Andi Kleen, SuSE Labs.
 */ 
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/string.h>
#include <linux/init.h>
#include <linux/bootmem.h>
#include <linux/mmzone.h>
#include <linux/ctype.h>
#include <linux/module.h>
#include <linux/nodemask.h>

#include <asm/e820.h>
#include <asm/proto.h>
#include <asm/dma.h>
#include <asm/numa.h>
#include <asm/acpi.h>

#ifndef Dprintk
#define Dprintk(x...)
#endif

struct pglist_data *node_data[MAX_NUMNODES] __read_mostly;
bootmem_data_t plat_node_bdata[MAX_NUMNODES];

struct memnode memnode;

unsigned char cpu_to_node[NR_CPUS] __read_mostly = {
	[0 ... NR_CPUS-1] = NUMA_NO_NODE
};
unsigned char apicid_to_node[MAX_LOCAL_APIC] __cpuinitdata = {
 	[0 ... MAX_LOCAL_APIC-1] = NUMA_NO_NODE
};
cpumask_t node_to_cpumask[MAX_NUMNODES] __read_mostly;

int numa_off __initdata;


/*
 * Given a shift value, try to populate memnodemap[]
 * Returns :
 * 1 if OK
 * 0 if memnodmap[] too small (of shift too small)
 * -1 if node overlap or lost ram (shift too big)
 */
static int __init
populate_memnodemap(const struct bootnode *nodes, int numnodes, int shift)
{
	int i; 
	int res = -1;
	unsigned long addr, end;

	if (shift >= 64)
		return -1;
	memset(memnodemap, 0xff, sizeof(memnodemap));
	for (i = 0; i < numnodes; i++) {
		addr = nodes[i].start;
		end = nodes[i].end;
		if (addr >= end)
			continue;
		if ((end >> shift) >= NODEMAPSIZE)
			return 0;
		do {
			if (memnodemap[addr >> shift] != 0xff)
				return -1;
			memnodemap[addr >> shift] = i;
                       addr += (1UL << shift);
		} while (addr < end);
		res = 1;
	} 
	return res;
}

int __init compute_hash_shift(struct bootnode *nodes, int numnodes)
{
	int shift = 20;

	while (populate_memnodemap(nodes, numnodes, shift + 1) >= 0)
		shift++;

	printk(KERN_DEBUG "NUMA: Using %d for the hash shift.\n",
		shift);

	if (populate_memnodemap(nodes, numnodes, shift) != 1) {
		printk(KERN_INFO
	"Your memory is not aligned you need to rebuild your kernel "
	"with a bigger NODEMAPSIZE shift=%d\n",
			shift);
		return -1;
	}
	return shift;
}

#ifdef CONFIG_SPARSEMEM
int early_pfn_to_nid(unsigned long pfn)
{
	return phys_to_nid(pfn << PAGE_SHIFT);
}
#endif

static void * __init
early_node_mem(int nodeid, unsigned long start, unsigned long end,
	      unsigned long size)
{
	unsigned long mem = find_e820_area(start, end, size);
	void *ptr;
	if (mem != -1L)
		return __va(mem);
	ptr = __alloc_bootmem_nopanic(size,
				SMP_CACHE_BYTES, __pa(MAX_DMA_ADDRESS));
	if (ptr == 0) {
		printk(KERN_ERR "Cannot find %lu bytes in node %d\n",
			size, nodeid);
		return NULL;
	}
	return ptr;
}

/* Initialize bootmem allocator for a node */
void __init setup_node_bootmem(int nodeid, unsigned long start, unsigned long end)
{ 
	unsigned long start_pfn, end_pfn, bootmap_pages, bootmap_size, bootmap_start; 
	unsigned long nodedata_phys;
	void *bootmap;
	const int pgdat_size = round_up(sizeof(pg_data_t), PAGE_SIZE);

	start = round_up(start, ZONE_ALIGN); 

	printk(KERN_INFO "Bootmem setup node %d %016lx-%016lx\n", nodeid, start, end);

	start_pfn = start >> PAGE_SHIFT;
	end_pfn = end >> PAGE_SHIFT;

	node_data[nodeid] = early_node_mem(nodeid, start, end, pgdat_size);
	if (node_data[nodeid] == NULL)
		return;
	nodedata_phys = __pa(node_data[nodeid]);

	memset(NODE_DATA(nodeid), 0, sizeof(pg_data_t));
	NODE_DATA(nodeid)->bdata = &plat_node_bdata[nodeid];
	NODE_DATA(nodeid)->node_start_pfn = start_pfn;
	NODE_DATA(nodeid)->node_spanned_pages = end_pfn - start_pfn;

	/* Find a place for the bootmem map */
	bootmap_pages = bootmem_bootmap_pages(end_pfn - start_pfn); 
	bootmap_start = round_up(nodedata_phys + pgdat_size, PAGE_SIZE);
	bootmap = early_node_mem(nodeid, bootmap_start, end,
					bootmap_pages<<PAGE_SHIFT);
	if (bootmap == NULL)  {
		if (nodedata_phys < start || nodedata_phys >= end)
			free_bootmem((unsigned long)node_data[nodeid],pgdat_size);
		node_data[nodeid] = NULL;
		return;
	}
	bootmap_start = __pa(bootmap);
	Dprintk("bootmap start %lu pages %lu\n", bootmap_start, bootmap_pages); 
	
	bootmap_size = init_bootmem_node(NODE_DATA(nodeid),
					 bootmap_start >> PAGE_SHIFT, 
					 start_pfn, end_pfn); 

	free_bootmem_with_active_regions(nodeid, end);

	reserve_bootmem_node(NODE_DATA(nodeid), nodedata_phys, pgdat_size); 
	reserve_bootmem_node(NODE_DATA(nodeid), bootmap_start, bootmap_pages<<PAGE_SHIFT);
#ifdef CONFIG_ACPI_NUMA
	srat_reserve_add_area(nodeid);
#endif
	node_set_online(nodeid);
} 

/* Initialize final allocator for a zone */
void __init setup_node_zones(int nodeid)
{ 
	unsigned long start_pfn, end_pfn, memmapsize, limit;

 	start_pfn = node_start_pfn(nodeid);
 	end_pfn = node_end_pfn(nodeid);

	Dprintk(KERN_INFO "Setting up memmap for node %d %lx-%lx\n",
		nodeid, start_pfn, end_pfn);

	/* Try to allocate mem_map at end to not fill up precious <4GB
	   memory. */
	memmapsize = sizeof(struct page) * (end_pfn-start_pfn);
	limit = end_pfn << PAGE_SHIFT;
#ifdef CONFIG_FLAT_NODE_MEM_MAP
	NODE_DATA(nodeid)->node_mem_map = 
		__alloc_bootmem_core(NODE_DATA(nodeid)->bdata, 
				memmapsize, SMP_CACHE_BYTES, 
				round_down(limit - memmapsize, PAGE_SIZE), 
				limit);
#endif
} 

void __init numa_init_array(void)
{
	int rr, i;
	/* There are unfortunately some poorly designed mainboards around
	   that only connect memory to a single CPU. This breaks the 1:1 cpu->node
	   mapping. To avoid this fill in the mapping for all possible
	   CPUs, as the number of CPUs is not known yet. 
	   We round robin the existing nodes. */
	rr = first_node(node_online_map);
	for (i = 0; i < NR_CPUS; i++) {
		if (cpu_to_node[i] != NUMA_NO_NODE)
			continue;
 		numa_set_node(i, rr);
		rr = next_node(rr, node_online_map);
		if (rr == MAX_NUMNODES)
			rr = first_node(node_online_map);
	}

}

#ifdef CONFIG_NUMA_EMU
int numa_fake __initdata = 0;

/* Numa emulation */
static int __init numa_emulation(unsigned long start_pfn, unsigned long end_pfn)
{
 	int i;
 	struct bootnode nodes[MAX_NUMNODES];
 	unsigned long sz = ((end_pfn - start_pfn)<<PAGE_SHIFT) / numa_fake;

 	/* Kludge needed for the hash function */
 	if (hweight64(sz) > 1) {
 		unsigned long x = 1;
 		while ((x << 1) < sz)
 			x <<= 1;
 		if (x < sz/2)
 			printk(KERN_ERR "Numa emulation unbalanced. Complain to maintainer\n");
 		sz = x;
 	}

 	memset(&nodes,0,sizeof(nodes));
 	for (i = 0; i < numa_fake; i++) {
 		nodes[i].start = (start_pfn<<PAGE_SHIFT) + i*sz;
 		if (i == numa_fake-1)
 			sz = (end_pfn<<PAGE_SHIFT) - nodes[i].start;
 		nodes[i].end = nodes[i].start + sz;
 		printk(KERN_INFO "Faking node %d at %016Lx-%016Lx (%LuMB)\n",
 		       i,
 		       nodes[i].start, nodes[i].end,
 		       (nodes[i].end - nodes[i].start) >> 20);
		node_set_online(i);
 	}
 	memnode_shift = compute_hash_shift(nodes, numa_fake);
 	if (memnode_shift < 0) {
 		memnode_shift = 0;
 		printk(KERN_ERR "No NUMA hash function found. Emulation disabled.\n");
 		return -1;
 	}
 	for_each_online_node(i) {
		e820_register_active_regions(i, nodes[i].start >> PAGE_SHIFT,
						nodes[i].end >> PAGE_SHIFT);
 		setup_node_bootmem(i, nodes[i].start, nodes[i].end);
	}
 	numa_init_array();
 	return 0;
}
#endif

void __init numa_initmem_init(unsigned long start_pfn, unsigned long end_pfn)
{ 
	int i;

#ifdef CONFIG_NUMA_EMU
	if (numa_fake && !numa_emulation(start_pfn, end_pfn))
 		return;
#endif

#ifdef CONFIG_ACPI_NUMA
	if (!numa_off && !acpi_scan_nodes(start_pfn << PAGE_SHIFT,
					  end_pfn << PAGE_SHIFT))
 		return;
#endif

#ifdef CONFIG_K8_NUMA
	if (!numa_off && !k8_scan_nodes(start_pfn<<PAGE_SHIFT, end_pfn<<PAGE_SHIFT))
		return;
#endif
	printk(KERN_INFO "%s\n",
	       numa_off ? "NUMA turned off" : "No NUMA configuration found");

	printk(KERN_INFO "Faking a node at %016lx-%016lx\n", 
	       start_pfn << PAGE_SHIFT,
	       end_pfn << PAGE_SHIFT); 
		/* setup dummy node covering all memory */ 
	memnode_shift = 63; 
	memnodemap[0] = 0;
	nodes_clear(node_online_map);
	node_set_online(0);
	for (i = 0; i < NR_CPUS; i++)
		numa_set_node(i, 0);
	node_to_cpumask[0] = cpumask_of_cpu(0);
	e820_register_active_regions(0, start_pfn, end_pfn);
	setup_node_bootmem(0, start_pfn << PAGE_SHIFT, end_pfn << PAGE_SHIFT);
}

__cpuinit void numa_add_cpu(int cpu)
{
	set_bit(cpu, &node_to_cpumask[cpu_to_node(cpu)]);
} 

void __cpuinit numa_set_node(int cpu, int node)
{
	cpu_pda(cpu)->nodenumber = node;
	cpu_to_node[cpu] = node;
}

unsigned long __init numa_free_all_bootmem(void) 
{ 
	int i;
	unsigned long pages = 0;
	for_each_online_node(i) {
		pages += free_all_bootmem_node(NODE_DATA(i));
	}
	return pages;
} 

#ifdef CONFIG_SPARSEMEM
static void __init arch_sparse_init(void)
{
	int i;

	for_each_online_node(i)
		memory_present(i, node_start_pfn(i), node_end_pfn(i));

	sparse_init();
}
#else
#define arch_sparse_init() do {} while (0)
#endif

void __init paging_init(void)
{ 
	int i;
	unsigned long max_zone_pfns[MAX_NR_ZONES];
	memset(max_zone_pfns, 0, sizeof(max_zone_pfns));
	max_zone_pfns[ZONE_DMA] = MAX_DMA_PFN;
	max_zone_pfns[ZONE_DMA32] = MAX_DMA32_PFN;
	max_zone_pfns[ZONE_NORMAL] = end_pfn;

	arch_sparse_init();

	for_each_online_node(i) {
		setup_node_zones(i); 
	}

	free_area_init_nodes(max_zone_pfns);
} 

static __init int numa_setup(char *opt)
{ 
	if (!opt)
		return -EINVAL;
	if (!strncmp(opt,"off",3))
		numa_off = 1;
#ifdef CONFIG_NUMA_EMU
	if(!strncmp(opt, "fake=", 5)) {
		numa_fake = simple_strtoul(opt+5,NULL,0); ;
		if (numa_fake >= MAX_NUMNODES)
			numa_fake = MAX_NUMNODES;
	}
#endif
#ifdef CONFIG_ACPI_NUMA
 	if (!strncmp(opt,"noacpi",6))
 		acpi_numa = -1;
	if (!strncmp(opt,"hotadd=", 7))
		hotadd_percent = simple_strtoul(opt+7, NULL, 10);
#endif
	return 0;
} 

early_param("numa", numa_setup);

/*
 * Setup early cpu_to_node.
 *
 * Populate cpu_to_node[] only if x86_cpu_to_apicid[],
 * and apicid_to_node[] tables have valid entries for a CPU.
 * This means we skip cpu_to_node[] initialisation for NUMA
 * emulation and faking node case (when running a kernel compiled
 * for NUMA on a non NUMA box), which is OK as cpu_to_node[]
 * is already initialized in a round robin manner at numa_init_array,
 * prior to this call, and this initialization is good enough
 * for the fake NUMA cases.
 */
void __init init_cpu_to_node(void)
{
	int i;
 	for (i = 0; i < NR_CPUS; i++) {
		u8 apicid = x86_cpu_to_apicid[i];
		if (apicid == BAD_APICID)
			continue;
		if (apicid_to_node[apicid] == NUMA_NO_NODE)
			continue;
		numa_set_node(i,apicid_to_node[apicid]);
	}
}

EXPORT_SYMBOL(cpu_to_node);
EXPORT_SYMBOL(node_to_cpumask);
EXPORT_SYMBOL(memnode);
EXPORT_SYMBOL(node_data);

#ifdef CONFIG_DISCONTIGMEM
/*
 * Functions to convert PFNs from/to per node page addresses.
 * These are out of line because they are quite big.
 * They could be all tuned by pre caching more state.
 * Should do that.
 */

int pfn_valid(unsigned long pfn)
{
	unsigned nid;
	if (pfn >= num_physpages)
		return 0;
	nid = pfn_to_nid(pfn);
	if (nid == 0xff)
		return 0;
	return pfn >= node_start_pfn(nid) && (pfn) < node_end_pfn(nid);
}
EXPORT_SYMBOL(pfn_valid);
#endif
