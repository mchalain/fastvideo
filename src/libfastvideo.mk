lib-y+=fastvideo
fastvideo_SOURCES+=sv4l2.c
fastvideo_SOURCES+=sfile.c
fastvideo_SOURCES+=smedia.c
fastvideo_SOURCES-$(HAVE_LIBDRM)+=sdrm.c
fastvideo_SOURCES-$(HAVE_EGL)+=segl.c
fastvideo_SOURCES-$(HAVE_EGL)+=segl_glprog.c
fastvideo_SOURCES-$(HAVE_GBM)+=segl_drm.c
fastvideo_SOURCES-$(HAVE_X11)+=segl_x11.c
fastvideo_LIBRARY-$(DRM)+=libdrm
fastvideo_LIBRARY-$(EGL)+=glesv2
fastvideo_LIBRARY-$(EGL)+=egl
fastvideo_LIBRARY-$(EGL)+=gbm
fastvideo_LIBRARY-$(EGL)+=x11
fastvideo_PKGCONFIG+=fastvideo
