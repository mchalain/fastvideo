#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <stdarg.h>

#include "fastvideo.h"
#include "config.h"
#include "log.h"
#include "sv4l2_meta.h"
#include "unixsocket.h"

#define MAX_BUFFERS 4

typedef struct MetaDev_s MetaDev_t;
struct MetaDev_s
{
	const char *name;
	device_type_e type;
	V4l2_Meta_Conf_t *config;
	FrameBuffer_t *buffers;
	int nbuffers;
	int currentid;
	const V4l2_Meta_t *meta;
	void *meta_ctx;
	client_t *client;
};

static const char _metadev_name[] = "v4l2_meta";

static int sv4l2_meta_loadjsonconfiguration(void *arg, void *entry);
EXT_API DeviceConf_t * sv4l2_meta_createconfig(void)
{
	DeviceConf_t *devconfig = (void *)(long) -1;
	devconfig = calloc(1, sizeof(DeviceConf_t));
	devconfig->ops.loadconfiguration = sv4l2_meta_loadjsonconfiguration;
	return devconfig;
}

static const V4l2_Meta_t *_metas[5] = {0};
EXT_API void sv4l2_meta_append(V4l2_Meta_t *meta)
{
	int i = 0;
	for (;_metas[i] && i < sizeof(_metas)/sizeof(*_metas); i++);
	if (i < sizeof(_metas)/sizeof(*_metas))
	{
		_metas[i] = meta;
	}
}

EXT_API MetaDev_t *sv4l2_meta_create(const char *devicename, device_type_e type, V4l2_Meta_Conf_t *config)
{
	if (type != device_output)
		return NULL;
	const V4l2_Meta_t *meta = NULL;
	if (config && config->parent.fourcc)
	{
		for (int i = 0; _metas[i] && i < sizeof(_metas)/sizeof(*_metas); i++)
		{
			if (_metas[i]->fourcc == config->parent.fourcc)
			{
				meta = _metas[i];
				break;
			}
		}
	}
	MetaDev_t *dev = calloc(1, sizeof(*dev));
	dev->type = type;
	dev->meta = meta;
	dev->config = config;
	dev->currentid = -1;
	dev->name = _metadev_name;
	if (config)
	{
		dev->name = config->parent.name;
		dev->client = client_create(config->server_path);
	}
	if (meta)
		dev->meta_ctx = meta->ops.create(dev->client, config);

	return dev;
}

EXT_API int sv4l2_meta_requestbuffer(MetaDev_t *dev, enum buf_type_e t, ...)
{
	va_list ap;
	va_start(ap, t);
	switch (t)
	{
		case (buf_type_memory):
		{
			int ntargets = va_arg(ap, int);
			void **targets = va_arg(ap, void **);
			size_t size = va_arg(ap, size_t);
			for (int i = 0; i < ntargets; i++)
			{
				FrameBuffer_t *buffer = calloc(1, sizeof(*buffer));
				if (buffer == NULL)
					break;
				buffer->id = dev->nbuffers;
				buffer->size = size;
				buffer->mem = targets[i];
				buffer->next = dev->buffers;
				dev->buffers = buffer;
				dev->nbuffers ++;
			}
			dev->nbuffers = ntargets;
		}
		break;
		case buf_type_dmabuf:
		{
			/// only need memory
			return -1;
			int ntargets = va_arg(ap, int);
			int *targets = va_arg(ap, int *);
			size_t size = va_arg(ap, size_t);
			for (int i = 0; i < ntargets; i++)
			{
				FrameBuffer_t *buffer = calloc(1, sizeof(*buffer));
				if (buffer == NULL)
					break;
				buffer->id = dev->nbuffers;
				buffer->size = size;
				buffer->dma_buf = targets[i];
				buffer->next = dev->buffers;
				dev->buffers = buffer;
				dev->nbuffers ++;
			}
			dev->nbuffers = ntargets;
		}
		break;
		default:
			va_end(ap);
			return -1;
	}
	va_end(ap);

	return 0;
}

EXT_API int sv4l2_meta_fd(MetaDev_t *dev, int writer)
{
	return -1;
}

EXT_API int sv4l2_meta_queue(MetaDev_t *dev, int id, void *mem, size_t size, int flags)
{
	if (id < 0 || id > dev->nbuffers)
		return -1;
	if (dev->currentid != -1)
	{
		errno = EAGAIN;
		return -1;
	}
	dev->currentid = id;
	FrameBuffer_t *buffer = NULL;
	buffer = &dev->buffers[id];
	buffer->state = queued;
	if (buffer->mem == NULL)
		buffer->mem = mem;
	if (dev->meta)
		dev->meta->ops.queue(dev->meta_ctx, buffer);
	else
	{
		/**
		 * treat the buffer here:
		 *  - push on device
		 *  - tranform the data
		 * The end of treatment must set
		 * the state to ready
		 */
		buffer->state = ready;
	}
	return 0;
}

EXT_API int sv4l2_meta_dequeue(MetaDev_t *dev, void **mem, size_t *bytesused, int *flags)
{
	int id = dev->currentid;
	if (id == -1)
	{
		errno = EAGAIN;
		return -1;
	}
	FrameBuffer_t *buffer = NULL;
	buffer = &dev->buffers[id];
	if (dev->meta && !dev->meta->ops.dequeue(dev->meta_ctx, buffer))
		buffer->state = ready;
	if (buffer->state != ready)
	{
		errno = EAGAIN;
		return -1;
	}
	dev->currentid = -1;

	if (*mem)
		*mem = buffer->mem;
	if (*bytesused)
		*bytesused = buffer->size;
	if (*flags)
		*flags = 0;
	buffer->state = dequeued;
	return id;
}

