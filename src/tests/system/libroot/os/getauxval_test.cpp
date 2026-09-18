/*
 * Copyright 2026 DeBeOS. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 */


// Prints the CPU feature words that getauxval() reports through the arm64
// commpage HWCAP block, decoded with the Linux/glibc AT_HWCAP bit names. Used
// to verify on Graviton hardware that ported runtime-dispatch code (ggml,
// llama.cpp, libjpeg-turbo, libhwy, openssl, x264) sees the caps it expects --
// e.g. HWCAP_ASIMDDP and HWCAP2_I8MM / HWCAP2_BF16 for the ML data paths.


#include <errno.h>
#include <stdio.h>

#include <sys/auxv.h>


#if defined(__aarch64__) || defined(__arm64__)

struct cap_name {
	unsigned long	mask;
	const char*		name;
};


static const cap_name kHwcap[] = {
	{ HWCAP_FP, "fp" },
	{ HWCAP_ASIMD, "asimd" },
	{ HWCAP_EVTSTRM, "evtstrm" },
	{ HWCAP_AES, "aes" },
	{ HWCAP_PMULL, "pmull" },
	{ HWCAP_SHA1, "sha1" },
	{ HWCAP_SHA2, "sha2" },
	{ HWCAP_CRC32, "crc32" },
	{ HWCAP_ATOMICS, "atomics" },
	{ HWCAP_FPHP, "fphp" },
	{ HWCAP_ASIMDHP, "asimdhp" },
	{ HWCAP_CPUID, "cpuid" },
	{ HWCAP_ASIMDRDM, "asimdrdm" },
	{ HWCAP_JSCVT, "jscvt" },
	{ HWCAP_FCMA, "fcma" },
	{ HWCAP_LRCPC, "lrcpc" },
	{ HWCAP_DCPOP, "dcpop" },
	{ HWCAP_SHA3, "sha3" },
	{ HWCAP_SM3, "sm3" },
	{ HWCAP_SM4, "sm4" },
	{ HWCAP_ASIMDDP, "asimddp" },
	{ HWCAP_SHA512, "sha512" },
	{ HWCAP_SVE, "sve" },
	{ HWCAP_ASIMDFHM, "asimdfhm" },
	{ HWCAP_DIT, "dit" },
	{ HWCAP_USCAT, "uscat" },
	{ HWCAP_ILRCPC, "ilrcpc" },
	{ HWCAP_FLAGM, "flagm" },
	{ HWCAP_SSBS, "ssbs" },
	{ HWCAP_SB, "sb" },
	{ HWCAP_PACA, "paca" },
	{ HWCAP_PACG, "pacg" },
};


static const cap_name kHwcap2[] = {
	{ HWCAP2_DCPODP, "dcpodp" },
	{ HWCAP2_SVE2, "sve2" },
	{ HWCAP2_SVEAES, "sveaes" },
	{ HWCAP2_SVEPMULL, "svepmull" },
	{ HWCAP2_SVEBITPERM, "svebitperm" },
	{ HWCAP2_SVESHA3, "svesha3" },
	{ HWCAP2_SVESM4, "svesm4" },
	{ HWCAP2_FLAGM2, "flagm2" },
	{ HWCAP2_FRINT, "frint" },
	{ HWCAP2_SVEI8MM, "svei8mm" },
	{ HWCAP2_SVEF32MM, "svef32mm" },
	{ HWCAP2_SVEF64MM, "svef64mm" },
	{ HWCAP2_SVEBF16, "svebf16" },
	{ HWCAP2_I8MM, "i8mm" },
	{ HWCAP2_BF16, "bf16" },
};


static void
print_caps(const char* label, unsigned long value, const cap_name* names,
	size_t count)
{
	printf("%s = 0x%016lx:", label, value);
	for (size_t i = 0; i < count; i++) {
		if ((value & names[i].mask) != 0)
			printf(" %s", names[i].name);
	}
	putchar('\n');
}

#endif	// __aarch64__


int
main()
{
	unsigned long hwcap = getauxval(AT_HWCAP);
	unsigned long hwcap2 = getauxval(AT_HWCAP2);

#if defined(__aarch64__) || defined(__arm64__)
	print_caps("AT_HWCAP ", hwcap, kHwcap,
		sizeof(kHwcap) / sizeof(kHwcap[0]));
	print_caps("AT_HWCAP2", hwcap2, kHwcap2,
		sizeof(kHwcap2) / sizeof(kHwcap2[0]));

	// The supported Graviton SoCs all implement Advanced SIMD; a zero AT_HWCAP
	// means the kernel never published the commpage block, so flag it.
	if ((hwcap & HWCAP_ASIMD) == 0) {
		fprintf(stderr, "warning: HWCAP_ASIMD not set -- commpage HWCAP block "
			"missing or kernel too old\n");
		return 1;
	}
	return 0;
#else
	printf("AT_HWCAP  = 0x%016lx\n", hwcap);
	printf("AT_HWCAP2 = 0x%016lx\n", hwcap2);
	return 0;
#endif
}
