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

#include  <X11/Xlib.h>
#include  <X11/Xatom.h>
#include  <X11/Xutil.h>

#ifndef TRUE
#define TRUE GL_TRUE
#define FALSE GL_FALSE
#endif

// X11 related local variables
typedef struct SEGLNative_ctx_s SEGLNative_ctx_t;
struct SEGLNative_ctx_s
{
	EGLConfig_t *config;
	Display *display;
	Window window;
};

static void *native_create(EGLConfig_t *config)
{
	Display *display;
	display = XOpenDisplay(NULL);
	if (display == NULL)
	{
		err("segl: no connection to X11");
		return NULL;
	}
	SEGLNative_ctx_t *ctx = calloc(1, sizeof(*ctx));
	ctx->display = display;
	ctx->config = config;
	return ctx;
}

static EGLNativeDisplayType native_display(void *native_ctx)
{
	SEGLNative_ctx_t *ctx = (SEGLNative_ctx_t*)native_ctx;
	EGLDisplay display = eglGetPlatformDisplay(EGL_PLATFORM_X11_KHR, ctx->display, NULL);
	return display;
}

static const EGLint g_attributes[] = {
	EGL_RED_SIZE, 8,
	EGL_GREEN_SIZE, 8,
	EGL_BLUE_SIZE, 8,
	EGL_ALPHA_SIZE, 8,
	//EGL_DEPTH_SIZE, 16, // DEPTH management in useless for this application
	EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
	EGL_NONE
};

static const GLint *native_attributes(void *natvie_ctx)
{
	return g_attributes;
}

static int native_fd(void *natvie_ctx)
{
	return -1;
}

static int native_flush(void *native_ctx)
{
	SEGLNative_ctx_t *ctx = (SEGLNative_ctx_t *)native_ctx;
	XEvent xev;
	KeySym key;

	while (XPending(ctx->display))
	{
		char text = 0;
		XNextEvent(ctx->display, &xev);
		if (xev.type == KeyPress)
		{
			if (XLookupString(&xev.xkey,&text,1,&key,0)==1)
			{
				if (text == 'q')
					return -1;
			}
			if (xev.xkey.keycode == 0x09)
				return -1;
		}
		if (xev.type == KeyRelease)
		{
		}
		if ( xev.type == DestroyNotify )
			return -1;
	}
	return 0;
}

static int native_sync(void *native_ctx)
{
	return 0;
}

static EGLSurface native_surface(void *native_ctx, EGLConfig eglConfig)
{
	SEGLNative_ctx_t *ctx = (SEGLNative_ctx_t*)native_ctx;
	Display *display = ctx->display;
	Window root = DefaultRootWindow(display);
	int width = ctx->config->transfer.width;
	int height = ctx->config->transfer.height;

	XSetWindowAttributes swa;
	swa.event_mask  =  ExposureMask | PointerMotionMask | KeyPressMask | KeyReleaseMask;
	Window win;
	win = XCreateWindow(
				display, root,
				0, 0, width, height, 0,
				CopyFromParent, InputOutput,
				CopyFromParent, CWEventMask,
				&swa );

	if (win == 0)
	{
		err("segl: X11 windows creation error %m");
		return 0;
	}
	XSetWindowAttributes  xattr;
	xattr.override_redirect = FALSE;
	XChangeWindowAttributes(display, win, CWOverrideRedirect, &xattr );

	XWMHints hints;
	hints.input = TRUE;
	hints.flags = InputHint;
	XSetWMHints(display, win, &hints);

	XMapWindow(display, win);
	XStoreName(display, win, "fastvideo");

	Atom wm_state;
	wm_state = XInternAtom(display, "_NET_WM_STATE", FALSE);

	XEvent xev;
	memset(&xev, 0, sizeof(xev) );
	xev.type                 = ClientMessage;
	xev.xclient.window       = win;
	xev.xclient.message_type = wm_state;
	xev.xclient.format       = 32;
	xev.xclient.data.l[0]    = 1;
	xev.xclient.data.l[1]    = FALSE;
	XSendEvent(display, DefaultRootWindow(display ),
		FALSE, SubstructureNotifyMask, &xev );
	ctx->window = win;
	EGLint attribs[] = {
		//EGL_GL_COLORSPACE,  EGL_GL_COLORSPACE_LINEAR,
		EGL_NONE,
	};
	EGLSurface eglSurface = eglCreateWindowSurface(native_display(ctx), eglConfig, (EGLNativeWindowType)ctx->window, attribs);
	return eglSurface;
}

static void native_destroy(void *native_ctx)
{
	SEGLNative_ctx_t *ctx = (SEGLNative_ctx_t*)native_ctx;
	free(ctx);
}

EGLNative_t eglnative_x11 =
{
	.name = "x11",
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
		_segl_native_append(&eglnative_x11);
	}
}
