/* wrap917: interpose uv_cpu_info and the libroot calls under it, to see which
 * step fails inside the node process. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <OS.h>
#include <uv.h>

int
uv_cpu_info(uv_cpu_info_t** infos, int* count)
{
	int (*real)(uv_cpu_info_t**, int*)
		= (int (*)(uv_cpu_info_t**, int*))dlsym(RTLD_NEXT, "uv_cpu_info");
	fprintf(stderr, "wrap917: real uv_cpu_info %p\n", (void*)real);
	int r = real(infos, count);
	fprintf(stderr, "wrap917: uv_cpu_info -> %d (%s) count %d\n", r,
		r ? uv_strerror(r) : "ok", r ? -1 : *count);
	return r;
}

status_t
get_system_info(system_info* info)
{
	status_t (*real)(system_info*)
		= (status_t (*)(system_info*))dlsym(RTLD_NEXT, "get_system_info");
	status_t s = real(info);
	fprintf(stderr, "wrap917: get_system_info -> %d cpu_count %u\n",
		(int)s, info->cpu_count);
	return s;
}

status_t
get_cpu_topology_info(cpu_topology_node_info* t, uint32* c)
{
	status_t (*real)(cpu_topology_node_info*, uint32*)
		= (status_t (*)(cpu_topology_node_info*, uint32*))dlsym(RTLD_NEXT,
			"get_cpu_topology_info");
	uint32 in = *c;
	status_t s = real(t, c);
	fprintf(stderr, "wrap917: get_cpu_topology_info(%p, in %u) -> %d out %u\n",
		(void*)t, in, (int)s, *c);
	return s;
}
