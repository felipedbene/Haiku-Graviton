/* posix-locale-probe.c -- issue 1018: libroot's POSIX locale backend (libroot-addon-icu)
 * sits on ICU 74. Prints what setlocale/strftime/nl_langinfo give for de_DE and fr_FR. */
#include <langinfo.h>
#include <locale.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
int main(void)
{
	const char *locs[] = { "de_DE.UTF-8", "fr_FR.UTF-8" };
	time_t t = 86400 * 3;	/* 1970-01-04, a Sunday */
	struct tm tm;
	gmtime_r(&t, &tm);
	for (int i = 0; i < 2; i++) {
		char buf[128];
		const char *r = setlocale(LC_ALL, locs[i]);
		strftime(buf, sizeof(buf), "%A %d %B %Y", &tm);
		printf("%s: setlocale=%s strftime=\"%s\" DAY_1=\"%s\" MON_1=\"%s\" RADIX=\"%s\"\n",
			locs[i], r ? r : "(null)", buf, nl_langinfo(DAY_1), nl_langinfo(MON_1),
			nl_langinfo(RADIXCHAR));
	}
	return 0;
}
