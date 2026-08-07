#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <stdarg.h>
#include <dlfcn.h>
#include <time.h>

#include <jansson.h>

#include "fastvideo.h"
#include "sconfig.h"
#include "log.h"
#include "sdmabuf.h"
#include "spassthrough.h"

/*
 * Splits a full video frame into several tiles
 */

static const char stile_name[] = "tile";

typedef struct STileBuffer_s STileBuffer_t;
struct STileBuffer_s
{
	int id;
	void *mem;
	int dmabuf;
	size_t size;
	size_t bytesused;
	enum { STile_free_e, STile_fill_e } state;
};

typedef struct STileConf_s STileConf_t;
struct STileConf_s
{
	union {
		struct {
			DeviceConf_t parent;
			DeviceConf_t transfer;
		};
		Passthrough_config_t passconfig;
	};
	int ntiles;
};

typedef struct STile_s STile_t;
struct STile_s
{
	device_type_e type;
	STileConf_t *config;
	STile_t *dup;

	uint32_t src_width;
	uint32_t src_height;
	uint32_t dst_width;
	uint32_t dst_height;
	uint32_t ntiles_x;
	uint32_t ntiles_y;
	uint32_t ntiles;
	uint32_t tile_xoff;
	uint32_t tile_yoff;
	int using_dmabuf;

	int nbuffers;
	STileBuffer_t *buffers;
	void **mems;
	int *dmabufs;
	int curbufferid;
	int writeid;
	int readid;
	int pending;

	size_t (*copy)(STile_t *dev, const char *src, char *dst, size_t stride);
	const Convert_t *copy_conv;
	void *copy_ctx;
	size_t tile_out_bytes;

	size_t frame_count;
	struct timespec last_report;
};

EXT_API int stile_loadjsonconfiguration(void *arg, void *entry);

EXT_API DeviceConf_t *stile_createconfig(const char *name)
{
	STileConf_t *config = calloc(1, sizeof(*config));
	config->parent.name = stile_name;
	config->parent.ops.loadconfiguration = stile_loadjsonconfiguration;
	return &config->parent;
}

static size_t copy_convert(STile_t *dev, const char *src, char *dst, size_t stride)
{
	return dev->copy_conv->ops.convert(dev->copy_ctx, src, dst, stride, stride);
}

/* plain pixel copy, no format conversion - used instead of copy_convert
 * when the selected Convert_t is a pure passthrough (fourcc_in ==
 * fourcc_out): calling into ops.convert() would do nothing but a memcpy
 * anyway, so skip that indirection */
static size_t copy_pixel(STile_t *dev, const char *src, char *dst, size_t stride)
{
	memcpy(dst, src, stride);
	return stride;
}

