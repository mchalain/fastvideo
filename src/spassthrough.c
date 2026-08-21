#include <stddef.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <dlfcn.h>

#include "log.h"
#include "sconfig.h"
#include "fastvideo.h"
#include "spassthrough.h"
#include "sfile.h"
#include "sdmabuf.h"

#if defined(__ARM_NEON)
#if !defined(__aarch64__)
#define NEON_COPY 2
#else
#define NEON_COPY 1
#endif
#endif

static const char _spassthroughdir[] = "/tmp/fastvideo.spassthrough";
static const char spassthrough[] = "spassthrough";
static int spassthrough_loadjsonsettings(Passthrough_t *dev, void *entry);

#define MODE_SHOOT 0x01
#define MODE_SHOOTING 0x10
#define MODE_TEE 0x02
#define MODE_DRYRUN 0x04
#define MODE_COPY 0x08

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
	FrameBuffer_t *buffers;
	FrameBuffer_t *fifo;
	int state;
	struct
	{
		DeviceConf_t *config;
		void *dev;
		const FastVideoDevice_ops_t *ops;
	} branch;
	size_t (*copy)(void *, const char *const , char *, size_t, size_t);
	void *convert_ctx;
	struct
	{
		int state;
	} *controls;
};

static FastVideoList_t *g_Converts = NULL;
void spassthrough_convert_append(Convert_t *convert)
{
	g_Converts = fastvideolist_append(g_Converts, convert);
}

Convert_t *spassthrough_convert_next(Convert_t *convert)
{
	if (convert == NULL && g_Converts)
		fastvideolist_reset(g_Converts);
	convert = fastvideolist_next(g_Converts);
	return convert;
}

EXT_API int spassthrough_loadjsonconfiguration(void *arg, void *entry);

DeviceConf_t * spassthrough_createconfig(const char *name)
{
	Passthrough_config_t *config = calloc(1, sizeof(*config));
	config->parent.name = spassthrough;
	config->transfer.name = spassthrough;
#ifdef HAVE_JANSSON
	config->parent.ops.loadconfiguration = spassthrough_loadjsonconfiguration;
#endif
	return &config->parent;
}

#if NEON_COPY == 1
static size_t _neon_copy(void *dev, const char *const src, char *dst, size_t size, size_t stride)
{
	asm volatile (
		"1:                                               \n"
		"subs     %[size], %[size], #32                   \n"
		"ld1      {v0.16b, v1.16b}, [%[src]], #32         \n"
		"st1      {v0.16b, v1.16b}, [%[dst]], #32         \n"
		"b.gt     1b                                      \n"
		: [dst]"+r"(dst), [size]"+r"(size)
		: [src]"r"(src)
		: "v0", "v1", "cc", "memory"
	);

	return size;
}
#elif NEON_COPY == 2
static size_t _neon_copy(void *dev, const char *const src, char *dst, size_t size, size_t stride)
{
	/// [%[src]:256] for alignment on 256bits
	asm volatile (
		"1:                                               \n"
		"subs     %[size], %[size], #32                   \n"
		"vld1.u8  {d0, d1, d2, d3}, [%[src]:256]!         \n"
		"vst1.u8  {d0, d1, d2, d3}, [%[dst]:256]!         \n"
		"bgt      1b                                      \n"
		: [dst]"+r"(dst)
		: [src]"r"(src), [size]"r"(size)
		: "d0", "d1", "d2", "d3", "cc", "memory"
	);
	return size;
}
#endif

static size_t _default_copy(void *dev, const char *const src, char *dst, size_t size, size_t stride)
{
	memcpy(dst, src, size);
	return size;
}

