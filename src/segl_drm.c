#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <inttypes.h>

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
#include "sdmabuf.h"

#define segl_dbg(...)

#ifndef GBM_FORMAT_XBGR16161616F
# define GBM_FORMAT_XBGR16161616F DRM_FORMAT_XBGR16161616F
#endif
#ifndef GBM_FORMAT_ABGR16161616F
# define GBM_FORMAT_ABGR16161616F DRM_FORMAT_ABGR16161616F
#endif

typedef enum {
	SDRM_PROPID_CRTC_ID,
	SDRM_PROPID_MODE_ID,
	SDRM_PROPID_FB_ID,
	SDRM_PROPID_ACTIVE,
	SDRM_PROPID_SRC_X,
	SDRM_PROPID_SRC_Y,
	SDRM_PROPID_SRC_W,
	SDRM_PROPID_SRC_H,
	SDRM_PROPID_CRTC_X,
	SDRM_PROPID_CRTC_Y,
	SDRM_PROPID_CRTC_W,
	SDRM_PROPID_CRTC_H,
	SDRM_PROPID_ROTATION,
	SDRM_PROPID_WRITEBACK_OUT_FENCE_PTR,
	SDRM_PROPID_WRITEBACK_FB_ID,
	SDRM_PROPID_LAST
} properties_id;

typedef struct EGLExportDRMWriteback_s EGLExportDRMWriteback_t;
struct EGLExportDRMWriteback_s
{
	int out_fd;
	EGLConfig_t *config;
	int fd;
	uint32_t connector_id;
	GLBuffer_t *buffers[MAX_BUFFERS];
	int nbuffers;
	int currentid;
};

static struct drm_s {
	uint32_t fourcc;
	uint32_t width;
	uint32_t height;
	int fd;
	drmModeModeInfo mode;
	unsigned int mode_id;
	uint32_t crtc_id;
	uint32_t connector_id;
	uint32_t plane_id;
	int waiting_for_flip;
#ifndef SEGL_DRM_DISABLE_ATOMIC_COMMIT
	uint32_t properties[SDRM_PROPID_LAST];
	uint32_t flags;
	EGLExportDRMWriteback_t *writeback;
#endif
} drm;

struct drm_fb {
	struct drm_s *drm;
	struct gbm_bo *bo;
	uint32_t fb_id;
};

#ifndef SEGL_DRM_DISABLE_ATOMIC_COMMIT
static uint32_t sdrm_propertyid(int fd,  uint32_t type, uint32_t id, const char *property)
{
	uint32_t ret = -1;
	drmModeObjectPropertiesPtr props;

	props = drmModeObjectGetProperties(fd, id, type);
	for (int i = 0; props && i < props->count_props; i++)
	{
		drmModePropertyPtr prop;

		prop = drmModeGetProperty(fd, props->props[i]);
		if (prop && !strcmp(prop->name, property))
		{
			ret = props->props[i];
		}
		if (prop)
			drmModeFreeProperty(prop);
	}
	drmModeFreeObjectProperties(props);
	return ret;
}
#if 0
static uint64_t sdrm_properties(int fd,  uint32_t type, uint32_t id, const char *property, uint64_t value)
{
	uint64_t ret = 0;
	drmModeObjectPropertiesPtr props;

	props = drmModeObjectGetProperties(fd, id, type);
	segl_dbg("sdrm: property for %#x", type);
	for (int i = 0; props && i < props->count_props; i++)
	{
		drmModePropertyPtr prop;

		prop = drmModeGetProperty(fd, props->props[i]);
		if (prop)
		{
#ifdef DEBUG
			segl_dbg("\t%s [%lu] => %lu", prop->name, props->props[i], props->prop_values[i]);
#endif
			if (!strcmp(prop->name, property))
			{
				ret = props->prop_values[i];
				if (value != (uint64_t) -1)
				{
					drmModeObjectSetProperty(fd, id, type, props->props[i], value);
				}
#ifndef DEBUG
				break;
#endif
			}
			drmModeFreeProperty(prop);
		}
	}
	drmModeFreeObjectProperties(props);
	return ret;
}
#endif
#endif

