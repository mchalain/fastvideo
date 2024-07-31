#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h> 
#include <errno.h>

#include <GLES2/gl2.h>
#include <EGL/egl.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <drm/drm_fourcc.h>
#include <gbm.h>

#include "segl.h"
#include "log.h"

static struct {
	struct gbm_device *dev;
	struct gbm_surface *surface;
} gbm;

static struct drm_s {
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

static uint32_t find_crtc_for_connector(const drmModeRes *resources,
					const drmModeConnector *connector) {
	int i;

	for (i = 0; i < connector->count_encoders; i++) {
		const uint32_t encoder_id = connector->encoders[i];
		drmModeEncoder *encoder = drmModeGetEncoder(drm.fd, encoder_id);

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

static int init_drm(const char *device)
{
	drmModeRes *resources;
	drmModeConnector *connector = NULL;
	drmModeEncoder *encoder = NULL;
	int i, area;

	drm.fd = open(device, O_RDWR);
	dbg("segl: open %s",device);
	
	if (drm.fd < 0) {
		err("segl: could not open drm device %s", device);
		return -1;
	}

	resources = drmModeGetResources(drm.fd);
	if (!resources) {
		err("segl: drmModeGetResources failed: %m");
		return -1;
	}

	/* find a connected connector: */
	for (i = 0; i < resources->count_connectors; i++) {
		connector = drmModeGetConnector(drm.fd, resources->connectors[i]);
		if (connector->connection == DRM_MODE_CONNECTED) {
			/* it's connected, let's use this! */
			break;
		}
		drmModeFreeConnector(connector);
		connector = NULL;
	}

	if (!connector) {
		/* we could be fancy and listen for hotplug events and wait for
		 * a connector..
		 */
		err("segl: no connected connector!");
		return -1;
	}

	/* find prefered mode or the highest resolution mode: */
	for (i = 0, area = 0; i < connector->count_modes; i++) {
		drmModeModeInfo *current_mode = &connector->modes[i];

		if (current_mode->type & DRM_MODE_TYPE_PREFERRED) {
			drm.mode = current_mode;
		}

		int current_area = current_mode->hdisplay * current_mode->vdisplay;
		if (current_area > area) {
			drm.mode = current_mode;
			area = current_area;
		}
	}

	if (!drm.mode) {
		err("segl: could not find mode!");
		return -1;
	}

	/* find encoder: */
	for (i = 0; i < resources->count_encoders; i++) {
		encoder = drmModeGetEncoder(drm.fd, resources->encoders[i]);
		if (encoder->encoder_id == connector->encoder_id)
			break;
		drmModeFreeEncoder(encoder);
		encoder = NULL;
	}

	if (encoder) {
		drm.crtc_id = encoder->crtc_id;
	} else {
		uint32_t crtc_id = find_crtc_for_connector(resources, connector);
		if (crtc_id == 0) {
			err("segl: no crtc found!");
			return -1;
		}

		drm.crtc_id = crtc_id;
	}

	drm.connector_id = connector->connector_id;

	return 0;
}

static void
drm_fb_destroy_callback(struct gbm_bo *bo, void *data)
{
	struct drm_fb *fb = data;
	struct gbm_device *gbm = gbm_bo_get_device(bo);

	if (fb->fb_id)
		drmModeRmFB(drm.fd, fb->fb_id);

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

static EGLNativeDisplayType native_display(const char *device)
{
	if (device == NULL)
		device = "/dev/dri/card0";
	if (init_drm(device))
	{
		return NULL;
	}
	gbm.dev = gbm_create_device(drm.fd);

	return (EGLNativeDisplayType)gbm.dev;
}

static EGLNativeWindowType native_createwindow(EGLNativeDisplayType display, GLuint width, GLuint height, const GLchar *name)
{
	struct gbm_device *dev = (struct gbm_device *)display;

	gbm.surface = gbm_surface_create(gbm.dev,
			drm.mode->hdisplay, drm.mode->vdisplay,
			GBM_FORMAT_XRGB8888,
			GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
	if (!gbm.surface) {
		err("segl: failed to create gbm surface");
		return -1;
	}

	return (EGLNativeWindowType) gbm.surface;
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

	if (old_bo == NULL)
	{
		old_bo = gbm_surface_lock_front_buffer(surface);
		struct drm_fb *fb;
		fb = drm_fb_get_from_bo(old_bo);

		/* set mode: */
		int ret = drmModeSetCrtc(drm.fd, drm.crtc_id, fb->fb_id, 0, 0,
				&drm.connector_id, 1, drm.mode);
		if (ret) {
			err("segl: failed to set mode: %m");
			return -1;
		}
		return 0;
	}
	struct gbm_bo *bo;
	bo = gbm_surface_lock_front_buffer(surface);
	struct drm_fb *fb;
	fb = drm_fb_get_from_bo(bo);

	drm.waiting_for_flip = 1;
	int ret = drmModePageFlip(fb->drm->fd, fb->drm->crtc_id, fb->fb_id,
			DRM_MODE_PAGE_FLIP_EVENT, &drm.waiting_for_flip);
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
			err("glmotor: select err: %m");
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
	.createwindow = native_createwindow,
	.fd = native_fd,
	.flush = native_flush,
	.sync = native_sync,
	.destroy = native_destroy,
};
