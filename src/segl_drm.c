#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#include <GLES2/gl2.h>
#include <EGL/egl.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm.h>
#include <drm_mode.h>
#include <drm_fourcc.h>
#include <gbm.h>

#include "segl.h"
#include "log.h"

#ifndef GBM_FORMAT_XBGR16161616F
# define GBM_FORMAT_XBGR16161616F DRM_FORMAT_XBGR16161616F
#endif
#ifndef GBM_FORMAT_ABGR16161616F
# define GBM_FORMAT_ABGR16161616F DRM_FORMAT_ABGR16161616F
#endif

static struct drm_s {
	uint32_t fourcc;
	int fd;
	drmModeModeInfo *mode;
	uint32_t crtc_id;
	uint32_t connector_id;
	int waiting_for_flip;
} drm;

struct drm_fb {
	struct drm_s *drm;
	struct gbm_bo *bo;
	uint32_t fb_id;
};

static uint32_t find_crtc_for_encoder(const drmModeRes *resources,
				      const drmModeEncoder *encoder) {
	int i;

	for (i = 0; i < resources->count_crtcs; i++) {
		/* possible_crtcs is a bitmask as described here:
		 * https://dvdhrm.wordpress.com/2012/09/13/linux-drm-mode-setting-api
		 */
		const uint32_t crtc_mask = 1 << i;
		const uint32_t crtc_id = resources->crtcs[i];
		if (encoder->possible_crtcs & crtc_mask) {
			return crtc_id;
		}
	}

	/* no match found */
	return -1;
}

static uint32_t find_crtc_for_connector(int fd, const drmModeRes *resources,
					const drmModeConnector *connector) {
	int i;

	for (i = 0; i < connector->count_encoders; i++) {
		const uint32_t encoder_id = connector->encoders[i];
		drmModeEncoder *encoder = drmModeGetEncoder(fd, encoder_id);

		if (encoder) {
			const uint32_t crtc_id = find_crtc_for_encoder(resources, encoder);

			drmModeFreeEncoder(encoder);
			if (crtc_id != 0) {
				return crtc_id;
			}
		}
	}

	/* no match found */
	return -1;
}

static drmModeConnector *find_connector(int fd, drmModeRes *resources, uint32_t width, uint32_t height, drmModeModeInfo **mode, int force)
{
	drmModeConnector *connector = NULL;
	for (int i = 0; i < resources->count_connectors; i++)
	{
		connector = drmModeGetConnector(fd, resources->connectors[i]);
		if (!force && connector->connection != DRM_MODE_CONNECTED)
			continue;
		for (int j = 0; j < connector->count_modes; j++)
		{
			drmModeModeInfo *current_mode = &connector->modes[j];

			if (current_mode->vdisplay == height &&
					current_mode->hdisplay >= width)
			{
				if (current_mode->hdisplay == width ||
					(current_mode->type & DRM_MODE_TYPE_PREFERRED))
				{
					*mode = current_mode;
					break;
				}
			}
		}
		if (*mode)
			break;
		drmModeFreeConnector(connector);
		connector = NULL;
	}
	return connector;
}

