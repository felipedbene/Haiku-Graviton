/*
 * devfd883 -- /dev/fd, /dev/stdin, /dev/stdout and /dev/stderr (issue 883).
 *
 * Checks the names exist, that /dev/fd lists the caller's descriptors, and
 * that opening /dev/fd/<n> reaches what descriptor <n> refers to: a pipe, a
 * socket and a file, with stdin replaced by a pipe for the /dev/stdin case.
 * The open-file-description checks (shared offset, no access beyond the
 * original's) are the dup() semantics of the BSDs and Haiku; Linux re-opens
 * the file instead, so they are counted only on Haiku.
 *
 * Build: cc -O2 -Wall -o devfd883 devfd883.c [-lnetwork]   Exit: failures.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef __HAIKU__
#	define DUP_SEMANTICS 1
#else
#	define DUP_SEMANTICS 0
#endif

static int sFailures;

static void
result(int counted, int ok, const char* label, const char* fmt, ...)
	__attribute__((format(printf, 4, 5)));

static void
result(int counted, int ok, const char* label, const char* fmt, ...)
{
	char detail[256];
	va_list args;
	va_start(args, fmt);
	vsnprintf(detail, sizeof(detail), fmt, args);
	va_end(args);
	printf("%-4s %-44s %s\n", !counted ? "INFO" : ok ? "PASS" : "FAIL", label,
		detail);
	if (counted && !ok)
		sFailures++;
}

static void
check_link(const char* path, const char* target)
{
	struct stat st;
	char buffer[PATH_MAX];
	int r = lstat(path, &st);
	ssize_t n = r == 0 ? readlink(path, buffer, sizeof(buffer) - 1) : -1;
	if (n >= 0)
		buffer[n] = '\0';
	int ok = r == 0 && S_ISLNK(st.st_mode) && n > 0 && strstr(buffer, target);
	result(1, ok, path, "lstat=%s link=%s", r == 0 ? "ok" : strerror(errno),
		n >= 0 ? buffer : "-");
}

/* Opens /dev/fd/<fd> (or path) with mode, writes into in, reads from out. */
static void
check_roundtrip(const char* label, const char* path, int mode, int writeFD,
	int readFD)
{
	int fd = open(path, mode, 0644);
	if (fd < 0) {
		result(1, 0, label, "open(%s): %s", path, strerror(errno));
		return;
	}
	const char message[] = "devfd883";
	char buffer[32] = "";
	int useFD = writeFD < 0 ? fd : writeFD;
	int readFrom = readFD < 0 ? fd : readFD;
	ssize_t w = write(useFD, message, sizeof(message));
	ssize_t r = read(readFrom, buffer, sizeof(buffer));
	struct stat st;
	fstat(fd, &st);
	int ok = w == sizeof(message) && r == sizeof(message)
		&& memcmp(buffer, message, sizeof(message)) == 0;
	result(1, ok, label, "open(%s)=%d type=%s write=%zd read=%zd", path, fd,
		S_ISFIFO(st.st_mode) ? "fifo" : S_ISSOCK(st.st_mode) ? "socket"
			: S_ISREG(st.st_mode) ? "file" : "other", w, r);
	close(fd);
}

