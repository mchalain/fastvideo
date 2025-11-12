#include <stdint.h>

#include "log.h"
#include "config.h"
#include "sdmabuf.h"
#include "spassthrough.h"

typedef struct Convert_BG10toR16_s Convert_BG10toR16_t;
struct Convert_BG10toR16_s
{
	Passthrough_config_t *config;
	uint16_t coefs[4];
};

static void *convert_create(Passthrough_config_t *config)
{
	Convert_BG10toR16_t *conv = calloc(1, sizeof(*conv));
	conv->config = config;
	int green1 = 0, green2 = 3, red = 2, blue = 1;
	conv->coefs[green1] = 32;
	conv->coefs[green2] = 32;
	conv->coefs[red] = 0;
	conv->coefs[blue] = 0;
	return conv;
create_error:
	free(conv);
	err("BG10toR16: format not supported");
	return NULL;
}

static size_t convert_convert(void *arg, const char *const src, char *dst, size_t size)
{
	Convert_BG10toR16_t *conv = (Convert_BG10toR16_t *)arg;
	uint16_t *coefs = conv->coefs;
#define BYLINE 1
#if BYLINE
	size_t origsize = size;
	const char *srcline = src;
	for (int i = 0; i < conv->config->parent.height; i++)
	{
		srcline = src + (i * origsize / conv->config->parent.height);
		size = origsize / conv->config->parent.height;
		coefs = conv->coefs + 2 * (i % 2);
#endif
#ifdef __ARM_NEON
	/// only d0 to d7 accept 16 bits scalars
	asm volatile (
		" vld1.u16      {d7}, [%[coefs]]            \n"
		"loop:                                      \n"
		" subs          %[size], %[size], #32       \n"
		" vld4.u16      {d0, d1, d2, d3}, [%[src]]! \n"
		" vmull.u16     q12, d0, d7[0]              \n"
		" vmull.u16     q13, d1, d7[1]              \n"
		" vmull.u16     q14, d2, d7[0]              \n"
		" vmull.u16     q15, d3, d7[1]              \n"
		" vqrshrn.u16   d0, q12, #8                 \n"
		" vqrshrn.u16   d1, q13, #8                 \n"
		" vqrshrn.u16   d2, q14, #8                 \n"
		" vqrshrn.u16   d3, q15, #8                 \n"
		" vst4.u16      {d0, d1, d2, d3}, [%[dst]]! \n"
		" bne      loop                             \n"
		: [dst]"+r"(dst)
		: [src]"r"(srcline), [size]"r"(size), [coefs]"r"(coefs)
		: "d0", "d1", "d2", "d3", "d7", "q12", "q13", "q14", "q15", "cc", "memory"
	);
#else
# warning must support ARM NEON
#endif
#if BYLINE
		//dst += origsize / conv->config->parent.height;
	}
#endif
	return size;
}

static void convert_destroy(void *arg)
{
	free(arg);
}

Convert_t convert_BG10toR16 =
{
	.name = "BG10toR16",
	.copy = 1,
	.fourcc_in = FOURCC_GB10,
	.fourcc_out = FOURCC_GR88,
	.ops =
	{
		.create = convert_create,
		.convert = convert_convert,
		.destroy = convert_destroy,
	},
};

#include <dlfcn.h>

static void __attribute__ ((constructor)) convert_BG10toR16_init()
{
	spassthrough_convert_append_t _spassthrough_convert_append = NULL;
	void *hdl = dlopen("libfastvideo.so", RTLD_NOW);
	if (hdl != NULL)
		_spassthrough_convert_append = dlsym(hdl, "spassthrough_convert_append");
	else
		err("BG10toR16: library not found");
	if (_spassthrough_convert_append)
	{
		_spassthrough_convert_append(&convert_BG10toR16);
	}
	else
	{
		err("BG10toR16: spassthrough is not loaded");
	}
}
