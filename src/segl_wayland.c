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
#include "xdg-shell-client-protocol.h"
#endif

#ifndef TRUE
#define TRUE GL_TRUE
#define FALSE GL_FALSE
#endif

// wayland related local variables
static struct native_context_s {
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
} g_context = {0};

static EGLNativeDisplayType native_display(EGLConfig_t *config)
{
	if (g_context.display == NULL)
		/** environment management */
		g_context.display = wl_display_connect(NULL);
	if (g_context.display == NULL)
		err("segl: no connection to Wayland");
	return (EGLNativeDisplayType)g_context.display;
}

static int native_fd(EGLNativeWindowType native_win)
{
	return -1;
}

static int native_flush(EGLNativeWindowType native_win)
{
	int run = 1;
	while (run)
	{
		if (wl_display_dispatch(g_context.display) != -1)
			run = 0;
	}
	return 0;
}

static int native_sync(EGLNativeWindowType native_win)
{
	if (g_context.run = 0)
		return -1;
	return 0;
}

#ifdef WL_SHELL
static void shell_surface_ping(void* data, struct wl_shell_surface* shell_surface, uint32_t serial)
{
	struct native_context_s *context = data;
	wl_shell_surface_pong(context->shell_surface, serial);
}

static void shell_surface_configure(void* data, struct wl_shell_surface* shell_surface, uint32_t edges, int32_t width, int32_t height)
{
	struct native_context_s *context = data;
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
	struct native_context_s *context = data;

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
	struct native_context_s *context = data;
	context->run = 0;
}

static const struct xdg_toplevel_listener xdg_toplevel_listener = {
	toplevel_configure,
	toplevel_close
};
#endif

static void registry_add_object(void* data, struct wl_registry* registry, uint32_t name, const char* interface, uint32_t version)
{
	struct native_context_s *context = data;
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

static EGLNativeWindowType native_createwindow(EGLNativeDisplayType native_display, GLuint width, GLuint height, const GLchar *name)
{
	g_context.registry = wl_display_get_registry(native_display);
	wl_registry_add_listener(g_context.registry, &registry_listener, &g_context);
	wl_display_dispatch(g_context.display);
	wl_display_roundtrip(g_context.display);
	g_context.width = width;
	g_context.height = height;

	/// compositor and shell created during wl_display_roundtrip with registry_add_object
	if (g_context.compositor == NULL)
		return (EGLNativeWindowType) NULL;
#if !defined(XDG_WM_BASE)
	if (g_context.shell == NULL)
#elif !defined(WL_SHELL)
	if (g_context.xdg_wm_base == NULL)
#else
	if (g_context.shell == NULL && g_context.xdg_wm_base == NULL)
#endif
		return (EGLNativeWindowType) NULL;

	g_context.surface = wl_compositor_create_surface(g_context.compositor);

#ifdef WL_SHELL
	if (g_context.shell)
	{
		g_context.shell_surface = wl_shell_get_shell_surface(g_context.shell, g_context.surface);
		wl_shell_surface_add_listener(g_context.shell_surface, &shell_surface_listener, &g_context);
		wl_shell_surface_set_toplevel(g_context.shell_surface);
	}
	else
#endif
#ifdef XDG_WM_BASE
	if (g_context.xdg_wm_base)
	{
		xdg_wm_base_add_listener(g_context.xdg_wm_base, &wm_base_listener, &g_context);

		g_context.xdg_surface = xdg_wm_base_get_xdg_surface(g_context.xdg_wm_base,
								g_context.surface);
		xdg_surface_add_listener(g_context.xdg_surface, &xdg_surface_listener, &g_context);
		g_context.xdg_toplevel = xdg_surface_get_toplevel(g_context.xdg_surface);
		xdg_toplevel_set_title(g_context.xdg_toplevel, name);
		xdg_toplevel_add_listener(g_context.xdg_toplevel, &xdg_toplevel_listener, &g_context);
	}
	else
#endif
	{
		err("segl: surface not found");
	}
	wl_surface_commit(g_context.surface);

	g_context.egl_window = wl_egl_window_create(g_context.surface, width, height);
	g_context.run = 1;

	return (EGLNativeWindowType) g_context.egl_window;
}

static void native_destroy(EGLNativeDisplayType native_display)
{
	wl_display_disconnect(native_display);
}

EGLNative_t *eglnative_wayland = &(EGLNative_t)
{
	.name = "wayland",
	.display = native_display,
	.createwindow = native_createwindow,
	.fd = native_fd,
	.flush = native_flush,
	.sync = native_sync,
	.destroy = native_destroy,
};
