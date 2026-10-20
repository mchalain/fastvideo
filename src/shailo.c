#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>
#include <dlfcn.h>
#include <unistd.h>
#include <sys/eventfd.h>

#include <jansson.h>

#include <hailo/hailort.h>

#include "fastvideo.h"
#include "sconfig.h"
#include "log.h"
#include "sdmabuf.h"

#define shailo_dbg(...)

static const char shailo[] = "shailo";

#define FOURCC_NMS   FOURCC('N','M','S',' ')

#define MAX_BUFFERS 12
#define MAX_DEVICES 4
#define SHAILO_VSTREAM_TIMEOUT_MS 50

typedef struct Hailo_s Hailo_t;

typedef struct HailoConfig_s HailoConfig_t;
struct HailoConfig_s
{
	DeviceConf_t parent;
	const char *hef;
	const char *device;
	int nbuffers;
};

struct Hailo_s
{
	device_type_e type;
	HailoConfig_t *config;
	Hailo_t *dup;

	hailo_vdevice vdevice;
	hailo_hef hef;
	hailo_configured_network_group network_group;
	hailo_activated_network_group activated_ng;
	union {
		hailo_input_stream input_stream;
		hailo_output_vstream output_vstream;
	};
	hailo_stream_info_t stream_info;
	hailo_vstream_info_t vstream_infos;

	volatile hailo_status async_status;
	int completion_fd;

	int nbuffers;
	FrameBuffer_t *buffers;
	FrameBuffer_t *curbuffer;
	volatile int pending;
	void **mems;
	int input_allocated;
};

EXT_API int shailo_loadjsonconfiguration(void *arg, void *entry);

EXT_API DeviceConf_t *shailo_createconfig(const char *name)
{
	HailoConfig_t *config = calloc(1, sizeof(*config));
	config->parent.name = shailo;
	config->parent.ops.loadconfiguration = shailo_loadjsonconfiguration;
	config->nbuffers = MAX_BUFFERS;
	const char *nbuffers = strstr(name, "nbuffers=");
	if (nbuffers)
		config->nbuffers = strtol(nbuffers + 9, NULL, 10);
	return &config->parent;
}

