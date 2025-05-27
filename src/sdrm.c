#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <limits.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm.h>
#include <drm_fourcc.h>
#ifdef HAVE_LIBKMS
#include <kms.h>
#endif

#include "log.h"
#include "sdrm.h"

#define MAX_BUFFERS 4

typedef struct DisplayBuffer_s DisplayBuffer_t;
struct DisplayBuffer_s
{
#ifdef HAVE_LIBKMS
	struct kms_bo *bo;
#endif
	int bo_handle;
	int dma_fd;
	uint32_t fb_id;
	uint32_t *memory;
	uint32_t pitch;
	uint32_t size;
	uint8_t queued :1;
};

typedef struct Display_s Display_t;
struct Display_s
{
#ifdef HAVE_LIBKMS
	struct kms_driver *kms;
#endif
	uint32_t connector_id;
	uint32_t encoder_id;
	uint32_t crtc_id;
	uint32_t plane_id;
	drmModeCrtc *crtc;
	uint32_t fourcc;
	int type;
	int fd;
	drmModeModeInfo mode;
	DisplayBuffer_t buffers[MAX_BUFFERS];
	int nbuffers;
	int buf_id;
	int queueid;
};

static int sdrm_ids(Display_t *disp, uint32_t *conn_id, uint32_t *enc_id, uint32_t *crtc_id, drmModeModeInfo *mode)
{
	drmModeResPtr resources;
	resources = drmModeGetResources(disp->fd);

	int32_t connector_id = -1;
	int32_t encoder_id = -1;
	for(int i = 0; i < resources->count_connectors; ++i)
	{
		connector_id = resources->connectors[i];
		drmModeConnectorPtr connector = drmModeGetConnector(disp->fd, connector_id);
		if (connector->connection == DRM_MODE_CONNECTED && connector->count_modes > 0)
		{
			drmModeModeInfo *preferred = NULL;
			if (mode->hdisplay && mode->vdisplay)
				preferred = mode;
			for (int m = 0; m < connector->count_modes; m++)
			{
				dbg("sdrm: mode: %s %dx%d %s",
						connector->modes[m].name,
						connector->modes[m].hdisplay,
						connector->modes[m].vdisplay,
						connector->modes[m].type & DRM_MODE_TYPE_PREFERRED ? "*" : "");
				if (!preferred && connector->modes[m].type & DRM_MODE_TYPE_PREFERRED)
				{
					preferred = &connector->modes[m];
				}
				if (preferred && connector->modes[m].hdisplay == preferred->hdisplay &&
								connector->modes[m].vdisplay == preferred->vdisplay)
				{
					preferred = &connector->modes[m];
				}
			}
			if (preferred == NULL || preferred == mode)
				preferred = &connector->modes[0];
			memcpy(mode, preferred, sizeof(*mode));
			encoder_id = connector->encoder_id;
			drmModeFreeConnector(connector);
			break;
		}
		drmModeFreeConnector(connector);
	}

	if (connector_id == -1 || encoder_id == -1)
	{
		drmModeFreeResources(resources);
		return -1;
	}
	*conn_id = connector_id;
	*enc_id = encoder_id;

	for(int i=0; i < resources->count_encoders; ++i)
	{
		drmModeEncoderPtr encoder;
		encoder = drmModeGetEncoder(disp->fd, resources->encoders[i]);
		if(encoder != NULL)
		{
			dbg("sdrm: encoder %d found", encoder->encoder_id);
			if(encoder->encoder_id == encoder_id)
			{
				*crtc_id = encoder->crtc_id;
				drmModeFreeEncoder(encoder);
				break;
			}
			drmModeFreeEncoder(encoder);
		}
		else
			err("sdrm: get a null encoder pointer");
	}

	int crtcindex = -1;
	for(int i=0; i < resources->count_crtcs; ++i)
	{
		if (resources->crtcs[i] == *crtc_id)
		{
			crtcindex = i;
			break;
		}
	}
	if (crtcindex == -1)
	{
		drmModeFreeResources(resources);
		err("sdrm: crtc not available");
		return -1;
	}
	dbg("sdrm: screen size %ux%u", disp->mode.hdisplay, disp->mode.vdisplay);
	drmModeFreeResources(resources);
	return 0;
}