static uint32_t find_plane_for_crtc(int fd, int crtc_index, uint32_t crtc_id)
{
	uint32_t plane_id;
	drmModePlaneResPtr planes;

	planes = drmModeGetPlaneResources(fd);
	for (int i = 0; i < planes->count_planes; ++i)
	{
		drmModePlanePtr plane;
		plane = drmModeGetPlane(fd, planes->planes[i]);
		if (plane->possible_crtcs & (1 << crtc_index))
		{
			plane_id = plane->plane_id;
		}
		drmModeFreePlane(plane);
	}
	drmModeFreePlaneResources(planes);
	return plane_id;
}

static uint32_t find_crtc_for_encoder(int fd, const drmModeRes *resources,
				      const drmModeEncoder *encoder, uint32_t *plane_id) {
	int i;

	for (i = 0; i < resources->count_crtcs; i++) {
		/* possible_crtcs is a bitmask as described here:
		 * https://dvdhrm.wordpress.com/2012/09/13/linux-drm-mode-setting-api
		 */
		const uint32_t crtc_mask = 1 << i;
		const uint32_t crtc_id = resources->crtcs[i];
		if (encoder->possible_crtcs & crtc_mask) {
			if (plane_id)
				*plane_id = find_plane_for_crtc(fd, i, crtc_id);
			return crtc_id;
		}
	}

	/* no match found */
	return -1;
}

static uint32_t find_crtc_for_connector(int fd, const drmModeRes *resources,
					const drmModeConnector *connector, uint32_t *plane_id) {
	int i;

	for (i = 0; i < connector->count_encoders; i++) {
		const uint32_t encoder_id = connector->encoders[i];
		drmModeEncoder *encoder = drmModeGetEncoder(fd, encoder_id);

		if (encoder) {
			const uint32_t crtc_id = find_crtc_for_encoder(fd, resources, encoder, plane_id);

			drmModeFreeEncoder(encoder);
			if (crtc_id != 0) {
				return crtc_id;
			}
		}
	}

	/* no match found */
	return -1;
}

static drmModeConnector *find_connector(int fd, drmModeRes *resources, uint32_t *width, uint32_t *height, drmModeModeInfo *mode, unsigned int *mode_id, int writeback)
{
	drmModeConnector *connector = NULL;
	for (int i = 0; i < resources->count_connectors; i++)
	{
		connector = drmModeGetConnector(fd, resources->connectors[i]);
		dbg("segl: drm connector %s", drmModeGetConnectorTypeName(connector->connector_type));
		if (connector->connection != DRM_MODE_CONNECTED)
		{
			drmModeFreeConnector(connector);
			connector = NULL;
			continue;
		}
		if (writeback && connector->connector_type != DRM_MODE_CONNECTOR_WRITEBACK)
		{
			drmModeFreeConnector(connector);
			connector = NULL;
			continue;
		}
		drmModeModeInfo *current_mode = NULL;
		dbg("segl: drm request %ux%u connector", *width, *height);
		for (int j = 0; current_mode == NULL && j < connector->count_modes; j++)
		{
			current_mode = &connector->modes[j];

			dbg("\tfound %ux%u %dHz %#x", current_mode->hdisplay, current_mode->vdisplay, current_mode->vrefresh, current_mode->type);
			if (current_mode->vdisplay == *height &&
					current_mode->hdisplay == *width)
			{
				break;
			}
			current_mode = NULL;
		}
		for (int j = 0; current_mode == NULL && j < connector->count_modes; j++)
		{
			current_mode = &connector->modes[j];

			if (current_mode->hdisplay == *width &&
					current_mode->vdisplay >= *height)
			{
				break;
			}
			current_mode = NULL;
		}
		for (int j = 0; current_mode == NULL && j < connector->count_modes; j++)
		{
			current_mode = &connector->modes[j];

			if (current_mode->vdisplay <= (*height * 6 / 5) &&
				current_mode->vdisplay >= *height &&
				current_mode->hdisplay <= (*width * 8 / 5) &&
					current_mode->hdisplay >= *width)
			{
				break;
			}
			current_mode = NULL;
		}
		for (int j = 0; current_mode == NULL && j < connector->count_modes; j++)
		{
			current_mode = &connector->modes[j];
			if (current_mode->type & DRM_MODE_TYPE_PREFERRED)
			{
				break;
			}
			current_mode = NULL;
		}
		if (current_mode)
		{
			*width = current_mode->hdisplay;
			*height = current_mode->vdisplay;
			dbg("segl: mode select %s %ux%u %d %#x", current_mode->name, current_mode->hdisplay, current_mode->vdisplay, current_mode->type, current_mode->flags);
			if (mode)
			{
				memcpy(mode, current_mode, sizeof(*mode));
				/* create the blob property using out->mode and save its id in the output*/
				if (drmModeCreatePropertyBlob(fd, mode, sizeof(*mode), mode_id) != 0)
				{
					err("ssegl: blob property error");
				}
			}
			break;
		}
		else
			err("segl: drm mode not found");
		drmModeFreeConnector(connector);
		connector = NULL;
	}
	return connector;
}