EXT_API Hailo_t *shailo_create(const char *devicename, device_type_e type, HailoConfig_t *config)
{
	if (type != device_transfer)
	{
		err("shailo: %s bad device type", config ? config->parent.name : "");
		return NULL;
	}
	if (!config->hef)
	{
		err("shailo: %s no hef file configured", config->parent.name);
		return NULL;
	}

	Hailo_t *dev = calloc(1, sizeof(*dev));
	dev->type = type;
	dev->config = config;
	dev->completion_fd = eventfd(0, EFD_NONBLOCK);
	if (dev->completion_fd < 0)
		warn("shailo: %s eventfd creation failed %m, falling back to unconditional polling", config->parent.name);

	hailo_status status;

	hailo_vdevice_params_t vdevice_params = {0};
	status = hailo_init_vdevice_params(&vdevice_params);
	if (status != HAILO_SUCCESS)
	{
		err("shailo: vdevice params init error %d", status);
		goto error;
	}
	vdevice_params.scheduling_algorithm = HAILO_SCHEDULING_ALGORITHM_NONE;

	hailo_device_id_t device_ids[MAX_DEVICES];
	size_t actual_count = MAX_DEVICES;
	status = hailo_scan_devices(NULL, device_ids, &actual_count);
	for (int i = 0; i < actual_count; i++)
	{
		shailo_dbg("shailo: device %.*s", HAILO_MAX_DEVICE_ID_LENGTH, device_ids[i]);
		if (config->device && !strncmp(config->device, device_ids[i].id, HAILO_MAX_DEVICE_ID_LENGTH))
		{
			dbg("shailo: device %s found", config->device);
			vdevice_params.device_count = 1;
			vdevice_params.device_ids = &device_ids[i];
			break;
		}
	}
	status = hailo_create_vdevice(&vdevice_params, &dev->vdevice);
	if (status != HAILO_SUCCESS)
	{
		err("shailo: vdevice creation error %d", status);
		goto error;
	}

	status = hailo_create_hef_file(&dev->hef, config->hef);
	if (status != HAILO_SUCCESS)
	{
		err("shailo: hef %s loading error %d", config->hef, status);
		goto error_vdevice;
	}

	hailo_configure_params_t configure_params = {0};
	status = hailo_init_configure_params_by_vdevice(dev->hef, dev->vdevice, &configure_params);
	if (status != HAILO_SUCCESS)
	{
		err("shailo: configure params init error %d", status);
		goto error_hef;
	}
	for (size_t g = 0; g < configure_params.network_group_params_count; g++)
	{
		hailo_configure_network_group_params_t *ng_params = &configure_params.network_group_params[g];
		for (size_t s = 0; s < ng_params->stream_params_by_name_count; s++)
		{
			hailo_stream_parameters_t *sparams = &ng_params->stream_params_by_name[s].stream_params;
			if (sparams->direction == HAILO_H2D_STREAM)
				sparams->flags = HAILO_STREAM_FLAGS_ASYNC;
		}
	}

	size_t number_of_network_groups = 1;
	status = hailo_configure_vdevice(dev->vdevice, dev->hef, &configure_params,
		&dev->network_group, &number_of_network_groups);
	if (status != HAILO_SUCCESS || number_of_network_groups != 1)
	{
		err("shailo: %s network group configuration error %d (%zu groups found, 1 expected)",
			config->hef, status, number_of_network_groups);
		goto error_hef;
	}

	hailo_activate_network_group_params_t activate_params = {0};
	status = hailo_activate_network_group(dev->network_group, &activate_params, &dev->activated_ng);
	if (status != HAILO_SUCCESS)
	{
		err("shailo: network group activation error %d", status);
		goto error_hef;
	}

	hailo_stream_info_t input_info;
	size_t number_of_streams = 1;
	status = hailo_network_group_get_input_stream_infos(dev->network_group, &input_info, 1, &number_of_streams);
	if (status != HAILO_SUCCESS || number_of_streams != 1)
	{
		err("shailo: %zu input stream(s) found, only one supported", number_of_streams);
		goto error_activated;
	}
	status = hailo_get_input_stream(dev->network_group, input_info.name, &dev->input_stream);
	if (status != HAILO_SUCCESS)
	{
		err("shailo: input stream retrieval error %d", status);
		goto error_activated;
	}
	status = hailo_get_input_stream_info(dev->input_stream, &dev->stream_info);
	if (status != HAILO_SUCCESS)
	{
		err("shailo: input stream info error %d", status);
		goto error_activated;
	}
	shailo_dbg("shailo: stream %s %s", dev->stream_info.name, (HAILO_H2D_STREAM == dev->stream_info.direction)?"host -> Dev":"Dev - > Host");

	warn("shailo: device %s ready with hef %s", config->parent.name, config->hef);
	return dev;

error_activated:
	hailo_deactivate_network_group(dev->activated_ng);
error_hef:
	hailo_release_hef(dev->hef);
error_vdevice:
	hailo_release_vdevice(dev->vdevice);
error:
	if (dev->completion_fd >= 0)
		close(dev->completion_fd);
	free(dev);
	return NULL;
}

EXT_API Hailo_t *shailo_duplicate(Hailo_t *dev, HailoConfig_t **pconfig)
{
	if (dev->type != device_transfer)
	{
		err("shailo: device may not be duplicated");
		return NULL;
	}
	dev->type = device_output;

	Hailo_t *dup = calloc(1, sizeof(*dup));
	dup->type = device_input;
	dup->network_group = dev->network_group;
	dup->dup = dev;
	dev->dup = dup;

	HailoConfig_t *config = calloc(1, sizeof(*config));
	memcpy(config, dev->config, sizeof(*config));
	/* the dup device carries a stream of JSON records, not an image */
	config->parent.fourcc = FOURCC_NMS;
	config->parent.width = 0;
	config->parent.height = 0;
	dup->config = config;
	*pconfig = config;

	hailo_output_vstream_params_by_name_t output_params[1];
	size_t output_params_count = 1;
	hailo_status status = hailo_make_output_vstream_params(dup->network_group, false, HAILO_FORMAT_TYPE_AUTO,
		output_params, &output_params_count);
	if (status != HAILO_SUCCESS || output_params_count != 1)
	{
		err("shailo: %zu output vstream(s) found, only one supported", output_params_count);
		goto error_dup;
	}
	output_params[0].params.timeout_ms = SHAILO_VSTREAM_TIMEOUT_MS;
	status = hailo_create_output_vstreams(dup->network_group, output_params, 1, &dup->output_vstream);
	if (status != HAILO_SUCCESS)
	{
		err("shailo: output vstream creation error %d", status);
		goto error_dup;
	}

	status = hailo_get_output_vstream_info(dup->output_vstream, &dup->vstream_infos);
	if (status != HAILO_SUCCESS)
	{
		err("shailo: input vstream creation error %d", status);
		goto error_output_vstream;
	}
	shailo_dbg("shailo: stream %s %s", dup->vstream_infos.name, (HAILO_H2D_STREAM == dup->vstream_infos.direction)?"host -> Dev":"Dev - > Host");
	return dup;
error_output_vstream:
	hailo_release_output_vstreams(&dup->output_vstream, 1);
error_dup:
	free(dup);
	return NULL;
}

