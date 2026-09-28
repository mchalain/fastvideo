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

#include <wayland-client.h>
#include <wayland-server.h>
#include <wayland-client-protocol.h>
#include <wayland-egl.h>

#define WL_SHELL
#define XDG_WM_BASE
#ifdef XDG_WM_BASE
#include "stable/xdg-shell/xdg-shell.xml.wliext.h"
#endif

#ifndef TRUE
#define TRUE GL_TRUE
#define FALSE GL_FALSE
#endif

// wayland related local variables
typedef struct SEGLNative_ctx_s SEGLNative_ctx_t;
struct SEGLNative_ctx_s {
	struct wl_display *display;
	struct wl_registry* registry;
	struct wl_compositor* compositor;
#ifdef WL_SHELL
	struct wl_shell* shell;
	struct wl_shell_surface* shell_surface;
#endif
#ifdef XDG_WM_BASE
	struct xdg_wm_base *xdg_wm_base;
	struct xdg_surface *xdg_surface;
	struct xdg_toplevel *xdg_toplevel;
#endif
	struct wl_surface* surface;
	struct wl_egl_window *egl_window;
	int32_t width;
	int32_t height;
	int run;
};

static void *native_create(EGLConfig_t *config)
{
	struct wl_display *display = wl_display_connect(NULL);
	if (display == NULL)
	{
		err("segl: no connection to Wayland");
		return NULL;
	}
	SEGLNative_ctx_t *ctx = calloc(1, sizeof(*ctx));
	ctx->display = display;
	return ctx;
}

static EGLNativeDisplayType native_display(void *native_ctx)
{
	SEGLNative_ctx_t *ctx = (SEGLNative_ctx_t*)native_ctx;
	return (EGLNativeDisplayType)ctx->display;
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
	SEGLNative_ctx_t *ctx = (SEGLNative_ctx_t*)native_ctx;
	int run = 1;
	while (run)
	{
		if (wl_display_dispatch(ctx->display) != -1)
			run = 0;
	}
	return 0;
}

static int native_sync(void *native_ctx)
{
	SEGLNative_ctx_t *ctx = (SEGLNative_ctx_t*)native_ctx;
	if (ctx->run == 0)
		return -1;
	return 0;
}

#ifdef WL_SHELL
static void shell_surface_ping(void* data, struct wl_shell_surface* shell_surface, uint32_t serial)
{
	SEGLNative_ctx_t *context = data;
	wl_shell_surface_pong(context->shell_surface, serial);
}

static void shell_surface_configure(void* data, struct wl_shell_surface* shell_surface, uint32_t edges, int32_t width, int32_t height)
{
	SEGLNative_ctx_t *context = data;
	wl_egl_window_resize(context->egl_window, width, height, 0, 0);
}

static void shell_surface_popup_done(void* data, struct wl_shell_surface* shell_surface)
{
}

static struct wl_shell_surface_listener shell_surface_listener = {
	&shell_surface_ping,
	&shell_surface_configure,
	&shell_surface_popup_done
};
#endif

