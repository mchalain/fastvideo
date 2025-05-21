#include <stddef.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>

#include "log.h"
#include "config.h"
#include "spassthrough.h"
#include "sfile.h"

static const char spassthrough[] = "spassthrough";

typedef struct PassBuffer_s PassBuffer_t;
struct PassBuffer_s
{
	int index;
	void *mem;
	int dmabuf;
	size_t size;
	size_t bytesused;
	PassBuffer_t *next;
	PassBuffer_t *previous;
	enum
	{
		PassBuffer_free_e,
		PassBuffer_fill_e,
	} state;
};

#define MODE_SHOOT 0x01
#define MODE_SHOOTING 0x10

struct Passthrough_config_s
{
	DeviceConf_t parent;
	int mode;
	DeviceConf_t shoot;
};

typedef struct Passthrough_s Passthrough_t;
struct Passthrough_s
{
	device_type_e type;
	Passthrough_config_t *config;
	Passthrough_t *dup;
	int nbuffers;
	void **mems;
	int *dmabufs;
	size_t size;
	PassBuffer_t *buffers;
	PassBuffer_t *fifo;
	int state;
	struct
	{
		DeviceConf_t *config;
		void *dev;
		FastVideoDevice_ops_t *ops;
	} shoot;
};

int spassthrough_loadjsonconfiguration(void *arg, void *entry);

DeviceConf_t * spassthrough_createconfig(void)
{
	Passthrough_config_t *config = calloc(1, sizeof(*config));
	config->parent.name = spassthrough;
#ifdef HAVE_JANSSON
	config->parent.ops.loadconfiguration = spassthrough_loadjsonconfiguration;
#endif
	return &config->parent;
}

void *spassthrough_create(const char *devicename, device_type_e type, Passthrough_config_t *config)
{
	if (type != device_transfer)
	{
		err("spassthrough: %s bad device type", config->parent.name);
		return NULL;
	}
	Passthrough_t *dev = calloc(1, sizeof(*dev));
	dev->config = config;
	dev->type = device_output;
	return dev;
}

void *spassthrough_duplicate(Passthrough_t *dev, Passthrough_config_t **pconfig)
{
	Passthrough_t *dup = calloc(1, sizeof(*dup));
	dup->type = device_input;
	dup->dup = dev;
	dev->dup = dup;
	dev->config = *pconfig;
	if (dev->config->mode & MODE_SHOOT)
	{
		FastVideoDevice_ops_t *opss[] = {
			&sfile_ops,
			NULL,
		};
		for (int i = 0; opss[i] != NULL; i++)
		{
			if (! strcmp(dev->config->shoot.type, opss[i]->name))
				dev->shoot.ops = opss[i];
		}
		DeviceConf_t *devconfig = NULL;
		if (dev->shoot.ops)
			dev->shoot.ops->createconfig();
		if (devconfig)
		{
			devconfig->name = dev->config->shoot.name;
			devconfig->type = dev->config->shoot.type;
			devconfig->entry = dev->config->shoot.entry;
			if (devconfig->ops.loadconfiguration)
				devconfig->ops.loadconfiguration(devconfig, devconfig->entry);
			dev->shoot.config = devconfig;
			dev->shoot.dev = dev->shoot.ops->create(devconfig->name, device_output, dev->shoot.config);
		}
	}
	return dup;
}

int spassthrough_loadsettings(Passthrough_t *dev, void *configentry)
{
	return 0;
}

static int _passthrough_createbuffers(Passthrough_t *dev, int nmems, void **mems, int *dmabufs, size_t size)
{
	dev->nbuffers = nmems;
	dev->buffers = calloc(nmems, sizeof(*dev->buffers));
	for (int i = 0; i < nmems; i++)
	{
		if (mems)
			dev->buffers[i].mem = mems[i];
		if (dmabufs)
			dev->buffers[i].dmabuf = dmabufs[i];
		dev->buffers[i].index = i;
		dev->buffers[i].size = size;
	}
	dev->mems = mems;
	dev->dmabufs = dmabufs;
	dev->size = size;
	return nmems;
}