EXT_API void *stile_create(const char *devicename, device_type_e type, STileConf_t *config)
{
	if (!config)
		return NULL;
	Passthrough_config_t *passconfig = &config->passconfig;
	if (type != device_transfer)
	{
		err("stile: %s bad device type", config ? config->parent.name : "");
		return NULL;
	}
	if (!config->parent.width || !config->parent.height)
	{
		err("stile: %s unknown source size", config->parent.name);
		return NULL;
	}
	if (!passconfig->convert)
	{
		err("stile: %s no tile-copy converter configured (\"convert\" JSON field)", config->parent.name);
		return NULL;
	}

	STile_t *dev = calloc(1, sizeof(*dev));
	dev->type = type;
	dev->config = config;
	dev->curbufferid = -1;
	dev->src_width = config->parent.width;
	dev->src_height = config->parent.height;
	dev->ntiles_x = 2;
	dev->ntiles_y = 1;
	dev->dst_width = dev->src_width / dev->ntiles_x;
	dev->dst_height = dev->src_height / dev->ntiles_y;
	if (config->transfer.width > 0)
	{
		dev->dst_width = config->transfer.width;
		dev->ntiles_x = dev->src_width / dev->dst_width;
		dev->tile_xoff = (dev->src_width - dev->ntiles_x * dev->dst_width) / 2;
	}
	if (config->transfer.height > 0)
	{
		dev->dst_height = config->transfer.height;
		dev->ntiles_y = dev->src_height / dev->dst_height;
		dev->tile_yoff = (dev->src_height - dev->ntiles_y * dev->dst_height) / 2;
	}
	dev->ntiles = dev->ntiles_x * dev->ntiles_y;
	if (dev->ntiles == 0)
	{
		err("stile: %s source %ux%u smaller than tile %ux%u",
			config->parent.name, dev->src_width, dev->src_height, dev->dst_width, dev->dst_height);
		free(dev);
		return NULL;
	}
	if (config->ntiles && config->ntiles < dev->ntiles && config->ntiles < dev->ntiles_x)
	{
		dev->ntiles_x = config->ntiles;
		dev->ntiles_y = 1;
	}

	/* real config passed (not NULL): a converter that needs to inspect
	 * config->parent.fourcc to pick its actual behaviour (e.g.
	 * BG10toR16's bayer order) can now do so correctly, and cleanly
	 * rejects a fourcc it doesn't support by returning NULL here instead
	 * of being called blind and crashing */
	dev->copy_conv = passconfig->convert;
	dev->copy_ctx = dev->copy_conv->ops.create(passconfig);
	if (!dev->copy_ctx)
	{
		err("stile: %s tile-copy converter '%s' rejected fourcc %.4s",
			config->parent.name, dev->copy_conv->name, (char *)&config->parent.fourcc);
		free(dev);
		return NULL;
	}
	dev->copy = copy_convert;
	if (dev->copy_conv->fourcc_in != 0 && dev->copy_conv->fourcc_in == dev->copy_conv->fourcc_out)
		dev->copy = copy_pixel;

	size_t tile_in_bytes = (size_t)dev->dst_width * dev->dst_height * dev->copy_conv->bpp;
	dev->tile_out_bytes = tile_in_bytes;
	if (dev->copy_conv->resize.denominator > 0)
	{
		dev->tile_out_bytes = tile_in_bytes * dev->copy_conv->resize.numerator
			/ dev->copy_conv->resize.denominator;
	}

	clock_gettime(CLOCK_MONOTONIC, &dev->last_report);

	warn("stile: device %s using '%s' tile copy (%zu bytes/tile)",
		config->parent.name, dev->copy_conv->name, dev->tile_out_bytes);
	warn("stile: device %s ready, %ux%u source -> %u tile(s) of %ux%u",
		config->parent.name, dev->src_width, dev->src_height, dev->ntiles, dev->dst_width, dev->dst_height);
	return dev;
}

EXT_API void *stile_duplicate(STile_t *dev, STileConf_t **pconfig)
{
	if (dev->type != device_transfer)
	{
		err("stile: device may not be duplicated");
		return NULL;
	}
	STile_t *dup = calloc(1, sizeof(*dup));
	dup->type = device_input;
	dup->curbufferid = -1;
	dup->ntiles = dev->ntiles;
	dup->tile_out_bytes = dev->tile_out_bytes;
	dev->dup = dup;

	STileConf_t *config = calloc(1, sizeof(*config));
	memcpy(config, dev->config, sizeof(*config));
	config->parent.fourcc = config->transfer.fourcc ? config->transfer.fourcc : dev->copy_conv->fourcc_out;
	config->parent.width = dev->dst_width;
	config->parent.height = dev->dst_height;
	config->parent.stride = 0;
	dup->config = config;
	*pconfig = config;

	return dup;
}