#ifdef XDG_WM_BASE
static void wm_ping(void *data, struct xdg_wm_base *xdg_wm_base, uint32_t serial)
{
	xdg_wm_base_pong(xdg_wm_base, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = {
	wm_ping,
};

static void surface_configure(void *data, struct xdg_surface *xdg_surface, uint32_t serial)
{
	xdg_surface_ack_configure(xdg_surface, serial);
}

static const struct xdg_surface_listener xdg_surface_listener = {
	surface_configure
};

static void toplevel_configure(void *data, struct xdg_toplevel *xdg_toplevel,
				int32_t width, int32_t height, struct wl_array *states)
{
	SEGLNative_ctx_t *context = data;

	if(!width && !height)
		return;

	if(context->width != width || context->height != height)
	{
		context->width = width;
		context->height = height;

		wl_egl_window_resize(context->egl_window, width, height, 0, 0);
		wl_surface_commit(context->surface);
	}
}

static void toplevel_close(void *data, struct xdg_toplevel *xdg_toplevel)
{
	SEGLNative_ctx_t *context = data;
	context->run = 0;
}

static const struct xdg_toplevel_listener xdg_toplevel_listener = {
	toplevel_configure,
	toplevel_close
};
#endif

static void registry_add_object(void* data, struct wl_registry* registry, uint32_t name, const char* interface, uint32_t version)
{
	SEGLNative_ctx_t *context = data;
	if (!strcmp(interface, wl_compositor_interface.name))
	{
		context->compositor = wl_registry_bind(context->registry, name, &wl_compositor_interface, 1);
		if (context->compositor == NULL)
			err("segl: compositor registry error %m");
	}
#ifdef WL_SHELL
	else if (!strcmp(interface, wl_shell_interface.name))
	{
		context->shell = wl_registry_bind(context->registry, name, &wl_shell_interface, 1);
		if (context->shell == NULL)
			err("segl: shell registry error %m");
	}
#endif
#ifdef XDG_WM_BASE
	else if (!strcmp(interface, xdg_wm_base_interface.name))
	{
		context->xdg_wm_base = wl_registry_bind(context->registry, name, &xdg_wm_base_interface, 1);
		if (context->xdg_wm_base == NULL)
			err("segl: xdg_wm_base registry error %m");
	}
#endif
	dbg("segl: registry objects %s", interface);
}

static void registry_remove_object(void* data, struct wl_registry* registry, uint32_t name)
{
}

static struct wl_registry_listener registry_listener = {
	&registry_add_object,
	&registry_remove_object
};

static EGLNativeWindowType native_createwindow(void *native_ctx, GLuint width, GLuint height, const GLchar *name)
{
	SEGLNative_ctx_t *ctx = (SEGLNative_ctx_t*)native_ctx;
	ctx->registry = wl_display_get_registry(ctx->display);
	wl_registry_add_listener(ctx->registry, &registry_listener, ctx);
	wl_display_dispatch(ctx->display);
	wl_display_roundtrip(ctx->display);
	ctx->width = width;
	ctx->height = height;

	/// compositor and shell created during wl_display_roundtrip with registry_add_object
	if (ctx->compositor == NULL)
		return (EGLNativeWindowType) NULL;
#if !defined(XDG_WM_BASE)
	if (ctx->shell == NULL)
#elif !defined(WL_SHELL)
	if (ctx->xdg_wm_base == NULL)
#else
	if (ctx->shell == NULL && ctx->xdg_wm_base == NULL)
#endif
		return (EGLNativeWindowType) NULL;

	ctx->surface = wl_compositor_create_surface(ctx->compositor);

#ifdef WL_SHELL
	if (ctx->shell)
	{
		ctx->shell_surface = wl_shell_get_shell_surface(ctx->shell, ctx->surface);
		wl_shell_surface_add_listener(ctx->shell_surface, &shell_surface_listener, ctx);
		wl_shell_surface_set_toplevel(ctx->shell_surface);
	}
	else
#endif
#ifdef XDG_WM_BASE
	if (ctx->xdg_wm_base)
	{
		xdg_wm_base_add_listener(ctx->xdg_wm_base, &wm_base_listener, ctx);

		ctx->xdg_surface = xdg_wm_base_get_xdg_surface(ctx->xdg_wm_base,
								ctx->surface);
		xdg_surface_add_listener(ctx->xdg_surface, &xdg_surface_listener, ctx);
		ctx->xdg_toplevel = xdg_surface_get_toplevel(ctx->xdg_surface);
		xdg_toplevel_set_title(ctx->xdg_toplevel, name);
		xdg_toplevel_add_listener(ctx->xdg_toplevel, &xdg_toplevel_listener, ctx);
	}
	else
#endif
	{
		err("segl: surface not found");
	}
	wl_surface_commit(ctx->surface);

	ctx->egl_window = wl_egl_window_create(ctx->surface, width, height);
	ctx->run = 1;

	return (EGLNativeWindowType) ctx->egl_window;
}

static void native_destroy(void *native_ctx)
{
	SEGLNative_ctx_t *ctx = (SEGLNative_ctx_t*)native_ctx;
	wl_display_disconnect(ctx->display);
	free(ctx);
}

EGLNative_t eglnative_wayland =
{
	.name = "wayland",
	.create = native_create,
	.display = native_display,
	.attributes = native_attributes,
	.createwindow = native_createwindow,
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
		_segl_native_append(&eglnative_wayland);
	}
}
