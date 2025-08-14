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
#include "sdmabuf.h"

static const char spassthrough[] = "spassthrough";
static int spassthrough_loadjsonsettings(Passthrough_t *dev, void *entry);

typedef struct PassBuffer_s PassBuffer_t;
struct PassBuffer_s
{
	int index;
	void *mem;
	int dmabuf;
	size_t size;
	size_t bytesused;
	int flags;
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
#define MODE_TEE 0x02
#define MODE_DRYRUN 0x04
#define MODE_COPY 0x08

struct Passthrough_config_s
{
	DeviceConf_t parent;
	int mode;
	DeviceConf_t branch;
};

typedef struct Passthrough_s Passthrough_t;
struct Passthrough_s
{
	device_type_e type;
	Passthrough_config_t *config;
	Passthrough_t *dup;
	const char *name;
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
	} branch;
	size_t (*copy)(Passthrough_t *, const char *const , char *, size_t);
};

EXT_API int spassthrough_loadjsonconfiguration(void *arg, void *entry);

DeviceConf_t * spassthrough_createconfig(void)
{
	Passthrough_config_t *config = calloc(1, sizeof(*config));
	config->parent.name = spassthrough;
#ifdef HAVE_JANSSON
	config->parent.ops.loadconfiguration = spassthrough_loadjsonconfiguration;
#endif
	return &config->parent;
}

#ifdef __ARM_NEON
static size_t _neon_copy(Passthrough_t *dev, const char *const src, char *dst, size_t size)
{
	asm volatile (
		"1:                                               \n"
		"subs     %[size], %[size], #32                   \n"
		"vld1.u8  {d0, d1, d2, d3}, [%[src],:128]!        \n"
		"vst1.u8  {d0, d1, d2, d3}, [%[dst],:128]!        \n"
		"bgt      1b                                      \n"
		: [dst]"+r"(dst)
		: [src]"r"(src), [size]"r"(size)
		: "d0", "d1", "d2", "d3", "cc", "memory"
	);
}
#endif

static size_t _default_copy(Passthrough_t *dev, const char *const src, char *dst, size_t size)
{
	memcpy(dst, src, size);
	return size;
}

static size_t _passthrough_copy(Passthrough_t *dev, PassBuffer_t *src, PassBuffer_t *dst, size_t bytesused)
{
	if (dst->size < bytesused)
		return -1;
	void *srcmem = NULL;
	if (src->mem)
		srcmem = src->mem;
	if (src->dmabuf)
	{
		sdmabuf_sync(src->dmabuf, 1);
		srcmem = sdmabuf_map(src->dmabuf, bytesused, 0);
	}
	sdmabuf_sync(dst->dmabuf, 1);
	if (dst->mem == NULL)
		dst->mem = sdmabuf_map(dst->dmabuf, dst->size, 1);
	bytesused = dev->copy(dev, srcmem, dst->mem, bytesused);
	sdmabuf_sync(dst->dmabuf, 0);
	if (src->dmabuf)
	{
		sdmabuf_sync(src->dmabuf, 0);
		sdmabuf_unmap(srcmem, bytesused);
	}
	return bytesused;
}

EXT_API void *spassthrough_create(const char *devicename, device_type_e type, Passthrough_config_t *config)
{
#if 0
	if (type == device_input)
	{
		err("spassthrough: %s bad device type", (config)?config->parent.name:"");
		return NULL;
	}
#endif
	Passthrough_t *dev = calloc(1, sizeof(*dev));
	dev->config = config;
	dev->name = devicename;
	dev->type = device_output;
	dev->copy = _default_copy;
#ifdef __ARM_NEON
	if (scpu_check(SCPU_NEON))
	{
		dev->copy = _neon_copy;
	}
#endif

	return dev;
}