static int init_drm(int fd, uint32_t fourcc, uint32_t width, uint32_t height, int writeback)
{
	drmModeRes *resources;
	drmModeConnector *connector = NULL;

	drm.fd = fd;
	drm.fourcc = fourcc;
	resources = drmModeGetResources(fd);
	if (!resources)
	{
		err("segl: drmModeGetResources failed: %m");
		return -1;
	}

	drm.width = width;
	drm.height = height;
	/* find a connected connector: */
	connector = find_connector(fd, resources, &drm.width, &drm.height, &drm.mode, &drm.mode_id, writeback);

	if (!connector)
	{
		/* we could be fancy and listen for hotplug events and wait for
		 * a connector..
		 */
		err("segl: no connected %s connector!", writeback?"writeback":"");
		return -1;
	}

	/* find encoder: */
	uint32_t plane_id = 0;
	uint32_t crtc_id = find_crtc_for_connector(fd, resources, connector, &plane_id);
	if (crtc_id == 0) {
		err("segl: no crtc found!");
		return -1;
	}

	drm.crtc_id = crtc_id;
	drm.plane_id = plane_id;
	dbg("segl: drm CRTC_ID %d", drm.crtc_id);

	drm.connector_id = connector->connector_id;

#ifndef SEGL_DRM_DISABLE_ATOMIC_COMMIT
	drm.properties[SDRM_PROPID_CRTC_ID] = sdrm_propertyid(fd, DRM_MODE_OBJECT_CONNECTOR, drm.connector_id, "CRTC_ID");
	uint32_t prop_plane_crtc_id = sdrm_propertyid(fd, DRM_MODE_OBJECT_PLANE, drm.plane_id, "CRTC_ID");
	if (drmModeObjectSetProperty(fd, prop_plane_crtc_id, DRM_MODE_OBJECT_PLANE, prop_plane_crtc_id, drm.plane_id) < 0)
		warn("segl: set CRTC to plane error");
	if (prop_plane_crtc_id != drm.properties[SDRM_PROPID_CRTC_ID])
	{
		warn("sdrm: CRTC_ID for plane(%u) and connector(%u) differents", prop_plane_crtc_id, drm.properties[SDRM_PROPID_CRTC_ID]);
	}
	if (writeback)
	{
		drm.properties[SDRM_PROPID_WRITEBACK_OUT_FENCE_PTR] = sdrm_propertyid(fd, DRM_MODE_OBJECT_CONNECTOR, drm.connector_id, "WRITEBACK_OUT_FENCE_PTR");
		if (drm.properties[SDRM_PROPID_WRITEBACK_OUT_FENCE_PTR] == (uint32_t)-1)
		{
			warn("sdrm: writeback connector's property error %m");
		}
		drm.properties[SDRM_PROPID_WRITEBACK_FB_ID] = sdrm_propertyid(fd, DRM_MODE_OBJECT_CONNECTOR, drm.connector_id, "WRITEBACK_FB_ID");
		if (drm.properties[SDRM_PROPID_WRITEBACK_FB_ID] == (uint32_t)-1)
		{
			warn("sdrm: writeback connector's property error %m");
		}
	}
	drm.properties[SDRM_PROPID_MODE_ID] = sdrm_propertyid(fd, DRM_MODE_OBJECT_CRTC, drm.crtc_id, "MODE_ID");
	drm.properties[SDRM_PROPID_ACTIVE] = sdrm_propertyid(fd, DRM_MODE_OBJECT_CRTC, drm.crtc_id, "ACTIVE");
	drm.properties[SDRM_PROPID_FB_ID] = sdrm_propertyid(fd, DRM_MODE_OBJECT_PLANE, drm.plane_id, "FB_ID");
	drm.properties[SDRM_PROPID_SRC_X] = sdrm_propertyid(fd, DRM_MODE_OBJECT_PLANE, drm.plane_id, "SRC_X");
	drm.properties[SDRM_PROPID_SRC_Y] = sdrm_propertyid(fd, DRM_MODE_OBJECT_PLANE, drm.plane_id, "SRC_Y");
	drm.properties[SDRM_PROPID_SRC_W] = sdrm_propertyid(fd, DRM_MODE_OBJECT_PLANE, drm.plane_id, "SRC_W");
	drm.properties[SDRM_PROPID_SRC_H] = sdrm_propertyid(fd, DRM_MODE_OBJECT_PLANE, drm.plane_id, "SRC_H");
	drm.properties[SDRM_PROPID_CRTC_X] = sdrm_propertyid(fd, DRM_MODE_OBJECT_PLANE, drm.plane_id, "CRTC_X");
	drm.properties[SDRM_PROPID_CRTC_Y] = sdrm_propertyid(fd, DRM_MODE_OBJECT_PLANE, drm.plane_id, "CRTC_Y");
	drm.properties[SDRM_PROPID_CRTC_W] = sdrm_propertyid(fd, DRM_MODE_OBJECT_PLANE, drm.plane_id, "CRTC_W");
	drm.properties[SDRM_PROPID_CRTC_H] = sdrm_propertyid(fd, DRM_MODE_OBJECT_PLANE, drm.plane_id, "CRTC_H");
	drm.properties[SDRM_PROPID_ROTATION] = sdrm_propertyid(fd, DRM_MODE_OBJECT_PLANE, drm.plane_id, "rotation");

	drm.flags = (DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT | DRM_MODE_ATOMIC_ALLOW_MODESET);
#endif

	drmModeFreeConnector(connector);
	drmModeFreeResources(resources);
	return 0;
}