static void _stile_createtilebuffers(STile_t *dup, int nframes, uint32_t ntiles)
{
	int nslots = nframes * (int)ntiles;
	size_t tile_bytes = dup->tile_out_bytes;
	dup->nbuffers = nslots;
	dup->buffers = calloc(nslots, sizeof(*dup->buffers));
	dup->mems = calloc(nslots, sizeof(*dup->mems));

	/*
	 * try to back the whole ring with dma_buf so the next stage (e.g.
	 * shailo10h.cpp) can import it via buf_type_dmabuf instead of a plain
	 * host-memory copy - HailoRT can then DMA straight from these tiles.
	 * All-or-nothing: fall back to malloc for the entire ring the moment
	 * the first allocation fails (no dma-heap on this system/permission
	 * issue), rather than mixing buffer kinds within one ring.
	 */
	int fd0 = sdmabuf_create(stile_name, tile_bytes);
	int use_dmabuf = (fd0 > 0);
	if (use_dmabuf)
		dup->dmabufs = calloc(nslots, sizeof(*dup->dmabufs));

	for (int i = 0; i < nslots; i++)
	{
		dup->buffers[i].id = i;
		dup->buffers[i].size = tile_bytes;
		if (use_dmabuf)
		{
			int fd = (i == 0) ? fd0 : sdmabuf_create(stile_name, tile_bytes);
			if (fd <= 0)
			{
				err("stile: dma_buf allocation failed for tile slot %d, falling back to malloc for the whole ring", i);
				use_dmabuf = 0;
				free(dup->dmabufs);
				dup->dmabufs = NULL;
				/* re-do already-allocated slots as malloc below */
				for (int j = 0; j < i; j++)
				{
					sdmabuf_unmap(dup->buffers[j].mem, tile_bytes);
					sdmabuf_destroy(dup->buffers[j].dmabuf);
					dup->buffers[j].dmabuf = 0;
					dup->buffers[j].mem = malloc(tile_bytes);
					dup->mems[j] = dup->buffers[j].mem;
				}
				dup->buffers[i].mem = malloc(tile_bytes);
			}
			else
			{
				dup->buffers[i].dmabuf = fd;
				dup->buffers[i].mem = sdmabuf_map(fd, tile_bytes, 1);
				dup->dmabufs[i] = fd;
			}
		}
		else
		{
			dup->buffers[i].mem = malloc(tile_bytes);
		}
		dup->mems[i] = dup->buffers[i].mem;
	}
	if (use_dmabuf)
		warn("stile: tile ring backed by %d dma_buf(s), %zu bytes each", nslots, tile_bytes);
	dup->writeid = 0;
	dup->readid = 0;
	dup->pending = 0;
}

EXT_API int stile_requestbuffer(STile_t *dev, enum buf_type_e t, ...)
{
	int ret = -1;
	va_list ap;
	va_start(ap, t);
	switch (t)
	{
		case buf_type_memory:
		{
			/* main dev: the GPU stage exports its buffers, we are the slave */
			int ntargets = va_arg(ap, int);
			void **targets = va_arg(ap, void **);
			size_t size = va_arg(ap, size_t);
			dev->nbuffers = ntargets;
			dev->buffers = calloc(ntargets, sizeof(*dev->buffers));
			for (int i = 0; i < ntargets; i++)
			{
				dev->buffers[i].id = i;
				dev->buffers[i].mem = targets[i];
				dev->buffers[i].size = size;
			}
			if (dev->dup)
				_stile_createtilebuffers(dev->dup, ntargets, dev->ntiles);
			ret = 0;
		}
		break;
		case buf_type_dmabuf:
		{
			int ntargets = va_arg(ap, int);
			int *targets = va_arg(ap, int *);
			size_t size = va_arg(ap, size_t);
			dev->nbuffers = ntargets;
			dev->buffers = calloc(ntargets, sizeof(*dev->buffers));
			dev->using_dmabuf = 1;
			ret = 0;
			for (int i = 0; i < ntargets; i++)
			{
				dev->buffers[i].id = i;
				dev->buffers[i].size = size;
				dev->buffers[i].dmabuf = targets[i];
				dev->buffers[i].mem = sdmabuf_map(targets[i], size, 0);
				if (dev->buffers[i].mem == (void *)(long)-1)
				{
					err("stile: impossible to map input buffer %d", targets[i]);
					ret = -1;
					break;
				}
			}
			if (dev->dup)
				_stile_createtilebuffers(dev->dup, ntargets, dev->ntiles);
		}
		break;
		case buf_type_memory_master:
		{
			/* dup dev: export our own tile buffers to the next stage */
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
				*size = dev->tile_out_bytes;
			ret = 0;
		}
		break;
		case buf_type_dmabuf_master:
		{
			/* dup dev: export our own dma_buf-backed tile buffers, so the
			 * next stage (e.g. shailo10h.cpp) can import them as buf_type_dmabuf
			 * instead of a plain host-memory pointer. Only advertised when
			 * the ring actually got dma_buf-backed (see
			 * _stile_createtilebuffers); returning -1 here makes
			 * fastvideo.c's negotiation ladder fall back to
			 * buf_type_memory_master transparently. */
			if (!dev->buffers || !dev->dmabufs)
				break;
			int *ntargets = va_arg(ap, int *);
			int **targets = va_arg(ap, int **);
			size_t *size = va_arg(ap, size_t *);
			if (ntargets != NULL)
				*ntargets = dev->nbuffers;
			if (targets != NULL)
				*targets = dev->dmabufs;
			if (size != NULL)
				*size = dev->tile_out_bytes;
			ret = 0;
		}
		break;
		default:
			err("stile: unknown buffer type %d", t);
	}
	va_end(ap);
	return ret;
}