EXT_API void *spassthrough_duplicate(Passthrough_t *dev, Passthrough_config_t **pconfig)
{
	Passthrough_t *dup = calloc(1, sizeof(*dup));
	dup->type = device_input;
	dup->dup = dev;
	dev->dup = dup;
	dev->config = *pconfig;
	/// only the main dev must manage the copy buffers, but dup dev contains the buffers
	dup->state &= ~MODE_COPY;
	if (dev->config->branch.type != 0)
	{
		FastVideoDevice_ops_t *opss[] = {
			&sfile_ops,
			NULL,
		};
		for (int i = 0; opss[i] != NULL; i++)
		{
			if (! strcmp(dev->config->branch.type, opss[i]->name))
				dev->branch.ops = opss[i];
		}
		DeviceConf_t *devconfig = NULL;
		if (dev->branch.ops)
			dev->branch.ops->createconfig();
		if (devconfig)
		{
			devconfig->name = dev->config->branch.name;
			devconfig->type = dev->config->branch.type;
			devconfig->entry = dev->config->branch.entry;
			if (devconfig->ops.loadconfiguration)
				devconfig->ops.loadconfiguration(devconfig, devconfig->entry);
			dev->branch.config = devconfig;
			dev->branch.dev = dev->branch.ops->create(devconfig->name, device_output, dev->branch.config);
		}
	}
	return dup;
}

EXT_API int spassthrough_loadsettings(Passthrough_t *dev, void *configentry)
{
	return spassthrough_loadjsonsettings(dev, configentry);
}

