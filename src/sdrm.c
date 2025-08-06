#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <limits.h>
#include <errno.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm.h>
#include <drm_fourcc.h>

#include "log.h"
#include "sdrm.h"

/**
 * The DRM driver debug system differs from the dev_dbg system
 * enable full traces:
 * > echo 0x19F > /sys/module/drm/parameters/debug
 */

/**
 * Writeback code for N=4 buffers:
 * * main entity creation
 *  - open device (/dev/dri/cardX) as NONBLOCK
 *  - enable ATOMIC and WRITEBACK features
 *  - search a free encoder (here 51, the encoder 31 is linked to HDMI-A conector)
 *  - search a WRITEBACK connector as CONNECTED and store the best MODEINFO for our usecase
 *  - search a crtc available on the encoder (51)
 *  - create a PropertyBlob to store the connectore's modeinfo
 *  - set the crtc's MODE_ID with the id of the PropertyBlob
 *  - search a primary plane supporting the crtc
 *  - create N buffers structure with only size, dimension, type values
 * * secondary entity creation
 *  - copy the main entity and change few settings
 *  - create N buffers structure with only size, dimension, type values
 * * buffers requesting of the main entity (default type buf_type_dmabuf)
 *  - for each N buffers, create a bo_handle from the dma_buf fd pushed by the previous device
 *  - for each N buffers, create a framebuffer blob on the device with the bo_handles
 *  - store all properties ID nedded by the future commits
 * * buffers requesting of the secondary entity (default type buf_type_dmabuf | buf_type_master)
 *  - for each N buffers, create a Dumb Buffer and take the dma_buf of the DUMB bo_handle
 *  - for each N buffers, create a framebuffer blob on the device with the bo_handles
 * * start the main entity
 *  - generate an atomic commit for modesetting
 *    = set the connector's CRTC_ID
 *    = set the crtc's MODE_ID (already made during the main entity creation |)
 *    = set the crtc's ACTIVE to 1
 *    = set the plane's FB_ID with the first buffer from the main entity
 *    = set the plane's CRTC_ID with the selected crtc
 *    = set the plane's SRC_(X,Y,W,H) with screen rectangle << 16 (I don't understand this value of 16)
 *    = set the plane's CRTC_(X,Y,W,H) with screen rectangle
 *    = set the plane's ROTATION
 *    = generate an AtomicComit with DRM_MODE_PAGE_FLIP_EVENT | DRM_MODE_ATOMIC_NONBLOCK | ALLOW_MODESET flag
 * * start the secondary entity
 *  - for each N buffers, set it as available
 * * select on the previous device returns a dma_buf
 * * queue the dma_buf to the main entity
 *  - find a buffer of the secondary entity to use
 *  - generate an atomic commit
 *    = set the plane's FB_ID with the first buffer from the main entity
 *    = set the plane's CRTC_ID with the selected crtc
 *    = set the plane's SRC_(X,Y,W,H) with screen rectangle << 16 (I don't understand this value of 16)
 *    = set the plane's CRTC_(X,Y,W,H) with screen rectangle
 *    = set the plane's ROTATION
 *    = set the connector's WRITEBACK_OUT_FENCE_PTR with a pointer on an "int" variable
 *    = set the connector's WRITEBACK_FB_ID with the FB_ID of the previously found buffer from secondary entity
 *    = generate an AtomicComit with DRM_MODE_PAGE_FLIP_EVENT | DRM_MODE_ATOMIC_NONBLOCK flag
 * * select on the DRM device fd
 * * dequeue the buffer from the main entity
 *  - HandleEvent
 *    * copy the value of the variable used with WRITEBACK_OUT_FENCE_PTR to the secondary entity
 * * select on the out_fence fd from the secondary entity
 * * dequeue the buffer from the secondary entity
 *  - close the out_fence fd
 *  - return the dma_buf to the next device
 * * select the next device
 * * queue the buffer to the secondary entity
 */
#define sdrm_dbg(...)

/**
 * the out_fence fd is ready immediately after the commit.
 * is not use to wait the Flip event to trig the out_fence event
 * But it may have some confusion during the buffer de/queueing.
 * The SDRM_FASTER_TRANSFER force to trig the out_fence event ASAP.
 */
#define MAX_BUFFERS 4

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

#define SDRM_FLAGS_ATOMIC_COMMIT 0x0001
#define SDRM_FLAGS_MODESET 0x0002

typedef struct Display_s Display_t;
struct Display_s
{
	DisplayConf_t *config;
	const char *name;
	uint32_t connector_id;
	uint32_t encoder_id;
	uint32_t crtc_id;
	int crtcindex;
	uint32_t plane_id;
	uint32_t mode_id;
	uint32_t properties[SDRM_PROPID_LAST];
	drmModeCrtc *crtc;
	uint32_t fourcc;
	int bpp;
	uint64_t modifier;
	int plane_type;
	device_type_e type;
	int fd;
	int out_fd;
	FrameBuffer_t *out_buffer;
	drmModeModeInfo mode;
	FrameBuffer_t buffers[MAX_BUFFERS];
	int nbuffers;
	int buf_id;
	int queueid;
	int flags;
	uint32_t rotation;
	Display_t *dup;
};