static size_t _passthrough_copy(Passthrough_t *dev, FrameBuffer_t *src, FrameBuffer_t *dst, size_t bytesused)
{
	size_t expected = bytesused;
	if (dev->config->convert && dev->config->convert->resize.denominator > 0)
	{
		expected *= dev->config->convert->resize.numerator;
		expected /= dev->config->convert->resize.denominator;
	}
	if (dst->size < expected)
		return -1;
	size_t stride = bytesused;
	if (dev->config->convert && dev->config->convert->bpp > 0 && dev->config->parent.width > 0)
		stride = (size_t)dev->config->parent.width * dev->config->convert->bpp;
	void *srcmem = NULL;
	if (src->mem)
		srcmem = src->mem;
	if (src->dma_buf)
		sdmabuf_sync(src->dma_buf, 1);
	if (dst->dma_buf)
		sdmabuf_sync(dst->dma_buf, 1);
	bytesused = dev->copy(dev->convert_ctx, srcmem, dst->mem, bytesused, stride);
	if (dst->dma_buf)
		sdmabuf_sync(dst->dma_buf, 0);
	if (src->dma_buf)
		sdmabuf_sync(src->dma_buf, 0);
	return bytesused;
}

EXT_API void *spassthrough_create(const char *devicename, device_type_e type, Passthrough_config_t *config)
{
	Passthrough_t *dev = calloc(1, sizeof(*dev));
	dev->config = config;
	dev->name = devicename;
	dev->type = type;
	dev->controls = fastcontrols_create(_spassthroughdir, config->parent.name, sizeof(*dev->controls));
	warn("spassthrough: create %s", config->parent.name);
	if (type == device_control)
		return dev;
	if (dev->controls)
		dev->controls->state = 0;
	if (config && config->mode & MODE_COPY)
	{
		dev->copy = _default_copy;
		dev->convert_ctx = dev;
#if NEON_COPY
		if (scpu_check(SCPU_NEON))
		{
			dev->copy = _neon_copy;
		}
#endif
	}
	if (config && config->convert &&
		(config->convert->fourcc_in == 0 || config->convert->fourcc_in == config->parent.fourcc))
	{
		dev->convert_ctx = config->convert->ops.create(config);
		if (dev->convert_ctx)
		{
			dev->copy = config->convert->ops.convert;
			config->mode |= (config->convert->copy)?MODE_COPY:0;
		}
		else
			err("spassthrough: %s convert create failed", config->parent.name);
	}

	return dev;
}

