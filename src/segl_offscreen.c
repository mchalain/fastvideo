#include <stdlib.h>
#include <string.h>

#ifdef HAVE_GLESV2
# include <GLES2/gl2.h>
#else
# include <GL/gl.h>
#endif
#include <EGL/egl.h>
#include <EGL/eglext.h>

#include "log.h"
#include "segl.h"

#ifndef TRUE
#define TRUE GL_TRUE
#define FALSE GL_FALSE
#endif

#define EXPORT_RENDER 1

typedef struct SEGLNative_ctx_s SEGLNative_ctx_t;
struct SEGLNative_ctx_s
{
	EGLConfig_t *config;
};

static void *native_create(EGLConfig_t *config)
{
	if (!_egl_hasextension(EGL_NO_DISPLAY, "EGL_EXT_platform_base"))
		return NULL;
	if (!_egl_hasextension(EGL_NO_DISPLAY, "EGL_MESA_platform_surfaceless"))
		return NULL;
	SEGLNative_ctx_t *ctx = calloc(1, sizeof(*ctx));
	ctx->config = config;
	return ctx;
}

static EGLDisplay native_display(void *native_ctx)
{
	//PFNEGLGETPLATFORMDISPLAYEXTPROC eglGetPlatformDisplay = (void *) eglGetProcAddress("eglGetPlatformDisplayEXT");
	EGLDisplay display = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
	dbg("%s %d, %p", __FILE__, __LINE__, display);
	return display;
}

static const EGLint g_attributes[] = {
	EGL_RED_SIZE, 8,
	EGL_GREEN_SIZE, 8,
	EGL_BLUE_SIZE, 8,
	EGL_ALPHA_SIZE, 8,
	//EGL_DEPTH_SIZE, 16, // DEPTH management in useless for this application
	EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
	EGL_COLOR_BUFFER_TYPE, EGL_RGB_BUFFER,
	EGL_BIND_TO_TEXTURE_RGBA , EGL_TRUE,
	EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
	EGL_NONE
};

static const GLint *native_attributes(void *native_ctx)
{
	return g_attributes;
}

static int native_fd(void *native_ctx)
{
	return -1;
}

static int native_flush(void *native_ctx)
{
	return 0;
}

static int native_sync(void *native_ctx)
{
	return 0;
}

static EGLSurface native_surface(void *native_ctx, EGLConfig eglConfig)
{
	SEGLNative_ctx_t *ctx = (SEGLNative_ctx_t *)native_ctx;
	EGLConfig_t *config = ctx->config;
	EGLint texture_format = EGL_TEXTURE_RGBA;
	EGLint texturergb = 0;
	eglGetConfigAttrib(native_display(ctx), eglConfig, EGL_BIND_TO_TEXTURE_RGB, &texturergb);
	if (texturergb)
		texture_format = EGL_TEXTURE_RGB;
	warn("segl: surface on pbuffer");
	EGLint attribs[] = {
		EGL_WIDTH, config->transfer.width,
		EGL_HEIGHT, config->transfer.height,
		EGL_TEXTURE_FORMAT, texture_format,
		EGL_TEXTURE_TARGET, EGL_TEXTURE_2D,
		//EGL_LARGEST_PBUFFER, EGL_TRUE, // no visible change
		EGL_NONE,
	};

	EGLSurface eglSurface = eglCreatePbufferSurface(native_display(ctx), eglConfig, attribs);
	return eglSurface;
}

static void native_destroy(void *native_ctx)
{
	free(native_ctx);
}

EGLNative_t eglnative_offscreen =
{
	.name = "offscreen",
	.create = native_create,
	.display = native_display,
	.attributes = native_attributes,
	.surface = native_surface,
	.fd = native_fd,
	.flush = native_flush,
	.sync = native_sync,
	.destroy = native_destroy,
};

#include <dlfcn.h>

static void __attribute__ ((constructor)) segl_init()
{
	segl_native_append_t _segl_native_append;
	void *hdl = dlopen(NULL, RTLD_NOW);
	_segl_native_append = dlsym(hdl, "segl_native_append");
	if (_segl_native_append)
	{
		_segl_native_append(&eglnative_offscreen);
	}
}
