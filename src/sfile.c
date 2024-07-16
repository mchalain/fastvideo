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

typedef struct FileBuffer_s FileBuffer_t;
struct FileBuffer_s
{
	void *mem;
	int dma_buf;
	size_t size;
	size_t bytesused;
	FileBuffer_t *next;
};

typedef struct File_s File_t;
struct File_s
{
	FileConfig_t *config;
	const char *path;
	int fd;
	size_t size;
	size_t nbuffers;
	FileBuffer_t *buffers;
	int lastbufferid;
};

File_t * sfile_create(const char *filename, FileConfig_t *config)
{
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

	size_t fsize = 0;
	int mode = 0;
	if (config->direction & File_Output_e)
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
		config->direction = File_Input_e;
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
	dev->config = config;
	dev->path = filename;
	return dev;
}

int sfile_requestbuffer(File_t *dev, enum buf_type_e t, ...)
{
	FileConfig_t *config = dev->config;
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
			FileBuffer_t *buffers = NULL;
			buffers = calloc(nmem, sizeof(FileBuffer_t));
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
			FileBuffer_t *buffers = NULL;
			buffers = calloc(ntargets, sizeof(FileBuffer_t));
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
	FileConfig_t *config = dev->config;
	dev->lastbufferid = 0;
	if (config->direction & File_Input_e)
	{
		switch (config->parent.fourcc)
		{
			case FOURCC('R','G', 'B', 'A'):
				dprintf(dev->fd, "P7 WIDTH %d HEIGHT %d DEPTH %d MAXVAL 255 TUPLTYPE RGB_ALPHA ENDHDR", config->parent.width, config->parent.height, config->parent.stride / config->parent.width);
			break;
			case FOURCC('J','P','E','G'):
			case FOURCC('M','J','P','G'):
			break;
			default:
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
	FileConfig_t *config = dev->config;
	int ret = dev->lastbufferid;
	FileBuffer_t *buffer = &dev->buffers[dev->lastbufferid];
	if (bytesused)
		*bytesused = buffer->bytesused;
	if (mem && buffer->mem)
		*mem = buffer->mem;
	dev->lastbufferid++;
	return ret;
}

int sfile_queue(File_t *dev, int index, size_t bytesused)
{
	FileConfig_t *config = dev->config;
	if (index > dev->nbuffers)
	{
		err("unkown %d buffer index to queue", index);
		return -1;
	}
	FileBuffer_t *buffer = &dev->buffers[index];
	if (bytesused == 0)
		bytesused = buffer->size;
	if (bytesused > buffer->size)
	{
		warn("buffer too small %lu %lu", buffer->size, bytesused);
	}
	if (config->direction & File_Input_e)
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
			err("write to file \"%s\" error: %m", dev->path);
			return -1;
		}
		buffer->bytesused = ret;
	}
	else if (config->direction & File_Output_e)
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
			err("read from file \"%s\" error: %m", dev->path);
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
			config->filename = filepath + 1;
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
#endif