static void
drm_fb_destroy_callback(struct gbm_bo *bo, void *data)
{
	struct drm_fb *fb = data;
#if 0
	struct gbm_device *gbm = gbm_bo_get_device(bo);
#else
	gbm_bo_get_device(bo);
#endif
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
	struct drm_s *drm = (struct drm_s *)data;
#ifndef SEGL_DRM_DISABLE_ATOMIC_COMMIT
	if (!drm->writeback || drm->writeback->out_fd == 0)
#endif
		drm->waiting_for_flip = 0;
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
	int fd = 0;
	if (!access(device, R_OK | W_OK))
		fd = open(device, O_RDWR);
	else /// open with the device name instead the device node
		fd = drmOpen(device, NULL);

	if (fd < 0)
	{
		err("segl: could not open drm device %s", device);
		return EGL_CAST(EGLNativeDisplayType, EGL_UNKNOWN);
	}

#ifndef SEGL_DRM_DISABLE_ATOMIC_COMMIT
	if (drmSetMaster(fd))
		err("segl: drm setmaster failed %m");
	/// enable atomic and writeback before setting the primary connector
	if (drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1))
		err("segl: drm atomic not supported %m");
	if (drmSetClientCap(fd, DRM_CLIENT_CAP_WRITEBACK_CONNECTORS, 1))
		err("segl: drm writeback not supported %m");
#endif

	struct gbm_device *gbm = gbm_create_device(fd);
	dbg("segl: open (%s) %s", device, gbm_device_get_backend_name(gbm));

	uint32_t defaultfourcc = 0;
#if 0
	/// The screen format doesn't depend on the texture format
	uint32_t requestfourcc = config->parent.fourcc;
