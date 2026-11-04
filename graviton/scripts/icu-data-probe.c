/* icu-data-probe.c -- issues 998 and 1018: can ICU <ver> open locale data on this image?
 * Declares the few versioned ICU C entry points it needs, so it builds with no
 * ICU headers installed. Build: gcc -DV=74 icu-data-probe.c -l:libicuuc.so.74 -l:libicui18n.so.74 */
#include <stdio.h>
#define CAT2(a, b) a##_##b
#define CAT(a, b) CAT2(a, b)
typedef int UErrorCode;
extern const char *CAT(u_errorName, V)(UErrorCode);
extern const char *CAT(u_getDataDirectory, V)(void);
extern void *CAT(ucol_open, V)(const char *, UErrorCode *);
extern void *CAT(udatpg_open, V)(const char *, UErrorCode *);
extern void *CAT(ures_open, V)(const char *, const char *, UErrorCode *);
int main(void)
{
	UErrorCode s;
	int failed = 0;
	printf("data dir: %s\n", CAT(u_getDataDirectory, V)());
	s = 0; CAT(ures_open, V)(NULL, "de", &s);
	printf("ures_open(de):   %s\n", CAT(u_errorName, V)(s)); failed |= s > 0;
	s = 0; CAT(ucol_open, V)("de", &s);
	printf("ucol_open(de):   %s\n", CAT(u_errorName, V)(s)); failed |= s > 0;
	s = 0; CAT(udatpg_open, V)("en_US", &s);
	printf("udatpg_open(en_US): %s\n", CAT(u_errorName, V)(s)); failed |= s > 0;
	return failed;	/* nonzero if any of the three failed (U_FAILURE: s > 0) */
}
