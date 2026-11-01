/*
 * link882 -- link() error precedence (issue 882).
 *
 * POSIX requires EEXIST when the new name already exists, whatever else is
 * wrong with the request, and Linux checks that before EXDEV or the file
 * system's support for hard links. Whether a link to a NEW name succeeds is
 * reported for information only: BFS has no hard links, so EPERM is the
 * expected answer there (it is what Linux documents for a file system
 * without them).
 *
 * Usage: link882 [dir...]   (default: /tmp and the current directory)
 * Build: cc -O2 -Wall -o link882 link882.c       Exit status: failures.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int sFailures;

static void
expect(const char* dir, const char* label, int r, int e, int want)
{
	int ok = want == 0 ? r == 0 : (r == -1 && e == want);
	printf("%-4s %-10s %-44s r=%d errno=%d (%s)%s\n", ok ? "PASS" : "FAIL", dir,
		label, r, r == 0 ? 0 : e, r == 0 ? "ok" : strerror(e),
		ok ? "" : want == EEXIST ? " [want EEXIST]"
			: want == ENOENT ? " [want ENOENT]" : " [want EXDEV]");
	if (!ok)
		sFailures++;
}

static void
run(const char* dir)
{
	char a[256], b[256], c[256], sub[256], dangling[256], missing[256];
	snprintf(a, sizeof(a), "%s/link882.%d.a", dir, (int)getpid());
	snprintf(b, sizeof(b), "%s/link882.%d.b", dir, (int)getpid());
	snprintf(c, sizeof(c), "%s/link882.%d.c", dir, (int)getpid());
	snprintf(sub, sizeof(sub), "%s/link882.%d.d/", dir, (int)getpid());
	snprintf(dangling, sizeof(dangling), "%s/link882.%d.l", dir, (int)getpid());
	snprintf(missing, sizeof(missing), "%s/link882.%d.missing", dir,
		(int)getpid());

	close(open(a, O_CREAT | O_WRONLY, 0644));
	close(open(b, O_CREAT | O_WRONLY, 0644));
	mkdir(sub, 0755);
	symlink("does-not-exist", dangling);

	int r;
	r = link(a, b);
	expect(dir, "link(file, existing file)", r, errno, EEXIST);
	r = link(a, a);
	expect(dir, "link(file, itself)", r, errno, EEXIST);
	r = link(a, sub);
	expect(dir, "link(file, existing dir/)", r, errno, EEXIST);
	r = link(a, dangling);
	expect(dir, "link(file, dangling symlink)", r, errno, EEXIST);
	r = linkat(AT_FDCWD, a, AT_FDCWD, b, 0);
	expect(dir, "linkat(file, existing file)", r, errno, EEXIST);
	r = link(a, "/dev/null");
	expect(dir, "link(file, /dev/null) (other volume)", r, errno, EEXIST);
	r = link(missing, b);
	expect(dir, "link(missing, existing file)", r, errno, ENOENT);
	r = link(a, "/dev/link882-new");
	expect(dir, "link(file, new name on other volume)", r, errno, EXDEV);

	r = link(a, c);
	printf("INFO %-10s %-44s r=%d errno=%d (%s)\n", dir,
		"link(file, new name)", r, r == 0 ? 0 : errno,
		r == 0 ? "ok" : strerror(errno));

	unlink(c);
	unlink(dangling);
	rmdir(sub);
	unlink(a);
	unlink(b);
}

int
main(int argc, char** argv)
{
	if (argc < 2) {
		run("/tmp");
		run(".");
	} else {
		for (int i = 1; i < argc; i++)
			run(argv[i]);
	}
	printf("link882: %d failed\n", sFailures);
	return sFailures;
}