EXT_API int stile_fd(STile_t *dev, int writer)
{
	return -1;
}

EXT_API int stile_start(STile_t *dev)
{
	return 0;
}

EXT_API int stile_stop(STile_t *dev)
{
	return 0;
}

EXT_API int stile_queue(STile_t *dev, int id, void *mem, size_t bytesused, int flags)
{
	if (dev->type == device_input)
	{
		/* buffer hand-back from the downstream stage's reverse pass
		 * (main_transferbuffer's recycling call) - our own ring bookkeeping
		 * in dequeue() already tracks free/filled slots, nothing to do. */
		return 0;
	}
	if (dev->type != device_transfer)
	{
		err("stile: bad device for queue");
		return -1;
	}
	if (id < 0 || id >= dev->nbuffers)
	{
		err("stile: unknown buffer id %d", id);
		return -1;
	}
	if (!mem)
		mem = dev->buffers[id].mem;
	int src_dmabuf = dev->using_dmabuf ? dev->buffers[id].dmabuf : 0;

	STile_t *dup = dev->dup;
	if (dup && dup->nbuffers > 0)
	{
		const uint8_t *src = (const uint8_t *)mem;
		uint32_t srcw = dev->src_width;
		if (src_dmabuf)
			sdmabuf_sync(src_dmabuf, 1);
		for (uint32_t ty = 0; ty < dev->ntiles_y; ty++)
		{
			for (uint32_t tx = 0; tx < dev->ntiles_x; tx++)
			{
				uint32_t x0 = tx * dev->dst_width;
				uint32_t y0 = dev->tile_yoff + ty * dev->dst_height;
				int slot = dup->writeid % dup->nbuffers;
				uint8_t *d = (uint8_t *)dup->buffers[slot].mem;
				if (dup->buffers[slot].dmabuf)
					sdmabuf_sync(dup->buffers[slot].dmabuf, 1);
				size_t stride = dev->dst_width * dev->copy_conv->bpp;
				for (uint32_t row = 0; row < dev->dst_height; row++)
				{
					const uint8_t *s = src + ((size_t)(y0 + row) * srcw + x0) * dev->copy_conv->bpp;
					size_t written = dev->copy(dev,	(const char *)s, (char *)d, stride);
					d += written;
				}
				if (dup->buffers[slot].dmabuf)
					sdmabuf_sync(dup->buffers[slot].dmabuf, 0);
				dup->buffers[slot].bytesused = dev->tile_out_bytes;
				dup->buffers[slot].state = STile_fill_e;
				dup->writeid++;
				dup->pending++;
				if (dup->pending > dup->nbuffers)
				{
					err("stile: tile ring overrun (consumer too slow), dropping oldest tile");
					dup->pending = dup->nbuffers;
				}
			}
		}
		if (src_dmabuf)
			sdmabuf_sync(src_dmabuf, 0);
	}

	dev->curbufferid = id;

	dev->frame_count++;
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	long elapsed_ms = (now.tv_sec - dev->last_report.tv_sec) * 1000
		+ (now.tv_nsec - dev->last_report.tv_nsec) / 1000000;
	if (elapsed_ms >= 1000)
	{
		warn("stile: %s real capture throughput: %zu frames/s (%zu tiles/s)",
			dev->config->parent.name, dev->frame_count, dev->frame_count * dev->ntiles);
		dev->frame_count = 0;
		dev->last_report = now;
	}
	return 0;
}

EXT_API int stile_dequeue(STile_t *dev, void **mem, size_t *bytesused, int *flags)
{
	errno = 0;
	if (dev->type == device_input)
	{
		if (dev->pending <= 0)
		{
			errno = EAGAIN;
			return -1;
		}
		int slot = dev->readid % dev->nbuffers;
		STileBuffer_t *buffer = &dev->buffers[slot];
		if (mem)
			*mem = buffer->mem;
		if (bytesused)
			*bytesused = buffer->bytesused;
		if (flags)
			*flags = 0;
		buffer->state = STile_free_e;
		dev->readid++;
		dev->pending--;
		return slot;
	}

	if (dev->curbufferid == -1)
	{
		errno = EAGAIN;
		return -1;
	}
	int id = dev->curbufferid;
	dev->curbufferid = -1;
	if (mem)
		*mem = dev->buffers[id].mem;
	if (bytesused)
		*bytesused = dev->buffers[id].size;
	return id;
}

