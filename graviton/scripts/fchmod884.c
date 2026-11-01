/*
 * fchmod884 -- fchmod(), fchown() and futimens() on a socket (issue 884).
 *
 * libuv's uv_pipe_chmod() calls fchmod() on the listening socket and falls
 * back to chmod() on its path only for EINVAL or EOPNOTSUPP, so either
 * success or one of those two errnos is acceptable; anything else (Haiku
 * answered B_UNSUPPORTED) breaks Node's server.listen({ readableAll }).
 * chmod() of the socket path and fchmod() of a regular file are controls.
 *
 * Build: cc -O2 -Wall -o fchmod884 fchmod884.c [-lnetwork]   Exit: failures.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

static int sFailures;

static void
report(const char* label, int r, int e, int acceptEINVAL)
{
	int ok = r == 0 || (acceptEINVAL && (e == EINVAL || e == EOPNOTSUPP));
	printf("%-4s %-42s r=%d errno=%d (%s)%s\n", ok ? "PASS" : "FAIL", label, r,
		r == 0 ? 0 : e, r == 0 ? "ok" : strerror(e),
		r != 0 && e == EINVAL ? " [EINVAL]"
			: r != 0 && e == EOPNOTSUPP ? " [EOPNOTSUPP]" : "");
	if (!ok)
		sFailures++;
}

static void
socket_ops(const char* what, int fd)
{
	char label[96];
	int r;

	snprintf(label, sizeof(label), "fchmod(%s, 0777)", what);
	r = fchmod(fd, 0777);
	report(label, r, errno, 1);

	snprintf(label, sizeof(label), "fchown(%s, uid, gid)", what);
	r = fchown(fd, getuid(), getgid());
	report(label, r, errno, 1);

	snprintf(label, sizeof(label), "futimens(%s, NULL)", what);
	r = futimens(fd, NULL);
	report(label, r, errno, 1);
}

int
main(void)
{
	printf("EINVAL=%d EOPNOTSUPP=%d\n", EINVAL, EOPNOTSUPP);

	char path[64];
	snprintf(path, sizeof(path), "/tmp/fchmod884.%d.sock", (int)getpid());
	unlink(path);

	int s = socket(AF_UNIX, SOCK_STREAM, 0);
	struct sockaddr_un addr;
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
	if (s < 0 || bind(s, (struct sockaddr*)&addr, sizeof(addr)) != 0
		|| listen(s, 4) != 0) {
		printf("FAIL setup: %s\n", strerror(errno));
		return 1;
	}
	socket_ops("listening AF_UNIX socket", s);

	// what uv_pipe_chmod() falls back to
	int r = chmod(path, 0777);
	report("chmod(socket path, 0777) (control)", r, errno, 0);
	struct stat st;
	if (stat(path, &st) == 0)
		printf("     socket path mode after chmod: %o\n", st.st_mode & 0777);

	int u = socket(AF_INET, SOCK_DGRAM, 0);
	if (u >= 0) {
		socket_ops("unbound AF_INET socket", u);
		close(u);
	}

	char tmpl[] = "/tmp/fchmod884.XXXXXX";
	int f = mkstemp(tmpl);
	r = fchmod(f, 0640);
	report("fchmod(regular file, 0640) (control)", r, errno, 0);
	if (fstat(f, &st) == 0 && (st.st_mode & 0777) != 0640) {
		printf("FAIL regular file mode is %o, not 640\n", st.st_mode & 0777);
		sFailures++;
	}
	close(f);
	unlink(tmpl);

	close(s);
	unlink(path);
	printf("fchmod884: %d failed\n", sFailures);
	return sFailures;
}
