#include <stddef.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdint.h>

#include "log.h"
#include "config.h"
#include "spassthrough.h"

typedef struct Passthrough_s Passthrough_t;
struct Passthrough_s
{
	DeviceConf_t *config;
	Passthrough_t *first;
	int nbuffers;
	int index;
	void **mems;
	int *dmabufs;
	size_t size;
	size_t bytesused;
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
	return dev;
}
void *spassthrough_duplicate(Passthrough_t *dev)
{
	Passthrough_t *dup = calloc(1, sizeof(*dup));
	dup->first = dev;
	return dup;
}
int spassthrough_loadsettings(Passthrough_t *dev, void *configentry)
{
	return 0;
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
			int nmem = va_arg(ap, int);
			void **mems = va_arg(ap, void **);
			size_t size = va_arg(ap, size_t);
			if (dev->first)
				break;
			dev->nbuffers = nmem;
			dev->mems = mems;
			dev->size = size;
			ret = 0;
		}
		break;
		case (buf_type_memory | buf_type_master):
		{
			if (dev->first == NULL || dev->first->mems == NULL)
				break;
			int *ntargets = va_arg(ap, int *);
			void ***targets = va_arg(ap, void ***);
			size_t *size = va_arg(ap, size_t *);
			if (ntargets != NULL)
				*ntargets = dev->first->nbuffers;
			if (targets != NULL)
			{
				*targets = dev->first->mems;
			}
			if (size != NULL)
				*size = dev->first->size;
			ret = 0;
		}
		break;
		case buf_type_dmabuf:
		{
			int ntargets = va_arg(ap, int);
			int *targets = va_arg(ap, int *);
			size_t size = va_arg(ap, size_t);
			if (dev->first)
				break;
			dev->nbuffers = ntargets;
			dev->dmabufs = targets;
			dev->size = size;
			ret = 0;
		}
		break;
		case buf_type_dmabuf | buf_type_master:
		{
			if (dev->first == NULL || dev->first->dmabufs == NULL)
				break;
			int *ntargets = va_arg(ap, int *);
			int **targets = va_arg(ap, int **);
			size_t *size = va_arg(ap, size_t *);
			if (ntargets != NULL)
				*ntargets = dev->first->nbuffers;
			if (targets != NULL)
			{
				*targets = dev->first->dmabufs;
			}
			if (size != NULL)
				*size = dev->first->size;
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
	if (! dev->first)
		return dev->index;
	if (bytesused)
		*bytesused = dev->first->bytesused;
	if (mem)
		*mem = dev->first->mems[dev->first->index];
	return dev->first->index;
}
int spassthrough_queue(Passthrough_t *dev, int index, size_t bytesused)
{
	if (dev->first)
		return 0;
	dev->index = index;
	dev->bytesused = bytesused;
	return 0;
}
void spassthrough_destroy(Passthrough_t *dev)
{
	free(dev->config);
	free(dev);
}