static uint64_t sdrm_properties(Display_t *disp, uint32_t plane_id, const char *property)
{
	uint64_t ret = 0;
#if 1
	drmModeObjectPropertiesPtr props;

	props = drmModeObjectGetProperties(disp->fd, plane_id, DRM_MODE_OBJECT_PLANE);
	for (int i = 0; i < props->count_props; i++)
	{
		drmModePropertyPtr prop;

		prop = drmModeGetProperty(disp->fd, props->props[i]);
		dbg("sdrm: property %s", prop->name);
		if (prop && !strcmp(prop->name, property))
		{
			// TODO: check if property must be freed
			ret = props->prop_values[i];
			break;
		}
		if (prop)
			drmModeFreeProperty(prop);
	}
	drmModeFreeObjectProperties(props);
#else
	struct drm_mode_obj_get_properties counter = {
		.obj_id = plane_id,
		.obj_type = DRM_MODE_OBJECT_PLANE,
	};
	if (drmIoctl(disp->fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &counter) == -1)
	{
		err("sdrm: Properties count error %m");
		return -1;
	}
    size_t count = counter.count_props;
    uint32_t *ids = calloc(count, sizeof (*ids));
    uint64_t *values = calloc(count, sizeof (*values));
	struct drm_mode_obj_get_properties props = {
        .props_ptr = (uintptr_t)(void *)ids,
        .prop_values_ptr = (uintptr_t)(void *)values,
        .count_props = count,
        .obj_id = plane_id,
        .obj_type = DRM_MODE_OBJECT_PLANE,
    };
	if (drmIoctl(disp->fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &props) == -1)
	{
		err("sdrm: Properties get error %m");
		return -1;
	}
    for (size_t i = 0; i < props.count_props; i++)
    {
		struct drm_mode_get_property prop = {
			.prop_id = ids[i],
		};
		if (drmIoctl(disp->fd, DRM_IOCTL_MODE_GETPROPERTY, &prop) == -1)
		{
			err("sdrm: Property %#x get error %m", ids[i]);
			continue;
		}
		if (!strcmp(property, prop.name))
		{
			ret = values[i];
			break;
		}
	}
#endif
	return ret;
}

static int sdrm_plane(Display_t *disp, uint32_t *plane_id)
{
	int ret = -1;
	drmModePlaneResPtr planes;

	planes = drmModeGetPlaneResources(disp->fd);

	drmModePlanePtr plane;
	for (int i = 0; i < planes->count_planes; ++i)
	{
		plane = drmModeGetPlane(disp->fd, planes->planes[i]);
		int type = (int)sdrm_properties(disp, plane->plane_id, "type");
		if (type == disp->type)
		{
			for (int j = 0; j < plane->count_formats; ++j)
			{
				uint32_t fourcc = plane->formats[j];
				dbg("sdrm: Plane[%d] %u: 4cc %.4s", i, plane->plane_id, (char *)&fourcc);
				if (plane->formats[j] == disp->fourcc)
				{
					ret = 0;
				}
			}
			*plane_id = plane->plane_id;
		}
		drmModeFreePlane(plane);
		if (ret == 0)
			break;
	}
	drmModeFreePlaneResources(planes);
	if (ret == -1)
	{
		err("sdrm: plane with 4cc %.4s not found", &disp->fourcc);
	}
	return ret;
}

