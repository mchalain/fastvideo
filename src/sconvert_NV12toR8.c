#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "sconfig.h"
#include "sdmabuf.h"
#include "spassthrough.h"

static Convert_t convert_NV12toR8;

typedef struct Convert_NV12toR8_s Convert_NV12toR8_t;
struct Convert_NV12toR8_s
{
	Passthrough_config_t *config;
	size_t width;
	size_t height;
	size_t src_stride;
};

static unsigned int _gcd(unsigned int a, unsigned int b)
{
	while (b)
	{
		unsigned int rest = a % b;
		a = b;
		b = rest;
	}
	return a;
}

static void *convert_create(Passthrough_config_t *config)
{
	Convert_NV12toR8_t *conv = calloc(1, sizeof(*conv));
	conv->config = config;
	conv->width = config->parent.width;
	conv->height = config->parent.height;
	conv->src_stride = config->parent.stride ? config->parent.stride : conv->width;

	if (conv->width && conv->src_stride)
	{
		unsigned int num = (unsigned int)conv->width;
		unsigned int den = (unsigned int)conv->src_stride;
		unsigned int div = _gcd(num, den);
		if (div)
		{
			num /= div;
			den /= div;
		}
		convert_NV12toR8.resize.numerator = num;
		convert_NV12toR8.resize.denominator = den;
	}

#ifndef SCONVERT_NV12TOR8_COPY
	if (conv->src_stride != conv->width)
		err("NV12toR8: %s source rows are padded (stride %zu for %zu pixels), "
			"the luma plane cannot be repacked without SCONVERT_NV12TOR8_COPY",
			config->parent.name, conv->src_stride, conv->width);
#endif

	return conv;
}

static size_t convert_convert(void *arg, const char *const src, char *dst, size_t size, size_t stride)
{
	Convert_NV12toR8_t *conv = (Convert_NV12toR8_t *)arg;
	size_t width = conv->width;
	size_t height = conv->height;

#ifdef SCONVERT_NV12TOR8_COPY
	if (stride == 0)
		stride = conv->src_stride;
	if (stride < width)
		stride = width;
	/// never read past what the device really handed over
	if ((height * stride) > size)
		height = size / stride;

	for (size_t row = 0; row < height; row++)
		sconvert_passthrough.ops.convert(NULL, src + (row * stride), dst + (row * width), width, width);
#else
	(void)src;
	(void)dst;
	(void)stride;
	if ((width * height) > size)
		height = size / width;
#endif

	return width * height;
}

static uint32_t convert_bpp(void *arg, int out)
{
	if (out)
		return 2 * sizeof(uint8_t);
	return sizeof(uint8_t);
}

static void convert_destroy(void *arg)
{
	free(arg);
}

static Convert_t convert_NV12toR8 =
{
	.name = "NV12toR8",
#ifdef SCONVERT_NV12TOR8_COPY
	/// a real destination buffer is needed: the rows get repacked, not aliased
	.copy = 1,
#else
	.copy = 0,
#endif
	/// recomputed from the real strides in convert_create()
	.resize = {.numerator = 1, .denominator = 1},
	.fourcc_in = FOURCC_NV12,
	.fourcc_out = FOURCC_R8,
	.ops =
	{
		.create = convert_create,
		.convert = convert_convert,
		.bpp = convert_bpp,
		.destroy = convert_destroy,
	},
};

#include <dlfcn.h>

static void __attribute__ ((constructor)) convert_NV12toR8_init()
{
	spassthrough_convert_append_t _spassthrough_convert_append = NULL;
	void *hdl = dlopen("libfastvideo.so", RTLD_NOW);
	if (hdl != NULL)
		_spassthrough_convert_append = dlsym(hdl, "spassthrough_convert_append");
	else
		err("NV12toR8: library not found");
	if (_spassthrough_convert_append)
	{
		_spassthrough_convert_append(&convert_NV12toR8);
	}
	else
	{
		err("NV12toR8: spassthrough is not loaded");
	}
}