#endif
	uint32_t requestfourcc = FOURCC_XR24;
#if 0
	/// The screen may accept but the GPU may not accpet another value
	if (config->transfer.fourcc)
		requestfourcc = config->transfer.fourcc;
#else
	if (config->transfer.fourcc && requestfourcc != config->transfer.fourcc)
		warn("segl: the gpu runs with %.4s format", (char*)&requestfourcc);
#endif
	uint32_t fourcc = 0;
	dbg("segl: screen formats (search %.4s):", (char *)&requestfourcc);
	for (int i = 0; i < sizeof(g_formats)/sizeof(*g_formats); i++)
	{
		int ret = gbm_device_is_format_supported(gbm, g_formats[i].fourcc,
				GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR);
		if (ret)
		{
			dbg("\t%.4s", (char *)&g_formats[i].fourcc);
			if (!defaultfourcc)
				defaultfourcc = g_formats[i].fourcc;
		}
		if (ret && requestfourcc && requestfourcc == g_formats[i].fourcc)
			fourcc = g_formats[i].fourcc;
	}
	if (! fourcc)
		fourcc = defaultfourcc;
	dbg("segl: screen format %.4s", (char *)&fourcc);

	if (init_drm(fd, fourcc, config->parent.width, config->parent.height, (config->type == device_transfer)))
	{
#if 0
		return EGL_CAST(EGLNativeDisplayType, EGL_UNKNOWN);
#endif
	}

	config->transfer.width = drm.width;
	config->transfer.height = drm.height;
	config->transfer.fourcc = drm.fourcc;
	return (EGLNativeDisplayType)gbm;
}

static const GLint *native_attributes(EGLNativeDisplayType display)
{
	const EGLint *attributes = NULL;
	for (int i = 0; i < sizeof(g_formats)/sizeof(*g_formats); i++)
	{
		if (g_formats[i].fourcc == drm.fourcc)
		{
			attributes = g_formats[i].attributes;
			dbg("found attributes %.4s", (char *)&g_formats[i].fourcc);
		}
	}
	return attributes;
}

static EGLNativeWindowType native_createwindow(EGLNativeDisplayType display, GLuint width, GLuint height, const GLchar *name)
{
	struct gbm_device *gbm = (struct gbm_device *)display;
	if (drm.mode_id == 0)
		return (EGLNativeWindowType)NULL;

	width = drm.width;
	height = drm.height;

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
		err("segl: failed to create gbm surface %.4s", (char *)&drm.fourcc);
		return (EGLNativeWindowType)NULL;
	}

	return (EGLNativeWindowType) surface;
}