static int init_drm(int fd, uint32_t fourcc, uint32_t width, uint32_t height)
{
	drmModeRes *resources;
	drmModeConnector *connector = NULL;
	drmModeEncoder *encoder = NULL;

	drm.fd = fd;
	drm.fourcc = fourcc;
	resources = drmModeGetResources(fd);
	if (!resources)
	{
		err("segl: drmModeGetResources failed: %m");
		return -1;
	}

	/* find a connected connector: */
	connector = find_connector(fd, resources, width, height, &drm.mode, 0);

	if (!connector)
	{
		/* we could be fancy and listen for hotplug events and wait for
		 * a connector..
		 */
		err("segl: no connected connector!");
		connector = find_connector(fd, resources, width, height, &drm.mode, 1);
	}

	if (!drm.mode)
	{
		err("segl: could not find mode!");
		connector = drmModeGetConnector(fd, resources->connectors[0]);
	}

	/* find encoder: */
	for (int i = 0; i < resources->count_encoders; i++)
	{
		encoder = drmModeGetEncoder(fd, resources->encoders[i]);
		if (encoder->encoder_id == connector->encoder_id)
			break;
		drmModeFreeEncoder(encoder);
		encoder = NULL;
	}

	if (encoder) {
		drm.crtc_id = encoder->crtc_id;
	} else {
		uint32_t crtc_id = find_crtc_for_connector(fd, resources, connector);
		if (crtc_id == 0) {
			err("segl: no crtc found!");
			return -1;
		}

		drm.crtc_id = crtc_id;
	}

	drm.connector_id = connector->connector_id;

	drmModeCrtc *saved_crtc = drmModeGetCrtc(fd, drm.crtc_id);
	return 0;
}

static void
drm_fb_destroy_callback(struct gbm_bo *bo, void *data)
{
	struct drm_fb *fb = data;
	struct gbm_device *gbm = gbm_bo_get_device(bo);
	int fd = gbm_bo_get_fd(bo);

	if (fb->fb_id)
		drmModeRmFB(fd, fb->fb_id);

	free(fb);
}

static struct drm_fb * drm_fb_get_from_bo(struct gbm_bo *bo)
{
	struct drm_fb *fb = gbm_bo_get_user_data(bo);
	uint32_t width, height, stride, handle;
	int ret;

	if (fb)
		return fb;

	fb = calloc(1, sizeof *fb);
	fb->bo = bo;
	fb->drm = &drm;

	width = gbm_bo_get_width(bo);
	height = gbm_bo_get_height(bo);
	stride = gbm_bo_get_stride(bo);
	handle = gbm_bo_get_handle(bo).u32;

	ret = drmModeAddFB(drm.fd, width, height, 24, 32, stride, handle, &fb->fb_id);
	if (ret) {
		err("segl: failed to create fb: %m");
		free(fb);
		return NULL;
	}

	gbm_bo_set_user_data(bo, fb, drm_fb_destroy_callback);

	return fb;
}

static void page_flip_handler(int fd, unsigned int frame,
		  unsigned int sec, unsigned int usec, void *data)
{
	int *waiting_for_flip = data;
	*waiting_for_flip = 0;
}

static const EGLint g_attributes[][21] = {
	{
		EGL_RED_SIZE, 8, /// set the minimum bit inside the color
		EGL_GREEN_SIZE, 8,
		EGL_BLUE_SIZE, 8,
		EGL_ALPHA_SIZE, 8,
		//EGL_DEPTH_SIZE, 16, // DEPTH management in useless for this application
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
		EGL_NONE
	},
	{
		EGL_RED_SIZE, 8,
		EGL_GREEN_SIZE, 8,
		EGL_BLUE_SIZE, 8,
		EGL_ALPHA_SIZE, 0,
		//EGL_DEPTH_SIZE, 16, // DEPTH management in useless for this application
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
		EGL_NONE
	},
	{
		EGL_RED_SIZE, 5,
		EGL_GREEN_SIZE, 6,
		EGL_BLUE_SIZE, 5,
		EGL_ALPHA_SIZE, 0,
		//EGL_DEPTH_SIZE, 16, // DEPTH management in useless for this application
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
		EGL_NONE
	},
	{
		EGL_RED_SIZE, 10,
		EGL_GREEN_SIZE, 0,
		EGL_BLUE_SIZE, 0,
		EGL_ALPHA_SIZE, 0,
		//EGL_DEPTH_SIZE, 16, // DEPTH management in useless for this application
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
		EGL_NONE
	},
};