EXT_API int sv4l2_meta_start(MetaDev_t *dev)
{
	if (dev->type == device_input)
	{
		for (FrameBuffer_t *buffer = dev->buffers; buffer != NULL; buffer = buffer->next)
		{
			sv4l2_meta_queue(dev, buffer->id, buffer->mem, buffer->size, 0);
		}
	}
	return 0;
}

EXT_API int sv4l2_meta_stop(MetaDev_t *dev)
{
	for (FrameBuffer_t *buffer = dev->buffers; buffer != NULL; buffer = buffer->next)
	{
		buffer->state = invalid;
	}
	return 0;
}

EXT_API void sv4l2_meta_destroy(MetaDev_t *dev)
{
	FrameBuffer_t *next = NULL;
	for (FrameBuffer_t *buffer = dev->buffers; buffer != NULL; buffer = next)
	{
		next = buffer->next;
		free(buffer);
	}
	if (dev->meta_ctx)
		dev->meta->ops.destroy(dev->meta_ctx);
	if (dev->client)
		client_destroy(dev->client);
	free(dev->config);
	free(dev);
}

#ifdef HAVE_JANSSON
static int sv4l2_meta_loadjsonconfiguration(void *arg, void *entry)
{
	int ret = 0;
	V4l2_Meta_Conf_t *config = (V4l2_Meta_Conf_t *)arg;
	json_t *jconfig = (json_t *)entry;

	ret = scommon_loadconfiguration(&config->parent, entry);
	json_t *path = json_object_get(jconfig, "server-path");
	if (path && json_is_string(path))
	{
		config->server_path = json_string_value(path);
	}

	return ret;
}

static int sv4l2_meta_capabilities(MetaDev_t *dev, json_t *capabilities, int all)
{
	json_t *names = json_array();
	json_array_append_new(names, json_string(dev->name));
	json_object_set_new(capabilities, "name", names);
	json_object_set_new(capabilities, "type", json_string(sv4l2_meta_ops.name));
	json_object_set_new(capabilities, "server-path", json_string(FASTSETTING_DEFAULT_SERVER));
	json_t *fourcc = NULL;
	if (all || (dev->meta && dev->meta->fourcc != 0) || (_metas[0] && _metas[0]->fourcc != 0))
	{
		fourcc = json_object();
		json_object_set_new(fourcc, "name", json_string("fourcc"));
		if (dev->meta && dev->meta->fourcc != 0)
			json_object_set_new(fourcc, "value", json_stringn((char*)&dev->meta->fourcc, 4));
		else if (_metas[0] && _metas[0]->fourcc != 0)
			json_object_set_new(fourcc, "value", json_stringn((char*)&_metas[0]->fourcc, 4));
		else
			json_object_set_new(fourcc, "value", json_string(""));
	}
	json_t *definition = NULL;
	if (!all)
	{
		if (fourcc)
		{
			definition = json_array();
			json_array_append_new(definition, fourcc);
			json_object_set_new(capabilities, "definition", definition);
		}
		return 0;
	}
	json_t *items = json_array();
	for (int i = 0; _metas[i] && i < sizeof(_metas)/sizeof(*_metas); i++)
	{
		json_array_append_new(items, json_stringn((char *)&_metas[i]->fourcc, 4));
	}
	json_array_append_new(fourcc, items);
	definition = json_array();
	json_array_append_new(definition, fourcc);
	json_object_set_new(capabilities, "definition", definition);

	return 0;
}
#else
#define sv4l2_meta_loadjsonconfiguration NULL
#endif

FastVideoDevice_ops_t sv4l2_meta_ops = {
	.name = _metadev_name,
	.createconfig = sv4l2_meta_createconfig,
	.create = (FastVideoDevice_create_t)sv4l2_meta_create,
	.duplicate = (FastVideoDevice_duplicate_t)NULL,
	.loadsettings = (FastVideoDevice_loadsettings_t)NULL,
	.capabilities = (FastVideoDevice_capabilities_t)sv4l2_meta_capabilities,
	.requestbuffer = (FastVideoDevice_requestbuffer_t)sv4l2_meta_requestbuffer,
	.eventfd = (FastVideoDevice_eventfd_t)sv4l2_meta_fd,
	.start = (FastVideoDevice_start_t)sv4l2_meta_start,
	.stop = (FastVideoDevice_stop_t)sv4l2_meta_stop,
	.dequeue = (FastVideoDevice_dequeue_t)sv4l2_meta_dequeue,
	.queue = (FastVideoDevice_queue_t)sv4l2_meta_queue,
	.destroy = (FastVideoDevice_destroy_t)sv4l2_meta_destroy,
};

#include <dlfcn.h>

static void __attribute__ ((constructor)) ssv4l2_meta_init()
{
	fastvideodevice_ops_append_t _fastvideodevice_ops_append;
	void *hdl = dlopen(NULL, RTLD_NOW);
	_fastvideodevice_ops_append = dlsym(hdl, "fastvideodevice_ops_append");
	if (_fastvideodevice_ops_append)
	{
		_fastvideodevice_ops_append(&sv4l2_meta_ops);
	}
}
