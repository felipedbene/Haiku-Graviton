/*
 * unix885.c -- issue 885: connect() (and a datagram sendto()) to an AF_UNIX
 * path that exists but is not a socket. Linux answers ECONNREFUSED, the BSDs
 * ENOTSOCK; Node's pipe tests accept only those two.
 *
 * Controls: a path that does not exist (ENOENT), and a socket file that
 * nobody is bound to any more (ECONNREFUSED), which were right before.
 *
 * Usage: unix885 [dir]   (default /tmp). Exit status: the number of FAILs.
 * Plain POSIX: builds and runs on Linux too.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>


static int sFailed = 0;


static void
report(const char* what, int r, int e, int ok)
{
	char detail[96];
	if (r >= 0)
		snprintf(detail, sizeof(detail), "r=%d", r);
	else
		snprintf(detail, sizeof(detail), "r=%d errno=%d (%s)", r, e,
			strerror(e));
	printf("%-44s %s  %s\n", what, ok ? "PASS" : "FAIL", detail);
	if (!ok)
		sFailed++;
}


static void
set_path(struct sockaddr_un* address, const char* path)
{
	memset(address, 0, sizeof(*address));
	address->sun_family = AF_UNIX;
	strncpy(address->sun_path, path, sizeof(address->sun_path) - 1);
}


/*! Tries a stream connect(), a datagram connect() and a datagram sendto() to
	\a path; each must fail with one of the two errnos given. */
static void
try_path(const char* label, const char* path, int want1, int want2)
{
	struct sockaddr_un address;
	set_path(&address, path);
	char what[80];

	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	errno = 0;
	int r = connect(fd, (struct sockaddr*)&address, sizeof(address));
	int e = errno;
	snprintf(what, sizeof(what), "stream connect, %s", label);
	report(what, r, e, r < 0 && (e == want1 || e == want2));
	close(fd);

	fd = socket(AF_UNIX, SOCK_DGRAM, 0);
	errno = 0;
	r = connect(fd, (struct sockaddr*)&address, sizeof(address));
	e = errno;
	snprintf(what, sizeof(what), "dgram connect, %s", label);
	report(what, r, e, r < 0 && (e == want1 || e == want2));

	errno = 0;
	r = sendto(fd, "x", 1, 0, (struct sockaddr*)&address, sizeof(address));
	e = errno;
	snprintf(what, sizeof(what), "dgram sendto, %s", label);
	report(what, r, e, r < 0 && (e == want1 || e == want2));
	close(fd);
}


int
main(int argc, char** argv)
{
	const char* dir = argc > 1 ? argv[1] : "/tmp";
	char file[200], gone[200], missing[200];
	snprintf(file, sizeof(file), "%s/unix885.file.%d", dir, (int)getpid());
	snprintf(gone, sizeof(gone), "%s/unix885.sock.%d", dir, (int)getpid());
	snprintf(missing, sizeof(missing), "%s/unix885.none.%d", dir,
		(int)getpid());

	// a plain, empty regular file
	int fd = open(file, O_CREAT | O_TRUNC | O_WRONLY, 0644);
	if (fd < 0) {
		perror("open");
		return 1;
	}
	close(fd);

	// a socket file whose socket has been closed
	struct sockaddr_un address;
	set_path(&address, gone);
	unlink(gone);
	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (bind(fd, (struct sockaddr*)&address, sizeof(address)) != 0) {
		perror("bind");
		return 1;
	}
	close(fd);

	try_path("regular file", file, ECONNREFUSED, ENOTSOCK);
	try_path("no such path (control)", missing, ENOENT, ENOENT);
	try_path("stale socket file (control)", gone, ECONNREFUSED,
		ECONNREFUSED);

	unlink(file);
	unlink(gone);
	printf("unix885: %d failed\n", sFailed);
	return sFailed;
}
