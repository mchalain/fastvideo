#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/dma-buf.h>
#include <errno.h>

#ifdef HAVE_JANSSON
#include <jansson.h>
#endif

#include "sfile.h"
#include "sconfig.h"
#include "sdmabuf.h"
#include "log.h"

#define MAX_BUFFERS 4
#define DEFAULT_PORT 1024

extern const Proto_t proto_file;

static const char str_rgba[] = "RGB_ALPHA";
static const char str_rgb[] = "RGB";
static const char str_cmyk[] = "CMYK";

struct File_s
{
	FileConfig_t *config;
	const char *path;
	void *ctx;
	const Proto_t *ops;
	uint32_t width;
	uint32_t height;
	uint32_t fourcc;
	device_type_e type;
	uint8_t	bpp;
	uint8_t nbuffers;
	FrameBuffer_t *buffers;
	int lastbufferid;
	char header[128];
	size_t headerlen;
};

EXT_API int sfile_queue(File_t *dev, int index, void *mem, size_t bytesused, int flags);

EXT_API File_t * sfile_create(const char *filename, device_type_e type, FileConfig_t *config)
{
	if (type != device_input && type != device_output)
	{
		err("sfile: support only input or output devices");
		return NULL;
	}
	const Proto_t *ops = &proto_file;
	if (!config)
	{
		err("sfile: need a configuration");
		return NULL;
	}
	if (config && config->proto)
		ops = config->proto;
	void *ctx = ops->create(&config->protoconf);
	if (ctx == NULL)
		return NULL;

	File_t *dev = calloc(1, sizeof(*dev));
	dev->config = config;
	dev->ctx = ctx;
	dev->ops = ops;
	dev->type = type;
	dev->path = config->filename;
	dev->width = config->parent.width;
	dev->height = config->parent.height;
	dev->fourcc = config->parent.fourcc;
	dev->bpp = dev->width ? (config->parent.stride / dev->width) : 0;
	if (strstr(dev->path, ".pam") != NULL)
		config->header = File_PAM_e;
	if (type == device_output)
	{
		switch (config->header)
		{
			case File_PAM_e:
			{
				const char *format;
				if (dev->fourcc == FOURCC_XB24)
					format = str_rgba;
				if (dev->fourcc == FOURCC_RGB3)
					format = str_rgb;
				if (dev->fourcc == FOURCC_YUYV)
					format = str_cmyk;
				/// add TIFF header for other fourcc
				dev->headerlen = snprintf(dev->header, sizeof(dev->header),
					"P7 WIDTH %.4d HEIGHT %.4d DEPTH %.1d MAXVAL 255 TUPLTYPE %s ENDHDR",
					dev->width, dev->height, dev->bpp, format);
			}
			break;
			default:
		}
		warn("sfile: %s opened for %ux%u %.4s", config->filename, dev->width, dev->height, (const char*)&dev->fourcc);
	}
	if (type == device_input)
	{
		ops->connect(dev->ctx);
		switch (config->header)
		{
			case File_PAM_e:
			{
				/// add TIFF header for other fourcc
				char format[16] = {0};
				int ret = ops->recv(dev->ctx, dev->header, sizeof(dev->header), 0);
				dev->headerlen = sscanf(dev->header,
					"P7 WIDTH %d HEIGHT %d DEPTH %d MAXVAL 255 TUPLTYPE %s ENDHDR",
					&dev->width, &dev->height, &dev->bpp, format);
				if (!strncasecmp(format, "RGB_ALPHA", 16))
					dev->fourcc = FOURCC_XB24;
				else if (!strncasecmp(format, "RGB", 16))
				{
					if (dev->bpp == 2)
						dev->fourcc = FOURCC_RG565;
					if (dev->bpp == 3)
						dev->fourcc = FOURCC_RGB3;
				}
				else if (!strncasecmp(format, "CMYK", 16))
				{
					if (dev->bpp == 2)
						dev->fourcc = FOURCC_YUYV;
				}
			}
			break;
			default:
		}
		config->parent.width = dev->width;
		config->parent.height = dev->height;
		config->parent.fourcc = dev->fourcc;
		config->parent.stride = dev->width * dev->bpp;

		warn("sfile: %s opened for %ux%u %.4s", config->filename, dev->width, dev->height, (const char*)&dev->fourcc);

		dev->buffers = calloc(MAX_BUFFERS, sizeof(FrameBuffer_t));
		size_t size = dev->width;
		size *= dev->height;
		size *= dev->bpp;
		for (int i = 0; i < MAX_BUFFERS; i++, dev->nbuffers++)
		{
			int dma_buf = sdmabuf_create("sfile", size);
			if (dma_buf < 0)
				dev->buffers[i].mem = calloc(1, size);
			else
				dev->buffers[i].mem = sdmabuf_map(dma_buf, size, 1);
			if (!dev->buffers[i].mem)
			{
				err("sfile: buffer allocation error %m");
				break;
			}
			dev->buffers[i].dma_buf = dma_buf;
			dev->buffers[i].id = i;
			dev->buffers[i].size = size;
			dev->buffers[i].bpp = dev->bpp;
		}

	}

	return dev;
}

