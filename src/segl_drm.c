#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h> 

#include <GLES2/gl2.h>
#include <EGL/egl.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <drm/drm_fourcc.h>
#include <gbm.h>

#include "log.h"

static struct {
	struct gbm_device *dev;
	struct gbm_surface *surface;
} gbm;

static struct {
	int fd;
	drmModeModeInfo *mode;
	uint32_t crtc_id;
	uint32_t connector_id;
} drm;

struct drm_fb {
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

static int init_drm(void)
{
	drmModeRes *resources;
	drmModeConnector *connector = NULL;
	drmModeEncoder *encoder = NULL;
	int i, area;

	drm.fd = open("/dev/dri/card0", O_RDWR);
	
	if (drm.fd < 0) {
		err("glmotor: could not open drm device");
		return -1;
	}

	resources = drmModeGetResources(drm.fd);
	if (!resources) {
		err("glmotor: drmModeGetResources failed: %m");
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
		err("glmotor: no connected connector!");
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
		err("glmotor: could not find mode!");
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
			err("glmotor: no crtc found!");
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

	width = gbm_bo_get_width(bo);
	height = gbm_bo_get_height(bo);
	stride = gbm_bo_get_stride(bo);
	handle = gbm_bo_get_handle(bo).u32;

	ret = drmModeAddFB(drm.fd, width, height, 24, 32, stride, handle, &fb->fb_id);
	if (ret) {
		err("glmotor: failed to create fb: %m");
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

static EGLNativeDisplayType native_display()
{
	GLuint width = 640;
	GLuint height = 480;

	if (init_drm())
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
		err("glmotor: failed to create gbm surface");
		return -1;
	}

	return (EGLNativeWindowType) gbm.surface;
}

static GLboolean native_running(EGLNativeWindowType native_win)
{
	static struct gbm_bo *old_bo = NULL;
	struct gbm_surface *surface = (struct gbm_surface *)native_win;

	if (old_bo == NULL)
	{
		old_bo = gbm_surface_lock_front_buffer(gbm.surface);
		struct drm_fb *fb;
		fb = drm_fb_get_from_bo(old_bo);

		/* set mode: */
		int ret = drmModeSetCrtc(drm.fd, drm.crtc_id, fb->fb_id, 0, 0,
				&drm.connector_id, 1, drm.mode);
		if (ret) {
			err("glmotor: failed to set mode: %m");
			return 0;
		}
		return 1;
	}
	struct gbm_bo *bo;
	bo = gbm_surface_lock_front_buffer(gbm.surface);
	struct drm_fb *fb;
	fb = drm_fb_get_from_bo(bo);

	int waiting_for_flip = 1;
	int ret = drmModePageFlip(drm.fd, drm.crtc_id, fb->fb_id,
			DRM_MODE_PAGE_FLIP_EVENT, &waiting_for_flip);
	if (ret) {
		err("glmotor: failed to queue page flip: %m");
		return 0;
	}

	drmEventContext evctx = {
			.version = DRM_EVENT_CONTEXT_VERSION,
			.page_flip_handler = page_flip_handler,
	};
	fd_set fds;
	FD_ZERO(&fds);
	FD_SET(0, &fds);
	FD_SET(drm.fd, &fds);
	while (waiting_for_flip) {
		ret = select(drm.fd + 1, &fds, NULL, NULL, NULL);
		if (ret < 0) {
			err("glmotor: select err: %m");
			return ret;
		} else if (ret == 0) {
			warn("select timeout!");
			return -1;
		} else if (FD_ISSET(0, &fds)) {
			warn("user interrupted!");
			break;
		}
		drmHandleEvent(drm.fd, &evctx);
	}

	/* release last buffer to render on again: */
	if (old_bo)
		gbm_surface_release_buffer(surface, old_bo);
	old_bo = bo;

	return 1;
}

static void native_destroy(EGLNativeDisplayType native_display)
{
}

EGLNative_t *eglnative_drm = &(EGLNative_t)
{
	.name = "drm",
	.display = native_display,
	.createwindow = native_createwindow,
	.running = native_running,
	.destroy = native_destroy,
};
