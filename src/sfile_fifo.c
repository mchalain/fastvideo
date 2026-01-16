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
	warn("sfile: wait fifo %s connection", name);
	fd = openat(atfd, name, mode, 0644);
	if (fd <= 0)
	{
		err("sfile: fifo %s error: %m", name);
		unlinkat(atfd, name, 0);
		return NULL;
	}
	warn("\tdone", name);
	return (void *)(long)fd;
}

static int _fifo_fd(void *arg)
{
	int fd = (long)arg;
	return fd;
}

static ssize_t _fifo_write(void *arg, void *mem, size_t size)
{
	ssize_t ret = 0;
	int fd = (long)arg;
	if (fd > 0)
	{
		ret = write(fd, mem, size);
	}
	return ret;
}

static ssize_t _fifo_read(void *arg, void *mem, size_t size)
{
	int fd = (long)arg;
	if (fd > 0)
		return read(fd, mem, size);
	return 0;
}

static void _fifo_close(void *arg)
{
	int fd = (long)arg;
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