static void _hailo_linkring(FrameBuffer_t *buffers, int n)
{
	for (int i = 0; i < n; i++)
		buffers[i].next = &buffers[(i + 1) % n];
}

static void _hailo_createoutputbuffers(Hailo_t *dev, int nmems, size_t framesize)
{
	shailo_dbg("shailo: create %d buffers of %zu", nmems, framesize);
	dev->nbuffers = nmems;
	dev->buffers = calloc(nmems, sizeof(*dev->buffers));
	dev->mems = calloc(nmems, sizeof(*dev->mems));
	for (int i = 0; i < nmems; i++)
	{
		dev->buffers[i].id = i;
		dev->buffers[i].size = framesize;
		dev->buffers[i].mem = malloc(framesize);
		dev->buffers[i].private = dev;
		dev->mems[i] = dev->buffers[i].mem;
	}
	_hailo_linkring(dev->buffers, dev->nbuffers);
}

EXT_API int shailo_requestbuffer(Hailo_t *dev, enum buf_type_e t, ...)
{
	hailo_status hstatus;
	int ret = -1;
	va_list ap;
	va_start(ap, t);
	switch (t)
	{
		case buf_type_memory:
		{
			size_t frame_size = hailo_get_input_stream_frame_size(dev->input_stream);
			shailo_dbg("shailo: frames[%s] format[%d %d %x]", dev->stream_info.name, dev->stream_info.format.type, dev->stream_info.format.order, dev->stream_info.format.flags);
			int ntargets = va_arg(ap, int);
			void **targets = va_arg(ap, void **);
			size_t size = va_arg(ap, size_t);
			if (frame_size != size)
				err("shailo: framesize mismatch %zu != %zu", frame_size, size);
			dev->buffers = calloc(ntargets, sizeof(*dev->buffers));
			for (int i = 0; i < ntargets; i++)
			{
				dev->buffers[i].id = i;
				dev->buffers[i].mem = targets[i];
				dev->buffers[i].size = size;
				dev->buffers[i].private = dev;
				dev->nbuffers = i + 1;
			}
			_hailo_linkring(dev->buffers, dev->nbuffers);
			ret = 0;
		}
		break;
		case buf_type_dmabuf:
		{
			size_t frame_size = hailo_get_input_stream_frame_size(dev->input_stream);
			shailo_dbg("shailo: frames[%s] format[%d %d %x]", dev->stream_info.name, dev->stream_info.format.type, dev->stream_info.format.order, dev->stream_info.format.flags);
			int ntargets = va_arg(ap, int);
			int *targets = va_arg(ap, int *);
			size_t size = va_arg(ap, size_t);
			if (frame_size != size)
				err("shailo: framesize mismatch %zu != %zu", frame_size, size);
			dev->buffers = calloc(ntargets, sizeof(*dev->buffers));
			ret = 0;
			for (int i = 0; i < ntargets; i++)
			{
				dev->buffers[i].id = i;
				dev->buffers[i].dma_buf = targets[i];
				dev->buffers[i].size = size;
				dev->buffers[i].mem = sdmabuf_map(targets[i], size, 0);
				dev->buffers[i].private = dev;
				if (dev->buffers[i].mem == (void *)(long)-1)
				{
					err("shailo: impossible to map input buffer %d", targets[i]);
					ret = -1;
					break;
				}
				dev->nbuffers = i + 1;
			}
			_hailo_linkring(dev->buffers, dev->nbuffers);
		}
		break;
		case buf_type_memory_master:
		{
			if (dev->type == device_output)
			{
				size_t frame_size = hailo_get_input_stream_frame_size(dev->input_stream);
				dev->nbuffers = dev->config->nbuffers;
				dev->buffers = calloc(dev->nbuffers, sizeof(*dev->buffers));
				dev->mems = calloc(dev->nbuffers, sizeof(*dev->mems));
				int i;
				for (i = 0; i < dev->nbuffers; i++)
				{
					void *buf = NULL;
#ifdef HAILO_DMABUF_ALLOC
					int fd = sdmabuf_create(dev->config->parent.name, frame_size);
					if (fd <= 0)
					{
						err("shailo: %s input dma_buf allocation failed", dev->config->parent.name);
						break;
					}
					buf = sdmabuf_map(fd, frame_size, 1);
					if (buf == (void *)(long)-1)
					{
						err("shailo: %s input dma_buf mapping failed", dev->config->parent.name);
						sdmabuf_destroy(fd);
						break;
					}
					dev->buffers[i].dma_buf = fd;
#else
					hailo_buffer_parameters_t alloc_params = { .flags = HAILO_BUFFER_FLAGS_DMA };
					hstatus = hailo_allocate_buffer(frame_size, &alloc_params, &buf);
					if (hstatus != HAILO_SUCCESS || !buf)
					{
						err("shailo: %s input buffer allocation error %d", dev->config->parent.name, hstatus);
						break;
					}
#endif
					dev->buffers[i].id = i;
					dev->buffers[i].mem = buf;
					dev->buffers[i].size = frame_size;
					dev->buffers[i].private = dev;
					dev->mems[i] = buf;
				}
				if (i == 0)
					break;
				dev->nbuffers = i;
				_hailo_linkring(dev->buffers, dev->nbuffers);
				dev->input_allocated = 1;
				int *ntargets = va_arg(ap, int *);
				void ***targets = va_arg(ap, void ***);
				size_t *size = va_arg(ap, size_t *);
				if (ntargets != NULL)
					*ntargets = dev->nbuffers;
				if (targets != NULL)
					*targets = dev->mems;
				if (size != NULL)
					*size = frame_size;
				ret = 0;
				break;
			}
			size_t frame_size = 0;
			hstatus = hailo_get_vstream_frame_size(&(dev->vstream_infos), &(dev->vstream_infos.format), &frame_size);
			if (hstatus != HAILO_SUCCESS)
				err("shailo: error on frame size");
			shailo_dbg("shailo: frames[%s] format[%d %d %x]", dev->vstream_infos.name, dev->vstream_infos.format.type, dev->vstream_infos.format.order, dev->vstream_infos.format.flags);
			_hailo_createoutputbuffers(dev, dev->config->nbuffers, frame_size);
			if (!dev->buffers || !dev->mems)
				break;
			int *ntargets = va_arg(ap, int *);
			void ***targets = va_arg(ap, void ***);
			size_t *size = va_arg(ap, size_t *);
			if (ntargets != NULL)
				*ntargets = dev->nbuffers;
			if (targets != NULL)
				*targets = dev->mems;
			if (size != NULL)
				*size = frame_size;
			ret = 0;
		}
		break;
		default:
			err("shailo: %s unknown buffer type %d", (dev->type == device_input) ? dev->vstream_infos.name : dev->stream_info.name, t);
	}
	va_end(ap);
	return ret;
}

