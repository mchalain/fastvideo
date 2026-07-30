#include <stddef.h>
#include <stdint.h>

/*
 * ARMv7 (32-bit NEON): vld4.u16 deinterleaves 16 samples (4 lanes x 4
 * u16 each) per iteration, vmull.u16 widens the multiply by a per-lane
 * coefficient (d7[0] for lanes 0/2, d7[1] for lanes 1/3 - matches
 * coefs[lane & 1] in the portable version), vqrshrn.u16 narrows back
 * with rounding + saturation, vst4.u16 re-interleaves and stores.
 */
size_t bg10tor16_default_convertline(const char *src, char *dst, size_t size, uint16_t *coefs)
{
	asm volatile (
		" vld1.u16      {d7}, [%[coefs]]            \n"
		"1:                                         \n"
		" subs          %[size], %[size], #32       \n"
		" vld4.u16      {d0, d1, d2, d3}, [%[src]]!  \n"
		" vmull.u16     q12, d0, d7[0]               \n"
		" vmull.u16     q13, d1, d7[1]               \n"
		" vmull.u16     q14, d2, d7[0]               \n"
		" vmull.u16     q15, d3, d7[1]               \n"
		" vqrshrn.u16   d0, q12, #8                  \n"
		" vqrshrn.u16   d1, q13, #8                  \n"
		" vqrshrn.u16   d2, q14, #8                  \n"
		" vqrshrn.u16   d3, q15, #8                  \n"
		" vst4.u16      {d0, d1, d2, d3}, [%[dst]]!  \n"
		" bne           1b                           \n"
		: [src]"+r"(src), [dst]"+r"(dst), [size]"+r"(size)
		: [coefs]"r"(coefs)
		: "d0", "d1", "d2", "d3", "d7", "q12", "q13", "q14", "q15", "cc", "memory"
	);
	return size;
}
