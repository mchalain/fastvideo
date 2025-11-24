#include <string.h>
#include <errno.h>

#include "fastvideo.h"
#include "config.h"
#include "log.h"

#define MAX_BUFFERS 4

typedef struct Dev_s Dev_t;
struct Dev_s
{
	device_type_e type;
	FrameBuffer_t *buffers;
	int nbuffers;
	int currentid;
};

static FrameBuffer_t *_create_buffer(DeviceConf_t *config)
{
	FrameBuffer_t *buffer = calloc(1, sizeof(*buffer));
	buffer->size = 1024;
	buffer->mem = malloc(buffer->size);
	buffer->dma_buf = 0;
	buffer->map_offset = 0;
	return buffer;
}
static void _destroy_buffer(FrameBuffer_t *buffer)
{
	free(buffer->mem);
}

EXT_API DeviceConf_t * skeleton_createconfig(void)
{
	DeviceConf_t *devconfig = (void *)(long) -1;
	return devconfig;
}

EXT_API Dev_t *skeleton_create(const char *devicename, device_type_e type, DeviceConf_t *config)
{
	Dev_t *dev = calloc(1, sizeof(*dev));
	dev->type = type;
	if (type == device_input)
	{
		/**
		 * this is a choice for skeleton
		 * but it may be different
		 */
		for (int i = 0; i < MAX_BUFFERS; i++)
		{
			FrameBuffer_t *buffer = _create_buffer(config);
			if (buffer == NULL)
				break;
			buffer->id = dev->nbuffers;
			buffer->next = dev->buffers;
			dev->buffers = buffer;
			dev->nbuffers ++;
		}
	}
	return dev;
}

EXT_API Dev_t *skeleton_duplicate(Dev_t *dev)
{
	Dev_t *dev2 = calloc(1, sizeof(*dev));
	memmove(dev2, dev, sizeof(*dev));
	return dev2;
}

EXT_API int skeleton_requestbuffer(Dev_t *dev, enum buf_type_e t, ...)
{
	va_list ap;
	va_start(ap, t);
	switch (t)
	{
		case (buf_type_memory):
		{
			if (dev->type == device_input)
				return -1;
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
		case (buf_type_memory | buf_type_master):
		{
			int *ntargets = va_arg(ap, int *);
			void **targets = va_arg(ap, void **);
			size_t *psize = va_arg(ap, size_t *);
			if (targets != NULL)
			{
				*targets = calloc(dev->nbuffers, sizeof(void*));
				int i = 0;
				for (FrameBuffer_t *buffer = dev->buffers; buffer != NULL; buffer = buffer->next)
				{
					targets[i] = buffer->mem;
					i++;
				}
			}
			if (ntargets != NULL)
				*ntargets = dev->nbuffers;
			if (psize != NULL)
				*psize = dev->buffers->size;
		}
		break;
		case buf_type_dmabuf:
		{
			if (dev->type == device_input)
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
		case buf_type_dmabuf | buf_type_master:
		{
			int *ntargets = va_arg(ap, int *);
			int **targets = va_arg(ap, int **);
			size_t *psize = va_arg(ap, size_t *);
			if (targets != NULL)
			{
				*targets = calloc(dev->nbuffers, sizeof(int));
				int i = 0;
				for (FrameBuffer_t *buffer = dev->buffers; buffer != NULL; buffer = buffer->next)
				{
					(*targets)[i] = buffer->dma_buf;
					i++;
				}
			}
			if (ntargets != NULL)
				*ntargets = dev->nbuffers;
			if (psize != NULL)
				*psize = dev->buffers->size;
		}
		break;
		default:
			va_end(ap);
			return -1;
	}
	va_end(ap);

	return 0;
}

EXT_API int skeleton_fd(Dev_t *dev, int writer)
{
	return -1;
}

EXT_API int skeleton_queue(Dev_t *dev, int id, void *mem, size_t size, int flags)
{
	if (id < 0 || id > dev->nbuffers)
		return -1;
	if (dev->currentid != -1)
	{
		errno = EAGAIN;
		return -1;
	}
	dev->currentid = id;
	dev->buffers[id].state = queued;
	/**
	 * treat the buffer here:
	 *  - push on device
	 *  - tranform the data
	 */
	return 0;
}

EXT_API int skeleton_dequeue(Dev_t *dev, void **mem, size_t *bytesused, int *flags)
{
	FrameBuffer_t *buffer = NULL;
	int id = dev->currentid;
	if (id == -1)
	{
		errno = EAGAIN;
		return -1;
	}
	buffer = &dev->buffers[dev->currentid];
	dev->currentid = -1;

	if (*mem)
		*mem = buffer->mem;
	if (*bytesused)
		*bytesused = buffer->size;
	buffer->state = dequeued;
	return id;
}

EXT_API int skeleton_start(Dev_t *dev)
{
	if (dev->type == device_input)
	{
		for (FrameBuffer_t *buffer = dev->buffers; buffer != NULL; buffer = buffer->next)
		{
			skeleton_queue(dev, buffer->id, buffer->mem, buffer->size, 0);
		}
	}
	return 0;
}

EXT_API int skeleton_stop(Dev_t *dev)
{
	for (FrameBuffer_t *buffer = dev->buffers; buffer != NULL; buffer = buffer->next)
	{
		buffer->state = invalid;
	}
	return 0;
}

EXT_API void skeleton_destroy(Dev_t *dev)
{
	FrameBuffer_t *next = NULL;
	for (FrameBuffer_t *buffer = dev->buffers; buffer != NULL; buffer = next)
	{
		next = buffer->next;
		_destroy_buffer(buffer);
	}
	free(dev);
}

FastVideoDevice_ops_t sskeleton_ops = {
	.name = "skeleton",
	.createconfig = skeleton_createconfig,
	.create = (FastVideoDevice_create_t)skeleton_create,
	.duplicate = (FastVideoDevice_duplicate_t)skeleton_duplicate,
	.loadsettings = (FastVideoDevice_loadsettings_t)NULL,
	.requestbuffer = (FastVideoDevice_requestbuffer_t)skeleton_requestbuffer,
	.eventfd = (FastVideoDevice_eventfd_t)skeleton_fd,
	.start = (FastVideoDevice_start_t)skeleton_start,
	.stop = (FastVideoDevice_stop_t)skeleton_stop,
	.dequeue = (FastVideoDevice_dequeue_t)skeleton_dequeue,
	.queue = (FastVideoDevice_queue_t)skeleton_queue,
	.destroy = (FastVideoDevice_destroy_t)skeleton_destroy,
};

#include <dlfcn.h>

static void __attribute__ ((constructor)) sskeleton_init()
{
	fastvideodevice_ops_append_t _fastvideodevice_ops_append;
	void *hdl = dlopen(NULL, RTLD_NOW);
	_fastvideodevice_ops_append = dlsym(hdl, "fastvideodevice_ops_append");
	if (_fastvideodevice_ops_append)
	{
		_fastvideodevice_ops_append(&sskeleton_ops);
	}
}