EXT_API int sfile_requestbuffer(File_t *dev, enum buf_type_e t, ...)
{
	int ret = 0;
	va_list ap;
	va_start(ap, t);
	switch (t)
	{
		case buf_type_memory:
		{
			int nmem = va_arg(ap, int);
			void **mems = va_arg(ap, void **);
			size_t size = va_arg(ap, size_t);
			FrameBuffer_t *buffers = NULL;
			buffers = calloc(nmem, sizeof(FrameBuffer_t));
			for (int i = 0; i < nmem; i++)
			{
				buffers[i].mem = mems[i];
				buffers[i].size = size;
				if (i < (nmem - 1))
					buffers[i].next = &buffers[i + 1];
			}
			dev->buffers = buffers;
			dev->nbuffers = nmem;
		}
		break;
		case buf_type_memory_master:
		{
			int *ntargets = va_arg(ap, int *);
			void ***targets = va_arg(ap, void ***);
			size_t *size = va_arg(ap, size_t *);
			if (targets != NULL)
			{
				*targets = calloc(dev->nbuffers, sizeof(void*));
				for (int i = 0; i < dev->nbuffers; i++)
				{
					(*targets)[i] = dev->buffers[i].mem;
					dbg("sfile: memory[%d]: %p %u", i, dev->buffers[i].mem, dev->buffers[i].size);
				}
			}
			if (ntargets != NULL)
				*ntargets = dev->nbuffers;
			if (size != NULL)
				*size = dev->buffers[0].size;
			ret = (dev->nbuffers == 0);
		}
		break;
		case buf_type_dmabuf:
		{
			int ntargets = va_arg(ap, int);
			int *targets = va_arg(ap, int *);
			size_t size = va_arg(ap, size_t);
			FrameBuffer_t *buffers = NULL;
			buffers = calloc(ntargets, sizeof(FrameBuffer_t));
			for (int i = 0; i < ntargets; i++)
			{
				buffers[i].dma_buf = targets[i];
				buffers[i].size = size;
				if (i < (ntargets - 1))
					buffers[i].next = &buffers[i + 1];
			}
			dev->buffers = buffers;
			dev->nbuffers = ntargets;
		}
		break;
		case buf_type_dmabuf_master:
		{
			if (dev->buffers[0].dma_buf == -1)
			{
				ret = -1;
				break;
			}
			int *ntargets = va_arg(ap, int *);
			int **targets = va_arg(ap, int **);
			size_t *size = va_arg(ap, size_t *);
			if (targets != NULL)
			{
				*targets = calloc(dev->nbuffers, sizeof(int));
				for (int i = 0; i < dev->nbuffers; i++)
				{
					(*targets)[i] = dev->buffers[i].dma_buf;
					dbg("sfile: memory[%d]: %d %u", i, dev->buffers[i].dma_buf, dev->buffers[i].size);
				}
			}
			if (ntargets != NULL)
				*ntargets = dev->nbuffers;
			if (size != NULL)
				*size = dev->buffers[0].size;
			ret = (dev->nbuffers == 0);
		}
		break;
		default:
			err("sfile: support only without master");
			va_end(ap);
			return -1;
	}
	va_end(ap);
	return ret;
}

EXT_API int sfile_fd(File_t *dev, int writer)
{
#if 0
	return dev->ops->fd(dev->ctx);
#else
	if (!writer && dev->type == device_input)
	{
		int ret = -1;
		for (int i = 0; i < dev->nbuffers; i++)
		{
			if (dev->buffers[i].state == queued)
			{
				ret = dev->ops->fd(dev->ctx);
				break;
			}
		}
		return ret;
	}
	return -1;
#endif
}

