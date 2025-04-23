#include <unistd.h>
#include <fcntl.h>

#include "sfile.h"
#include "log.h"

struct File_s
{
	const char *path;
	void *ctx;
	File_ops_t *ops;
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

static void *_passthrough_open(int atfd, const char *name, int mode)
{
	int fd = -1;
	fd = openat(atfd, name, mode, 0644);
	if (fd <= 0)
		return NULL;
	return (void *)fd;
}

static int _passthrough_fd(File_t *dev)
{
	int fd = (int)dev->ctx;
	return fd;
}

static ssize_t _passthrough_write(File_t *dev, void *mem, size_t size)
{
	ssize_t ret = 0;
	int fd = (int)dev->ctx;
	if (fd > 0)
	{
		switch (dev->fourcc)
		{
			case FOURCC('A','B', '2', '4'):
			case FOURCC('R','G', 'B', 'A'):
				dprintf(fd, "P7 WIDTH %d HEIGHT %d DEPTH %d MAXVAL 255 TUPLTYPE RGB_ALPHA ENDHDR", dev->width, dev->height, dev->stride / dev->width);
				ret = write(fd, mem, size);
			break;
			case FOURCC('J','P','E','G'):
			case FOURCC('M','J','P','G'):
				ret = write(fd, mem, size);
			break;
			case FOURCC('Y','U','Y','V'):
				ret = write(fd, mem, size);
			break;
			default:
			break;
		}
	}
	return ret;
}

static ssize_t _passthrough_read(File_t *dev, void *mem, size_t size)
{
	int fd = (int)dev->ctx;
	if (fd > 0)
		return read(fd, mem, size);
	return 0;
}

static void _passthrough_close(File_t *dev)
{
	int fd = (int)dev->ctx;
	if (fd > 0)
		close(fd);
}

File_ops_t _passthrough_ops = {
	.name = "passthrough",
	.open = _passthrough_open,
	.fd = _passthrough_fd,
	.read = _passthrough_read,
	.write = _passthrough_write,
	.close = _passthrough_close,
};
