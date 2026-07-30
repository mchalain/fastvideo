#include <stddef.h>
#include <stdint.h>

/*
 * ARMv8 (AArch64/A64 ASIMD): ld4 deinterleaves 16 samples (4 lanes x 4
 * u16 each) per iteration, umull widens the multiply by a per-lane
 * coefficient (v7.h[0] for lanes 0/2, v7.h[1] for lanes 1/3 - matches
 * coefs[lane & 1] in the portable version), uqrshrn narrows back with
 * rounding + saturation, st4 re-interleaves and stores. Direct A64
 * equivalent of sconvert_BG10toR16_armv7.c's ARMv7 NEON code.
 */
size_t bg10tor16_convertline(const char *src, char *dst, size_t size, uint16_t *coefs)
{
	asm volatile (
		"ld1     {v7.4h}, [%[coefs]]                        \n"
		"1:                                                 \n"
		"subs    %w[size], %w[size], #32                    \n"
		"ld4     {v0.4h, v1.4h, v2.4h, v3.4h}, [%[src]], #32 \n"
		"umull   v12.4s, v0.4h, v7.h[0]                      \n"
		"umull   v13.4s, v1.4h, v7.h[1]                      \n"
		"umull   v14.4s, v2.4h, v7.h[0]                      \n"
		"umull   v15.4s, v3.4h, v7.h[1]                      \n"
		"uqrshrn v0.4h, v12.4s, #8                           \n"
		"uqrshrn v1.4h, v13.4s, #8                           \n"
		"uqrshrn v2.4h, v14.4s, #8                           \n"
		"uqrshrn v3.4h, v15.4s, #8                           \n"
		"st4     {v0.4h, v1.4h, v2.4h, v3.4h}, [%[dst]], #32 \n"
		"b.ne    1b                                          \n"
		: [src]"+r"(src), [dst]"+r"(dst), [size]"+r"(size)
		: [coefs]"r"(coefs)
		: "v0", "v1", "v2", "v3", "v7", "v12", "v13", "v14", "v15", "cc", "memory"
	);
	return size;
}
