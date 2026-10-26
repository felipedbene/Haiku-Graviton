/*
** Copyright 2003, Axel Dörfler, axeld@pinc-software.de. All rights reserved.
** Distributed under the terms of the MIT License.
*/


#include <Collator.h>
#include <Locale.h>
#include <LocaleRoster.h>
#include <StopWatch.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>


const char *kStrings[] = {
	"a-b-c",
	"a b c",
	"A.b.c",
	"ä,b,c",
	"abc",
	"gehen",
	"géhen",
	"aus",
	"äUß",
	"auss",
	"WO",
	"wÖ",
	"SO",
	"so",
	"açñ",
	"acn",
	"pêche",
	"pêché",
	"peché",
	"peche",
	"pecher",
	"eñe",
	"ene",
	"nz",
	"ña",
	"llamar",
	"luz",
};
const uint32 kNumStrings = sizeof(kStrings) / sizeof(kStrings[0]);
const uint32 kIterations = 50000;


void
test(BCollator *collator, const char *name, int8 strength)
{
	collator->SetStrength(strength);

	BStopWatch watch(name, true);

	for (uint32 j = 0; j < kIterations; j++) {
		for (uint32 i = 0; i < kNumStrings; i++) {
			BString key;
			collator->GetSortKey(kStrings[i], &key);
		}
	}

	watch.Suspend();
	double secs = watch.ElapsedTime() / 1000000.0;
	printf("\t%s%9Ld usecs, %6.3g secs,%9lu keys/s\n",
		name, watch.ElapsedTime(), secs, uint32(kIterations * kNumStrings / secs));
}


void
usage()
{
	fprintf(stderr,
		"usage: collatorSpeed [-i]\n"
		"  -i\tignore punctuation (defaults to: punctuation matters)\n");
	exit(-1);
}


int
main(int argc, char **argv)
{
	// Parse command line arguments

	bool ignorePunctuation = false;

	while ((++argv)[0]) {
		if (argv[0][0] == '-') {
			if (!strcmp(argv[0], "-i"))
				ignorePunctuation = true;
			else if (!strcmp(argv[0], "--help"))
				usage();
		} else {
			// The add-on path argument used to name a collator add-on to load.
			// That architecture (BCollatorAddOn, instantiate_collator()) was
			// removed, so there is nothing left for a positional argument to
			// select.
			usage();
		}
	}

	// The default locale's collator is the only one there is now.

	BCollator collator;
	const BLocale* locale = BLocaleRoster::Default()->GetDefaultLocale();
	if (locale == NULL || locale->GetCollator(&collator) != B_OK) {
		fprintf(stderr, "could not get the default locale's collator.\n");
		return 1;
	}

	// test key creation speed

	collator.SetIgnorePunctuation(ignorePunctuation);

	printf("default:\n");
	test(&collator, "primary:   ", B_COLLATE_PRIMARY);
	test(&collator, "secondary: ", B_COLLATE_SECONDARY);
	test(&collator, "tertiary:  ", B_COLLATE_TERTIARY);
	test(&collator, "quaternary:", B_COLLATE_QUATERNARY);
	test(&collator, "identical: ", B_COLLATE_IDENTICAL);

	return 0;
}