static int native_fd(EGLNativeWindowType native_win)
{
	return drm.fd;
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

	int ret = 0;
#ifndef SEGL_DRM_DISABLE_ATOMIC_COMMIT
	drmModeAtomicReq *req;
	req = drmModeAtomicAlloc();
	if (drmModeAtomicAddProperty(req, drm->connector_id, drm->properties[SDRM_PROPID_CRTC_ID], drm->crtc_id) < 0)
		goto commit_error;
	if (drm->mode_id && drmModeAtomicAddProperty(req, drm->crtc_id, drm->properties[SDRM_PROPID_MODE_ID], drm->mode_id) < 0)
		goto commit_error;
	if (drmModeAtomicAddProperty(req, drm->crtc_id, drm->properties[SDRM_PROPID_ACTIVE], 1) < 0)
		goto commit_error;

	if (drmModeAtomicAddProperty(req, drm->plane_id, drm->properties[SDRM_PROPID_FB_ID], fb->fb_id) < 0)
		goto commit_error;
	if (drmModeAtomicAddProperty(req, drm->plane_id, drm->properties[SDRM_PROPID_CRTC_ID], drm->crtc_id) < 0)
		goto commit_error;
	if (drm->properties[SDRM_PROPID_SRC_X] != (uint32_t)-1 &&
		drmModeAtomicAddProperty(req, drm->plane_id, drm->properties[SDRM_PROPID_SRC_X], 0 << 16) < 0)
		goto commit_error;
	if (drm->properties[SDRM_PROPID_SRC_Y] != (uint32_t)-1 &&
		drmModeAtomicAddProperty(req, drm->plane_id, drm->properties[SDRM_PROPID_SRC_Y], 0 << 16) < 0)
		goto commit_error;
	if (drm->properties[SDRM_PROPID_SRC_W] != (uint32_t)-1 &&
		drmModeAtomicAddProperty(req, drm->plane_id, drm->properties[SDRM_PROPID_SRC_W], drm->mode.hdisplay << 16) < 0)
		goto commit_error;
	if (drm->properties[SDRM_PROPID_SRC_H] != (uint32_t)-1 &&
		drmModeAtomicAddProperty(req, drm->plane_id, drm->properties[SDRM_PROPID_SRC_H], drm->mode.vdisplay << 16) < 0)
		goto commit_error;
	if (drm->properties[SDRM_PROPID_CRTC_X] != (uint32_t)-1 &&
		drmModeAtomicAddProperty(req, drm->plane_id, drm->properties[SDRM_PROPID_CRTC_X], 0) < 0)
		goto commit_error;
	if (drm->properties[SDRM_PROPID_CRTC_Y] != (uint32_t)-1 &&
		drmModeAtomicAddProperty(req, drm->plane_id, drm->properties[SDRM_PROPID_CRTC_Y], 0) < 0)
		goto commit_error;
	if (drm->properties[SDRM_PROPID_CRTC_W] != (uint32_t)-1 &&
		drmModeAtomicAddProperty(req, drm->plane_id, drm->properties[SDRM_PROPID_CRTC_W], drm->mode.hdisplay) < 0)
		goto commit_error;
	if (drm->properties[SDRM_PROPID_CRTC_H] != (uint32_t)-1 &&
		drmModeAtomicAddProperty(req, drm->plane_id, drm->properties[SDRM_PROPID_CRTC_H], drm->mode.vdisplay) < 0)
		goto commit_error;
	if (drm->writeback)
	{
		if (drmModeAtomicAddProperty(req, drm->writeback->connector_id, drm->properties[SDRM_PROPID_WRITEBACK_OUT_FENCE_PTR], (uint64_t)(long)&(drm->writeback)->out_fd) < 0)
			goto commit_error;
		GLBuffer_t *buffer = drm->writeback->buffers[drm->writeback->currentid];
		if (drmModeAtomicAddProperty(req, drm->writeback->connector_id, drm->properties[SDRM_PROPID_WRITEBACK_FB_ID], buffer->id) < 0)
			goto commit_error;
		drm->writeback->currentid++;
		drm->writeback->currentid %= drm->writeback->nbuffers;
	}

	ret = drmModeAtomicCommit(drm->fd, req, drm->flags, drm);
	drmModeAtomicFree(req);
	drm->flags = DRM_MODE_PAGE_FLIP_EVENT | DRM_MODE_ATOMIC_NONBLOCK;
#else
	drm->waiting_for_flip = 1;
	ret = drmModePageFlip(drm->fd, drm->crtc_id, fb->fb_id,
			DRM_MODE_PAGE_FLIP_EVENT, drm);
#endif
	if (ret)
	{
		goto commit_error;
	}
	/* release last buffer to render on again: */
	if (old_bo)
		gbm_surface_release_buffer(surface, old_bo);
	old_bo = bo;

	return 0;
commit_error:
	err("segl: drm commit error %m");
	return -1;
}

static int native_sync(EGLNativeWindowType native_win)
{
	drmEventContext evctx = {
			.version = DRM_EVENT_CONTEXT_VERSION,
			.page_flip_handler = page_flip_handler,
	};
	drmHandleEvent(drm.fd, &evctx);
	if (drm.waiting_for_flip)
	{
		errno = EAGAIN;
		return -1;
	}
	return 0;
}

static void native_destroy(EGLNativeDisplayType native_display)
{
}

