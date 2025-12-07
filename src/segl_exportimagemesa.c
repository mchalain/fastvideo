#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include "segl.h"
#include "log.h"

#define EXPORT_RENDER 0

#ifndef EGL_EGLEXT_PROTOTYPES
#ifdef EGL_KHR_image
static PFNEGLCREATEIMAGEKHRPROC eglCreateImageKHR = NULL;
static PFNEGLDESTROYIMAGEKHRPROC eglDestroyImageKHR = NULL;
#else
#error "Mesa export not available"
#endif
#ifdef EGL_MESA_image_dma_buf_export
static PFNEGLEXPORTDMABUFIMAGEQUERYMESAPROC eglExportDMABUFImageQueryMESA = NULL;
static PFNEGLEXPORTDMABUFIMAGEMESAPROC eglExportDMABUFImageMESA = NULL;
#endif

static int _egl_initprototypes(void)
{
#ifdef EGL_MESA_image_dma_buf_export
	eglExportDMABUFImageQueryMESA = (void *) eglGetProcAddress("eglExportDMABUFImageQueryMESA");
	if(eglExportDMABUFImageQueryMESA == NULL)
	{
		return -1;
	}
	eglExportDMABUFImageMESA = (void *) eglGetProcAddress("eglExportDMABUFImageMESA");
	if(eglExportDMABUFImageMESA == NULL)
	{
		return -1;
	}
#endif
#ifdef EGL_KHR_image
	eglCreateImageKHR = (void *) eglGetProcAddress("eglCreateImageKHR");
	if(eglCreateImageKHR == NULL)
	{
		return -1;
	}
	eglDestroyImageKHR = (void *) eglGetProcAddress("eglDestroyImageKHR");
	if(eglDestroyImageKHR == NULL)
	{
		return -1;
	}
#endif
	return 0;
}
#endif

typedef struct EGLExportImageMesa_s EGLExportImageMesa_t;
struct EGLExportImageMesa_s
{
	EGLConfig_t *config;
	EGLDisplay egldisplay;
	EGLContext eglcontext;
	GLuint fbo;
	GLuint rbo;
	GL_Buffer_t out;
};

static void *_egl_export_create(EGLConfig_t *config, EGLDisplay eglDisplay, EGLContext eglContext)
{
	EGLExportImageMesa_t *ctx = calloc(1, sizeof(*ctx));
	ctx->config = config;
	ctx->egldisplay = eglDisplay;
	ctx->eglcontext = eglContext;

	uint32_t width = ctx->config->parent.width;
	uint32_t height = ctx->config->parent.height;
	GLuint glget = 0;
	glGetIntegerv(GL_MAX_RENDERBUFFER_SIZE, &glget);
	if (glget <= width)
		warn("segl: width to large max %d", glget);
	if (glget <= height)
		warn("segl: width to height max %d", glget);

	const FourccFormat_t *fformat = fourcc_getformat(config->parent.fourcc);

	/*  Framebuffer */
	glGenFramebuffers(1, &ctx->fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, ctx->fbo);
#if EXPORT_RENDER
	ctx->out.texture = ctx->rbo;
	ctx->out.textype = GL_RENDERBUFFER;
	ctx->out.egltarget = EGL_GL_RENDERBUFFER;

	glGenRenderbuffers(1, &ctx->rbo);
	glBindRenderbuffer(GL_RENDERBUFFER, ctx->rbo);
	GLuint format = fformat->internal;
	/* Storage must be one of: */
	/* GL_RGBA4, GL_RGB565, GL_RGB5_A1, GL_DEPTH_COMPONENT16, GL_STENCIL_INDEX8. */
	glRenderbufferStorage(ctx->out.textype, format, width, height);
	dbg("segl: renderbuffer %lux%lu %#x/%#x", width, height, format, fformat->internal);
	GLuint glerror = glGetError();
	if (glerror)
	{
		err ("segl: Renderbuffer error %#x", glerror);
		return NULL;
	}

	//glGetRenderbufferParameteriv(ctx->textype, GL_RENDERBUFFER_SAMPLES, &samples);
	glGetRenderbufferParameteriv(ctx->out.textype, GL_RENDERBUFFER_INTERNAL_FORMAT, &format);
	glGetRenderbufferParameteriv(ctx->out.textype, GL_RENDERBUFFER_WIDTH, &width);
	glGetRenderbufferParameteriv(ctx->out.textype, GL_RENDERBUFFER_HEIGHT, &height);

	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
		ctx->out.textype, ctx->rbo);
