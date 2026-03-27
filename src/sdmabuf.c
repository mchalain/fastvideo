#define _GNU_SOURCE
#include <stddef.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <errno.h>
#include <sys/ioctl.h>

#include <linux/dma-buf.h>
#include <linux/dma-heap.h>

#include "log.h"
#include "sdmabuf.h"

static int _dma_heap = 0;
static int _cref = 0;

static char *_cpuext = NULL;
static char _nullstring[] = "";
static uint32_t _cacheextension = 0;
const char *scpu_queryextensions()
{
	if (_cpuext)
		return _cpuext;
	_cpuext = _nullstring;
	int fd = 0;
	char buffer[1024] = {0};
	fd = open ("/proc/cpuinfo", O_RDONLY);
	if (fd)
	{
		char *features = NULL;
		while (features == NULL)
		{
			ssize_t bytes = read(fd, buffer, sizeof(buffer) - 1);
			features = strstr(buffer, "Features");
			if (bytes == -1)
				break;
		}
		if (features)
		{
			size_t length = (features - buffer);
			char *endline = NULL;
			memmove(buffer, features, sizeof(buffer) - length);
			features = buffer;
			endline = strchr(buffer, '\n');
			if (endline == NULL)
				length += read(fd, features + length, sizeof(buffer) - length - 1);
			if (endline == NULL)
				return _cpuext;
			endline[0] = '\0';
			features = strchr(features, ':') + 1;
			length = endline - features;
			_cpuext = malloc(length);
			strncpy(_cpuext, features, length);
		}
	}
	_cacheextension |= (scpu_checkextension("neon") << SCPU_NEON);
	_cacheextension |= (scpu_checkextension("vfpv4") << SCPU_VFPV4);
	return _cpuext;
}

int scpu_checkextension(const char *ext)
{
	const char *extensions = scpu_queryextensions();
	return !(strcasestr(extensions, ext) == NULL);
}

int scpu_check(int ext)
{
	scpu_queryextensions();
	return (_cacheextension >> ext) & 0x01;
}

static int _dmabuf_open()
{
	if (_dma_heap)
		return _dma_heap;
	static const char *devices[] = {
		"/dev/dma_heap/linux,cma",
		"/dev/dma_heap/reserved"
	};

	for(int i = 0; i < (sizeof(devices)/sizeof(*devices)); i++)
	{
		_dma_heap = open(devices[i], O_RDWR, 0);
		if (_dma_heap > 0)
			break;
	}
	return _dma_heap;
}

int sdmabuf_create(const char *name, size_t size)
{
	if (_dmabuf_open() < 0)
	{
		err("sdmabuf: dma allocator not available");
		return -1;
	}
	struct dma_heap_allocation_data alloc = { 0 };
	alloc.len = size;
	alloc.fd_flags = O_CLOEXEC | O_RDWR;

	if(ioctl(_dma_heap, DMA_HEAP_IOCTL_ALLOC, &alloc) < 0)
		return -1;

	if(name)
		ioctl(alloc.fd, DMA_BUF_SET_NAME, name);
	_cref++;
	return alloc.fd;
}

int sdmabuf_sync(int dmafd, int start)
{
	struct dma_buf_sync sync = { 0 };
	sync.flags = (start ? DMA_BUF_SYNC_START : DMA_BUF_SYNC_END) | DMA_BUF_SYNC_RW;

	do
	{
		if(ioctl(dmafd, DMA_BUF_IOCTL_SYNC, &sync) == 0)
			return 0;
	} while((errno == EINTR) || (errno == EAGAIN));

	return -1;
}

void *sdmabuf_map(int dmafd, size_t size, int write)
{
	int flags = PROT_READ;
	if (write)
		flags |= PROT_WRITE;
	void *mem = mmap(0, size, flags, MAP_SHARED, dmafd, 0);
	if (mem == MAP_FAILED)
		err("sdmabuf: map failed %m");
	return mem;
}

void sdmabuf_unmap(void *mem, size_t size)
{
	munmap(mem, size);
}

void sdmabuf_destroy(int dmafd)
{
	if (!_cref)
		return;
	close(dmafd);
	_cref--;
	if (_cref == 0)
	{
		close(_dma_heap);
		_dma_heap = 0;
	}
}
