/*
 * elfmachine1000.c -- issue 1000: the runtime_loader and the kernel must
 * refuse an ELF object built for another machine or byte order before
 * parsing any of it, and say why.
 *
 * Usage: elfmachine1000 <native-executable> <native-shared-object>
 *          [<foreign-executable>...]
 *
 * From copies of the two native objects it makes variants that differ only
 * in their ELF header -- e_machine set to x86_64 (62), to i386 (3) and to an
 * unassigned number (0x7777), and EI_DATA set to big-endian -- then checks:
 *   - execve() of each variant fails with ENOEXEC (and does not run it);
 *   - dlopen() of each shared-object variant fails (dlerror() is printed;
 *     on DeBeOS the runtime_loader's reason goes to the syslog);
 *   - the unmodified native copies still execute and dlopen;
 *   - each <foreign-executable> (e.g. a real x86_64 binary) fails execve()
 *     with ENOEXEC.
 * The variants are written to the current directory. The last line is the
 * failure count; the exit status is non-zero if any row failed.
 */

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int sFailures;


static void
check(const char* what, const char* got, int ok)
{
	printf("%-44s %-40s %s\n", what, got, ok ? "ok" : "FAIL");
	if (!ok)
		sFailures++;
}


static int
copy_with_patch(const char* from, const char* to, off_t offset,
	const unsigned char* bytes, size_t count)
{
	int in = open(from, O_RDONLY);
	if (in < 0)
		return -1;
	int out = open(to, O_WRONLY | O_CREAT | O_TRUNC, 0755);
	if (out < 0) {
		close(in);
		return -1;
	}
	char buffer[65536];
	ssize_t length;
	while ((length = read(in, buffer, sizeof(buffer))) > 0) {
		if (write(out, buffer, length) != length) {
			length = -1;
			break;
		}
	}
	close(in);
	if (length == 0 && count > 0
		&& pwrite(out, bytes, count, offset) != (ssize_t)count)
		length = -1;
	close(out);
	return length == 0 ? 0 : -1;
}


/*!	Runs path with execve() in a child; returns the child's execve() errno,
	or 0 if it ran (the child's exit status then goes into *_status). */
static int
try_exec(const char* path, int* _status)
{
	int pipeFDs[2];
	if (pipe(pipeFDs) != 0)
		return -1;
	pid_t child = fork();
	if (child == 0) {
		close(pipeFDs[0]);
		fcntl(pipeFDs[1], F_SETFD, FD_CLOEXEC);
		int devNull = open("/dev/null", O_WRONLY);
		dup2(devNull, 1);
		char* const args[] = { (char*)path, (char*)"--version", NULL };
		char* const env[] = { NULL };
		execve(path, args, env);
		int error = errno;
		if (write(pipeFDs[1], &error, sizeof(error)) < 0)
			_exit(126);
		_exit(127);
	}
	close(pipeFDs[1]);
	int error = 0;
	if (read(pipeFDs[0], &error, sizeof(error)) != sizeof(error))
		error = 0;
	close(pipeFDs[0]);
	int status;
	waitpid(child, &status, 0);
	*_status = status;
	return error;
}


static void
check_exec(const char* label, const char* path, int expectError)
{
	int status = 0;
	int error = try_exec(path, &status);
	char got[64];
	if (error != 0)
		snprintf(got, sizeof(got), "execve: %s", strerror(error));
	else if (WIFSIGNALED(status))
		snprintf(got, sizeof(got), "ran, killed by signal %d",
			WTERMSIG(status));
	else
		snprintf(got, sizeof(got), "ran, exit %d", WEXITSTATUS(status));
	char what[300];
	snprintf(what, sizeof(what), "execve %s", label);
	check(what, got, expectError ? error == ENOEXEC : error == 0);
}


static void
check_dlopen(const char* label, const char* path, int expectRefused)
{
	void* handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
	const char* message = handle != NULL ? "loaded" : dlerror();
	char what[300];
	snprintf(what, sizeof(what), "dlopen %s", label);
	if (!expectRefused)
		check(what, message, handle != NULL);
	else {
		if (handle == NULL)
			printf("    dlerror: %s\n", message != NULL ? message : "(null)");
		check(what, handle != NULL ? "loaded" : "refused", handle == NULL);
	}
	if (handle != NULL)
		dlclose(handle);
}


int
main(int argc, char** argv)
{
	if (argc < 3) {
		fprintf(stderr, "usage: %s <native-executable> <native-shared-object>"
			" [<foreign-executable>...]\n", argv[0]);
		return 2;
	}

	static const struct {
		const char*		suffix;
		off_t			offset;
		unsigned char	bytes[2];
		size_t			count;
	} kVariants[] = {
		{ "native", 0, { 0 }, 0 },
		{ "x86_64", 18, { 62, 0 }, 2 },
		{ "i386", 18, { 3, 0 }, 2 },
		{ "em7777", 18, { 0x77, 0x77 }, 2 },
		{ "bigendian", 5, { 2 }, 1 },
	};

	for (size_t i = 0; i < sizeof(kVariants) / sizeof(kVariants[0]); i++) {
		char exe[256], lib[256];
		snprintf(exe, sizeof(exe), "./exe.%s", kVariants[i].suffix);
		snprintf(lib, sizeof(lib), "./lib.%s.so", kVariants[i].suffix);
		if (copy_with_patch(argv[1], exe, kVariants[i].offset,
				kVariants[i].bytes, kVariants[i].count) != 0
			|| copy_with_patch(argv[2], lib, kVariants[i].offset,
				kVariants[i].bytes, kVariants[i].count) != 0) {
			fprintf(stderr, "cannot make the %s variant: %s\n",
				kVariants[i].suffix, strerror(errno));
			return 2;
		}
		int native = kVariants[i].count == 0;
		check_exec(exe, exe, !native);
		check_dlopen(lib, lib, !native);
	}

	for (int i = 3; i < argc; i++)
		check_exec(argv[i], argv[i], 1);

	printf("failures: %d\n", sFailures);
	return sFailures != 0;
}
