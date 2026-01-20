#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include "segl.h"
#include "sdmabuf.h"
#include "log.h"

#define EXPORT_USERDMABUF 1

typedef struct EGLExportPixels_s EGLExportPixels_t;
struct EGLExportPixels_s
{
	EGLConfig_t *config;
};

static const char segl[] = "segl";

static void *_egl_export_create(EGLConfig_t *config, EGLDisplay eglDisplay, EGLContext eglContext)
{
	EGLExportPixels_t *ctx = calloc(1, sizeof(*ctx));
	ctx->config = config;
	return ctx;
}

static GLuint _egl_export_fbo(void *arg)
{
	return 0;
}

static GL_Buffer_t *_egl_export_out(void *arg)
{
	return NULL;
}

static int _egl_export_setbuffer(void *arg, GLBuffer_t *buffer)
{
	int ret = -1;
#if EXPORT_USERDMABUF
	int dmabufs_tmp = 0;
	dmabufs_tmp = sdmabuf_create(segl, buffer->size);
	if (dmabufs_tmp > 0)
	{
		buffer->memory = sdmabuf_map(dmabufs_tmp, buffer->size, 1);
		if (buffer->memory == (void *)(long)-1)
		{
			err("spassthrough: buffer creation error");
			sdmabuf_destroy(dmabufs_tmp);
			buffer->memory = NULL;
		}
		else
			buffer->dma_fd = dmabufs_tmp;
		ret = 0;
	}
#else
	buffer->memory = calloc(1, buffer->size);
	if (buffer->memory)
		ret = 0;
#endif
	return ret;
}

static int _egl_export_flush(void *arg, GLBuffer_t *buffer)
{
	EGLExportPixels_t *ctx = (EGLExportPixels_t *)arg;
	int ret = -1;
	if (buffer->memory != NULL)
	{
		uint32_t width = ctx->config->parent.width;
		uint32_t height = ctx->config->parent.height;
		glPixelStorei(GL_PACK_ALIGNMENT, 1);
		glReadPixels(0, 0, width, height, GL_RGBA,
				GL_UNSIGNED_BYTE, buffer->memory);
		ret = 0;
	}
	return ret;
}

static int _egl_export_releasebuffer(void *arg, GLBuffer_t *buffer)
{
#if EXPORT_USERDMABUF
	if (buffer->memory != NULL)
		sdmabuf_unmap(buffer->memory, buffer->size);
	sdmabuf_destroy(buffer->dma_fd);
#else
	if (buffer->memory != NULL)
		free(buffer->memory);
#endif
	return 0;
}

static int _egl_export_fd(void *arg)
{
	return -1;
}

static void _egl_export_destroy(void *arg)
{
	free(arg);
}

static EGLExport_t eglexport_pixels =
{
	.name = "pixels",
	.create = _egl_export_create,
	.fbo = _egl_export_fbo,
	.out = _egl_export_out,
	.fd = _egl_export_fd,
	.setbuffer = _egl_export_setbuffer,
	.flush = _egl_export_flush,
	.destroy = _egl_export_destroy,
};

#include <dlfcn.h>

static void __attribute__ ((constructor)) segl_init()
{
	segl_export_append_t _segl_export_append;
	void *hdl = dlopen(NULL, RTLD_NOW);
	_segl_export_append = dlsym(hdl, "segl_export_append");
	if (_segl_export_append)
	{
		_segl_export_append(&eglexport_pixels);
	}
}