static int sdrm_buffer_generic(Display_t *disp, uint32_t width, uint32_t height, uint32_t fourcc, DisplayBuffer_t *buffer)
{
#ifdef HAVE_LIBKMS
	unsigned attr[] = {
		KMS_BO_TYPE, KMS_BO_TYPE_SCANOUT_X8R8G8B8,
		KMS_WIDTH, width,
		KMS_HEIGHT, height,
		KMS_TERMINATE_PROP_LIST
	};

	if (kms_bo_create(disp->kms, attr, &buffer->bo))
	{
		err("sdrm: kms bo error");
		return -1;
	}
	if (kms_bo_get_prop(buffer->bo, KMS_HANDLE, &buffer->bo_handle))
	{
		err("sdrm: kms bo handle error");
		return -1;
	}
	if (kms_bo_get_prop(buffer->bo, KMS_PITCH, &buffer->pitch))
	{
		err("sdrm: kms bo pitch error");
		return -1;
	}
#else
	dbg("sdrm: buffer for width %u height %u ", width, height);
	struct drm_mode_create_dumb gem = {
		.width = width,
		.height = height,
		.bpp = 32,
	};
	if (drmIoctl(disp->fd, DRM_IOCTL_MODE_CREATE_DUMB, &gem) == -1)
	{
		err("sdrm: dumb allocation error %m");
		return -1;
	}

	buffer->bo_handle = gem.handle;
	buffer->pitch = gem.pitch;
	buffer->size = gem.size;
#endif

	return 0;
}

static int sdrm_buffer_mmap(Display_t *disp, uint32_t width, uint32_t height, uint32_t fourcc, DisplayBuffer_t *buffer)
{
	sdrm_buffer_generic(disp, width, height, fourcc, buffer);
#ifdef HAVE_LIBKMS
	if (kms_bo_map(buffer->bo, &buffer->memory))
	{
		err("sdrm: kms bo map error");
		return -1;
	}
#else
	struct drm_mode_map_dumb map = {
		.handle = buffer->bo_handle,
	};
	if (drmIoctl(disp->fd, DRM_IOCTL_MODE_MAP_DUMB, &map) == -1)
	{
		err("sdrm: dumb map error %m");
		return -1;
	}
	buffer->memory = (uint32_t *)mmap(NULL, buffer->size, PROT_READ | PROT_WRITE, MAP_SHARED,
		disp->fd, map.offset);
#endif

	if (drmModeAddFB(disp->fd, width, height, 24, 32, buffer->pitch,
		buffer->bo_handle, &buffer->fb_id))
	{
		err("sdrm: Frame buffer unavailable %m");
		return -1;
	}
	return 0;
}

static int sdrm_buffer_dma(Display_t *disp, uint32_t width, uint32_t height, uint32_t fourcc, DisplayBuffer_t *buffer)
{

	sdrm_buffer_generic(disp, width, height, fourcc, buffer);

	uint32_t offsets[4] = { 0 };
	uint32_t pitches[4] = { buffer->pitch };
	uint32_t bo_handles[4] = { buffer->bo_handle };

#if 0
	struct drm_prime_handle prime = {0};
	prime.handle = buffer->bo_handle;

	if (ioctl(disp->fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime))
	{
		err("sdrm: dmabuf not allowed %m");
	}
	buffer->dma_fd = prime.fd;
#else
	if (drmPrimeHandleToFD(disp->fd, buffer->bo_handle, DRM_CLOEXEC, &buffer->dma_fd))
	{
		err("sdrm: dmabuf not allowed %m");
	}
#endif

	if (drmModeAddFB2(disp->fd, width, height, disp->fourcc, bo_handles,
		pitches, offsets, &buffer->fb_id, 0))
	{
		err("sdrm: Frame buffer unavailable %m");
		return -1;
	}
	return 0;
}

static int sdrm_buffer_setdma(Display_t *disp, uint32_t size, int fd, DisplayBuffer_t *buffer)
{
	buffer->size = size;
	buffer->pitch = size / disp->mode.vdisplay;

	uint32_t offsets[4] = { 0 };
	uint32_t pitches[4] = { buffer->pitch };

	uint32_t handle;
	if (drmPrimeFDToHandle(disp->fd, fd, &handle))
	{
		err("sdrm: buffer %d association error", fd);
		return -1;
	}
	buffer->bo_handle = handle;
	uint32_t bo_handles[4] = { buffer->bo_handle };

	if (drmModeAddFB2(disp->fd, disp->mode.hdisplay, disp->mode.vdisplay, disp->fourcc,
		bo_handles, pitches, offsets, &buffer->fb_id, 0))
	{
		err("sdrm: Frame buffer unavailable %m");
		return -1;
	}
	return 0;
}