EXT_API int shailo_fd(Hailo_t *dev, int writer)
{
	if (dev->type == device_output && writer != 1)
		return dev->completion_fd;
	return -1;
}

EXT_API int shailo_start(Hailo_t *dev)
{
	if (dev->type == device_input)
	{
		for (int i = 0; i < dev->nbuffers; i++)
		{
			dev->buffers[i].state = dequeued;
		}
		dev->pending = 0;
	}
	dev->curbuffer = &dev->buffers[0];
	return 0;
}

EXT_API int shailo_stop(Hailo_t *dev)
{
	return 0;
}

static void _shailo_write_cb(const hailo_stream_write_async_completion_info_t *info)
{
	FrameBuffer_t *buffer = (FrameBuffer_t *)info->opaque;
	Hailo_t *dev = (Hailo_t *)buffer->private;
	dev->async_status = info->status;
	buffer->state = ready;
	if (dev->dup)
		__atomic_fetch_add(&dev->dup->pending, 1, __ATOMIC_RELAXED);
	if (dev->completion_fd >= 0)
	{
		uint64_t one = 1;
		(void)write(dev->completion_fd, &one, sizeof(one));
	}
}

EXT_API int shailo_queue(Hailo_t *dev, int id, void *mem, size_t bytesused, int flags)
{
	shailo_dbg("shailo: queue %s", (dev->type == device_input) ? dev->vstream_infos.name : dev->stream_info.name);
	if (id < 0 || id >= dev->nbuffers)
	{
		err("shailo: unkown %d buffer index to queue", id);
		return -1;
	}
	FrameBuffer_t *buffer = &dev->buffers[id];
	if (dev->type == device_input)
	{
		shailo_dbg("shailo:   queue input buffer %d", id);
		buffer->state = queued;
		buffer->bytesused = 0;
		return 0;
	}

	shailo_dbg("shailo:   queue output buffer %d", id);
	if (bytesused == 0)
		bytesused = buffer->size;
	buffer->flags = flags;
	if (bytesused > buffer->size)
	{
		warn("shailo: buffer too small %zu %zu", buffer->size, bytesused);
	}

	if (mem == NULL)
		mem = buffer->mem;
	if (mem == NULL)
	{
		err("shailo: no memory for buffer %d", id);
		return -1;
	}

	buffer->state = queued;
	buffer->bytesused = bytesused;
	hailo_status status = hailo_stream_write_raw_buffer_async(dev->input_stream, mem, bytesused, _shailo_write_cb, buffer);
	if (status == HAILO_QUEUE_IS_FULL)
	{
		errno = EAGAIN;
		return -1;
	}
	if (status != HAILO_SUCCESS)
	{
		err("shailo: input stream async write submit error %d", status);
		return -1;
	}

	return 0;
}

