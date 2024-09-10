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

#ifdef HAVE_JANSSON
#include <jansson.h>
#endif

#include "sfile.h"
#include "config.h"
#include "log.h"

typedef struct File_s File_t;
struct File_s
{
	const char *path;
	int fd;
	device_type_e type;
	uint32_t fourcc;
	uint32_t width;
	uint32_t height;
	uint32_t stride;
	size_t size;
	size_t nbuffers;
	FrameBuffer_t *buffers;
	int lastbufferid;
};

File_t * sfile_create(const char *filename, device_type_e type, FileConfig_t *config)
{
	if (type == device_transfer)
	{
		err("sfile: %s bad device type", config->parent.name);
		return NULL;
	}
	int rootfd = AT_FDCWD;
	if (config == NULL)
	{
		err("config object must be set");
		return NULL;
	}
	if (config->rootpath != NULL)
	{
		int fd = open(config->rootpath, O_DIRECTORY);
		if (fd < 0)
		{
			err("root path is \"%s\" defined but unavailable %m", config->rootpath);
			return NULL;
		}
		rootfd = fd;
	}

	if (config->filename != NULL)
		filename = config->filename;
	size_t fsize = 0;
	int mode = 0;
	if (type == device_input)
	{
		mode = O_RDONLY;
		if (faccessat(rootfd, filename, R_OK, 0) < 0)
		{
			err("file \"%s\" not accessible", filename);
			close(rootfd);
			return NULL;
		}
		struct stat sb;
		if (fstatat(rootfd, filename, &sb, 0) < 0)
		{
			err("statistic access error: %m");
			close(rootfd);
			return NULL;
		}
		fsize = sb.st_size;
		for (int i = 0; i < sb.st_blocks; i++)
		{

		}
	}
	else
	{
		mode = O_WRONLY;
		if (faccessat(rootfd, filename, F_OK, 0) < 0)
			mode |= O_CREAT;
	}

	int fd = openat(rootfd, filename, mode, 0644);
	if (fd < 0)
	{
		err("file \"%s\" opening error %m", filename);
		return NULL;
	}
	close(rootfd);
	File_t *dev = calloc(1, sizeof(*dev));
	dev->fd = fd;
	dev->size = fsize;
	dev->type = type;
	dev->fourcc = config->parent.fourcc;
	dev->width = config->parent.width;
	dev->height = config->parent.height;
	if (config->parent.stride)
		dev->stride = config->parent.stride;
	else if (fsize)
		dev->stride = fsize / config->parent.height;
	dev->path = filename;
	return dev;
}

int sfile_requestbuffer(File_t *dev, enum buf_type_e t, ...)
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

int sfile_fd(File_t *dev)
{
	return dev->fd;
}

int sfile_start(File_t *dev)
{
	dev->lastbufferid = 0;
	if (dev->type & device_output)
	{
		switch (dev->fourcc)
		{
			case FOURCC('R','G', 'B', 'A'):
				dprintf(dev->fd, "P7 WIDTH %d HEIGHT %d DEPTH %d MAXVAL 255 TUPLTYPE RGB_ALPHA ENDHDR", dev->width, dev->height, dev->stride / dev->width);
			break;
			case FOURCC('J','P','E','G'):
			case FOURCC('M','J','P','G'):
			break;
			default:
			break;
		}
	}
	else
	{
		dbg("start buffers enqueuing");
		for (int i = 0; i < dev->nbuffers; i++)
		{
			if (sfile_queue(dev, i, 0))
				return -1;
		}
	}
	return 0;
}

int sfile_stop(File_t *dev)
{
	return 0;
}

int sfile_dequeue(File_t *dev, void **mem, size_t *bytesused)
{
	int ret = dev->lastbufferid;
	FrameBuffer_t *buffer = &dev->buffers[dev->lastbufferid];
	if (bytesused)
		*bytesused = buffer->bytesused;
	if (mem && buffer->mem)
		*mem = buffer->mem;
	dev->lastbufferid++;
	dev->lastbufferid %= dev->nbuffers;
	return ret;
}

int sfile_queue(File_t *dev, int index, size_t bytesused)
{
	if (index > dev->nbuffers)
	{
		err("unkown %d buffer index to queue", index);
		return -1;
	}
	FrameBuffer_t *buffer = &dev->buffers[index];
	if (bytesused == 0)
		bytesused = buffer->size;
	if (bytesused > buffer->size)
	{
		warn("sfile: buffer too small %lu %lu", buffer->size, bytesused);
	}
	if (dev->type == device_output)
	{
		if (buffer->dma_buf > 0)
		{
			struct dma_buf_sync sync = { 0 };
			sync.flags = DMA_BUF_SYNC_READ | DMA_BUF_SYNC_START;
			ioctl(buffer->dma_buf, DMA_BUF_IOCTL_SYNC, sync);
			buffer->mem = mmap(NULL, buffer->size, PROT_READ, MAP_SHARED, buffer->dma_buf, 0 );
		}
		ssize_t ret = write(dev->fd, buffer->mem, bytesused);
		if (buffer->dma_buf > 0)
		{
			struct dma_buf_sync sync = { 0 };
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
		ssize_t ret = read(dev->fd, buffer->mem, bytesused);
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
	return 0;
}

void sfile_destroy(File_t *dev)
{
	close(dev->fd);
	if (dev->nbuffers > 0)
		free(dev->buffers);
	free(dev);
}

#ifdef HAVE_JANSSON

int sfile_loadjsonconfiguration(void *arg, void *entry)
{
	json_t *jconfig = entry;

	FileConfig_t *config = (FileConfig_t *)arg;
	if (config->parent.name != NULL)
	{
		const char *filepath = strchr(config->parent.name, ':');
		if (filepath)
		{
			filepath++;
			/// the filepath may be an URL
			if (filepath[0] == '/' && filepath[1] == '/') filepath += 2;
			config->filename = filepath;
		}
	}
	json_t *path = json_object_get(jconfig, "path");
	if (path && json_is_string(path))
	{
		const char *value = json_string_value(path);
		config->rootpath = value;
	}
library_end:
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

DeviceConf_t * sfile_createconfig()
{
	FileConfig_t *devconfig = NULL;
	devconfig = calloc(1, sizeof(FileConfig_t));
#ifdef HAVE_JANSSON
	devconfig->parent.ops.loadconfiguration = sfile_loadjsonconfiguration;
#endif
	return (DeviceConf_t *)devconfig;
}
