#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

#include "sfile.h"
#include "log.h"

static void *_fifo_open(int atfd, const char *name, device_type_e type)
{
	int fd = -1;
	int mode = 0;
	struct stat sb;
	if (fstatat(atfd, name, &sb, 0) &&
		(sb.st_mode & S_IFMT != S_IFIFO))
	{
		err("sfile: file %s is not a named pipe", name);
		return NULL;
	}
	if (type == device_input)
		mode = O_RDONLY;
	else if (device_output)
	{
		mode = O_WRONLY;
		mode |= O_TRUNC;
	}
	if (faccessat(atfd, name, F_OK, 0) < 0)
	{
		mkfifoat(atfd, name , 0644);
	}
	warn("sfile: start the fifo client");
	fd = openat(atfd, name, mode, 0644);
	if (fd <= 0)
	{
		unlinkat(atfd, name, 0);
		return NULL;
	}
	return (void *)(long)fd;
}

static int _fifo_fd(File_t *dev)
{
	int fd = (long)dev->ctx;
	return fd;
}

static ssize_t _fifo_write(File_t *dev, void *mem, size_t size)
{
	ssize_t ret = 0;
	int fd = (long)dev->ctx;
	if (fd > 0)
	{
		ret = write(fd, mem, size);
	}
	return ret;
}

static ssize_t _fifo_read(File_t *dev, void *mem, size_t size)
{
	int fd = (long)dev->ctx;
	if (fd > 0)
		return read(fd, mem, size);
	return 0;
}

static void _fifo_close(File_t *dev)
{
	int fd = (long)dev->ctx;
	fsync(fd);
	if (fd > 0)
		close(fd);
}

File_ops_t _fifo_ops = {
	.name = "fifo",
	.open = _fifo_open,
	.fd = _fifo_fd,
	.read = _fifo_read,
	.write = _fifo_write,
	.close = _fifo_close,
};