static void sdrm_freebuffer(Display_t *disp, DisplayBuffer_t *buffer)
{
	drmModeRmFB(disp->fd, buffer->fb_id);
#ifdef HAVE_LIBKMS
	kms_bo_unmap(buffer->bo);
	kms_bo_destroy(buffer->bo);
#else
	struct drm_mode_destroy_dumb dumb = {
		.handle = buffer->bo_handle,
	};
	munmap(buffer->memory, buffer->size);
	drmIoctl(disp->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dumb);
#endif
}

Display_t *sdrm_create2(int fd, const char *name, device_type_e type, DisplayConf_t *config)
{
	if (type != device_output)
	{
		err("sdrm: %s bad device type", config->parent.name);
		return NULL;
	}
	if (drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1))
	{
		err("sdrm: Universal plane not supported %m");
		return NULL;
	}

	Display_t *disp = calloc(1, sizeof(*disp));
	disp->fd = fd;
	disp->fourcc = FOURCC('A','R','2','4');
	disp->type = DRM_PLANE_TYPE_PRIMARY;

	if (config)
	{
		disp->mode.hdisplay = config->parent.width;
		disp->mode.vdisplay = config->parent.height;
		if (config->parent.fourcc)
			disp->fourcc = config->parent.fourcc;
	}
	if (sdrm_ids(disp, &disp->connector_id, &disp->encoder_id, &disp->crtc_id, &disp->mode) == -1)
	{
		free(disp);
		return NULL;
	}
	if (sdrm_plane(disp, &disp->plane_id) == -1)
	{
		free(disp);
		return NULL;
	}
#ifdef HAVE_LIBKMS
	if (kms_create(fd, &disp->kms))
		err("sdrm: kms create error");
#endif

	if (config)
	{
		config->parent.dev = disp;
	}
	return disp;
}

Display_t *sdrm_create(const char *name, device_type_e type, DisplayConf_t *config)
{
	if (type != device_output)
	{
		err("sdrm: %s bad device type", config->parent.name);
		return NULL;
	}
	int fd = 0;
	if (!access(config->device, R_OK | W_OK))
		fd = open(config->device, O_RDWR);
	else
		fd = drmOpen(config->device, NULL);
	if (fd < 0)
	{
		err("device %s (%s) bad argument %m", name, config->device);
		return NULL;
	}
	Display_t *disp = sdrm_create2(fd, name, type, config);
	if (disp == NULL)
		close(fd);
	return disp;
}