#else
	/// glFramebufferTexture2D support only GL_TEXTURE_2D
	ctx->out.egltarget = EGL_GL_TEXTURE_2D;
	ctx->out.textype = GL_TEXTURE_2D;

	glGenTextures(1, &ctx->out.texture);
	if (ctx->out.texture == 0)
	{
		err("segl: output texture creation error");
		free(ctx);
		return NULL;
	}
	glBindTexture(ctx->out.textype, ctx->out.texture);
	glTexParameteri(ctx->out.textype, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(ctx->out.textype, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri(ctx->out.textype, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(ctx->out.textype, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

	glTexImage2D(ctx->out.textype, 0, fformat->internal,
			width, height, 0, fformat->full, fformat->data, NULL);
	GLuint glerror = glGetError();
	if (glerror)
	{
		err ("segl: Texturebuffer error %#x", glerror);
		free(ctx);
		return NULL;
	}
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
		ctx->out.textype, ctx->out.texture, 0);
#endif
	/* Sanity check. */
	GLint ret = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	if (ret != GL_FRAMEBUFFER_COMPLETE)
	{
		err("segl: generator failed for %.4s", &ctx->config->parent.fourcc);
		return NULL;
	}
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	return ctx;
}

static GLuint _egl_export_fbo(void *arg)
{
	EGLExportImageMesa_t *ctx = (EGLExportImageMesa_t *)arg;
	return ctx->fbo;
}

static GL_Buffer_t *_egl_export_out(void *arg)
{
	EGLExportImageMesa_t *ctx = (EGLExportImageMesa_t *)arg;
	return &ctx->out;
}

static int _egl_export_setbuffer(void *arg, GLBuffer_t *buffer)
{
	EGLExportImageMesa_t *ctx = (EGLExportImageMesa_t *)arg;
	const EGLint tattributes[] = {
		EGL_IMAGE_PRESERVED, EGL_TRUE,
		EGL_NONE,
	};
	const EGLint *attributes = tattributes;

	/// eglCreateImage and eglCreateImageKHR have the same result
	EGLImage image = eglCreateImageKHR(ctx->egldisplay, ctx->eglcontext,
		ctx->out.egltarget, (void *)(long)ctx->out.texture, attributes);
	if (image == EGL_NO_IMAGE)
		return -1;

	int numplanes = 0;
	EGLint stride[5] = {0};
	EGLint offset[5] = {0};
	int fourcc = ctx->config->parent.fourcc;
	int dma_buf[5] = {0};
	uint64_t modifiers[4] = {0};

	eglExportDMABUFImageQueryMESA(ctx->egldisplay, image,
								&fourcc, &numplanes, &modifiers[0]);
	if (numplanes < 5)
	{
		eglExportDMABUFImageMESA(ctx->egldisplay, image, &dma_buf[0], &stride[0], &offset[0]);
	}
//	if (stride[0] != dev->buffers[id].size / dev->config->parent.height)
//		err("segl: exported format not aligned");
	if (ctx->config->parent.fourcc && ctx->config->parent.fourcc != fourcc)
		err("segl: requests %.4s, obtains %.4s", &ctx->config->parent.fourcc, &fourcc);
	ctx->config->parent.fourcc = fourcc;

	dbg("segl: export format modifier %.4s, %#x", &fourcc, modifiers[0]);
	for (int i = 0; i < 4 && modifiers[0] != ctx->config->parent.modifiers; i++)
		err("segl: format modifier present but not set (%lld/%lld)", modifiers[i], ctx->config->parent.modifiers);
	ctx->config->parent.modifiers = modifiers[0];
	eglDestroyImageKHR(ctx->egldisplay, image);

	uint32_t size = stride[0] * ctx->config->parent.height;
	if (!buffer->size)
		buffer->size = size;
	if (buffer->size != size)
		err("segl: export image size different");
	buffer->pitch = stride[0];
	buffer->dma_fd = dma_buf[0];
	buffer->offset = offset[0];
	return 0;
}

static int _egl_export_flush(void *arg, GLBuffer_t *buffer)
{
	return 0;
}

static int _egl_export_releasebuffer(void *arg, GLBuffer_t *buffer)
{
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

EGLExport_t export_imagemesa =
{
	.name = "imagemesa",
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
	_egl_initprototypes();
	segl_export_append_t _segl_export_append;
	void *hdl = dlopen(NULL, RTLD_NOW);
	_segl_export_append = dlsym(hdl, "segl_export_append");
	if (_segl_export_append)
	{
		_segl_export_append(&export_imagemesa);
	}
}
