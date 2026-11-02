/* probe917: what each layer under os.cpus() returns on this machine. */
#include <stdio.h>
#include <OS.h>
#include <uv.h>

int
main(void)
{
	uint32 count = 0;
	status_t st = get_cpu_topology_info(NULL, &count);
	printf("get_cpu_topology_info(NULL): status %d (%s) count %u\n", (int)st,
		strerror(st), count);
	cpu_topology_node_info nodes[256];
	uint32 n = count;
	st = get_cpu_topology_info(nodes, &n);
	printf("get_cpu_topology_info(buf): status %d count %u\n", (int)st, n);
	for (uint32 i = 0; i < n && i < 256; i++) {
		printf("  node %u type %d level %u id %u", i, nodes[i].type,
			nodes[i].level, nodes[i].id);
		if (nodes[i].type == B_TOPOLOGY_CORE)
			printf(" model %#x freq %llu", nodes[i].data.core.model,
				(unsigned long long)nodes[i].data.core.default_frequency);
		printf("\n");
	}
	system_info si;
	st = get_system_info(&si);
	printf("get_system_info: status %d cpu_count %u\n", (int)st, si.cpu_count);
	cpu_info ci[64];
	st = get_cpu_info(0, si.cpu_count, ci);
	printf("get_cpu_info: status %d\n", (int)st);
	for (uint32 i = 0; st == B_OK && i < si.cpu_count; i++)
		printf("  cpu %u active %lld enabled %d cur_freq %llu\n", i,
			(long long)ci[i].active_time, ci[i].enabled,
			(unsigned long long)ci[i].current_frequency);

	uv_cpu_info_t* infos = NULL;
	int c = -1;
	int r = uv_cpu_info(&infos, &c);
	printf("uv_cpu_info: r %d (%s) count %d\n", r, r ? uv_strerror(r) : "ok", c);
	for (int i = 0; r == 0 && i < c; i++)
		printf("  [%d] model \"%s\" speed %d user %llu sys %llu idle %llu\n",
			i, infos[i].model, infos[i].speed,
			(unsigned long long)infos[i].cpu_times.user,
			(unsigned long long)infos[i].cpu_times.sys,
			(unsigned long long)infos[i].cpu_times.idle);
	if (r == 0)
		uv_free_cpu_info(infos, c);
	return 0;
}