struct
{
	uint32_t fourcc;
	const EGLint *attributes;
} g_formats[] =
{
	{
		.fourcc = GBM_FORMAT_C8		,
		.attributes = g_attributes[1],
	},
	{
		.fourcc = GBM_FORMAT_R8		,
		.attributes = g_attributes[1],
	},
	{
		.fourcc = GBM_FORMAT_GR88		,
		.attributes = g_attributes[1],
	},
	{
		.fourcc = GBM_FORMAT_RGB332	,
		.attributes = g_attributes[2],
	},
	{
		.fourcc = GBM_FORMAT_BGR233	,
		.attributes = g_attributes[2],
	},
	{
		.fourcc = GBM_FORMAT_XRGB4444	,
		.attributes = g_attributes[2],
	},
	{
		.fourcc = GBM_FORMAT_XBGR4444	,
		.attributes = g_attributes[2],
	},
	{
		.fourcc = GBM_FORMAT_RGBX4444	,
		.attributes = g_attributes[2],
	},
	{
		.fourcc = GBM_FORMAT_BGRX4444	,
		.attributes = g_attributes[2],
	},
	{
		.fourcc = GBM_FORMAT_ARGB4444	,
		.attributes = g_attributes[2],
	},
	{
		.fourcc = GBM_FORMAT_ABGR4444	,
		.attributes = g_attributes[2],
	},
	{
		.fourcc = GBM_FORMAT_RGBA4444	,
		.attributes = g_attributes[2],
	},
	{
		.fourcc = GBM_FORMAT_BGRA4444	,
		.attributes = g_attributes[2],
	},
	{
		.fourcc = GBM_FORMAT_XRGB1555	,
		.attributes = g_attributes[2],
	},
	{
		.fourcc = GBM_FORMAT_XBGR1555	,
		.attributes = g_attributes[2],
	},
	{
		.fourcc = GBM_FORMAT_RGBX5551	,
		.attributes = g_attributes[2],
	},
	{
		.fourcc = GBM_FORMAT_BGRX5551	,
		.attributes = g_attributes[2],
	},
	{
		.fourcc = GBM_FORMAT_ARGB1555	,
		.attributes = g_attributes[2],
	},
	{
		.fourcc = GBM_FORMAT_ABGR1555	,
		.attributes = g_attributes[2],
	},
	{
		.fourcc = GBM_FORMAT_RGBA5551	,
		.attributes = g_attributes[2],
	},
	{
		.fourcc = GBM_FORMAT_BGRA5551	,
		.attributes = g_attributes[2],
	},
	{
		.fourcc = GBM_FORMAT_RGB565	,
		.attributes = g_attributes[2],
	},
	{
		.fourcc = GBM_FORMAT_BGR565	,
		.attributes = g_attributes[2],
	},
	{
		.fourcc = GBM_FORMAT_XRGB2101010	,
		.attributes = g_attributes[3],
	},
	{
		.fourcc = GBM_FORMAT_XBGR2101010	,
		.attributes = g_attributes[3],
	},
	{
		.fourcc = GBM_FORMAT_RGBX1010102	,
		.attributes = g_attributes[3],
	},
	{
		.fourcc = GBM_FORMAT_BGRX1010102	,
		.attributes = g_attributes[3],
	},
	{
		.fourcc = GBM_FORMAT_ARGB2101010	,
		.attributes = g_attributes[3],
	},
	{
		.fourcc = GBM_FORMAT_ABGR2101010	,
		.attributes = g_attributes[3],
	},
	{
		.fourcc = GBM_FORMAT_RGBA1010102	,
		.attributes = g_attributes[3],
	},
	{
		.fourcc = GBM_FORMAT_BGRA1010102	,
		.attributes = g_attributes[3],
	},
	{
		.fourcc = GBM_FORMAT_XBGR16161616F,
		.attributes = g_attributes[3],
	},
	{
		.fourcc = GBM_FORMAT_ABGR16161616F,
		.attributes = g_attributes[3],
	},
	{
		.fourcc = GBM_FORMAT_RGB888	,
		.attributes = g_attributes[1],
	},
	{
		.fourcc = GBM_FORMAT_BGR888	,
		.attributes = g_attributes[1],
	},
	{
		.fourcc = GBM_FORMAT_XRGB8888	,
		.attributes = g_attributes[0],
	},
	{
		.fourcc = GBM_FORMAT_XBGR8888	,
		.attributes = g_attributes[0],
	},
	{
		.fourcc = GBM_FORMAT_RGBX8888	,
		.attributes = g_attributes[0],
	},
	{
		.fourcc = GBM_FORMAT_BGRX8888	,
		.attributes = g_attributes[0],
	},
	{
		.fourcc = GBM_FORMAT_ARGB8888	,
		.attributes = g_attributes[0],
	},
	{
		.fourcc = GBM_FORMAT_ABGR8888	,
		.attributes = g_attributes[0],
	},
	{
		.fourcc = GBM_FORMAT_RGBA8888	,
		.attributes = g_attributes[0],
	},
	{
		.fourcc = GBM_FORMAT_BGRA8888	,
		.attributes = g_attributes[0],
	},
#if 0
	GBM_FORMAT_YUYV,
	GBM_FORMAT_YVYU,
	GBM_FORMAT_UYVY,
	GBM_FORMAT_VYUY,
	GBM_FORMAT_AYUV,
	GBM_FORMAT_NV12,
	GBM_FORMAT_NV21,
	GBM_FORMAT_NV16,
	GBM_FORMAT_NV61,
	GBM_FORMAT_YUV410,
	GBM_FORMAT_YVU410,
	GBM_FORMAT_YUV411,
	GBM_FORMAT_YVU411,
	GBM_FORMAT_YUV420,
	GBM_FORMAT_YVU420,
	GBM_FORMAT_YUV422,
	GBM_FORMAT_YVU422,
	GBM_FORMAT_YUV444,
	GBM_FORMAT_YVU444,
#endif
};

