#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <dlfcn.h>

#include "log.h"
#include "spassthrough.h"

#if defined(__ARM_NEON)
#if !defined(__aarch64__)
#define NEON_COPY 2
#else
#define NEON_COPY 1
#endif
#else
#define NEON_COPY 0
#endif

#if NEON_COPY == 1
static size_t passthrough_default_copy(void *dev, const char *const src, char *dst, size_t size, size_t stride)
{
	size_t bulk = size & ~(size_t)31;

	if (bulk)
	{
		const char *s = src;
		char *d = dst;
		size_t n = bulk;
		/*
		 * subs comes AFTER the load/store so the loop is entered only
		 * for a count that is known to be >= 32, and n is a multiple of
		 * 32 so it lands exactly on 0 - hence b.ne rather than b.gt.
		 * s, d and n are all read-write operands: the asm modifies the
		 * three registers, and telling the compiler otherwise would let
		 * it assume they still held their entry values.
		 */
		asm volatile (
			"1:                                               \n"
			"ld1      {v0.16b, v1.16b}, [%[s]], #32           \n"
			"st1      {v0.16b, v1.16b}, [%[d]], #32           \n"
			"subs     %[n], %[n], #32                         \n"
			"b.ne     1b                                      \n"
			: [s]"+r"(s), [d]"+r"(d), [n]"+r"(n)
			:
			: "v0", "v1", "cc", "memory"
		);
	}
	if (size != bulk)
		memcpy(dst + bulk, src + bulk, size - bulk);

	return size;
}
#elif NEON_COPY == 2
static size_t passthrough_default_copy(void *dev, const char *const src, char *dst, size_t size, size_t stride)
{
	size_t bulk = size & ~(size_t)31;

	if (bulk)
	{
		const char *s = src;
		char *d = dst;
		size_t n = bulk;
		/*
		 * No [%[s]:256] / [%[d]:256] alignment qualifier: it asserts a
		 * 32 byte alignment that nothing here guarantees. stile.c enters
		 * this function at ((y0 + row) * srcw + x0) * bpp, which is not
		 * 32 byte aligned in general, and an unaligned access with that
		 * qualifier faults instead of being fixed up.
		 */
		asm volatile (
			"1:                                               \n"
			"vld1.u8  {d0, d1, d2, d3}, [%[s]]!               \n"
			"vst1.u8  {d0, d1, d2, d3}, [%[d]]!               \n"
			"subs     %[n], %[n], #32                         \n"
			"bne      1b                                      \n"
			: [s]"+r"(s), [d]"+r"(d), [n]"+r"(n)
			:
			: "d0", "d1", "d2", "d3", "cc", "memory"
		);
	}
	if (size != bulk)
		memcpy(dst + bulk, src + bulk, size - bulk);

	return size;
}
#else
static size_t passthrough_default_copy(void *dev, const char *const src, char *dst, size_t size, size_t stride)
{
	memcpy(dst, src, size);
	return size;
}
#endif

static void *passthrough_create(Passthrough_config_t *config)
{
	return (void *)config;
}

static uint32_t passthrough_bpp(void *arg, int out)
{
	Passthrough_config_t *config = (Passthrough_config_t *)arg;
	DeviceConf_t *device = &config->parent;
	if (out)
		device = &config->transfer;
	switch (device->fourcc)
	{
	case FOURCC_AB24:
	case FOURCC_XB24:
	case FOURCC_AR24:
	case FOURCC_XR24:
	case FOURCC_BGR4:
	case FOURCC_RGB4:
	case FOURCC_RGBA:
		return sizeof(uint32_t);
	break;
	case FOURCC_RGB3:
		return 3 * sizeof(uint8_t);
	break;
	case FOURCC_RGBP:
	case FOURCC_R16:
	case FOURCC_R12:
	case FOURCC_R10:
		return 2 * sizeof(uint8_t);
	break;
	case FOURCC_R8:
		return 1 * sizeof(uint8_t);
	break;
	}
	return 0;
}

static void passthrough_destroy(void *arg)
{
}

Convert_t sconvert_passthrough =
{
	.name = "passthrough",
	.copy = 1,
	.bpp = 1,
	.ops =
	{
		.create = passthrough_create,
		.convert = passthrough_default_copy,
		.destroy = passthrough_destroy,
	},
};

static void __attribute__ ((constructor)) convert_passthrough_init()
{
	spassthrough_convert_append_t _spassthrough_convert_append = NULL;
	void *hdl = dlopen("libfastvideo.so", RTLD_NOW);
	if (hdl != NULL)
		_spassthrough_convert_append = dlsym(hdl, "spassthrough_convert_append");
	else
		err("convert passthrough: library not found");
	if (_spassthrough_convert_append)
	{
		_spassthrough_convert_append(&sconvert_passthrough);
	}
	else
	{
		err("convert passthrough: spassthrough is not loaded");
	}
}
