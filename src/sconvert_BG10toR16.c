#include <stdint.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "log.h"
#include "config.h"
#include "sdmabuf.h"
#include "spassthrough.h"

typedef struct BayerFormat_s BayerFormat_t;
struct BayerFormat_s
{
	int green1:4;
	int red:4;
	int green2:4;
	int blue:4;
};

static BayerFormat_t bayer_formats[] = {
{
	.blue = 0,
	.green1 = 1,
	.green2 = 2,
	.red = 3,
},
{
	.blue = 1,
	.green1 = 0,
	.green2 = 3,
	.red = 2,
},
};

size_t bg10tor16_convertline(const char *src, char *dst, size_t size, uint16_t *coefs);

typedef struct Convert_BG10toR16_s Convert_BG10toR16_t;
struct Convert_BG10toR16_s
{
	Passthrough_config_t *config;
	uint16_t coefs[4];
};

static void *bg10tor16_create(Passthrough_config_t *config)
{
	int format;
	switch (config->parent.fourcc)
	{
		case FOURCC_BG10:
		case FOURCC_BYR2:
			/* BGGR Bayer order: each 4-sample group is B,G,G,R -
			 * BYR2 is the real V4L2 fourcc some sensors negotiate
			 * (e.g. imx296) for the same MSB-justified 16-bit
			 * layout as BG10, just under its official V4L2 name */
			format = 0;
		break;
		case FOURCC_GB10:
		case FOURCC_GB16:
			/* GBRG Bayer order: each 4-sample group is G,B,R,G */
			format = 1;
		break;
		default:
			err("BG10toR16: unsupported bayer format %.4s", (char *)&config->parent.fourcc);
			return NULL;
	}

	Convert_BG10toR16_t *conv = calloc(1, sizeof(*conv));
	conv->config = config;
	conv->coefs[bayer_formats[format].green1] = 32;
	conv->coefs[bayer_formats[format].green2] = 32;
	conv->coefs[bayer_formats[format].red] = 0;
	conv->coefs[bayer_formats[format].blue] = 0;
	return conv;
}

static size_t bg10tor16_convert(void *arg, const char *const src, char *dst, size_t size, size_t stride)
{
	Convert_BG10toR16_t *conv = (Convert_BG10toR16_t *)arg;
	if (stride == 0)
		stride = size;
	size_t nrows = size / stride;
	char *d = dst;
	for (size_t i = 0; i < nrows; i++)
	{
		const char *srcline = src + i * stride;
		uint16_t *coefs = conv->coefs + 2 * (i % 2);
		d += bg10tor16_convertline(srcline, d, stride, coefs);
	}
	return size;
}

static void bg10tor16_destroy(void *arg)
{
	free(arg);
}

/*
 * Same per-sample math as the NEON/ASIMD variants: a 16x16->32-bit
 * multiply by a per-lane coefficient, narrowed back to 16 bits with
 * rounding and saturation - coefs[0] applies to samples 0 and 2 of every
 * 4-sample group, coefs[1] to samples 1 and 3, matching the vld4/vst4
 * 4-way deinterleave the NEON/ASIMD code relies on.
 */
#ifndef BG10toR16_CONVERTLINE
#define BG10toR16_CONVERTLINE
size_t bg10tor16_convertline(const char *src, char *dst, size_t size, uint16_t *coefs)
{
	const uint16_t *s = (const uint16_t *)src;
	uint16_t *d = (uint16_t *)dst;
	size_t nsamples = size / sizeof(uint16_t);
	for (size_t i = 0; i < nsamples; i += 4)
	{
		for (int lane = 0; lane < 4; lane++)
		{
			uint32_t product = (uint32_t)s[i + lane] * coefs[lane & 1];
			uint32_t rounded = (product + 128) >> 8;
			d[i + lane] = (rounded > 0xFFFF) ? 0xFFFF : (uint16_t)rounded;
		}
	}
	return size;
}
#endif

static Convert_t bg10tor16_default =
{
	.name = "BG10toR16",
	.copy = 1,
	.fourcc_in = 0,
	.fourcc_out = FOURCC_R16,
	.bpp = sizeof(uint16_t),
	.ops =
	{
		.create = bg10tor16_create,
		.convert = bg10tor16_convert,
		.destroy = bg10tor16_destroy,
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
		_spassthrough_convert_append(&bg10tor16_default);
	}
	else
	{
		err("BG10toR16: spassthrough is not loaded");
	}
}
