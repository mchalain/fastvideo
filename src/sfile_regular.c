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
	else if (type == device_output)
	{
		mode = O_WRONLY;
		if (faccessat(atfd, name, F_OK, 0) < 0)
			mode |= O_CREAT;
		else
			mode |= O_TRUNC;
	}
	else
		return NULL;
	fd = openat(atfd, name, mode, 0644);
	if (fd <= 0)
		return NULL;
	return (void *)(long)fd;
}

static int _regular_fd(void *arg)
{
	int fd = (long)arg;
	return fd;
}

static ssize_t _regular_write(void *arg, void *mem, size_t size)
{
	ssize_t ret = 0;
	int fd = (long)arg;
	if (fd > 0)
	{
		ret = write(fd, mem, size);
	}
	return ret;
}

static ssize_t _regular_read(void *arg, void *mem, size_t size)
{
	int fd = (long)arg;
	if (fd > 0)
		return read(fd, mem, size);
	return 0;
}

static void _regular_close(void *arg)
{
	int fd = (long)arg;
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
