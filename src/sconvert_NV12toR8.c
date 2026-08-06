#include <stdint.h>

#include "log.h"
#include "config.h"
#include "sdmabuf.h"
#include "spassthrough.h"

typedef struct Convert_NV12toR8_s Convert_NV12toR8_t;
struct Convert_NV12toR8_s
{
	Passthrough_config_t *config;
};

static void *convert_create(Passthrough_config_t *config)
{
	Convert_NV12toR8_t *conv = calloc(1, sizeof(*conv));
	conv->config = config;
	return conv;
}

static size_t convert_convert(void *arg, const char *const src, char *dst, size_t size, size_t stride)
{
	size *= 2;
	size /= 3;
	return size;
}

static void convert_destroy(void *arg)
{
	free(arg);
}

Convert_t convert_NV12toR8 =
{
	.name = "NV12toR8",
	.copy = 0,
	.resize = {.numerator = 2, .denominator = 3},
	.fourcc_in = FOURCC_NV12,
	.fourcc_out = FOURCC_R8,
	.ops =
	{
		.create = convert_create,
		.convert = convert_convert,
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
