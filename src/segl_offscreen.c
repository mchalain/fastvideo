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

static EGLNativeDisplayType native_display(EGLConfig_t *config)
{
	if (!_egl_hasextension(EGL_NO_DISPLAY, "EGL_EXT_platform_base"))
		return (EGLNativeDisplayType)EGL_DEFAULT_DISPLAY;
	if (!_egl_hasextension(EGL_NO_DISPLAY, "EGL_MESA_platform_surfaceless"))
		return (EGLNativeDisplayType)EGL_DEFAULT_DISPLAY;
	PFNEGLGETPLATFORMDISPLAYEXTPROC eglGetPlatformDisplayEXT = (void *) eglGetProcAddress("eglGetPlatformDisplayEXT");

	return eglGetPlatformDisplayEXT(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
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

static const GLint *native_attributes(EGLNativeDisplayType display)
{
	return g_attributes;
}

static int native_fd(EGLNativeWindowType native_win)
{
	return -1;
}

static int native_flush(EGLNativeWindowType native_win)
{
	return 0;
}

static int native_sync(EGLNativeWindowType native_win)
{
	return 0;
}

static EGLNativeWindowType native_createwindow(EGLNativeDisplayType native_display, GLuint width, GLuint height, const GLchar *name)
{
	return (EGLNativeWindowType) NULL;
}

static void native_destroy(EGLNativeDisplayType native_display)
{
}

EGLNative_t *eglnative_offscreen = &(EGLNative_t)
{
	.name = "offscreen",
	.display = native_display,
	.attributes = native_attributes,
	.createwindow = native_createwindow,
	.fd = native_fd,
	.flush = native_flush,
	.sync = native_sync,
	.destroy = native_destroy,
};