static EGLNativeDisplayType native_display(EGLConfig_t *config)
{
	const char *device = config->device;
	if (device == NULL)
		device = "/dev/dri/card0";
	int fd = open(device, O_RDWR);

	if (fd < 0)
	{
		err("segl: could not open drm device %s", device);
		return EGL_CAST(EGLNativeDisplayType, EGL_UNKNOWN);
	}

	struct gbm_device *gbm = gbm_create_device(fd);
	dbg("segl: open (%s) %s", device, gbm_device_get_backend_name(gbm));

	uint32_t defaultfourcc = 0;
	uint32_t requestfourcc = config->transfer;
	if (requestfourcc == FOURCC_NV12)
		requestfourcc = FOURCC_R8;
	uint32_t fourcc = 0;
	dbg("segl: screen formats (search %.4s):", &requestfourcc);
	for (int i = 0; i < sizeof(g_formats)/sizeof(*g_formats); i++)
	{
		int ret = gbm_device_is_format_supported(gbm, g_formats[i].fourcc,
				GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR);
		if (ret)
		{
			dbg("\t%.4s", &g_formats[i].fourcc);
			if (!defaultfourcc)
				defaultfourcc = g_formats[i].fourcc;
		}
		if (ret && requestfourcc && requestfourcc == g_formats[i].fourcc)
			fourcc = g_formats[i].fourcc;
	}
	if (! fourcc)
		fourcc = defaultfourcc;
	dbg("segl: screen format %.4s", &drm.fourcc);

	if (init_drm(fd, fourcc, config->parent.width, config->parent.height))
	{
		return EGL_CAST(EGLNativeDisplayType, EGL_UNKNOWN);
	}

	return (EGLNativeDisplayType)gbm;
}

static const GLint *native_attributes(EGLNativeDisplayType display)
{
	const EGLint *attributes = NULL;
	for (int i = 0; i < sizeof(g_formats)/sizeof(*g_formats); i++)
	{
		if (g_formats[i].fourcc == drm.fourcc)
			attributes = g_formats[i].attributes;
	}
	return attributes;
}