static int _passthrough_createbuffers(Passthrough_t *dev, int nmems, void **mems, int *dmabufs, size_t size, int copy)
{
	int ret = 0;
	dev->nbuffers = nmems;
	dev->buffers = calloc(nmems, sizeof(*dev->buffers));
	void **tmems = NULL;
	int *tdmabufs = NULL;
	if (copy)
	{
		if (mems)
			tmems = calloc(nmems, sizeof(*mems));
		tdmabufs = calloc(nmems, sizeof(*dmabufs));
	}
	for (int i = 0; i < nmems; i++)
	{
		if (copy)
		{
			int dmabufs_tmp = 0;
			dmabufs_tmp = sdmabuf_create(spassthrough, size);
			if (dmabufs_tmp > 0)
			{
				if (tmems)
					tmems[i] = sdmabuf_map(dmabufs_tmp, size, 0);
				tdmabufs[i] = dmabufs_tmp;
				mems = tmems;
				dmabufs = tdmabufs;
			}
			else
			{
				err("spassthrough: the buffer copy is disallowed");
				ret = -1;
				copy = 0;
			}
		}
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
	return ret;
}

EXT_API int spassthrough_requestbuffer(Passthrough_t *dev, enum buf_type_e t, ...)
{
	int ret = -1;
	if (dev->state & MODE_COPY && !dev->dup)
	{
		err("spassthrough: copy state is allowed in transfer mode");
		dev->state &= ~MODE_COPY;
	}
	va_list ap;
	va_start(ap, t);
	/**
	 * currently spassthrough works as a slave for device_output (main dev)
	 * and as master for device_input (dup dev)
	 */
	switch (t)
	{
		case buf_type_memory:
		{
			/**
			 * for device_output (main dev)
			 */
			if (dev->buffers)
				break;
			int ntargets = va_arg(ap, int);
			void **targets = va_arg(ap, void **);
			size_t size = va_arg(ap, size_t);
			/**
			 * We need buffers in the first dev to allow the DRYRUN state.
			 */
			_passthrough_createbuffers(dev, ntargets, targets, NULL, size, 0);
			/**
			 * create buffers for the output dev
			 */
			if (dev->dup &&
				_passthrough_createbuffers(dev->dup, ntargets, targets, NULL, size,
					(dev->state & MODE_COPY)) < 0)
			{
				dev->state &= ~MODE_COPY;
			}
			ret = 0;
			if (dev->type == device_input && dev->branch.dev)
			{
				dev->branch.ops->destroy(dev->branch.dev);
				dev->branch.dev = NULL;
			}
		}
		break;
		case (buf_type_memory | buf_type_master):
		{
			/**
			 * device_input (dup dev)
			 */
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
			if (dev->type == device_input && dev->branch.dev)
			{
				dev->branch.ops->requestbuffer(dev->branch.dev, buf_type_memory, dev->nbuffers, dev->mems, dev->size, NULL);
			}
		}
		break;
		case buf_type_dmabuf:
		{
			if (dev->buffers)
				break;
			int ntargets = va_arg(ap, int);
			int *targets = va_arg(ap, int *);
			size_t size = va_arg(ap, size_t);
			_passthrough_createbuffers(dev, ntargets, NULL, targets, size, 0);
			if (dev->dup &&
				_passthrough_createbuffers(dev->dup, ntargets, NULL, targets, size,
					(dev->state & MODE_COPY)) < 0)
			{
				dev->state &= ~MODE_COPY;
			}
			ret = 0;
			if (dev->type == device_input && dev->branch.dev)
			{
				dev->branch.ops->destroy(dev->branch.dev);
				dev->branch.dev = NULL;
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
			if (dev->type == device_input && dev->branch.dev)
			{
				dev->branch.ops->requestbuffer(dev->branch.dev, buf_type_dmabuf, dev->nbuffers, dev->dmabufs, dev->size, NULL);
			}
		}
		break;
		default:
			err("spassthrough: unkonwn buffer type");
	}
	va_end(ap);
	return ret;
}

EXT_API int spassthrough_fd(Passthrough_t *dev, int writer)
{
	return -1;
}

EXT_API int spassthrough_start(Passthrough_t *dev)
{
	if (dev->type == device_input && dev->branch.dev)
	{
		dev->branch.ops->start(dev->branch.dev);
	}
	return 0;
}

EXT_API int spassthrough_stop(Passthrough_t *dev)
{
	if (dev->type == device_input && dev->branch.dev)
	{
		dev->branch.ops->stop(dev->branch.dev);
	}
	return 0;
}

EXT_API int spassthrough_dequeue(Passthrough_t *dev, void **mem, size_t *bytesused, int *flags)
{
	PassBuffer_t *last = dev->fifo;
	if (last == NULL || last->state == PassBuffer_free_e)
	{
		errno = EAGAIN;
		return -1;
	}
	if (dev->branch.dev && (dev->state & MODE_SHOOTING))
	{
		int index = dev->branch.ops->dequeue(dev->branch.dev, mem, bytesused, NULL);
		if (index == last->index && dev->state & MODE_SHOOT)
		{
			dev->state &= ~MODE_SHOOTING;
			dev->state &= ~MODE_SHOOT; /// shoot only once
		}
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
	if (flags)
		*flags = last->flags;
	return last->index;
}

EXT_API int spassthrough_queue(Passthrough_t *dev, int index, void *mem, size_t bytesused, int flags)
{
	if (dev->state & MODE_COPY)
	{
		if (mem)
			dev->buffers[index].mem = mem;
		_passthrough_copy(dev, &dev->buffers[index], &dev->dup->buffers[index], bytesused);
	}
	if (!(dev->state & MODE_DRYRUN) && dev->dup != NULL)
	{
		dev = dev->dup;
	}
	PassBuffer_t *buffer = &dev->buffers[index];
	if (mem)
		buffer->mem = mem;
	buffer->bytesused = bytesused;
	buffer->flags = flags;
	buffer->state = PassBuffer_fill_e;
#if 0
	/** prepare fifo's items **/
	buffer->next = dev->fifo;
	if (dev->fifo)
		dev->fifo->previous = buffer;
#endif
	/** insert into fifo **/
	dev->fifo = buffer;
	if ((dev->branch.dev) &&
		(dev->state & (MODE_SHOOT | MODE_TEE)))
	{
		dev->branch.ops->queue(dev->branch.dev, index, mem, bytesused, 0);
		dev->state |= MODE_SHOOTING;
	}
	return 0;
}

static void _passthrough_freedmabuf(Passthrough_t *dev)
{
	if (dev->dup && dev->dup->buffers)
	{
		for (int i = 0; i < dev->nbuffers; i++)
		{
			sdmabuf_destroy(dev->dup->buffers[i].dmabuf);
		}
	}
}

EXT_API void spassthrough_destroy(Passthrough_t *dev)
{
	if (dev->type == device_input && dev->branch.dev)
	{
		dev->branch.ops->destroy(dev->branch.dev);
	}
	if (dev->state & MODE_COPY)
	{
		_passthrough_freedmabuf(dev);
	}
	if (dev->config)
		free(dev->config);
	free(dev);
}

static int _passthrough_loadstate(Passthrough_t *dev, json_t *jconfig)
{
	if (json_is_object(jconfig))
	{
		json_t *jdryrun = json_object_get(jconfig, "dryrun");
		if (jdryrun && json_is_true(jdryrun))
			dev->state |= MODE_DRYRUN;
		else if (jdryrun)
			dev->state &= ~MODE_DRYRUN;
		json_t *jshoot = json_object_get(jconfig, "shoot");
		if (jshoot && json_is_true(jshoot))
			dev->state |= MODE_SHOOT;
		else if (jshoot)
			dev->state &= ~MODE_SHOOT;
		json_t *jtee = json_object_get(jconfig, "tee");
		if (jtee && json_is_true(jtee))
			dev->state |= MODE_TEE;
		else if (jtee)
			dev->state &= ~MODE_TEE;
		json_t *jcopy = json_object_get(jconfig, "copy");
		if (jcopy && json_is_true(jcopy))
			dev->state |= MODE_COPY;
		else if (jcopy)
			dev->state &= ~MODE_COPY;
	}
	if (json_is_string(jconfig))
	{
		const char *value = json_string_value(jconfig);
		if (!strcmp(value, "dryrun"))
			dev->state |= MODE_DRYRUN;
		else if (!strcmp(value, "shoot"))
			dev->state |= MODE_SHOOT;
		else if (!strcmp(value, "tee"))
			dev->state |= MODE_TEE;
		else if (!strcmp(value, "copy"))
			dev->state |= MODE_COPY;
	}
}

static int spassthrough_loadjsonsettings(Passthrough_t *dev, void *entry)
{
	json_t *jconfig = entry;
	json_t *jcontrols = json_object_get(jconfig,"controls");
	if (jcontrols && (json_is_array(jcontrols) || json_is_object(jcontrols)))
	{
		jconfig = jcontrols;
	}
	if (json_is_array(jconfig))
	{
		int index;
		json_t *jentry;
		json_array_foreach(jconfig, index, jentry)
		{
			_passthrough_loadstate(dev, jentry);
		}
	}
	else
		_passthrough_loadstate(dev, jconfig);
	if (!(dev->state & MODE_COPY))
	{
		_passthrough_freedmabuf(dev);
	}

	return 0;
}

EXT_API int spassthrough_loadjsonconfiguration(void *arg, void *entry)
{
	json_t *jconfig = entry;

	Passthrough_config_t *config = (Passthrough_config_t *)arg;
	json_t *branch = json_object_get(jconfig, "branch");
	config->branch.entry = branch;
	if (branch && json_is_object(branch))
	{
		json_t *name = json_object_get(branch, "name");
		config->branch.name = json_string_value(name);
		json_t *type = json_object_get(branch, "type");
		config->branch.type = json_string_value(type);
	}

	return 0;
}

int spassthrough_capabilities(Passthrough_t *dev, json_t *capabilities, int all)
{
	json_t *names = json_array();
	json_array_append_new(names, json_string(dev->name));
	json_object_set_new(capabilities, "name", names);
	json_object_set_new(capabilities, "type", json_string(spassthrough_ops.name));
	json_t *dryrun = json_object();
	json_object_set_new(dryrun, "name", json_string("dryrun"));
	json_object_set_new(dryrun, "value", json_false());
	json_t *tee = json_object();
	json_object_set_new(tee, "name", json_string("tee"));
	json_object_set_new(tee, "value", json_false());
	json_t *controls = json_array();
	json_array_append_new(controls, dryrun);
	json_array_append_new(controls, tee);
	json_object_set_new(capabilities, "control", controls);
	return 0;
}

FastVideoDevice_ops_t spassthrough_ops = {
	.name = "passthrough",
	.createconfig = spassthrough_createconfig,
	.create = (FastVideoDevice_create_t)spassthrough_create,
	.duplicate = (FastVideoDevice_duplicate_t)spassthrough_duplicate,
	.loadsettings = (FastVideoDevice_loadsettings_t)spassthrough_loadsettings,
	.capabilities = (FastVideoDevice_capabilities_t)spassthrough_capabilities,
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
