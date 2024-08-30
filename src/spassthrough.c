#include <stddef.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdint.h>

#include "log.h"
#include "config.h"
#include "spassthrough.h"

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
};

static const char name1_str[] = "input";
static const char name2_str[] = "output";

typedef struct Passthrough_s Passthrough_t;
struct Passthrough_s
{
	const char *name;
	DeviceConf_t *config;
	Passthrough_t *dup;
	int nbuffers;
	void **mems;
	int *dmabufs;
	size_t size;
	PassBuffer_t *buffers;
	PassBuffer_t *fifo;
};

DeviceConf_t * spassthrough_createconfig(void)
{
	DeviceConf_t *config = calloc(1, sizeof(*config));
	return config;
}
void *spassthrough_create(const char *devicename, DeviceConf_t *config)
{
	Passthrough_t *dev = calloc(1, sizeof(*dev));
	dev->config = config;
	dev->name = name1_str;
	return dev;
}
void *spassthrough_duplicate(Passthrough_t *dev)
{
	Passthrough_t *dup = calloc(1, sizeof(*dup));
	dup->name = name2_str;
	dup->dup = dev;
	dev->dup = dup;
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
		{dbg("%s %d", __FILE__, __LINE__);
			if (!dev->dup || dev->buffers)
				break;
			int ntargets = va_arg(ap, int);
			void **targets = va_arg(ap, void **);
			size_t size = va_arg(ap, size_t);
			_passthrough_createbuffers(dev, ntargets, targets, NULL, size);
			_passthrough_createbuffers(dev->dup, ntargets, targets, NULL, size);
			ret = 0;
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
	return 0;
}
int spassthrough_stop(Passthrough_t *dev)
{
	return 0;
}
int spassthrough_dequeue(Passthrough_t *dev, void **mem, size_t *bytesused)
{
	PassBuffer_t *last = dev->fifo;
	if (last == NULL)
		return -1;
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
int spassthrough_queue(Passthrough_t *dev, int index, size_t bytesused)
{
	dev = dev->dup;
	dev->buffers[index].bytesused = bytesused;
#if 0
	/** prepare fifo's items **/
	dev->buffers[index].next = dev->fifo;
	if (dev->fifo)
		dev->fifo->previous = &dev->buffers[index];
#endif
	/** insert into fifo **/
	dev->fifo = &dev->buffers[index];
	return 0;
}
void spassthrough_destroy(Passthrough_t *dev)
{
	free(dev->config);
	free(dev);
}