int spassthrough_requestbuffer(Passthrough_t *dev, enum buf_type_e t, ...)
{
	int ret = -1;
	va_list ap;
	va_start(ap, t);
	switch (t)
	{
		case buf_type_memory:
		{
			if (!dev->dup || dev->buffers)
				break;
			int ntargets = va_arg(ap, int);
			void **targets = va_arg(ap, void **);
			size_t size = va_arg(ap, size_t);
			_passthrough_createbuffers(dev, ntargets, targets, NULL, size);
			_passthrough_createbuffers(dev->dup, ntargets, targets, NULL, size);
			ret = 0;
			if (dev->type == device_input && dev->shoot.dev)
			{
				dev->shoot.ops->destroy(dev->shoot.dev);
				dev->shoot.dev = NULL;
			}
		}
		break;
		case (buf_type_memory | buf_type_master):
		{
			if (!dev->buffers || !dev->mems)
				break;
			int *ntargets = va_arg(ap, int *);
			void ***targets = va_arg(ap, void ***);
			size_t *size = va_arg(ap, size_t *);
			if (ntargets != NULL)
				*ntargets = dev->nbuffers;
			if (targets != NULL)
			{
				*targets = dev->mems;
			}
			if (size != NULL)
				*size = dev->size;
			ret = 0;
			if (dev->type == device_input && dev->shoot.dev)
			{
				dev->shoot.ops->requestbuffer(dev->shoot.dev, buf_type_memory, dev->nbuffers, dev->mems, dev->size, NULL);
			}
		}
		break;
		case buf_type_dmabuf:
		{
			if (!dev->dup || dev->buffers)
				break;
			int ntargets = va_arg(ap, int);
			int *targets = va_arg(ap, int *);
			size_t size = va_arg(ap, size_t);
			_passthrough_createbuffers(dev, ntargets, NULL, targets, size);
			_passthrough_createbuffers(dev->dup, ntargets, NULL, targets, size);
			ret = 0;
			if (dev->type == device_input && dev->shoot.dev)
			{
				dev->shoot.ops->destroy(dev->shoot.dev);
				dev->shoot.dev = NULL;
			}
		}
		break;
		case buf_type_dmabuf | buf_type_master:
		{
			if (!dev->buffers || !dev->dmabufs)
				break;
			int *ntargets = va_arg(ap, int *);
			int **targets = va_arg(ap, int **);
			size_t *size = va_arg(ap, size_t *);
			if (ntargets != NULL)
				*ntargets = dev->nbuffers;
			if (targets != NULL)
			{
				*targets = dev->dmabufs;
			}
			if (size != NULL)
				*size = dev->size;
			ret = 0;
			if (dev->type == device_input && dev->shoot.dev)
			{
				dev->shoot.ops->requestbuffer(dev->shoot.dev, buf_type_dmabuf, dev->nbuffers, dev->dmabufs, dev->size, NULL);
			}
		}
		break;
		default:
			err("spassthrough: unkonwn buffer type");
	}
	va_end(ap);
	return ret;
}

int spassthrough_fd(Passthrough_t *dev)
{
	return -1;
}

int spassthrough_start(Passthrough_t *dev)
{
	if (dev->type == device_input && dev->shoot.dev)
	{
		dev->shoot.ops->start(dev->shoot.dev);
	}
	return 0;
}

int spassthrough_stop(Passthrough_t *dev)
{
	if (dev->type == device_input && dev->shoot.dev)
	{
		dev->shoot.ops->stop(dev->shoot.dev);
	}
	return 0;
}