EXT_API int shailo_dequeue(Hailo_t *dev, void **mem, size_t *bytesused, int *flags)
{
	shailo_dbg("shailo: dequeue %s", (dev->type == device_input) ? dev->vstream_infos.name : dev->stream_info.name);
	if (dev->type == device_input)
	{
		if (dev->pending <= 0)
		{
			errno = EAGAIN;
			return -1;
		}
		FrameBuffer_t *buffer = dev->curbuffer;
		shailo_dbg("shailo: dequeue input buffer %d", buffer->id);
		hailo_status status = hailo_vstream_read_raw_buffer(dev->output_vstream, buffer->mem, buffer->size);
		if (status == HAILO_TIMEOUT)
		{
			errno = EAGAIN;
			return -1;
		}
		if (status != HAILO_SUCCESS)
		{
			err("shailo: output vstream read error %d", status);
			return -1;
		}
		dev->curbuffer = buffer->next;
		__atomic_fetch_sub(&dev->pending, 1, __ATOMIC_RELAXED);
		buffer->bytesused = buffer->size;
		if (bytesused)
			*bytesused = buffer->bytesused;
		if (mem)
			*mem = buffer->mem;
		if (flags)
			*flags = 0;
		buffer->state = dequeued;
		return buffer->id;
	}

	FrameBuffer_t *buffer = dev->curbuffer;
	if (buffer->state != ready)
	{
		errno = EAGAIN;
		return -1;
	}
	dev->curbuffer = buffer->next;
	shailo_dbg("shailo: dequeue output buffer %d", buffer->id);

	if (dev->completion_fd >= 0)
	{
		uint64_t val;
		(void)read(dev->completion_fd, &val, sizeof(val));
	}
	if (dev->async_status != HAILO_SUCCESS)
	{
		err("shailo: input stream async write error %d", dev->async_status);
		buffer->state = dequeued;
		return -1;
	}
	if (bytesused)
		*bytesused = buffer->bytesused;
	if (mem)
		*mem = buffer->mem;
	buffer->state = dequeued;
	return buffer->id;
}