EXT_API void stile_destroy(STile_t *dev)
{
	if (dev->type == device_transfer)
	{
		if (dev->copy_conv && dev->copy_conv->ops.destroy)
			dev->copy_conv->ops.destroy(dev->copy_ctx);
		if (dev->using_dmabuf)
		{
			for (int i = 0; i < dev->nbuffers; i++)
				if (dev->buffers[i].mem)
					sdmabuf_unmap(dev->buffers[i].mem, dev->buffers[i].size);
		}
	}
	else
	{
		for (int i = 0; i < dev->nbuffers; i++)
		{
			if (dev->dmabufs)
			{
				if (dev->buffers[i].mem)
					sdmabuf_unmap(dev->buffers[i].mem, dev->buffers[i].size);
				if (dev->buffers[i].dmabuf)
					sdmabuf_destroy(dev->buffers[i].dmabuf);
			}
			else
			{
				free(dev->buffers[i].mem);
			}
		}
		if (dev->mems)
			free(dev->mems);
		if (dev->dmabufs)
			free(dev->dmabufs);
	}
	if (dev->buffers)
		free(dev->buffers);
	if (dev->config)
		free(dev->config);
	free(dev);
}

EXT_API int stile_loadjsonconfiguration(void *arg, void *entry)
{
	json_t *jconfig = entry;
	Passthrough_config_t *passconfig = (Passthrough_config_t *)arg;
	STileConf_t *config = (STileConf_t *)arg;

	json_t *definition = json_object_get(jconfig, "definition");
	sconfig_loaddefinition(&config->parent, definition);

	json_t *transfer = json_object_get(jconfig, "transfer");
	sconfig_loaddefinition(&config->transfer, transfer);

	/* same "convert" JSON model as spassthrough.c: either
	 * {"convert": "Name"} or {"convert": {"name": "Name"}} - resolved to
	 * a Convert_t* right away so stile_create() can pass the real config
	 * to ops.create() (see there for why that matters) */
	json_t *convert = json_object_get(jconfig, "convert");
	if (convert && json_is_object(convert))
		convert = json_object_get(convert, "name");
	if (convert && json_is_string(convert))
	{
		const char *value = json_string_value(convert);
		for (Convert_t *conv = spassthrough_convert_next(NULL); conv != NULL; conv = spassthrough_convert_next(conv))
		{
			if (conv->name && !strcmp(value, conv->name))
			{
				passconfig->convert = conv;
				break;
			}
		}
	}
	json_t *ntiles = json_object_get(jconfig, "ntiles");
	if (ntiles && json_is_integer(ntiles))
	{
		config->ntiles = json_integer_value(ntiles);
	}
	return 0;
}

static const FastVideoDevice_ops_t stile_ops = {
	.name = "tile",
	.createconfig = stile_createconfig,
	.create = (FastVideoDevice_create_t)stile_create,
	.duplicate = (FastVideoDevice_duplicate_t)stile_duplicate,
	.loadsettings = (FastVideoDevice_loadsettings_t)NULL,
	.requestbuffer = (FastVideoDevice_requestbuffer_t)stile_requestbuffer,
	.eventfd = (FastVideoDevice_eventfd_t)stile_fd,
	.start = (FastVideoDevice_start_t)stile_start,
	.stop = (FastVideoDevice_stop_t)stile_stop,
	.dequeue = (FastVideoDevice_dequeue_t)stile_dequeue,
	.queue = (FastVideoDevice_queue_t)stile_queue,
	.destroy = (FastVideoDevice_destroy_t)stile_destroy,
};

static void __attribute__ ((constructor)) stile_init()
{
	fastvideodevice_ops_append_t _fastvideodevice_ops_append;
	void *hdl = dlopen(NULL, RTLD_NOW);
	_fastvideodevice_ops_append = dlsym(hdl, "fastvideodevice_ops_append");
	if (_fastvideodevice_ops_append)
	{
		_fastvideodevice_ops_append(&stile_ops);
	}
}