int spassthrough_dequeue(Passthrough_t *dev, void **mem, size_t *bytesused)
{
	PassBuffer_t *last = dev->fifo;
	errno = EAGAIN;
	if (last == NULL)
		return -1;
	if (last->state == PassBuffer_free_e)
		return -1;
	if (dev->type == device_input && dev->state == MODE_SHOOTING)
	{
		int index = dev->shoot.ops->dequeue(dev->shoot.dev, mem, bytesused);
		if (index == last->index)
			dev->state = 0;
	}
	last->state = PassBuffer_free_e;
	/** the real fifo is useless as the entry is immediately pushed **/
#if 0
	while (last->next) last = last->next;
	if (last->previous)
		last->previous->next = NULL;
	last->previous = NULL;
#endif

	if (bytesused)
		*bytesused = last->bytesused;
	if (mem)
		*mem = last->mem;
	return last->index;
}

int spassthrough_queue(Passthrough_t *dev, int index, void *mem, size_t bytesused)
{
	dev = dev->dup;
	if (mem)
		dev->buffers[index].mem = mem;
	dev->buffers[index].bytesused = bytesused;
	dev->buffers[index].state = PassBuffer_fill_e;
#if 0
	/** prepare fifo's items **/
	dev->buffers[index].next = dev->fifo;
	if (dev->fifo)
		dev->fifo->previous = &dev->buffers[index];
#endif
	/** insert into fifo **/
	dev->fifo = &dev->buffers[index];
	if (dev->type == device_input && dev->state == MODE_SHOOT)
	{
		dev->shoot.ops->queue(dev->shoot.dev, index, mem, bytesused);
		dev->state = MODE_SHOOTING;
	}
	return 0;
}

void spassthrough_destroy(Passthrough_t *dev)
{
	if (dev->type == device_input && dev->shoot.dev)
	{
		dev->shoot.ops->destroy(dev->shoot.dev);
	}
	free(dev->config);
	free(dev);
}

int spassthrough_loadjsonconfiguration(void *arg, void *entry)
{
	json_t *jconfig = entry;

	Passthrough_config_t *config = (Passthrough_config_t *)arg;
	json_t *mode = json_object_get(jconfig, "mode");
	if (mode && json_is_string(mode))
	{
		const char *value = json_string_value(mode);
		if (!strncasecmp(value, "shoot",6))
			config->mode = MODE_SHOOT;
	}
	json_t *shoot = json_object_get(jconfig, "shoot");
	config->shoot.entry = shoot;
	if (mode && json_is_object(mode))
	{
		json_t *name = json_object_get(jconfig, "name");
		config->shoot.name = json_string_value(name);
		json_t *type = json_object_get(jconfig, "type");
		config->shoot.type = json_string_value(type);
	}

library_end:
	return 0;
}

FastVideoDevice_ops_t spassthrough_ops = {
	.name = "passthrough",
	.createconfig = spassthrough_createconfig,
	.create = (FastVideoDevice_create_t)spassthrough_create,
	.duplicate = (FastVideoDevice_duplicate_t)spassthrough_duplicate,
	.loadsettings = (FastVideoDevice_loadsettings_t)spassthrough_loadsettings,
	.requestbuffer = (FastVideoDevice_requestbuffer_t)spassthrough_requestbuffer,
	.eventfd = (FastVideoDevice_eventfd_t)spassthrough_fd,
	.start = (FastVideoDevice_start_t)spassthrough_start,
	.stop = (FastVideoDevice_stop_t)spassthrough_stop,
	.dequeue = (FastVideoDevice_dequeue_t)spassthrough_dequeue,
	.queue = (FastVideoDevice_queue_t)spassthrough_queue,
	.destroy = (FastVideoDevice_destroy_t)spassthrough_destroy,
};

#include <dlfcn.h>

static void __attribute__ ((constructor)) spassthrough_init()
{
	fastvideodevice_ops_append_t _fastvideodevice_ops_append;
	void *hdl = dlopen(NULL, RTLD_NOW);
	_fastvideodevice_ops_append = dlsym(hdl, "fastvideodevice_ops_append");
	if (_fastvideodevice_ops_append)
	{
		_fastvideodevice_ops_append(&spassthrough_ops);
	}
}