int sdrm_requestbuffer(Display_t *disp, enum buf_type_e t, ...)
{
	va_list ap;
	va_start(ap, t);
	switch (t)
	{
		case (buf_type_memory | buf_type_master):
		{
			int *ntargets = va_arg(ap, int *);
			void **targets = va_arg(ap, void **);
			size_t *psize = va_arg(ap, size_t *);
			if (targets != NULL)
			{
				*targets = calloc(disp->nbuffers, sizeof(void*));
				for (int i = 0; i < MAX_BUFFERS; i++, disp->nbuffers ++)
				{
					if (sdrm_buffer_mmap(disp,  disp->mode.hdisplay, disp->mode.vdisplay,
						disp->fourcc, &disp->buffers[i]) == -1)
					{
						err("sdrm: buffer %d allocation error", i);
						break;
					}
					targets[i] = disp->buffers[i].memory;
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
				if (sdrm_buffer_setdma(disp, size, targets[i], &disp->buffers[i]))
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
			dbg("request buf_type_dmabuf");
			int *ntargets = va_arg(ap, int *);
			int **targets = va_arg(ap, int **);
			size_t *psize = va_arg(ap, size_t *);
			if (targets != NULL)
			{
				*targets = calloc(disp->nbuffers, sizeof(int));
				for (int i = 0; i < MAX_BUFFERS; i++, disp->nbuffers ++)
				{
					if (sdrm_buffer_dma(disp,  disp->mode.hdisplay, disp->mode.vdisplay,
						disp->fourcc, &disp->buffers[i]) == -1)
					{
						err("sdrm: buffer %d allocation error", i);
						break;
					}
					(*targets)[i] = disp->buffers[i].dma_fd;
				}
			}
			if (ntargets != NULL)
				*ntargets = disp->nbuffers;
			if (psize != NULL)
				*psize = disp->buffers[0].size;
		}
		break;
		default:
			va_end(ap);
			return -1;
	}
	va_end(ap);

	disp->crtc = drmModeGetCrtc(disp->fd, disp->crtc_id);
	if (drmModeSetCrtc(disp->fd, disp->crtc_id, disp->buffers[0].fb_id, 0, 0, &disp->connector_id, 1, &disp->mode))
	{
		err("srdm: Crtc setting error %m");
		for (int j = 0; j < MAX_BUFFERS; j++)
			sdrm_freebuffer(disp, &disp->buffers[j]);
		free(disp);
		return -1;
	}
	drmModePageFlip(disp->fd, disp->crtc_id, disp->buffers[0].fb_id, DRM_MODE_PAGE_FLIP_EVENT, disp);
	return 0;
}

static void page_flip_handler(int fd, unsigned int frame,
		  unsigned int sec, unsigned int usec, void *data)
{
	Display_t *disp = data;
	int id = disp->queueid;
	disp->buffers[(int)id].queued = 0;
}

int sdrm_queue(Display_t *disp, int id, void *mem, size_t bytesused)
{
	if (disp->buffers[id].queued)
		return -1;
	drmModePageFlip(disp->fd, disp->crtc_id, disp->buffers[(int)id].fb_id, DRM_MODE_PAGE_FLIP_EVENT, disp);
	disp->buffers[id].queued = 1;
	return 0;
}

int sdrm_dequeue(Display_t *disp, void **mem, size_t *bytesused)
{
	drmEventContext evctx = {
				.version = DRM_EVENT_CONTEXT_VERSION,
				.page_flip_handler = page_flip_handler,
	};
	drmHandleEvent(disp->fd, &evctx);
	int id = disp->queueid;
	if (disp->buffers[id].queued)
		return -1;
	disp->queueid = (disp->queueid + 1) % disp->nbuffers;
	if (mem)
		*mem = disp->buffers[id].memory;
	if (bytesused)
		*bytesused = disp->buffers[id].size;
	return id;
}

int sdrm_fd(Display_t *disp)
{
	return disp->fd;
}

int sdrm_start(Display_t *disp)
{
	return 0;
}

#ifdef HAVE_JANSSON
#include <jansson.h>

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
		json_object_set_new(pixelformat, "name", json_string("pixelformat"));
		json_object_set_new(pixelformat, "type", json_string("menu"));
	}

	json_t *items = json_array();
	uint32_t format = 0;
	drmModePlanePtr plane;
	for (int i = 0; i < 1 /*planes->count_planes*/; ++i)
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

int sdrm_capabilities(Display_t *disp, json_t *capabilities)
{
	json_t *definition = json_array();
	if (sdrm_capabilities_size(disp, definition))
		return -1;
	if (sdrm_capabilities_fourcc(disp, definition))
		return -1;
	if (json_is_object(capabilities))
	{
		json_object_set_new(capabilities, "definition", definition);
	}
	return 0;
}

int sdrm_loadjsonsettings(void *arg, void *entry)
{
	json_t *jconfig = entry;
	Display_t *disp = (Display_t *)arg;
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
	return 0;
}
#endif

int sdrm_stop(Display_t *disp)
{
	return 0;
}

void sdrm_destroy(Display_t *disp)
{
	drmModeFreeCrtc(disp->crtc);
	for (int j = 0; j < MAX_BUFFERS; j++)
		sdrm_freebuffer(disp, &disp->buffers[j]);
	close(disp->fd);
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
	.duplicate = (FastVideoDevice_duplicate_t)NULL,
	.loadsettings = (FastVideoDevice_loadsettings_t)sdrm_loadsettings,
	.requestbuffer = (FastVideoDevice_requestbuffer_t)sdrm_requestbuffer,
	.eventfd = (FastVideoDevice_eventfd_t)NULL,
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