static EGLNativeWindowType native_createwindow(EGLNativeDisplayType display, GLuint width, GLuint height, const GLchar *name)
{
	struct gbm_device *gbm = (struct gbm_device *)display;

	uint64_t modifiers[1] = {DRM_FORMAT_MOD_LINEAR};
	int modifiers_length = 1;
	struct gbm_surface *surface =NULL;
	surface = gbm_surface_create_with_modifiers(gbm,
			width, height, drm.fourcc, modifiers, modifiers_length);
	if (!surface)
		surface = gbm_surface_create(gbm, width, height, drm.fourcc,
			GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR);
	if (!surface)
		surface = gbm_surface_create(gbm, width, height, drm.fourcc,
			GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);

	if (!surface) {
		err("segl: failed to create gbm surface %.4s", &drm.fourcc);
		return (EGLNativeWindowType)NULL;
	}

	return (EGLNativeWindowType) surface;
}

static int native_fd(EGLNativeWindowType native_win)
{
#if 0
	return drm.fd;
#else
	return -1;
#endif
}

static struct gbm_bo *old_bo = NULL;
static int native_flush(EGLNativeWindowType native_win)
{
	struct gbm_surface *surface = (struct gbm_surface *)native_win;
	struct gbm_bo *bo;
	bo = gbm_surface_lock_front_buffer(surface);
	struct drm_fb *fb;
	fb = drm_fb_get_from_bo(bo);
	struct drm_s *drm = fb->drm;

	if (old_bo == NULL)
	{
		dbg("segl: drm modifiers %lli", gbm_bo_get_modifier(bo));
		/* set mode: */
		if (drm->mode)
		{
			int ret = drmModeSetCrtc(drm->fd, drm->crtc_id, fb->fb_id, 0, 0,
					&drm->connector_id, 1, drm->mode);
			if (ret) {
				err("segl: failed to set mode: %m");
				return -1;
			}
		}
		old_bo = bo;
		return 0;
	}
	drm->waiting_for_flip = 1;
	int ret = drmModePageFlip(drm->fd, drm->crtc_id, fb->fb_id,
			DRM_MODE_PAGE_FLIP_EVENT, &drm->waiting_for_flip);
	if (ret)
	{
		err("segl: failed to queue page flip: %m");
		return -1;
	}
	/* release last buffer to render on again: */
	if (old_bo)
		gbm_surface_release_buffer(surface, old_bo);
	old_bo = bo;

	return 0;
}

static int native_sync(EGLNativeWindowType native_win)
{
	struct gbm_surface *surface = (struct gbm_surface *)native_win;

	drmEventContext evctx = {
			.version = DRM_EVENT_CONTEXT_VERSION,
			.page_flip_handler = page_flip_handler,
	};
#if 0
	drmHandleEvent(drm.fd, &evctx);
	if (drm.waiting_for_flip)
	{
		errno = EAGAIN;
		return -1;
	}
#else
	fd_set fds;
	FD_ZERO(&fds);
	FD_SET(drm.fd, &fds);
	while (drm.waiting_for_flip) {
		int ret = select(drm.fd + 1, &fds, NULL, NULL, NULL);
		if (ret < 0) {
			err("select err: %m");
			return ret;
		} else if (ret == 0) {
			warn("select timeout!");
			return -1;
		}
		drmHandleEvent(drm.fd, &evctx);
	}
#endif
	return 0;
}

static void native_destroy(EGLNativeDisplayType native_display)
{
}

EGLNative_t *eglnative_drm = &(EGLNative_t)
{
	.name = "drm",
	.display = native_display,
	.attributes = native_attributes,
	.createwindow = native_createwindow,
	.fd = native_fd,
	.flush = native_flush,
	.sync = native_sync,
	.destroy = native_destroy,
};