int
main(void)
{
	struct stat st;
	char path[64], buffer[PATH_MAX];

	int r = stat("/dev/fd", &st);
	result(1, r == 0 && S_ISDIR(st.st_mode), "stat(/dev/fd) is a directory",
		"%s", r == 0 ? "ok" : strerror(errno));

	check_link("/dev/stdin", "fd/0");
	check_link("/dev/stdout", "fd/1");
	check_link("/dev/stderr", "fd/2");

	char* real = realpath("/dev/stdin", buffer);
	result(1, real != NULL, "realpath(/dev/stdin)", "%s",
		real != NULL ? real : strerror(errno));

	// /dev/fd lists our descriptors
	int marker = open("/dev/null", O_RDONLY);
	int seen0 = 0, seenMarker = 0, entries = 0;
	DIR* dir = opendir("/dev/fd");
	if (dir != NULL) {
		struct dirent* entry;
		while ((entry = readdir(dir)) != NULL) {
			entries++;
			if (strcmp(entry->d_name, "0") == 0)
				seen0 = 1;
			if (atoi(entry->d_name) == marker && entry->d_name[0] != '.')
				seenMarker = 1;
		}
		closedir(dir);
	}
	result(1, seen0 && seenMarker, "readdir(/dev/fd) lists 0 and a new fd",
		"entries=%d has0=%d has%d=%d", entries, seen0, marker, seenMarker);

	snprintf(path, sizeof(path), "/dev/fd/%d", marker);
	r = stat(path, &st);
	result(1, r == 0 && S_ISCHR(st.st_mode), "stat(/dev/fd/<open /dev/null>)",
		"%s mode=0%o", r == 0 ? "ok" : strerror(errno), r == 0 ? st.st_mode : 0);
	close(marker);
	r = stat(path, &st);
	result(1, r != 0 && (errno == ENOENT || errno == EBADF),
		"stat(/dev/fd/<closed fd>)", "%s",
		r == 0 ? "exists" : strerror(errno));
	r = open("/dev/fd/9999", O_RDONLY);
	result(1, r < 0 && (errno == ENOENT || errno == EBADF),
		"open(/dev/fd/9999)", "%s", r >= 0 ? "opened" : strerror(errno));

	// a pipe, both ends
	int p[2];
	pipe(p);
	snprintf(path, sizeof(path), "/dev/fd/%d", p[1]);
	check_roundtrip("pipe write end via /dev/fd", path, O_WRONLY, -1, p[0]);
	snprintf(path, sizeof(path), "/dev/fd/%d", p[0]);
	check_roundtrip("pipe read end via /dev/fd", path, O_RDONLY, p[1], -1);
	// the shell's "> /dev/stdout" opens with O_CREAT | O_TRUNC
	snprintf(path, sizeof(path), "/dev/fd/%d", p[1]);
	check_roundtrip("pipe via /dev/fd, O_CREAT|O_TRUNC", path,
		O_WRONLY | O_CREAT | O_TRUNC, -1, p[0]);

	// stdin replaced by a pipe, read through /dev/stdin
	int saved = dup(0);
	int q[2];
	pipe(q);
	dup2(q[0], 0);
	close(q[0]);
	check_roundtrip("/dev/stdin with stdin a pipe", "/dev/stdin", O_RDONLY,
		q[1], -1);
	dup2(saved, 0);
	close(saved);
	close(q[1]);

	// a socket
	int s[2];
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, s) == 0) {
		snprintf(path, sizeof(path), "/dev/fd/%d", s[0]);
		int fd = open(path, O_RDWR);
		if (fd < 0 && !DUP_SEMANTICS) {
			// Linux cannot re-open a socket (ENXIO)
			result(0, 1, "socket via /dev/fd", "open: %s", strerror(errno));
		} else
			check_roundtrip("socket via /dev/fd", path, O_RDWR, s[1], -1);
		if (fd >= 0)
			close(fd);
		close(s[0]);
		close(s[1]);
	}

	// a regular file: shared open file description, no access beyond it
	char tmpl[] = "/tmp/devfd883.XXXXXX";
	int f = mkstemp(tmpl);
	write(f, "0123456789", 10);
	int ro = open(tmpl, O_RDONLY);
	snprintf(path, sizeof(path), "/dev/fd/%d", ro);
	int dupFD = open(path, O_RDONLY);
	char c = 0;
	read(dupFD, &c, 1);
	off_t offset = lseek(ro, 0, SEEK_CUR);
	result(DUP_SEMANTICS, dupFD >= 0 && c == '0' && offset == 1,
		"file via /dev/fd shares the offset", "fd=%d read='%c' original "
		"offset=%lld", dupFD, c ? c : '-', (long long)offset);
	int rw = open(path, O_RDWR);
	result(DUP_SEMANTICS, rw < 0 && errno == EACCES,
		"O_RDWR of an O_RDONLY fd via /dev/fd", "%s",
		rw >= 0 ? "opened" : strerror(errno));
	if (rw >= 0)
		close(rw);
	int cloexec = open(path, O_RDONLY | O_CLOEXEC);
	int flags = cloexec >= 0 ? fcntl(cloexec, F_GETFD) : -1;
	result(1, cloexec >= 0 && (flags & FD_CLOEXEC) != 0,
		"O_CLOEXEC applies to the new fd", "fd=%d F_GETFD=%d", cloexec, flags);
	int noexec = open(path, O_RDONLY);
	flags = noexec >= 0 ? fcntl(noexec, F_GETFD) : -1;
	result(1, noexec >= 0 && (flags & FD_CLOEXEC) == 0,
		"no O_CLOEXEC, no FD_CLOEXEC", "fd=%d F_GETFD=%d", noexec, flags);
	close(cloexec);
	close(noexec);
	close(dupFD);
	close(ro);
	close(f);
	unlink(tmpl);

	printf("devfd883: %d failed\n", sFailures);
	return sFailures;
}