static void _shailo_release_vstream(Hailo_t *dup)
{
	if (dup && dup->output_vstream)
	{
		hailo_release_output_vstreams(&dup->output_vstream, 1);
		dup->output_vstream = NULL;
	}
}

EXT_API void shailo_destroy(Hailo_t *dev)
{
	if (dev->type == device_output)
	{
		if (dev->network_group)
			hailo_shutdown_network_group(dev->network_group);
		_shailo_release_vstream(dev->dup);
		if (dev->dup)
			dev->dup->dup = NULL;
		dev->dup = NULL;
		if (dev->activated_ng)
			hailo_deactivate_network_group(dev->activated_ng);
		if (dev->completion_fd >= 0)
			close(dev->completion_fd);
		if (dev->input_allocated)
		{
			for (int i = 0; i < dev->nbuffers; i++)
			{
#ifdef HAILO_DMABUF_ALLOC
				if (dev->buffers[i].mem)
					sdmabuf_unmap(dev->buffers[i].mem, dev->buffers[i].size);
				if (dev->buffers[i].dma_buf)
					sdmabuf_destroy(dev->buffers[i].dma_buf);
#else
				if (dev->buffers[i].mem)
					hailo_free_buffer(dev->buffers[i].mem);
#endif
			}
			if (dev->mems)
				free(dev->mems);
		}
		if (dev->hef)
			hailo_release_hef(dev->hef);
		if (dev->vdevice)
			hailo_release_vdevice(dev->vdevice);
	}
	else
	{
		_shailo_release_vstream(dev);
		if (dev->dup)
			dev->dup->dup = NULL;
		for (int i = 0; i < dev->nbuffers; i++)
			free(dev->buffers[i].mem);
		if (dev->mems)
			free(dev->mems);
	}
	if (dev->buffers)
		free(dev->buffers);
	if (dev->config)
		free(dev->config);
	free(dev);
}

EXT_API int shailo_loadjsonconfiguration(void *arg, void *entry)
{
	json_t *jconfig = entry;
	HailoConfig_t *config = (HailoConfig_t *)arg;

	json_t *hef = json_object_get(jconfig, "hef");
	if (hef && json_is_string(hef))
		config->hef = json_string_value(hef);

	json_t *device = json_object_get(jconfig, "device");
	if (device && json_is_string(device))
		config->device = json_string_value(device);

	json_t *definition = json_object_get(jconfig, "definition");
	sconfig_loaddefinition(&config->parent, definition);

	json_t *nbuffers = json_object_get(jconfig, "nbuffers");
	if (nbuffers && config->nbuffers == MAX_BUFFERS && json_is_integer(nbuffers))
		config->nbuffers = json_integer_value(nbuffers);

	return 0;
}

static const FastVideoDevice_ops_t shailo_ops = {
	.name = "hailo",
	.createconfig = shailo_createconfig,
	.create = (FastVideoDevice_create_t)shailo_create,
	.duplicate = (FastVideoDevice_duplicate_t)shailo_duplicate,
	.loadsettings = (FastVideoDevice_loadsettings_t)NULL,
	.requestbuffer = (FastVideoDevice_requestbuffer_t)shailo_requestbuffer,
	.eventfd = (FastVideoDevice_eventfd_t)shailo_fd,
	.start = (FastVideoDevice_start_t)shailo_start,
	.stop = (FastVideoDevice_stop_t)shailo_stop,
	.dequeue = (FastVideoDevice_dequeue_t)shailo_dequeue,
	.queue = (FastVideoDevice_queue_t)shailo_queue,
	.destroy = (FastVideoDevice_destroy_t)shailo_destroy,
};

static void __attribute__ ((constructor)) shailo_init()
{
	fastvideodevice_ops_append_t _fastvideodevice_ops_append;
	void *hdl = dlopen(NULL, RTLD_NOW);
	_fastvideodevice_ops_append = dlsym(hdl, "fastvideodevice_ops_append");
	if (_fastvideodevice_ops_append)
	{
		_fastvideodevice_ops_append(&shailo_ops);
	}
}