EGLNative_t eglnative_drm =
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
/*****************************************************************************/
#ifndef SEGL_DRM_DISABLE_ATOMIC_COMMIT
static void *_egl_export_create(EGLConfig_t *config, EGLDisplay eglDisplay, EGLContext eglContext)
{
	drmModeRes *resources;

	resources = drmModeGetResources(drm.fd);
	if (!resources)
	{
		err("segl: drmModeGetResources failed: %m");
		return NULL;
	}

	/* find a connected connector: */
	drmModeConnector *connector;
	connector = find_connector(drm.fd, resources, &drm.width, &drm.height, NULL, NULL, 1);
	if (!connector)
		return NULL;

	EGLExportDRMWriteback_t *ctx = calloc(1, sizeof(*ctx));
	ctx->config = config;
	ctx->fd = drm.fd;

	ctx->connector_id = connector->connector_id;
	drmModeFreeConnector(connector);
	drmModeObjectSetProperty(ctx->fd, ctx->connector_id, DRM_MODE_OBJECT_CONNECTOR,
			drm.properties[SDRM_PROPID_CRTC_ID], drm.crtc_id);
	drmModeFreeResources(resources);
	drm.writeback = ctx;
	return ctx;
}

static GLuint _egl_export_fbo(void *arg)
{
	return 0;
}

static GL_Buffer_t *_egl_export_out(void *arg)
{
	return NULL;
}

static int _egl_export_setbuffer(void *arg, GLBuffer_t *buffer)
{
	EGLExportDRMWriteback_t *ctx = (EGLExportDRMWriteback_t *)arg;

	uint32_t width = ctx->config->parent.width;
	uint32_t height = ctx->config->parent.height;
	uint32_t bo_handle;
	uint64_t size;
	ctx->buffers[buffer->id] = buffer;
	ctx->nbuffers++;
	drmModeCreateDumbBuffer(ctx->fd, width, height, 32, 0, &bo_handle, &buffer->pitch, &size);
	if (buffer->size && size != buffer->size)
		err("segl: drm buffer size error (%"PRIu64" for %u", size, buffer->size);
	drmModeAddFB(ctx->fd, width, height, 24, 32, buffer->pitch, bo_handle, (unsigned int *)&buffer->id);
	drmPrimeHandleToFD(ctx->fd, bo_handle, 0, (int *)&buffer->dma_fd);
//	buffer->memory = sdmabuf_map(buffer->dma_fd, buffer->size, 1);
	return 0;
}

static int _egl_export_flush(void *arg, GLBuffer_t *buffer)
{
	EGLExportDRMWriteback_t *ctx = (EGLExportDRMWriteback_t *)arg;
	if (ctx->out_fd > 0)
	{
		close(ctx->out_fd);
		ctx->out_fd = 0;
	}
	return 0;
}

static int _egl_export_releasebuffer(void *arg, GLBuffer_t *buffer)
{
	return 0;
}

static int _egl_export_fd(void *arg)
{
#if 0
	EGLExportDRMWriteback_t *ctx = (EGLExportDRMWriteback_t *)arg;
	/// This is too slow
	return ctx->out_fd;
#else
	return -1;
#endif
}

static void _egl_export_destroy(void *arg)
{
	free(arg);
}

EGLExport_t export_drmwriteback =
{
	.name = "drmwriteback",
	.native = "drm",
	.create = _egl_export_create,
	.fbo = _egl_export_fbo,
	.out = _egl_export_out,
	.fd = _egl_export_fd,
	.setbuffer = _egl_export_setbuffer,
	.releasebuffer = _egl_export_releasebuffer,
	.flush = _egl_export_flush,
	.destroy = _egl_export_destroy,
};
#endif

#include <dlfcn.h>

static void __attribute__ ((constructor)) segl_init()
{
	segl_native_append_t _segl_native_append;
	void *hdl = dlopen(NULL, RTLD_NOW);
	_segl_native_append = dlsym(hdl, "segl_native_append");
	if (_segl_native_append)
	{
		_segl_native_append(&eglnative_drm);
	}
#ifndef SEGL_DRM_DISABLE_ATOMIC_COMMIT
	segl_export_append_t _segl_export_append;
	_segl_export_append = dlsym(hdl, "segl_export_append");
	if (_segl_export_append)
	{
		_segl_export_append(&export_drmwriteback);
	}
#endif
}
