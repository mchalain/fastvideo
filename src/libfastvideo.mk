lib-y+=fastvideo
fastvideo_SOURCES+=spassthrough.c
fastvideo_SOURCES+=sv4l2.c
fastvideo_SOURCES+=sv4l2_subdev.c
fastvideo_SOURCES-$(DVB)+=sdvb.c
fastvideo_SOURCES+=sfile.c
fastvideo_SOURCES+=sfile_passthrough.c
fastvideo_SOURCES+=smedia.c
fastvideo_SOURCES-$(HAVE_JANSSON)+=config.c
fastvideo_SOURCES-$(HAVE_LIBDRM)+=sdrm.c
fastvideo_SOURCES-$(HAVE_EGL)+=segl.c
fastvideo_SOURCES-$(HAVE_EGL)+=segl_offscreen.c
fastvideo_SOURCES-$(HAVE_EGL)+=segl_glprog.c
fastvideo_SOURCES-$(HAVE_GBM)+=segl_drm.c
fastvideo_SOURCES-$(HAVE_X11)+=segl_x11.c
fastvideo_SOURCES-$(HAVE_WAYLAND_EGL)+=segl_wayland.c
fastvideo_GENERATED-$(HAVE_WAYLAND_EGL)+=xdg-shell-protocol.c
fastvideo_LIBS+=dl
fastvideo_LIBRARY-$(DRM)+=libdrm
fastvideo_LIBRARY-$(EGL)+=glesv2
fastvideo_LIBRARY-$(EGL)+=egl
fastvideo_LIBRARY-$(EGL)+=gbm
fastvideo_LIBRARY-$(EGL)+=x11
fastvideo_LIBRARY-$(EGL)+=wayland-egl
fastvideo_PKGCONFIG+=fastvideo

PKG_CONFIG?=pkg-config
WAYLAND_FLAGS = $(shell $(PKG_CONFIG) wayland-client --cflags --libs)
WAYLAND_PROTOCOLS_DIR = $(shell $(PKG_CONFIG) wayland-protocols --variable=pkgdatadir)
WAYLAND_SCANNER = $(shell $(PKG_CONFIG) --variable=wayland_scanner wayland-scanner)
XDG_SHELL_PROTOCOL = $(WAYLAND_PROTOCOLS_DIR)/stable/xdg-shell/xdg-shell.xml

$(objdir)xdg-shell-client-protocol.h: $(objdir)
	$(WAYLAND_SCANNER) client-header $(XDG_SHELL_PROTOCOL) $@

$(objdir)xdg-shell-protocol.c: $(objdir)xdg-shell-client-protocol.h
	$(WAYLAND_SCANNER) private-code $(XDG_SHELL_PROTOCOL) $@
