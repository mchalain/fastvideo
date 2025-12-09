#include <unistd.h>
#include <fcntl.h>

#include "sfile.h"
#include "log.h"

static void *_regular_open(int atfd, const char *name, device_type_e type)
{
	int fd = -1;
	int mode = 0;
	if (type == device_input)
	{
		mode = O_RDONLY;
		if (faccessat(atfd, name, R_OK, 0) < 0)
		{
			err("file \"%s\" not accessible", name);
			close(atfd);
			return NULL;
		}
	}
	else if (device_output)
	{
		mode = O_WRONLY;
		if (faccessat(atfd, name, F_OK, 0) < 0)
			mode |= O_CREAT;
		else
			mode |= O_TRUNC;
	}
	fd = openat(atfd, name, mode, 0644);
	if (fd <= 0)
		return NULL;
	return (void *)(long)fd;
}

static int _regular_fd(File_t *dev)
{
	int fd = (long)dev->ctx;
	return fd;
}

static ssize_t _regular_write(File_t *dev, void *mem, size_t size)
{
	ssize_t ret = 0;
	int fd = (long)dev->ctx;
	if (fd > 0)
	{
		switch (dev->fourcc)
		{
			case FOURCC('A','B', '2', '4'):
			case FOURCC('R','G', 'B', 'A'):
				dprintf(fd, "P7 WIDTH %d HEIGHT %d DEPTH %d MAXVAL 255 TUPLTYPE RGB_ALPHA ENDHDR", dev->config->parent.width, dev->config->parent.height, dev->config->parent.stride / dev->config->parent.width);
				ret = write(fd, mem, size);
			break;
			case FOURCC('J','P','E','G'):
			case FOURCC('M','J','P','G'):
			case FOURCC('Y','U','Y','V'):
			default:
				ret = write(fd, mem, size);
			break;
			break;
		}
	}
	return ret;
}

static ssize_t _regular_read(File_t *dev, void *mem, size_t size)
{
	int fd = (long)dev->ctx;
	if (fd > 0)
		return read(fd, mem, size);
	return 0;
}

static void _regular_close(File_t *dev)
{
	int fd = (long)dev->ctx;
	fsync(fd);
	if (fd > 0)
		close(fd);
}

File_ops_t _regular_ops = {
	.name = "regular",
	.open = _regular_open,
	.fd = _regular_fd,
	.read = _regular_read,
	.write = _regular_write,
	.close = _regular_close,
};