/// build the connector pipeline
static int sdrm_ids(Display_t *disp, uint32_t *conn_id, uint32_t *enc_id, uint32_t *crtc_id, drmModeModeInfo *mode)
{
	drmModeResPtr resources;
	resources = drmModeGetResources(disp->fd);
	if (resources == NULL)
	{
		err("sdrm: resources not  availables");
		return -1;
	}

	/// search a free encoder
	uint32_t encoder_id = 0;
	for(int i=0; i < resources->count_encoders; ++i)
	{
		drmModeEncoderPtr encoder;
		encoder = drmModeGetEncoder(disp->fd, resources->encoders[i]);
		if(encoder != NULL)
		{
			encoder_id = encoder->encoder_id;
			for(int i = 0; i < resources->count_connectors; ++i)
			{
				drmModeConnectorPtr connector = drmModeGetConnector(disp->fd, resources->connectors[i]);
				if (! connector)
					continue;
				if(encoder->encoder_id == connector->encoder_id)
				{
					encoder_id = 0;
					break;
				}
				drmModeFreeConnector(connector);
			}
			drmModeFreeEncoder(encoder);
		}
	}

	/// find a connected connector
	uint32_t connector_type = 0;
	uint32_t connector_id = 0;
	for(int i = 0; i < resources->count_connectors; ++i)
	{
		drmModeConnectorPtr connector = drmModeGetConnector(disp->fd, resources->connectors[i]);
		if (! connector)
			continue;
		if (disp->type == device_transfer &&
			connector->connector_type != DRM_MODE_CONNECTOR_WRITEBACK)
			continue;
		if (connector->connection == DRM_MODE_CONNECTED)
		{
			/// if connector has not an encoder, use the freed one
			if (!encoder_id ||
				(encoder_id && connector->encoder_id && connector->encoder_id != encoder_id))
				encoder_id = connector->encoder_id;

			/// search the modeinfo to store into a blob and push into the CRTC
			drmModeModeInfo *preferred = NULL;
			if (mode->hdisplay && mode->vdisplay)
				preferred = mode;
			for (int m = 0; m < connector->count_modes; m++)
			{
				if (!preferred && connector->modes[m].type & DRM_MODE_TYPE_PREFERRED)
				{
					preferred = &connector->modes[m];
				}
				if (preferred &&
					connector->modes[m].hdisplay == preferred->hdisplay &&
					connector->modes[m].vdisplay == preferred->vdisplay)
				{
					preferred = &connector->modes[m];
				}
			}
			if (preferred == NULL || preferred == mode)
				preferred = &connector->modes[0];
			memcpy(mode, preferred, sizeof(*mode));
			connector_id = connector->connector_id;
			connector_type = connector->connector_type;
			drmModeFreeConnector(connector);
			break;
		}
		drmModeFreeConnector(connector);
	}

	if (connector_id == 0 || encoder_id == 0)
	{
		err("sdrm: display not connected");
		drmModeFreeResources(resources);
		return -1;
	}
	*conn_id = connector_id;
	*enc_id = encoder_id;
	drmModeEncoderPtr encoder;
	encoder = drmModeGetEncoder(disp->fd, encoder_id);
	if(encoder != NULL)
	{
		if (encoder->crtc_id)
			*crtc_id = encoder->crtc_id;
		for(int j = 0;!*crtc_id && j < resources->count_crtcs; ++j)
		{
			if (encoder->possible_crtcs & (1 << j))
			{
				*crtc_id = resources->crtcs[j];
			}
		}
		drmModeFreeEncoder(encoder);
	}

	disp->crtcindex = -1;
	for(int i=0;*crtc_id && i < resources->count_crtcs; ++i)
	{
		if (resources->crtcs[i] == *crtc_id)
		{
			disp->crtcindex = i;
			break;
		}
	}
	drmModeFreeResources(resources);
	if (disp->crtcindex == -1)
	{
		err("sdrm: connector not available");
		return -1;
	}
	if (drmModeCreatePropertyBlob(disp->fd, mode, sizeof(*mode), &disp->mode_id))
	{
		err("sdrm: blob creation error %m");
		return -1;
	}

	const char *type = drmModeGetConnectorTypeName(connector_type);
	dbg("sdrm: connector %lu %s, encoder %lu, crtc %lu", disp->connector_id,
		type, disp->encoder_id, disp->crtc_id);
	return 0;
}

