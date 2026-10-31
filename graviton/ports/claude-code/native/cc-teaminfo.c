/*
 * cc-teaminfo -- process (team) identity for Claude Code on Haiku.
 *
 * Claude Code proves that a pid still names the process it recorded by comparing
 * a start-time token, and walks process trees by parent pid. On Linux it reads
 * /proc/<pid>/stat; elsewhere it runs `ps -o lstart= -p` / `ps -A -o pid= -o
 * ppid=`. Haiku has neither /proc nor a ps with those columns (src/bin/ps.c takes
 * -ihaso: and its -o knows only Team Id Threads Gid Uid). The kernel does keep
 * both values in team_info (headers/os/kernel/OS.h): `parent` and `start_time`.
 * This tool prints them. The bundle runs it from <install-prefix>/bin/cc-teaminfo
 * (PATCHES.md P17).
 *
 * Usage:
 *   cc-teaminfo <team>...   the named teams; ids that are not teams are skipped
 *   cc-teaminfo -a          every team (get_next_team_info)
 *
 * Output: one line per team, fields separated by exactly one space:
 *
 *   team parent group session uid start_us start_epoch_us args
 *
 *   team, parent, group, session, uid  team_info.team, .parent, .group_id,
 *                                      .session_id, .uid (decimal)
 *   start_us        team_info.start_time: microseconds since boot (system_time()
 *                   base). This is the identity token, and wall-clock changes
 *                   do not move it.
 *   start_epoch_us  start_us + (real_time_clock_usecs() - system_time()),
 *                   sampled once per run: start time as Unix epoch microseconds.
 *                   For display/age only; it shifts if the clock is set.
 *   args            team_info.args, the command line as the kernel keeps it
 *                   (at most 63 bytes, truncated by the kernel). Control
 *                   characters become spaces. It is the rest of the line and may
 *                   contain spaces or be empty.
 *
 * Exit status: 0 after any query (an id that is not a team is just absent from
 * the output), 2 on a usage error. Nothing else is written to stdout.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <OS.h>


static bigtime_t sBootEpoch;


static void
print_team(const team_info* info)
{
	char args[sizeof(info->args) + 1];
	size_t i;

	memcpy(args, info->args, sizeof(info->args));
	args[sizeof(info->args)] = '\0';
	for (i = 0; args[i] != '\0'; i++) {
		if ((unsigned char)args[i] < 0x20 || args[i] == 0x7f)
			args[i] = ' ';
	}

	printf("%" B_PRId32 " %" B_PRId32 " %" B_PRId32 " %" B_PRId32 " %u %" B_PRId64
		" %" B_PRId64 " %s\n", info->team, info->parent, (int32)info->group_id,
		(int32)info->session_id, (unsigned)info->uid, info->start_time,
		info->start_time + sBootEpoch, args);
}


static int
usage(void)
{
	fprintf(stderr, "usage: cc-teaminfo -a | cc-teaminfo <team>...\n");
	return 2;
}


int
main(int argc, char** argv)
{
	team_info info;
	int i;

	if (argc < 2)
		return usage();

	sBootEpoch = real_time_clock_usecs() - system_time();

	if (strcmp(argv[1], "-a") == 0) {
		int32 cookie = 0;
		if (argc != 2)
			return usage();
		while (get_next_team_info(&cookie, &info) == B_OK)
			print_team(&info);
		return fflush(stdout) == 0 ? 0 : 1;
	}

	for (i = 1; i < argc; i++) {
		char* end;
		long id;

		errno = 0;
		id = strtol(argv[i], &end, 10);
		if (errno != 0 || end == argv[i] || *end != '\0' || id <= 0
			|| id > 0x7fffffffL)
			return usage();
		if (get_team_info((team_id)id, &info) == B_OK)
			print_team(&info);
	}
	return fflush(stdout) == 0 ? 0 : 1;
}
