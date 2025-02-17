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

static EGLNativeDisplayType native_display(const char *device)
{
	return (EGLNativeDisplayType)EGL_DEFAULT_DISPLAY;
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
	.createwindow = native_createwindow,
	.fd = native_fd,
	.flush = native_flush,
	.sync = native_sync,
	.destroy = native_destroy,
};