EXT_API void *spassthrough_duplicate(Passthrough_t *dev, Passthrough_config_t **pconfig)
{
	Passthrough_config_t *config = *pconfig;
	if (dev->type == device_input)
	{
		err("spassthrough: %s bad device type", (config)?config->parent.name:"");
		return NULL;
	}
	Passthrough_t *dup = calloc(1, sizeof(*dup));
	dev->type = device_output;
	dup->type = device_input;
	dup->dup = dev;
	dev->dup = dup;
	*pconfig = dup->config = malloc(sizeof(*dev->config));
	memmove(dup->config, config, sizeof(*dev->config));
#if 0
	sconfig_mergedefinition(&dup->config->parent, &config->transfer);
#else
	if (config->transfer.fourcc)
		dup->config->parent.fourcc = config->transfer.fourcc;
	if (config->transfer.width)
		dup->config->parent.width = config->transfer.width;
	if (config->transfer.height)
		dup->config->parent.height = config->transfer.height;
	if (config->transfer.stride)
		dup->config->parent.stride = config->transfer.stride;
#endif
	/// only the main dev must manage the copy buffers, but dup dev contains the buffers
	dup->config->mode &= ~MODE_COPY;
	if (dev->config->branch.type != 0)
	{
		const FastVideoDevice_ops_t *opss[] = {
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
			devconfig = dev->branch.ops->createconfig("");
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
#ifdef HAVE_JANSSON
	return spassthrough_loadjsonsettings(dev, configentry);
#else
	return 0;
#endif
}

static int _passthrough_createbuffers(Passthrough_t *dev, int nmems, void **mems, int *dmabufs, size_t size, int copy)
{
	int ret = 0;
	dev->nbuffers = nmems;
	dev->buffers = calloc(nmems, sizeof(*dev->buffers));
	dev->size = size;
	void **tmems = NULL;
	int *tdmabufs = NULL;
	if (copy)
	{
		if (mems)
			tmems = calloc(nmems, sizeof(*mems));
		if (dmabufs)
			tdmabufs = calloc(nmems, sizeof(*dmabufs));
	}
	for (int i = 0; i < nmems; i++)
	{
		if (copy)
		{
			if (dmabufs &&
				(tdmabufs[i] = sdmabuf_create(spassthrough, size)) > 0)
			{
				dmabufs = tdmabufs;
				if (!tmems)
					tmems = calloc(nmems, sizeof(*tmems));
				tmems[i] = sdmabuf_map(tdmabufs[i], size, 1);
				mems = tmems;
			}
			else if (mems && (tmems[i] = malloc(size)) != NULL)
			{
				mems = tmems;
			}
			else
			{
				err("spassthrough: buffer allocation error %m");
				ret = -1;
				copy = 0;
				if (tdmabufs)
					free(tdmabufs);
				tdmabufs = NULL;
				if (tmems)
					free(tmems);
				tmems = NULL;
				size = 0;
			}
		}
		if (dmabufs)
			dev->buffers[i].dma_buf = dmabufs[i];
		if (mems)
			dev->buffers[i].mem = mems[i];
		dev->buffers[i].id = i;
		dev->buffers[i].size = size;
	}
	dev->mems = mems;
	dev->dmabufs = dmabufs;
	return ret;
}

static void _passthrough_freebuffers(Passthrough_t *dev)
{
	if (!dev->buffers)
		return;
	for (int i = 0; i < dev->nbuffers; i++)
	{
		if (dev->buffers[i].dma_buf && dev->buffers[i].mem &&
			dev->buffers[i].mem != (void *)(long)-1)
			sdmabuf_unmap(dev->buffers[i].mem, dev->buffers[i].size);
	}
	free(dev->buffers);
	dev->buffers = NULL;
	dev->nbuffers = 0;
}

EXT_API int spassthrough_requestbuffer(Passthrough_t *dev, enum buf_type_e t, ...)
{
	int ret = -1;
	Passthrough_config_t *config = dev->config;
	if (config->mode & MODE_COPY && !dev->dup)
	{
		err("spassthrough: copy mode is allowed in transfer mode");
		config->mode &= ~MODE_COPY;
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
			int ntargets = va_arg(ap, int);
			void **targets = va_arg(ap, void **);
			size_t size = va_arg(ap, size_t);
			/**
			 * We need buffers in the first dev to allow the DRYRUN state.
			 */
			ret = 0;
			if (dev->buffers == NULL)
				_passthrough_createbuffers(dev, ntargets, targets, NULL, size, 0);
			/**
			 * create buffers for the output dev
			 */
			if (config->convert && config->convert->resize.denominator > 0)
			{
				size *= config->convert->resize.numerator;
				size /= config->convert->resize.denominator;
			}
			if (dev->dup &&
				_passthrough_createbuffers(dev->dup, ntargets, targets, NULL, size,
					(dev->config->mode & MODE_COPY)) < 0)
			{
				ret = -1;
				_passthrough_freebuffers(dev);
				break;
			}
			if (dev->type == device_input && dev->branch.dev)
			{
				dev->branch.ops->destroy(dev->branch.dev);
				dev->branch.dev = NULL;
			}
		}
		break;
		case (buf_type_memory_master):
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
			int ntargets = va_arg(ap, int);
			int *targets = va_arg(ap, int *);
			size_t size = va_arg(ap, size_t);
			ret = 0;
			if (dev->buffers == NULL)
				_passthrough_createbuffers(dev, ntargets, NULL, targets, size, 0);
			for (int i = 0; i < ntargets; i++)
			{
				dev->buffers[i].mem = sdmabuf_map(dev->buffers[i].dma_buf, size, 0); /// the write argument should be 0
				if (dev->buffers[i].mem == (void *)(long)-1)
				{
					err("spassthrough: impossible to map the inpur buffer");
					ret = -1;
					break;
				}
			}
			if (ret < 0)
			{
				_passthrough_freebuffers(dev);
				break;
			}
			/// prepare buffers for output stream.
			if (config->convert && config->convert->resize.denominator > 0)
			{
				size *= config->convert->resize.numerator;
				size /= config->convert->resize.denominator;
			}
			if (dev->dup &&
				_passthrough_createbuffers(dev->dup, ntargets, NULL, targets, size,
					(dev->config->mode & MODE_COPY)) < 0)
			{
				ret = -1;
				_passthrough_freebuffers(dev);
				break;
			}
			if (dev->type == device_input && dev->branch.dev)
			{
				dev->branch.ops->destroy(dev->branch.dev);
				dev->branch.dev = NULL;
			}
		}
		break;
		case buf_type_dmabuf_master:
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
	if (dev->copy && dev->dup->buffers[0].size == 0)
	{
		/// disable copy mode on buffer allocation error
		err("spassthrough: copy disabling");
		dev->config->mode &= ~MODE_COPY;
		dev->copy = NULL;
	}
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
	FrameBuffer_t *buffer = dev->fifo;
	if (buffer == NULL || buffer->state != ready)
	{
		errno = EAGAIN;
		return -1;
	}
	if (dev->branch.dev && dev->state & MODE_TEE)
	{
		dev->branch.ops->dequeue(dev->branch.dev, mem, bytesused, NULL);
	}
	buffer->state = dequeued;
	/** the real fifo is useless as the entry is immediately pushed **/
#if 0
	while (buffer->next) buffer = buffer->next;
	if (buffer->previous)
		buffer->previous->next = NULL;
	buffer->previous = NULL;
#endif

	if (bytesused)
		*bytesused = buffer->bytesused;
	if (mem)
		*mem = buffer->mem;
	if (flags)
		*flags = buffer->flags;

	if ((dev->state & MODE_SHOOTING) && dev->dup != NULL)
	{
		dev->state |= MODE_DRYRUN;
		dev->state &= ~MODE_SHOOTING;
	}
	return buffer->id;
}

EXT_API int spassthrough_queue(Passthrough_t *dev, int index, void *mem, size_t bytesused, int flags)
{
	if (dev->controls && dev->controls->state)
	{
		dev->state |= dev->controls->state;
		dev->controls->state = 0;
	}
	if ((dev->state & MODE_SHOOT) && dev->dup != NULL)
	{
		dev->state &= ~MODE_DRYRUN;
		dev->state &= ~MODE_SHOOT;
		dev->state |= MODE_SHOOTING;
	}
	if (dev->branch.dev && dev->state & MODE_TEE)
	{
		dev->branch.ops->queue(dev->branch.dev, index, mem, bytesused, 0);
	}
	if (dev->copy)
	{
		if (mem && !dev->buffers[index].mem)
			dev->buffers[index].mem = mem;
		bytesused = _passthrough_copy(dev, &dev->buffers[index], &dev->dup->buffers[index], bytesused);
	}
	if (!(dev->state & MODE_DRYRUN) && dev->dup != NULL)
	{
		dev = dev->dup;
	}
	FrameBuffer_t *buffer = &dev->buffers[index];
	if (mem)
		buffer->mem = mem;
	buffer->bytesused = bytesused;
	buffer->flags = flags;
	buffer->state = ready;
#if 0
	/** prepare fifo's items **/
	buffer->next = dev->fifo;
	if (dev->fifo)
		dev->fifo->previous = buffer;
#endif
	/** insert into fifo **/
	dev->fifo = buffer;
	return 0;
}

static void _passthrough_freedmabuf(Passthrough_t *dev)
{
	if (dev->dup && dev->dup->buffers)
	{
		for (int i = 0; i < dev->dup->nbuffers; i++)
		{
			sdmabuf_destroy(dev->dup->buffers[i].dma_buf);
		}
	}
}

EXT_API void spassthrough_destroy(Passthrough_t *dev)
{
	if (dev->type == device_input && dev->branch.dev)
	{
		dev->branch.ops->destroy(dev->branch.dev);
	}
	for (int i = 0; i < dev->nbuffers; i++)
	{
		if (dev->buffers[i].mem)
			sdmabuf_unmap(dev->buffers[i].mem, dev->buffers[i].size);
	}
	free(dev->buffers);
	if (dev->config)
	{
		if (dev->config->mode & MODE_COPY)
		{
			_passthrough_freedmabuf(dev);
		}
		if (dev->config->convert && dev->convert_ctx)
			dev->config->convert->ops.destroy(dev->convert_ctx);
		if (dev->config->libraryhdl)
			dlclose(dev->config->libraryhdl);
		free(dev->config);
	}
#if 0
	/**
	 * currently this member may contain local buffers info or the pipe client
	 * buffers information
	 */
	if (dev->mems)
		free(dev->mems);
	if (dev->dmabufs)
		free(dev->dmabufs);
#endif
	if (dev->controls)
		fastcontrols_destroy(dev->controls);
	free(dev);
}

#ifdef HAVE_JANSSON
static int _passthrough_loadstate(Passthrough_t *dev, json_t *jconfig)
{
	if (json_is_object(jconfig))
	{
		json_t *jdryrun = json_object_get(jconfig, "dryrun");
		if (jdryrun && json_is_true(jdryrun))
			dev->controls->state |= MODE_DRYRUN;
		else if (jdryrun)
			dev->controls->state &= ~MODE_DRYRUN;
		json_t *jshoot = json_object_get(jconfig, "shoot");
		if (jshoot && json_is_true(jshoot))
			dev->controls->state |= MODE_SHOOT;
		else if (jshoot)
			dev->controls->state &= ~MODE_SHOOT;
		json_t *jtee = json_object_get(jconfig, "tee");
		if (jtee && json_is_true(jtee))
			dev->controls->state |= MODE_TEE;
		else if (jtee)
			dev->controls->state &= ~MODE_TEE;
	}
	if (json_is_string(jconfig))
	{
		const char *value = json_string_value(jconfig);
		if (!strcmp(value, "dryrun"))
			dev->controls->state |= MODE_DRYRUN;
		else if (!strcmp(value, "shoot"))
			dev->controls->state |= MODE_SHOOT;
		else if (!strcmp(value, "tee"))
			dev->controls->state |= MODE_TEE;
	}
	dev->state = dev->controls->state;
	return 0;
}

static int spassthrough_loadjsonsettings(Passthrough_t *dev, void *entry)
{
	json_t *jconfig = entry;
	if (dev->controls == NULL)
		return 0;
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
	json_t *definition = json_object_get(jconfig, "definition");
	sconfig_loaddefinition(&config->parent, definition);

	json_t *transfer = json_object_get(jconfig, "transfer");
	sconfig_loaddefinition(&config->transfer, transfer);
	if (config->transfer.width == 0)
		config->transfer.width = config->parent.width;
	if (config->transfer.height == 0)
		config->transfer.height = config->parent.height;
	if (config->transfer.fourcc == 0)
		config->transfer.fourcc = config->parent.fourcc;
	if (config->transfer.modifiers == 0)
		config->transfer.modifiers = config->parent.modifiers;

	json_t *branch = json_object_get(jconfig, "branch");
	config->branch.entry = branch;
	if (branch && json_is_object(branch))
	{
		json_t *name = json_object_get(branch, "name");
		config->branch.name = json_string_value(name);
		json_t *type = json_object_get(branch, "type");
		config->branch.type = json_string_value(type);
	}

	json_t *jcopy = json_object_get(jconfig, "copy");
	if (jcopy && json_is_true(jcopy))
		config->mode |= MODE_COPY;

	json_t *convert = json_object_get(jconfig, "convert");
	if (convert && json_is_object(convert))
	{
		json_t *library = json_object_get(convert, "library");
		if (library && json_is_string(library))
		{
			config->libraryhdl = dlopen(json_string_value(library), RTLD_NOW);
			json_decref(library);
		}
		convert = json_object_get(convert, "name");
	}
	if (convert && json_is_string(convert))
	{
		const char *value = json_string_value(convert);

		for (Convert_t *convert = spassthrough_convert_next(NULL);
			convert != NULL; convert = spassthrough_convert_next(convert))
		{
			if (! strcmp(value, convert->name))
			{
				config->convert = convert;
				if (!config->transfer.fourcc)
					config->transfer.fourcc = convert->fourcc_out;
				if (convert->copy)
					config->mode |= MODE_COPY;
				break;
			}
		}
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
	json_object_set_new(capabilities, "controls", controls);
	json_t *branch = json_object();
	json_object_set_new(branch, "name", json_string("enc-h264"));
	json_object_set_new(branch, "type", json_string("v4l2"));
	json_object_set_new(capabilities, "branch", branch);
	json_object_set_new(capabilities, "copy", json_false());
	json_t *convert = json_object();
	json_object_set_new(convert, "name", json_string("rgba"));
	json_object_set_new(convert, "library", json_string("/usr/lib/libsconvert_rgba.so"));
	json_object_set_new(capabilities, "convert", convert);
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
#endif