EXT_API int sfile_start(File_t *dev)
{
	dev->lastbufferid = 0;
	if (dev->type == device_input)
	{
		dbg("sfile: start buffers enqueuing");
		for (int i = 0; i < dev->nbuffers; i++)
		{
			if (sfile_queue(dev, i, NULL, 0, 0))
				return -1;
		}
	}
	return dev->ops->connect(dev->ctx);
}

EXT_API int sfile_stop(File_t *dev)
{
	dev->ops->close(dev->ctx);
	return 0;
}

EXT_API int sfile_dequeue(File_t *dev, void **mem, size_t *bytesused, int *flags)
{
	int ret = dev->lastbufferid;
	FrameBuffer_t *buffer = &dev->buffers[dev->lastbufferid];
	if (buffer->state != queued)
	{
		errno = EAGAIN;
		return -1;
	}
	if (dev->type == device_input && dev->config->parent.fps)
	{
		useconds_t usec = -dev->config->parent.fps * 1000000;
		if (dev->config->parent.fps > 0)
			usec = 1000000 / dev->config->parent.fps;
		usleep(usec);
	}
	if (bytesused)
		*bytesused = buffer->bytesused;
	if (mem && buffer->mem)
		*mem = buffer->mem;
	if (flags)
		*flags = buffer->flags;
	dev->lastbufferid++;
	dev->lastbufferid %= dev->nbuffers;
	buffer->state = dequeued;
	return ret;
}

EXT_API int sfile_queue(File_t *dev, int index, void *mem, size_t bytesused, int flags)
{
	if (index > dev->nbuffers)
	{
		err("sfile: unkown %d buffer index to queue", index);
		return -1;
	}
	FrameBuffer_t *buffer = &dev->buffers[index];
	if (bytesused == 0)
		bytesused = buffer->size;
	if (bytesused > buffer->size)
	{
		warn("sfile: buffer too small %zu %zu", buffer->size, bytesused);
	}
	if (dev->type == device_output)
	{
		if (buffer->dma_buf > 0)
		{
			struct dma_buf_sync sync = { 0 };
			sync.flags = DMA_BUF_SYNC_READ | DMA_BUF_SYNC_START;
			ioctl(buffer->dma_buf, DMA_BUF_IOCTL_SYNC, sync);
			mem = mmap(NULL, buffer->size, PROT_READ, MAP_SHARED, buffer->dma_buf, 0 );
		}
		if (mem == NULL)
			mem = buffer->mem;
		ssize_t ret = 0;
		if (dev->headerlen)
			ret = dev->ops->send(dev->ctx, dev->header, dev->headerlen, Proto_More);
		if (ret >= 0)
			ret = dev->ops->send(dev->ctx, mem, bytesused, 0);
		if (buffer->dma_buf > 0)
		{
			struct dma_buf_sync sync = { 0 };
			munmap(mem, buffer->size);
			sync.flags = DMA_BUF_SYNC_READ | DMA_BUF_SYNC_END;
			ioctl(buffer->dma_buf, DMA_BUF_IOCTL_SYNC, sync);
		}
		if (ret < 0)
		{
			err("sfile: write to file \"%s\" error: %m", dev->path);
			return -1;
		}
		buffer->bytesused = ret;
	}
	else if (dev->type == device_input)
	{
		if (buffer->dma_buf > 0)
		{
			sdmabuf_sync(buffer->dma_buf, 1);
		}
		ssize_t ret = dev->ops->recv(dev->ctx, buffer->mem, bytesused, 0);
		if (buffer->dma_buf > 0)
		{
			sdmabuf_sync(buffer->dma_buf, 0);
		}
		if (ret <= 0)
		{
			err("sfile: read from file \"%s\" error: %m", dev->path);
			return -1;
		}
		buffer->bytesused = ret;
	}
	buffer->flags = flags;
	buffer->state = queued;
	return 0;
}

EXT_API void sfile_destroy(File_t *dev)
{
	dev->ops->destroy(dev->ctx);
	if (dev->nbuffers > 0)
		free(dev->buffers);
	if (dev->config)
		free(dev->config);
	free(dev);
}

#ifdef HAVE_JANSSON