static uint32_t sdrm_propertyid(Display_t *disp,  uint32_t type, uint32_t id, const char *property)
{
	uint32_t ret = -1;
	drmModeObjectPropertiesPtr props;

	props = drmModeObjectGetProperties(disp->fd, id, type);
	for (int i = 0; props && i < props->count_props; i++)
	{
		drmModePropertyPtr prop;

		prop = drmModeGetProperty(disp->fd, props->props[i]);
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

static uint64_t sdrm_properties(Display_t *disp,  uint32_t type, uint32_t id, const char *property, uint64_t value)
{
	uint64_t ret = 0;
	drmModeObjectPropertiesPtr props;

	props = drmModeObjectGetProperties(disp->fd, id, type);
	sdrm_dbg("sdrm: property for %#x", type);
	for (int i = 0; props && i < props->count_props; i++)
	{
		drmModePropertyPtr prop;

		prop = drmModeGetProperty(disp->fd, props->props[i]);
		if (prop)
		{
#ifdef DEBUG
			sdrm_dbg("\t%s [%lu] => %lu", prop->name, props->props[i], props->prop_values[i]);
#endif
			if (!strcmp(prop->name, property))
			{
				ret = props->prop_values[i];
				if (value != (uint64_t) -1)
				{
					drmModeObjectSetProperty(disp->fd, id, type, props->props[i], value);
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

#ifdef DEBUG
static int sdrm_listconnector(Display_t *disp)
{
	drmModeResPtr resources;
	resources = drmModeGetResources(disp->fd);
	if (resources == NULL)
	{
		err("sdrm: No resource available");
		return -1;
	}

	for(int i = 0; i < resources->count_connectors; ++i)
	{
		int32_t connector_id = -1;
		connector_id = resources->connectors[i];
		drmModeConnectorPtr connector = drmModeGetConnector(disp->fd, connector_id);
		if (! connector)
			continue;
		dbg("connector id %d",connector->connector_id);
		const char *type = drmModeGetConnectorTypeName(connector->connector_type);
		if (type)
			dbg("\ttype %s",type);
		else
			dbg("\ttype unkown");
		dbg("\tconnected %d",connector->connection == DRM_MODE_CONNECTED);
		dbg("\tencoder %lu",connector->encoder_id);

		for (int m = 0; m < connector->count_modes; m++)
		{
			dbg("\tmode \"%s\": %dx%d %s",
					connector->modes[m].name,
					connector->modes[m].hdisplay,
					connector->modes[m].vdisplay,
					connector->modes[m].type & DRM_MODE_TYPE_PREFERRED ? "*" : "");
		}
		drmModeObjectPropertiesPtr props;

		props = drmModeObjectGetProperties(disp->fd, connector_id, DRM_MODE_OBJECT_CONNECTOR);
		for (int j = 0; j < props->count_props; j++)
		{
			drmModePropertyPtr prop;

			prop = drmModeGetProperty(disp->fd, props->props[j]);
			if (prop)
			{
				dbg("\tproperty %s %d", prop->name, prop->prop_id);
				dbg("\t\t%s : %llu", prop->name, props->prop_values[j]);
			}
		}
	}
	return 0;
}

static int sdrm_listproperties(Display_t *disp,  uint32_t type)
{
	drmModeResPtr resources;
	resources = drmModeGetResources(disp->fd);
	if (resources == NULL)
	{
		err("sdrm: No resource available");
		return -1;
	}
	int count = 0;
	int32_t defid = 0;
	int32_t *id = &defid;
	const char *name;
	const char *connector_name = "connector";
	const char *crtc_name = "crtc";
	const char *plane_name = "plane";
	const char *encoder_name = "encoder";
	switch (type)
	{
		case DRM_MODE_OBJECT_CONNECTOR:
			count = resources->count_connectors;
			id = resources->connectors;
			name = connector_name;
		break;
		case DRM_MODE_OBJECT_PLANE:
			count = 1;
			*id = disp->plane_id;
			name = plane_name;
		break;
		case DRM_MODE_OBJECT_CRTC:
			count = resources->count_crtcs;
			id = resources->crtcs;
			name = crtc_name;
		break;
		case DRM_MODE_OBJECT_ENCODER:
			count = resources->count_encoders;
			id = resources->encoders;
			name = encoder_name;
		break;
	}
	for(int i = 0; i < count; ++i)
	{
		drmModeObjectPropertiesPtr props;

		props = drmModeObjectGetProperties(disp->fd, id[i], type);
		dbg("sdrm: properties %s[%d] %lu", name, i, id[i]);
		for (int j = 0; props && j < props->count_props; j++)
		{
			drmModePropertyPtr prop;
			prop = drmModeGetProperty(disp->fd, props->props[j]);
			if (prop)
			{
				dbg("\tproperty %s %d", prop->name, prop->prop_id);
				dbg("\t\t%s : %llu", prop->name, props->prop_values[j]);
			}
		}
	}
	return 0;
}
#endif

static int sdrm_plane(Display_t *disp, uint32_t *plane_id)
{
	int ret = -1;
	drmModePlaneResPtr planes;

	planes = drmModeGetPlaneResources(disp->fd);

	*plane_id = (uint32_t)-1;
	drmModePlanePtr plane;
	dbg("sdrm: Plane");
	for (int i = 0; i < planes->count_planes; ++i)
	{
		plane = drmModeGetPlane(disp->fd, planes->planes[i]);
		int type = (int)sdrm_properties(disp, DRM_MODE_OBJECT_PLANE, plane->plane_id, "type", (uint64_t)-1);
		dbg("  [%d] %u: %si %#x %d", i, plane->plane_id, (type == DRM_PLANE_TYPE_PRIMARY)?"primary":(type == DRM_PLANE_TYPE_OVERLAY)?"overlay":"cursor");
		if (*plane_id == (uint32_t)-1 && plane->possible_crtcs & (1 << disp->crtcindex) && type == disp->plane_type)
		{
			for (int j = 0; j < plane->count_formats; ++j)
			{
				uint32_t fourcc = plane->formats[j];
				warn("\tformat %.4s", (char *)&fourcc);
				if ((!disp->fourcc || plane->formats[j] == disp->fourcc) && plane->possible_crtcs & (1 << disp->crtcindex))
				{
					ret = 0;
					disp->fourcc = plane->formats[j];
					*plane_id = plane->plane_id;
				}
#ifndef DEBUG
				if (ret == 0)
					break;
#endif
			}
		}
		drmModeFreePlane(plane);
#ifndef DEBUG
		if (ret == 0)
			break;
#endif
	}
	drmModeFreePlaneResources(planes);
	if (ret == -1)
	{
		err("sdrm: plane with 4cc %.4s not found", &disp->fourcc);
	}
	return ret;
}

static int sdrm_buffer_generic(Display_t *disp, uint32_t width, uint32_t height, uint32_t fourcc, FrameBuffer_t *buffer)
{
	uint32_t stride;
	uint64_t size;
	int bpp = 32;
	dbg("sdrm: buffer for width %u height %u ", width, height);
	switch (fourcc)
	{
		case FOURCC_RGBP:
		case FOURCC_YUYV:
			bpp = 16;
		break;
		case FOURCC_NV12:
			bpp = 8;
		break;
	}

	buffer->size = width * height * bpp / 8;
	buffer->bpp = bpp;
	buffer->nplanes = 1;
	buffer->width = width;
	buffer->height = height;
	buffer->strides[0] = buffer->size / height;

	switch (fourcc)
	{
		case FOURCC_YUYV:
			buffer->strides[1] = buffer->strides[0] / 2;
			buffer->offsets[1] = buffer->strides[0] * height;
			buffer->strides[2] = buffer->strides[1];
			buffer->offsets[2] = buffer->offsets[1] + buffer->strides[1] * height;
			buffer->nplanes = 3;
		break;
		case FOURCC_NV12:
			buffer->strides[1] = buffer->strides[0];
			buffer->offsets[1] = buffer->strides[0] * height;
			buffer->nplanes = 2;
		break;
	}

	return 0;
}

static int sdrm_buffer_dumb(Display_t *disp, FrameBuffer_t *buffer)
{
	uint32_t stride;
	uint64_t size;
	uint32_t bo_handle;
	drmModeCreateDumbBuffer(disp->fd, buffer->width, buffer->height, buffer->bpp, 0, &bo_handle, &stride, &size);
	buffer->private = (void *)(long)bo_handle;
	if (size != buffer->size)
	{
		warn("sdrm: buffer size changed!!!");
	}
	buffer->size = size;
	return 0;
}

static int sdrm_buffer_fb(Display_t *disp, uint32_t fourcc, uint64_t modifier, FrameBuffer_t *buffer)
{
	uint32_t handles[4] = {0};
	for (int i = 0; i < buffer->nplanes; i++)
		handles[i] = (long)buffer->private;
	uint64_t modifiers[4] = { modifier };
	int flags = 0;
	if (modifier)
		flags = DRM_MODE_FB_MODIFIERS;
	if (drmModeAddFB2WithModifiers(disp->fd, buffer->width, buffer->height, fourcc, handles,
		buffer->strides, buffer->offsets, modifiers, &buffer->id, flags))
	{
		err("sdrm: Frame buffer unavailable 2 (%dx%d %.4s) %m", buffer->width, buffer->height, &fourcc);
		return -1;
	}
	return 0;
}

static int sdrm_buffer_setmemory(Display_t *disp, uint32_t dumb, FrameBuffer_t *buffer)
{
	uint64_t map_offset;
	drmModeMapDumbBuffer(disp->fd, dumb, &map_offset);
	buffer->map_offset = map_offset;

	/// set the value of mem to force the use of memory during the queueing
	buffer->mem = (uint32_t *)mmap(NULL, buffer->size, PROT_READ | PROT_WRITE,
		MAP_SHARED, disp->fd, buffer->map_offset);

	munmap(buffer->mem, buffer->size);
	return 0;
}

static int sdrm_buffer_setdma(Display_t *disp, uint32_t dumb, FrameBuffer_t *buffer)
{
	if (drmPrimeHandleToFD(disp->fd, dumb, 0, &buffer->dma_buf) < 0)
	{
		err("sdrm: dmabuf not allowed %m");
		return -1;
	}
	return 0;
}

static int sdrm_buffer_setdma2(Display_t *disp, uint32_t size, int fd, FrameBuffer_t *buffer)
{
	if (size != buffer->size)
	{
		warn("sdrm: buffer size changed!!!");
	}
	buffer->size = size;

	uint32_t bo_handle;
	if (drmPrimeFDToHandle(disp->fd, fd, &bo_handle))
	{
		err("sdrm: buffer %d association error", fd);
		return -1;
	}
	buffer->private = (void *)(long)bo_handle;
	return 0;
}

static void sdrm_freebuffer(Display_t *disp, FrameBuffer_t *buffer)
{
	drmModeRmFB(disp->fd, buffer->id);
	drmModeDestroyDumbBuffer(disp->fd, (uint32_t)(long)buffer->private);
}

static int sdrm_atomic_prepare(Display_t *disp, drmModeModeInfo *mode)
{
	disp->properties[SDRM_PROPID_CRTC_ID] = sdrm_propertyid(disp, DRM_MODE_OBJECT_CONNECTOR, disp->connector_id, "CRTC_ID");
	uint32_t prop_plane_crtc_id = sdrm_propertyid(disp, DRM_MODE_OBJECT_PLANE, disp->plane_id, "CRTC_ID");
	if (prop_plane_crtc_id != disp->properties[SDRM_PROPID_CRTC_ID])
	{
		warn("sdrm: CRTC_ID for plane(%lu) and connector(%lu) differents", prop_plane_crtc_id, disp->properties[SDRM_PROPID_CRTC_ID]);
	}
	if (disp->dup && disp->dup->type == device_input)
	{
		disp->properties[SDRM_PROPID_WRITEBACK_OUT_FENCE_PTR] = sdrm_propertyid(disp, DRM_MODE_OBJECT_CONNECTOR, disp->connector_id, "WRITEBACK_OUT_FENCE_PTR");
		if (disp->properties[SDRM_PROPID_WRITEBACK_OUT_FENCE_PTR] == (uint32_t)-1)
		{
			err("sdrm: writeback connector's property error %m");
			return -1;
		}
		disp->properties[SDRM_PROPID_WRITEBACK_FB_ID] = sdrm_propertyid(disp, DRM_MODE_OBJECT_CONNECTOR, disp->connector_id, "WRITEBACK_FB_ID");
		if (disp->properties[SDRM_PROPID_WRITEBACK_FB_ID] == (uint32_t)-1)
		{
			err("sdrm: writeback connector's property error %m");
			return -1;
		}
	}
	disp->properties[SDRM_PROPID_MODE_ID] = sdrm_propertyid(disp, DRM_MODE_OBJECT_CRTC, disp->crtc_id, "MODE_ID");
	disp->properties[SDRM_PROPID_ACTIVE] = sdrm_propertyid(disp, DRM_MODE_OBJECT_CRTC, disp->crtc_id, "ACTIVE");
	disp->properties[SDRM_PROPID_FB_ID] = sdrm_propertyid(disp, DRM_MODE_OBJECT_PLANE, disp->plane_id, "FB_ID");
	disp->properties[SDRM_PROPID_SRC_X] = sdrm_propertyid(disp, DRM_MODE_OBJECT_PLANE, disp->plane_id, "SRC_X");
	disp->properties[SDRM_PROPID_SRC_Y] = sdrm_propertyid(disp, DRM_MODE_OBJECT_PLANE, disp->plane_id, "SRC_Y");
	disp->properties[SDRM_PROPID_SRC_W] = sdrm_propertyid(disp, DRM_MODE_OBJECT_PLANE, disp->plane_id, "SRC_W");
	disp->properties[SDRM_PROPID_SRC_H] = sdrm_propertyid(disp, DRM_MODE_OBJECT_PLANE, disp->plane_id, "SRC_H");
	disp->properties[SDRM_PROPID_CRTC_X] = sdrm_propertyid(disp, DRM_MODE_OBJECT_PLANE, disp->plane_id, "CRTC_X");
	disp->properties[SDRM_PROPID_CRTC_Y] = sdrm_propertyid(disp, DRM_MODE_OBJECT_PLANE, disp->plane_id, "CRTC_Y");
	disp->properties[SDRM_PROPID_CRTC_W] = sdrm_propertyid(disp, DRM_MODE_OBJECT_PLANE, disp->plane_id, "CRTC_W");
	disp->properties[SDRM_PROPID_CRTC_H] = sdrm_propertyid(disp, DRM_MODE_OBJECT_PLANE, disp->plane_id, "CRTC_H");
	disp->properties[SDRM_PROPID_ROTATION] = sdrm_propertyid(disp, DRM_MODE_OBJECT_PLANE, disp->plane_id, "rotation");
	for (int i = 0; i < SDRM_PROPID_LAST; i++)
	{
		if (disp->properties[i] == (uint32_t)-1)
		{
			err("sdrm: property %d not found", i);
			//return -1;
		}
	}
	return 0;
}

static FrameBuffer_t *sdrm_pulloutbuffer(Display_t *disp, int id)
{
	disp->queueid = id;
	disp->buffers[id].state = queued;
	return &disp->buffers[id];
}

static int sdrm_pushoutbuffer(Display_t *disp, FrameBuffer_t *buffer)
{
	if (buffer->state == queued)
	{
		disp->out_buffer = buffer;
		return 0;
	}
	return -1;
}

static int sdrm_atomic_commit(Display_t *disp, FrameBuffer_t *buffer)
{
	drmModeAtomicReq *req;
	req = drmModeAtomicAlloc();

	if (!(disp->flags & SDRM_FLAGS_MODESET))
	{
		if (drmModeAtomicAddProperty(req, disp->connector_id, disp->properties[SDRM_PROPID_CRTC_ID], disp->crtc_id) < 0)
			goto commit_error;
		if (drmModeAtomicAddProperty(req, disp->crtc_id, disp->properties[SDRM_PROPID_MODE_ID], disp->mode_id) < 0)
			goto commit_error;
		if (drmModeAtomicAddProperty(req, disp->crtc_id, disp->properties[SDRM_PROPID_ACTIVE], 1) < 0)
			goto commit_error;
	}
	if (drmModeAtomicAddProperty(req, disp->plane_id, disp->properties[SDRM_PROPID_FB_ID], buffer->id) < 0)
		goto commit_error;
	if (drmModeAtomicAddProperty(req, disp->plane_id, disp->properties[SDRM_PROPID_CRTC_ID], disp->crtc_id) < 0) /// <=== failed ???
		goto commit_error;
	/// the src rectangle must be move from 16 bits without any reason found ???
	if (disp->properties[SDRM_PROPID_SRC_X] != (uint32_t)-1 &&
		drmModeAtomicAddProperty(req, disp->plane_id, disp->properties[SDRM_PROPID_SRC_X], 0 << 16) < 0)
		goto commit_error;
	if (disp->properties[SDRM_PROPID_SRC_Y] != (uint32_t)-1 &&
		drmModeAtomicAddProperty(req, disp->plane_id, disp->properties[SDRM_PROPID_SRC_Y], 0 << 16) < 0)
		goto commit_error;
	if (disp->properties[SDRM_PROPID_SRC_W] != (uint32_t)-1 &&
		drmModeAtomicAddProperty(req, disp->plane_id, disp->properties[SDRM_PROPID_SRC_W], buffer->width << 16) < 0)
		goto commit_error;
	if (disp->properties[SDRM_PROPID_SRC_H] != (uint32_t)-1 &&
		drmModeAtomicAddProperty(req, disp->plane_id, disp->properties[SDRM_PROPID_SRC_H], buffer->height << 16) < 0)
		goto commit_error;
	if (disp->properties[SDRM_PROPID_CRTC_X] != (uint32_t)-1 &&
		drmModeAtomicAddProperty(req, disp->plane_id, disp->properties[SDRM_PROPID_CRTC_X], 0) < 0)
		goto commit_error;
	if (disp->properties[SDRM_PROPID_CRTC_Y] != (uint32_t)-1 &&
		drmModeAtomicAddProperty(req, disp->plane_id, disp->properties[SDRM_PROPID_CRTC_Y], 0) < 0)
		goto commit_error;
	if (disp->properties[SDRM_PROPID_CRTC_W] != (uint32_t)-1 &&
		drmModeAtomicAddProperty(req, disp->plane_id, disp->properties[SDRM_PROPID_CRTC_W], buffer->width) < 0)
		goto commit_error;
	if (disp->properties[SDRM_PROPID_CRTC_H] != (uint32_t)-1 &&
		drmModeAtomicAddProperty(req, disp->plane_id, disp->properties[SDRM_PROPID_CRTC_H], buffer->height) < 0)
		goto commit_error;
	if (disp->properties[SDRM_PROPID_ROTATION] != (uint32_t)-1)
		drmModeAtomicAddProperty(req, disp->plane_id, disp->properties[SDRM_PROPID_ROTATION], disp->rotation);
	if (disp->out_buffer && disp->dup != NULL && disp->dup->type == device_input)
	{
		drmModeAtomicAddProperty(req, disp->connector_id, disp->properties[SDRM_PROPID_WRITEBACK_OUT_FENCE_PTR], (uint64_t)(long)&disp->out_fd);
		FrameBuffer_t *out_buffer = disp->out_buffer;
		drmModeAtomicAddProperty(req, disp->connector_id, disp->properties[SDRM_PROPID_WRITEBACK_FB_ID], out_buffer->id);
	}

	int flags = DRM_MODE_ATOMIC_TEST_ONLY;
	if (!(disp->flags & SDRM_FLAGS_MODESET))
		flags |= DRM_MODE_ATOMIC_ALLOW_MODESET;
	if (!(disp->flags & SDRM_FLAGS_MODESET) && drmModeAtomicCommit(disp->fd, req, flags, NULL) < 0)
	{
		err("sdrm: atomic commit test failed %m");
		goto commit_error;
	}

	flags = DRM_MODE_PAGE_FLIP_EVENT | DRM_MODE_ATOMIC_NONBLOCK;
	if (!(disp->flags & SDRM_FLAGS_MODESET))
	{
		flags |= DRM_MODE_ATOMIC_ALLOW_MODESET;
		disp->flags |= SDRM_FLAGS_MODESET;
	}
	if (drmModeAtomicCommit(disp->fd, req, flags, disp) < 0)
	{
		err("sdrm: atomic commit failed %m");
		goto commit_error;
	}
	drmModeAtomicFree(req);

	return 0;
commit_error:
	drmModeAtomicFree(req);
	return -1;
}

Display_t *sdrm_create2(int fd, const char *name, device_type_e type, DisplayConf_t *config)
{
	if (drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1))
	{
		err("sdrm: Universal plane not supported %m");
		return NULL;
	}

	Display_t *disp = calloc(1, sizeof(*disp));
	disp->fd = fd;
	disp->plane_type = DRM_PLANE_TYPE_PRIMARY;
	disp->type = type;
	disp->name = name;
	disp->rotation = DRM_MODE_ROTATE_0;

#ifndef SDRM_DISABLE_ATOMIC_COMMIT
	if (drmSetMaster(fd))
		err("sdrm: setmaster failed %m");
	/// enable atomic and writeback before setting the primary connector
	if (drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1))
		err("sdrm: atomic not supported %m");
#ifndef SDRM_DISABLE_WRITEBACK
	if (drmSetClientCap(fd, DRM_CLIENT_CAP_WRITEBACK_CONNECTORS, 1))
		err("sdrm: writeback not supported %m");
#endif
	if (type == device_control)
	{
		drmDropMaster(fd);
	}
#endif
#ifdef DEBUG
	sdrm_listconnector(disp);
	sdrm_listproperties(disp, DRM_MODE_OBJECT_CRTC);
	sdrm_listproperties(disp, DRM_MODE_OBJECT_ENCODER);
#endif
	if (config)
	{
		disp->mode.hdisplay = config->parent.width;
		disp->mode.vdisplay = config->parent.height;
		if (config->parent.fourcc)
			disp->fourcc = config->parent.fourcc;
		if (config->parent.modifiers)
			disp->modifier = config->parent.modifiers;
	}
	if (sdrm_ids(disp, &disp->connector_id, &disp->encoder_id, &disp->crtc_id, &disp->mode) == -1)
	{
		free(disp);
		return NULL;
	}
	if (disp->crtc_id && disp->mode_id)
		sdrm_properties(disp, DRM_MODE_OBJECT_CRTC, disp->crtc_id, "MODE_ID", disp->mode_id);

	if (sdrm_plane(disp, &disp->plane_id) == -1)
	{
		free(disp);
		return NULL;
	}
#ifdef DEBUG
	sdrm_listproperties(disp, DRM_MODE_OBJECT_PLANE);
#endif

	for (int i = 0; i < MAX_BUFFERS; i++, disp->nbuffers ++)
	{
		if (sdrm_buffer_generic(disp,  disp->mode.hdisplay, disp->mode.vdisplay,
				disp->fourcc, &disp->buffers[i]))
		{
			err("sdrm: buffer allocation error %m");
			free(disp);
			return NULL;
		}
	}
	return disp;
}

EXT_API Display_t *sdrm_create(const char *name, device_type_e type, DisplayConf_t *config)
{
	int fd = 0;
	if (!access(config->device, R_OK | W_OK))
		fd = open(config->device, O_RDWR| O_NONBLOCK | O_CLOEXEC);
	else
		fd = drmOpen(config->device, NULL);
	if (fd < 0)
	{
		err("device %s (%s) bad argument %m", name, config->device);
		return NULL;
	}
	Display_t *disp = sdrm_create2(fd, name, type, config);
	if (disp == NULL)
	{
		close(fd);
		return NULL;
	}
	warn("sdrm: create %s device on  %s", name, config->device);

	disp->config = config;
//	config->parent.dev = disp;

	return disp;
}

#ifndef SDRM_DISABLE_WRITEBACK
EXT_API Display_t *sdrm_duplicate(Display_t *dev, DisplayConf_t **pconfig)
{
	Display_t *disp = NULL;
	if (dev->type != device_transfer)
		return NULL;
	dev->type = device_output;
	*pconfig = calloc(1, sizeof(**pconfig));
	memcpy(*pconfig, dev->config, sizeof(**pconfig));
	memmove(&(*pconfig)->parent, &dev->config->transfer, sizeof((*pconfig)->parent));

	disp = calloc(1, sizeof(*disp));
	if (!disp)
		return NULL;
	memcpy(disp, dev, sizeof(*disp));
	disp->type = device_input;
	dev->dup = disp;
	disp->dup = dev;
	disp->config = *pconfig;
	disp->mode = dev->mode;
#ifdef DEBUG
	uint64_t blob_id = sdrm_properties(disp, DRM_MODE_OBJECT_CONNECTOR, disp->connector_id, "WRITEBACK_PIXEL_FORMATS", -1);
	drmModePropertyBlobRes *blob = NULL;
	blob = drmModeGetPropertyBlob(disp->fd, blob_id);
	if (blob)
	{
		dbg("sdrm: writeback pixel formats");
		uint32_t *fourccs = blob->data;
		for (int i = 0; i < blob->length / sizeof(uint32_t); i++)
			dbg("\t%.4s", &fourccs[i]);
		drmModeFreePropertyBlob(blob);
	}
#endif
	disp->nbuffers = 0;
	for (int i = 0; i < MAX_BUFFERS; i++, disp->nbuffers ++)
	{
		if (sdrm_buffer_generic(disp,  disp->mode.hdisplay, disp->mode.vdisplay,
				disp->fourcc, &disp->buffers[i]))
		{
			err("sdrm: buffer allocation error %m");
			free(disp);
			return NULL;
		}
	}
	return disp;
}
#else
#define sdrm_duplicate NULL
#endif

static int sdrm_requestbuffer_output(Display_t *disp, enum buf_type_e t, va_list ap)
{
	switch (t)
	{
		case (buf_type_memory | buf_type_master):
		{
			int *ntargets = va_arg(ap, int *);
			void **targets = va_arg(ap, void **);
			size_t *psize = va_arg(ap, size_t *);
			disp->nbuffers = 0;
			if (targets != NULL)
			{
				*targets = calloc(disp->nbuffers, sizeof(void*));
				for (int i = 0; i < MAX_BUFFERS; i++, disp->nbuffers ++)
				{
					if (sdrm_buffer_dumb(disp, &disp->buffers[i]))
					{
						err("sdrm: buffer master error %m");
						break;
					}
					if (sdrm_buffer_setmemory(disp, (long)disp->buffers[i].private, &disp->buffers[i]) == -1)
					{
						err("sdrm: buffer %d allocation error", i);
						break;
					}
					targets[i] = disp->buffers[i].mem;
				}
			}
			if (ntargets != NULL)
				*ntargets = disp->nbuffers;
			if (psize != NULL)
				*psize = disp->buffers[0].size;
		}
		break;
		case buf_type_dmabuf:
		{
			int ntargets = va_arg(ap, int);
			int *targets = va_arg(ap, int *);
			size_t size = va_arg(ap, size_t);
			for (int i = 0; i < ntargets; i++)
			{
				if (sdrm_buffer_setdma2(disp, size, targets[i], &disp->buffers[i]))
				{
					err("sdrm: buffer %d association error", i);
					break;
				}
			}
			disp->nbuffers = ntargets;
		}
		break;
		case buf_type_dmabuf | buf_type_master:
		{
			int *ntargets = va_arg(ap, int *);
			int **targets = va_arg(ap, int **);
			size_t *psize = va_arg(ap, size_t *);
			disp->nbuffers = 0;
			if (targets != NULL)
			{
				*targets = calloc(disp->nbuffers, sizeof(int));
				for (int i = 0; i < MAX_BUFFERS; i++, disp->nbuffers ++)
				{
					if (sdrm_buffer_dumb(disp, &disp->buffers[i]))
					{
						err("sdrm: output buffer master error %m");
						break;
					}
					if (sdrm_buffer_setdma(disp, (long)disp->buffers[i].private, &disp->buffers[i]) == -1)
					{
						err("sdrm: output buffer %d allocation error", i);
						break;
					}
					(*targets)[i] = disp->buffers[i].dma_buf;
				}
			}
			if (ntargets != NULL)
				*ntargets = disp->nbuffers;
			if (psize != NULL)
				*psize = disp->buffers[0].size;
		}
		break;
		default:
			return -1;
	}

	for (int i = 0; i < MAX_BUFFERS; i++, disp->nbuffers ++)
	{
		if (sdrm_buffer_fb(disp, disp->fourcc, disp->modifier, &disp->buffers[i]))
		{
			err("sdrm: frame buffer error %m");
			return -1;
		}
	}

#ifndef SDRM_DISABLE_ATOMIC_COMMIT
	if (!sdrm_atomic_prepare(disp, &disp->mode))
	{
		disp->flags |= SDRM_FLAGS_ATOMIC_COMMIT;
		dbg("sdrm: run in atomic mode");
	}
#endif

	return 0;
}

static int sdrm_requestbuffer_input(Display_t *disp, enum buf_type_e t, va_list ap)
{
	uint32_t width = disp->config->parent.width;
	uint32_t height = disp->config->parent.height;
	switch (t)
	{
		case buf_type_dmabuf | buf_type_master:
		{
			int *ntargets = va_arg(ap, int *);
			int **targets = va_arg(ap, int **);
			size_t *psize = va_arg(ap, size_t *);
			if (targets != NULL)
			{
				*targets = calloc(disp->nbuffers, sizeof(int));
				for (int i = 0; i < disp->nbuffers; i++)
				{

					if (sdrm_buffer_dumb(disp, &disp->buffers[i]))
					{
						err("sdrm: input buffer master error %m");
						break;
					}
					if (sdrm_buffer_setdma(disp, (long)disp->buffers[i].private, &disp->buffers[i]) == -1)
					{
						err("sdrm: input buffer %d allocation error", i);
						break;
					}

					(*targets)[i] = disp->buffers[i].dma_buf;
				}
			}
			if (ntargets != NULL)
				*ntargets = disp->nbuffers;
			if (psize != NULL)
				*psize = disp->buffers[0].size;
		}
		break;
		default:
			return -1;
	}
	for (int i = 0; i < MAX_BUFFERS; i++)
	{
		if (sdrm_buffer_fb(disp, disp->fourcc, disp->modifier, &disp->buffers[i]))
		{
			err("sdrm: frame buffer error %m");
			return -1;
		}
	}

	return 0;
}

EXT_API int sdrm_requestbuffer(Display_t *disp, enum buf_type_e t, ...)
{
	int ret = -1;
	va_list ap;
	va_start(ap, t);
	if (disp->type != device_input)
		ret = sdrm_requestbuffer_output(disp, t, ap);
	else
		ret = sdrm_requestbuffer_input(disp, t, ap);
	va_end(ap);
	return ret;
}

static void page_flip_handler(int fd, unsigned int frame,
		  unsigned int sec, unsigned int usec, void *data)
{
	Display_t *disp = data;
	int id = disp->queueid;
	if (id != -1)
	{
		disp->buffers[(int)id].state = ready;
#ifndef SDRM_FASTER_TRANSFER
		/**
		 * we could use directly the disp->out_fd variable
		 * But in this case the dequeue function of the device_input
		 * is called before this function, and the buffer is not ready
		 */
		disp->dup->out_fd = disp->out_fd;
		disp->out_fd = 0;
		disp->out_buffer->state = ready;
#endif
	}
	/**
	 * queueid = -1 will force the main application
	 * to have the fd = 0 and wait the queueing
	 * But why the fd is not blocked into the select call
	 * after the drmHandleEvent and must wait the drmModePageFlip ?
	 */
	disp->queueid = -1;
}

EXT_API int sdrm_queue(Display_t *disp, int id, void *mem, size_t bytesused, int flags)
{
	if (id > disp->nbuffers)
	{
		err("unkown %d buffer index to queue", id);
		return -1;
	}
	FrameBuffer_t *buffer = &disp->buffers[id];
	sdrm_dbg("sdrm:   queue %s buffer %d", (disp->type == device_input)?"input":"output", id);
	if (bytesused == 0)
		bytesused = buffer->size;
	buffer->flags = flags;
	if (disp->type != device_input)
	{
		if (buffer->mem)
			munmap(buffer->mem, buffer->size);
		if (bytesused > buffer->size)
		{
			warn("sfile: buffer too small %lu %lu", buffer->size, bytesused);
		}
		if (disp->flags & SDRM_FLAGS_ATOMIC_COMMIT)
		{
			if (disp->dup)
			{
				/// real buffer enqueueing for the output (device_input)
				FrameBuffer_t *buffer = sdrm_pulloutbuffer(disp->dup, id);
				if (sdrm_pushoutbuffer(disp, buffer))
				{
					errno = EAGAIN;
					return -1;
				}
			}
			if (sdrm_atomic_commit(disp, buffer))
			{
				errno = EAGAIN;
				return -1;
			}
			/**
			 * Note for SDRM_FASTER_TRANSFER:
			 * at this point the out_fd is already set
			 * but the DRM doesn't terminate the treatement.
			 */
		}
		else
		{
			drmModePageFlip(disp->fd, disp->crtc_id, buffer->id, DRM_MODE_PAGE_FLIP_EVENT, disp);
		}
	}
	buffer->state = queued;
	disp->queueid = id;
	return 0;
}

EXT_API int sdrm_dequeue(Display_t *disp, void **mem, size_t *bytesused, int *flags)
{
	int id = disp->queueid;
	FrameBuffer_t *buffer = NULL;
	if (id >= 0)
		buffer = &disp->buffers[id];
	sdrm_dbg("sdrm: dequeue %s buffer %d", (disp->type == device_input)?"input":"output", id);
	if (!buffer)
	{
		errno = EAGAIN;
		return -1;
	}
	if (disp->type != device_input)
	{
		if (buffer->state != queued)
		{
			errno = EAGAIN;
			return -1;
		}
		drmEventContext evctx = {
			.version = DRM_EVENT_CONTEXT_VERSION,
			.page_flip_handler = page_flip_handler,
		};
		int ret ;
		do {
			ret = drmHandleEvent(disp->fd, &evctx);
		} while (!ret);
	}
	else
	{
#ifndef SDRM_FASTER_TRANSFER
		close(disp->out_fd);
		disp->out_fd = 0;
#else
		close(disp->dup->out_fd);
		disp->dup->out_fd = 0;
		buffer->state = ready;
#endif
	}

	if (buffer->state != ready)
	{
		errno = EAGAIN;
		return -1;
	}
	if (buffer->mem) /// the value is set but the memory is unmaped
		buffer->mem = (uint32_t *)mmap(NULL, buffer->size, PROT_READ | PROT_WRITE, MAP_SHARED,
			disp->fd, buffer->offsets[0]);
	if (bytesused)
		*bytesused = buffer->size;
	if (mem && buffer->mem)
		*mem = buffer->mem;
	buffer->state = dequeued;
	return id;
}

EXT_API int sdrm_fd(Display_t *disp, int writer)
{
	if (writer == 1)
		return -1;
	if (disp->queueid == -1)
		return 0;
	if (disp->type == device_input)
#ifndef SDRM_FASTER_TRANSFER
		return disp->out_fd;
#else
		return disp->dup->out_fd;
#endif
	return disp->fd;
}

EXT_API int sdrm_start(Display_t *disp)
{
	if (disp->type == device_input)
	{
		for (int i = 0; i < disp->nbuffers; i++)
		{
			sdrm_queue(disp, i, disp->buffers[i].mem, 0, 0);
		}
	}
	else
	{
		if (disp->flags & SDRM_FLAGS_ATOMIC_COMMIT)
		{
			if (sdrm_atomic_commit(disp, &disp->buffers[0]))
			{
				return -1;
			}
		}
		else
		{
			disp->crtc = drmModeGetCrtc(disp->fd, disp->crtc_id);
			if (disp->crtc && drmModeSetCrtc(disp->fd, disp->crtc_id, disp->buffers[0].id, 0, 0, &disp->connector_id, 1, &disp->mode))
			{
				err("srdm: Crtc setting error %m");
				return -1;
			}
			disp->flags |= SDRM_FLAGS_MODESET;
			drmModePageFlip(disp->fd, disp->crtc_id, disp->buffers[0].id, DRM_MODE_PAGE_FLIP_EVENT, disp);
		}
	}
	disp->queueid = -1;
	return 0;
}

int sdrm_stop(Display_t *disp)
{
	return 0;
}

#ifdef HAVE_JANSSON
#include <jansson.h>

static int sdrm_capabilities_controls(Display_t *disp, json_t *capabilities)
{
	if (json_is_array(capabilities))
	{
		json_t *rotation = NULL;
		rotation = json_object();
		json_object_set_new(rotation, "name", json_string("rotation"));
		json_object_set_new(rotation, "type", json_string("menu"));
		json_t *items = json_array();
		json_array_append_new(items, json_string("reflect"));
		json_array_append_new(items, json_string("90"));
		json_array_append_new(items, json_string("180"));
		json_array_append_new(items, json_string("270"));
		json_object_set_new(rotation, "items", items);
		json_array_append_new(capabilities, rotation);
	}
	return 0;
}

static int sdrm_capabilities_fourcc(Display_t *disp, json_t *capabilities)
{
	drmModePlaneResPtr planes;

	planes = drmModeGetPlaneResources(disp->fd);
	if (planes == NULL)
		return -1;
	json_t *pixelformat = capabilities;
	if (json_is_array(capabilities))
	{
		pixelformat = json_object();
		json_object_set_new(pixelformat, "name", json_string("fourcc"));
		json_object_set_new(pixelformat, "type", json_string("menu"));
	}

	json_t *items = json_array();
	uint32_t format = 0;
	drmModePlanePtr plane;
	for (int i = 0; i < planes->count_planes; ++i)
	{
		plane = drmModeGetPlane(disp->fd, planes->planes[i]);
		if (plane->plane_id == disp->plane_id)
		{
			for (int j = 0; j < plane->count_formats; ++j)
			{
				json_array_append_new(items, json_stringn((char*)&plane->formats[j], 4));
				if (format == 0)
					format = plane->formats[0];
			}
			drmModeFreePlane(plane);
			break;
		}
		drmModeFreePlane(plane);
	}
	drmModeFreePlaneResources(planes);
	if (format != 0)
	{
		json_object_set(pixelformat, "value", json_stringn((char*)&format, 4));
	}
	if (json_is_array(capabilities))
	{
		json_object_set_new(pixelformat, "items", items);
		json_array_append_new(capabilities, pixelformat);
	}
	return 0;
}

static int sdrm_capabilities_size(Display_t *disp, json_t *capabilities)
{
	drmModeResPtr resources;
	resources = drmModeGetResources(disp->fd);

	json_t *width = json_object();
	json_object_set_new(width, "name", json_string("width"));
	json_object_set_new(width, "type", json_string("integer"));

	json_t *height = json_object();
	json_object_set_new(height, "name", json_string("height"));
	json_object_set_new(height, "type", json_string("integer"));

	json_t *items = json_array();
	unsigned int min_height = UINT_MAX;
	unsigned int max_height = 0;
	unsigned int def_height = 0;
	unsigned int min_width = UINT_MAX;
	unsigned int max_width = 0;
	unsigned int def_width = 0;
	for(int i = 0; i < resources->count_connectors; ++i)
	{
		uint32_t connector_id = resources->connectors[i];
		drmModeConnectorPtr connector = drmModeGetConnector(disp->fd, connector_id);
		if (connector->connection == DRM_MODE_CONNECTED && connector->count_modes > 0)
		{
			drmModeModeInfo *preferred = NULL;
			for (int m = 0; m < connector->count_modes; m++)
			{
				if (min_height > connector->modes[m].vdisplay)
					min_height = connector->modes[m].vdisplay;
				if (max_height < connector->modes[m].vdisplay)
					max_height = connector->modes[m].vdisplay;
				if (min_width > connector->modes[m].hdisplay)
					min_width = connector->modes[m].hdisplay;
				if (max_width < connector->modes[m].hdisplay)
					max_width = connector->modes[m].hdisplay;
				if (connector->modes[m].type & DRM_MODE_TYPE_PREFERRED)
				{
					def_height = connector->modes[m].vdisplay;
					def_width = connector->modes[m].hdisplay;
				}
				json_array_append_new(items, json_string(connector->modes[m].name));
			}
			drmModeFreeConnector(connector);
			break;
		}
		drmModeFreeConnector(connector);
	}

	if (json_is_object(capabilities))
	{
		json_object_set(capabilities, "width", json_integer(def_height));
		json_object_set(capabilities, "height", json_integer(def_width));
		json_object_set(capabilities, "formats", items);
	}
	else if (json_is_array(capabilities))
	{
		if (min_height != max_height)
		{
			json_object_set_new(height, "minimum", json_integer(min_height));
			json_object_set_new(height, "maximum", json_integer(max_height));
			json_object_set_new(height, "default", json_integer(def_height));
		}
		json_object_set_new(height, "value", json_integer(def_height));
		if (min_width != max_width)
		{
			json_object_set_new(width, "minimum", json_integer(min_width));
			json_object_set_new(width, "maximum", json_integer(max_width));
			json_object_set_new(width, "default", json_integer(def_width));
		}
		json_object_set_new(width, "value", json_integer(def_width));
		json_t *formats = NULL;
		formats = json_object();
		json_object_set_new(formats, "name", json_string("formats"));
		json_object_set_new(formats, "type", json_string("menu"));
		json_object_set_new(formats, "items", items);
		json_object_set_new(formats, "value", json_array_get(items, 0));

		json_array_append_new(capabilities, width);
		json_array_append_new(capabilities, height);
		json_array_append_new(capabilities, formats);
	}
	return 0;
}

int sdrm_capabilities(Display_t *disp, json_t *capabilities, int all)
{
	json_t *definition = json_array();
	if (sdrm_capabilities_size(disp, definition))
		return -1;
	if (sdrm_capabilities_fourcc(disp, definition))
		return -1;
	json_t *controls = json_array();
	if (sdrm_capabilities_controls(disp, controls))
		return -1;
	if (json_is_object(capabilities))
	{
		json_object_set_new(capabilities, "definition", definition);
		json_object_set_new(capabilities, "control", controls);
	}
	return 0;
}

static uint32_t sdrm_setrotation(Display_t *disp, json_t *jrotation)
{
	int ret = -1;
	if (jrotation && json_is_integer(jrotation))
	{
		int rotation = json_integer_value(jrotation);
		if (rotation < 45)
			disp->rotation |= DRM_MODE_ROTATE_0;
		else if (rotation < 135)
			disp->rotation |= DRM_MODE_ROTATE_90;
		else if (rotation < 225)
			disp->rotation |= DRM_MODE_ROTATE_180;
		else if (rotation < 315)
			disp->rotation |= DRM_MODE_ROTATE_270;
		else
			disp->rotation = DRM_MODE_ROTATE_0;
		ret = 0;
	}
	if (jrotation && json_is_string(jrotation))
	{
		const char *value = json_string_value(jrotation);
		if (!strcasecmp(value, "90"))
			disp->rotation |= DRM_MODE_ROTATE_90;
		else if (!strcasecmp(value, "180"))
			disp->rotation |= DRM_MODE_ROTATE_180;
		else if (!strcasecmp(value, "270"))
			disp->rotation |= DRM_MODE_ROTATE_270;
		else if (!strcasecmp(value, "reflect"))
			disp->rotation |= DRM_MODE_REFLECT_X;
	}
	return ret;
}

int sdrm_loadjsonsettings(void *arg, void *entry)
{
	json_t *jconfig = entry;
	Display_t *disp = (Display_t *)arg;
	json_t *jrotation = NULL;
	if (json_is_object(jconfig))
		jrotation = json_object_get(jconfig, "rotation");
	if (json_is_array(jconfig))
	{
		int index;
		json_t *control;
		json_array_foreach(jconfig, index, control)
		{
			if (!json_is_object(control))
				break;
			json_t *name = json_object_get(control, "name");
			if (name && json_is_string(name) &&
				! strcmp(json_string_value(name), "rotation"))
				jrotation = json_object_get(control, "value");
			else
				jrotation = json_object_get(control, "rotation");
		}
	}

	if (sdrm_setrotation(disp, jrotation) &&
		jrotation && json_is_array(jrotation))
	{
		int index;
		json_t *jentry;
		json_array_foreach(jrotation, index, jentry)
		{
			sdrm_setrotation(disp, jentry);
		}
	}
	return 0;
}

int sdrm_loadjsonconfiguration(void *arg, void *entry)
{
	json_t *jconfig = entry;

	DisplayConf_t *config = (DisplayConf_t *)arg;
	json_t *device = json_object_get(jconfig, "device");
	if (device && json_is_string(device))
	{
		const char *value = json_string_value(device);
		config->device = value;
	}
	json_t *definition = json_object_get(jconfig, "definition");
	scommon_loaddefinition(&config->parent, definition);
	json_t *transfer = json_object_get(jconfig, "transfer");
	scommon_loaddefinition(&config->transfer, transfer);
	if (config->transfer.width == 0)
		config->transfer.width = config->parent.width;
	if (config->transfer.height == 0)
		config->transfer.height = config->parent.height;
	if (config->transfer.fourcc == 0)
		config->transfer.fourcc = config->parent.fourcc;
	if (config->transfer.modifiers == 0)
		config->transfer.modifiers = config->parent.modifiers;

	return 0;
}
#endif

void sdrm_destroy(Display_t *disp)
{
	drmModeFreeCrtc(disp->crtc);
	for (int j = 0; j < MAX_BUFFERS; j++)
		sdrm_freebuffer(disp, &disp->buffers[j]);
	close(disp->fd);
	if (disp->mode_id)
	{
		drmModeDestroyPropertyBlob(disp->fd, disp->mode_id);
	}
	free(disp);
}

DeviceConf_t * sdrm_createconfig()
{
	DisplayConf_t *devconfig = NULL;
	devconfig = calloc(1, sizeof(DisplayConf_t));
#ifdef HAVE_JANSSON
	devconfig->parent.ops.loadconfiguration = sdrm_loadjsonconfiguration;
#endif
	return (DeviceConf_t *)devconfig;
}

FastVideoDevice_ops_t sdrm_ops = {
	.name = "screen",
	.createconfig = sdrm_createconfig,
	.create = (FastVideoDevice_create_t)sdrm_create,
	.create2 = (FastVideoDevice_create2_t)sdrm_create2,
	.duplicate = (FastVideoDevice_duplicate_t)sdrm_duplicate,
	.loadsettings = (FastVideoDevice_loadsettings_t)sdrm_loadsettings,
	.capabilities = (FastVideoDevice_capabilities_t)sdrm_capabilities,
	.requestbuffer = (FastVideoDevice_requestbuffer_t)sdrm_requestbuffer,
	.eventfd = (FastVideoDevice_eventfd_t)sdrm_fd,
	.start = (FastVideoDevice_start_t)sdrm_start,
	.stop = (FastVideoDevice_stop_t)sdrm_stop,
	.dequeue = (FastVideoDevice_dequeue_t)sdrm_dequeue,
	.queue = (FastVideoDevice_queue_t)sdrm_queue,
	.destroy = (FastVideoDevice_destroy_t)sdrm_destroy,
};

#include <dlfcn.h>

static void __attribute__ ((constructor)) sdrm_init()
{
	fastvideodevice_ops_append_t _fastvideodevice_ops_append;
	void *hdl = dlopen(NULL, RTLD_NOW);
	_fastvideodevice_ops_append = dlsym(hdl, "fastvideodevice_ops_append");
	if (_fastvideodevice_ops_append)
	{
		_fastvideodevice_ops_append(&sdrm_ops);
	}
}
