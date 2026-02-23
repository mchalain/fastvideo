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
#include "config.h"
#include "log.h"

extern const Proto_t proto_file;

EXT_API int sfile_queue(File_t *dev, int index, void *mem, size_t bytesused, int flags);

EXT_API File_t * sfile_create(const char *filename, device_type_e type, FileConfig_t *config)
{
	if (type != device_input && type != device_output)
	{
		err("sfile: support only input or output devices");
		return NULL;
	}
	const Proto_t *ops = &proto_file;
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
	dev->path = filename;
	switch (config->header)
	{
		case File_TIFF_e:
			/// add TIFF header for other fourcc
			dev->headerlen = snprintf(dev->header, sizeof(dev->header),
				"P7 WIDTH %.4d HEIGHT %.4d DEPTH %.1d MAXVAL 255 TUPLTYPE RGB_ALPHA ENDHDR",
				config->parent.width, config->parent.height, config->parent.stride / config->parent.width);
		break;
		default:
	}
	warn("sfile: %s opened for %.4s", config->filename, (const char*)&config->parent.fourcc);
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
	return dev->ops->fd(dev);
#else
	if (!writer && dev->type == device_input)
	{
		int ret = dev->ops->fd(dev->ctx);
		for (int i = 0; i < dev->nbuffers; i++)
		{
			if (dev->buffers[i].state == queued)
			{
				ret = -1;
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
			struct dma_buf_sync sync = { 0 };
			sync.flags = DMA_BUF_SYNC_WRITE | DMA_BUF_SYNC_START;
			ioctl(buffer->dma_buf, DMA_BUF_IOCTL_SYNC, sync);
			buffer->mem = mmap(NULL, buffer->size, PROT_WRITE, MAP_SHARED, buffer->dma_buf, 0 );
		}
		ssize_t ret = dev->ops->send(dev->ctx, buffer->mem, bytesused, 0);
		if (buffer->dma_buf > 0)
		{
			struct dma_buf_sync sync = { 0 };
			sync.flags = DMA_BUF_SYNC_WRITE | DMA_BUF_SYNC_END;
			ioctl(buffer->dma_buf, DMA_BUF_IOCTL_SYNC, sync);
		}
		if (ret < 0)
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
				for (int i = 0; i < (sizeof(_protos)/sizeof(*_protos)); i++)
				{
					if (_protos[i] && !strcasecmp(value, _protos[i]->name))
					{
						config->proto = _protos[i];
						break;
					}
				}
				if (! strncasecmp(value, "tiff", 6))
					config->header = File_TIFF_e;
			}
		}
	}
	if (modes && json_is_string(modes))
	{
		const char *value = json_string_value(modes);
		for (int i = 0; i < (sizeof(_protos)/sizeof(*_protos)); i++)
		{
			if (_protos[i] && !strcasecmp(value, _protos[i]->name))
			{
				config->proto = _protos[i];
				break;
			}
		}
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
