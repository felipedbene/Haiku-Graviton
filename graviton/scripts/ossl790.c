/*
 * ossl790 -- does OpenSSL still know what a Haiku errno is?
 *
 * The golden check for issue 790. Build it against the libcrypto you want to
 * judge and run it on arm64 Haiku:
 *
 *     gcc -o ossl790 ossl790.c -lcrypto && ./ossl790
 *
 * It must FAIL on an unpatched openssl3 and PASS on a patched one. A check that
 * cannot go red proves nothing, so the first thing it prints is the stored error
 * word -- if that word is not the errno itself, the premise is wrong and the
 * rest of the output does not matter.
 *
 * What is being checked
 * ---------------------
 * Haiku's errno values are negative: EAGAIN is B_WOULD_BLOCK, INT_MIN + 11,
 * 0x8000000b. err_set_error() stores a system error as ERR_SYSTEM_FLAG | reason
 * and ERR_SYSTEM_FLAG is 0x80000000, so for a Haiku errno the flag bit IS the
 * sign bit and the stored word is the errno unchanged. Nothing is lost there.
 *
 * ERR_GET_REASON() then returns `errcode & ERR_SYSTEM_MASK`, and
 * ERR_SYSTEM_MASK is INT_MAX -- which strips exactly that bit. EAGAIN comes back
 * as 11. BIO_err_is_non_fatal() compares that against EAGAIN/EWOULDBLOCK/EINTR,
 * never matches, and a transient "no datagram yet" is reported as a fatal
 * network error. That is why OpenSSL's QUIC stack tears down a connection on the
 * first empty non-blocking recvmmsg (issue 766), and why the error string reads
 * "No Error (11)".
 *
 * This affects every consumer that inspects a system error, not only QUIC. QUIC
 * is just where it is fatal rather than merely confusing.
 */

#include <limits.h>
#include <stdio.h>
#include <string.h>

#include <errno.h>

#include <openssl/bio.h>
#include <openssl/err.h>

static int sFailures;
static int sChecks;


static void
check(const char *what, int ok, const char *detail)
{
	sChecks++;
	if (!ok)
		sFailures++;
	printf("%-4s %-52s %s\n", ok ? "ok" : "FAIL", what, detail);
}


/*
 * BIO_err_is_non_fatal() is what the QUIC port actually consults, so it is
 * checked directly rather than reimplemented here. It returns 1 for an error
 * that a caller may retry.
 */
static void
check_one_errno(const char *name, int errnum, int expectNonFatal)
{
	char detail[256];
	unsigned long code;
	int reason;

	ERR_clear_error();
	ERR_raise(ERR_LIB_SYS, errnum);
	code = ERR_peek_last_error();

	reason = ERR_GET_REASON(code);
	snprintf(detail, sizeof(detail),
		"errno=%d stored=0x%08lx ERR_GET_REASON=%d", errnum, code, reason);
	check(name, reason == errnum, detail);

	if (expectNonFatal >= 0) {
		int nonFatal = BIO_err_is_non_fatal(code);
		snprintf(detail, sizeof(detail), "BIO_err_is_non_fatal=%d (want %d)",
			nonFatal, expectNonFatal);
		check("  and it is classed retryable", nonFatal == expectNonFatal,
			detail);
	}
}


int
main(void)
{
	unsigned long code;
	char buf[256];

	printf("openssl %s\n\n", OpenSSL_version(OPENSSL_VERSION_STRING));

	/*
	 * The premise. If the stored word is not the errno, the defect this checks
	 * for is somewhere else and every assertion below is meaningless.
	 */
	ERR_clear_error();
	ERR_raise(ERR_LIB_SYS, EAGAIN);
	code = ERR_peek_last_error();
	snprintf(buf, sizeof(buf), "EAGAIN=%d (0x%08x) stored=0x%08lx",
		EAGAIN, (unsigned int)EAGAIN, code);
	check("premise: the stored word carries the whole errno",
		(unsigned int)code == (unsigned int)EAGAIN, buf);

	printf("\n");

	/* The reason this issue exists: a retryable error must read as retryable. */
	check_one_errno("EAGAIN survives ERR_GET_REASON", EAGAIN, 1);
	check_one_errno("EWOULDBLOCK survives ERR_GET_REASON", EWOULDBLOCK, 1);
	check_one_errno("EINTR survives ERR_GET_REASON", EINTR, 1);

	printf("\n");

	/*
	 * ENOMEM is B_NO_MEMORY, which is INT_MIN exactly -- the same stored word a
	 * system error raised with reason 0 would produce. It must read as ENOMEM:
	 * reading it as 0 would turn an out-of-memory into "No Error", which is the
	 * same class of failure as the one above.
	 */
	check_one_errno("ENOMEM (== INT_MIN) survives ERR_GET_REASON", ENOMEM, -1);

	/* A genuinely fatal errno must keep being fatal. */
	check_one_errno("ECONNRESET survives, and stays fatal", ECONNRESET, 0);
	check_one_errno("EPIPE survives, and stays fatal", EPIPE, 0);

	printf("\n");

	/* Non-system errors must be completely unaffected. */
	ERR_clear_error();
	ERR_raise(ERR_LIB_SSL, 1);
	code = ERR_peek_last_error();
	snprintf(buf, sizeof(buf), "ERR_GET_LIB=%d ERR_GET_REASON=%d",
		ERR_GET_LIB(code), ERR_GET_REASON(code));
	check("a non-system error is untouched",
		ERR_GET_LIB(code) == ERR_LIB_SSL && ERR_GET_REASON(code) == 1, buf);

	/*
	 * The error string is the other half of the symptom: ossl_err_string_int()
	 * feeds ERR_GET_REASON() to openssl_strerror_r(), so a masked reason prints
	 * as "No Error" no matter what actually went wrong.
	 */
	ERR_clear_error();
	ERR_raise(ERR_LIB_SYS, EAGAIN);
	code = ERR_peek_last_error();
	ERR_error_string_n(code, buf, sizeof(buf));
	check("the error string names the real errno",
		strstr(buf, "No Error") == NULL, buf);

	printf("\n%d check(s), %d failure(s): %s\n", sChecks, sFailures,
		sFailures == 0 ? "PASS" : "FAIL");
	return sFailures != 0;
}