int sfile_loadjsonconfiguration(void *arg, void *entry)
{
	json_t *jconfig = entry;

	FileConfig_t *config = (FileConfig_t *)arg;
	json_t *path = json_object_get(jconfig, "path");
	if (path && json_is_string(path))
	{
		const char *value = json_string_value(path);
		config->filename = value;
	}
	json_t *definition = json_object_get(jconfig, "definition");
	if (definition)
		sconfig_loaddefinition(&config->parent, definition);
	json_t *port = json_object_get(jconfig, "port");
	if (config->port == DEFAULT_PORT && port && json_is_integer(port))
	{
		int value = json_integer_value(port);
		config->port = value;
	}
	json_t *modes = json_object_get(jconfig, "protocol");
	if (modes == NULL)
		modes = json_object_get(jconfig, "proto");
	if (modes == NULL)
		modes = json_object_get(jconfig, "mode");
	if (modes && json_is_array(modes))
	{
		json_t *mode;
		int index;
		json_array_foreach(modes, index, mode)
		{
			if (mode && json_is_string(mode))
			{
				const char *value = json_string_value(mode);
				config->protoconf.mode = value;
				for (int i = 0; i < (sizeof(_protos)/sizeof(*_protos)); i++)
				{
					if (_protos[i] && !strcasecmp(value, _protos[i]->name))
					{
						config->proto = _protos[i];
						break;
					}
				}
				if (! strncasecmp(value, "pam", 3))
					config->header = File_PAM_e;
			}
		}
	}
	if (modes && json_is_string(modes))
	{
		const char *value = json_string_value(modes);
		config->protoconf.mode = value;
		for (int i = 0; i < (sizeof(_protos)/sizeof(*_protos)); i++)
		{
			if (_protos[i] && !strcasecmp(value, _protos[i]->name))
			{
				config->proto = _protos[i];
				break;
			}
		}
		if (! strncasecmp(value, "pam", 3))
			config->header = File_PAM_e;
	}
	return 0;
}
#else
int sfile_loadjsonconfiguration(void *arg, void *entry)
{
	FileConfig_t *config = (FileConfig_t *)arg;
	if (config->parent.name != NULL)
	{
		const char *filepath = strchr(config->parent.name, ':');
		if (filepath)
		{
			config->filename = filepath + 1;
		}
	}
	return 0;
}
#endif

DeviceConf_t * sfile_createconfig(const char *name)
{
	FileConfig_t *devconfig = NULL;

	/// this is possible if name variable exits when "create" is called
	devconfig = calloc(1, sizeof(FileConfig_t));
	const char *filepath = strchr(name, ':');
	if (filepath)
	{
		filepath++;
		/// the filepath may be an URL
		if (filepath[0] == '/' && filepath[1] == '/') filepath += 2;
		devconfig->filename = filepath;
	}
	devconfig->port = DEFAULT_PORT;
#ifdef HAVE_JANSSON
	devconfig->parent.ops.loadconfiguration = sfile_loadjsonconfiguration;
#endif
	return (DeviceConf_t *)devconfig;
}

const FastVideoDevice_ops_t sfile_ops = {
	.name = "file",
	.createconfig = sfile_createconfig,
	.create = (FastVideoDevice_create_t)sfile_create,
	.duplicate = (FastVideoDevice_duplicate_t)NULL,
	.loadsettings = (FastVideoDevice_loadsettings_t)NULL,
	.requestbuffer = (FastVideoDevice_requestbuffer_t)sfile_requestbuffer,
	.eventfd = (FastVideoDevice_eventfd_t)sfile_fd,
	.start = (FastVideoDevice_start_t)sfile_start,
	.stop = (FastVideoDevice_stop_t)sfile_stop,
	.dequeue = (FastVideoDevice_dequeue_t)sfile_dequeue,
	.queue = (FastVideoDevice_queue_t)sfile_queue,
	.destroy = (FastVideoDevice_destroy_t)sfile_destroy,
};

#include <dlfcn.h>

static void __attribute__ ((constructor)) sfile_init()
{
	fastvideodevice_ops_append_t _fastvideodevice_ops_append;
	void *hdl = dlopen(NULL, RTLD_NOW);
	_fastvideodevice_ops_append = dlsym(hdl, "fastvideodevice_ops_append");
	if (_fastvideodevice_ops_append)
	{
		_fastvideodevice_ops_append(&sfile_ops);
	}
}
