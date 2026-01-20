#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <fcntl.h>

#include "fastvideo.h"
#include "config.h"
#include "smpegts.h"
#include "log.h"

typedef struct Proto_FILE_s Proto_FILE_t;
struct Proto_FILE_s
{
	Proto_Config_t *config;
	int rootfd;
	int fd[15];
	int maxfiles;
	int currentfd;
	char filename[64];
	int fileid;
	size_t mtu;
};

static void *proto_create(Proto_Config_t *config)
{
	int rootfd;
	size_t mtu = 188 * 10;
	if (config->host == NULL)
		return NULL;
	char *host = strndup(config->host, 1024);
	char *filename = NULL;
	struct stat fs;
	if (stat(host, &fs) < 0 || (fs.st_mode & S_IFMT) != S_IFDIR)
	{
		filename = strrchr(host, '/');
		if (filename)
		{
			host[filename - host] = '\0';
			filename++;
		}
		else
		{
			err("sproto: host must contain at least a directory");
			free(host);
			return NULL;
		}
	}
	rootfd = open(host, O_DIRECTORY);
	if (rootfd < 0)
	{
		mkdir(host, 0777);
		rootfd = open(host, O_DIRECTORY);
	}
	if (rootfd < 0)
	{
		err("sfile: directory %s not found", host);
		free(host);
		return NULL;
	}
	Proto_FILE_t *proto = calloc(1, sizeof(*proto));
	proto->config = config;
	proto->mtu = mtu;
	proto->rootfd = rootfd;
	if (config->maxclients > (sizeof(proto->fd) / sizeof(*proto->fd)))
		config->maxclients = (sizeof(proto->fd) / sizeof(*proto->fd));
	if (filename)
	{
		snprintf(proto->filename, sizeof(proto->filename) - 1, filename);
		proto->maxfiles = 1;
	}
	else
	{
		proto->maxfiles = config->maxclients;
		if (config->maxclients < 2)
		{
			for (int i = 0; i < 1024; i++)
			{
				snprintf(proto->filename, sizeof(proto->filename) - 1, "stream_%.04d.ts", i);
				if (faccessat(rootfd, proto->filename, F_OK, 0) < 0)
					break;
			}
		}
	}
	free(host);
	return proto;
}

static int proto_connect_fifo(void *arg)
{
	Proto_FILE_t *proto = (Proto_FILE_t *)arg;
	Proto_Config_t *config = proto->config;
	if (faccessat(proto->rootfd, proto->filename, F_OK, 0) < 0)
	{
		mkfifoat(proto->rootfd, proto->filename, 0644);
	}
	struct stat sb;
	if (fstatat(proto->rootfd, proto->filename, &sb, 0) &&
		(sb.st_mode & S_IFMT != S_IFIFO))
	{
		err("sfproto: file %s is not a named pipe", proto->filename);
		return -1;
	}
	warn("sfile: wait fifo %s", proto->filename);
	proto->fd[0] = openat(proto->rootfd, proto->filename, O_TRUNC | O_RDWR, 0644);
	if (proto->fd[0] < 0)
		return -1;
	proto->currentfd = 0;
	return 0;
}

static int proto_connect_reg(void *arg)
{
	Proto_FILE_t *proto = (Proto_FILE_t *)arg;
	Proto_Config_t *config = proto->config;

	int newfd = proto->currentfd + 1;
	newfd %= proto->maxfiles;
	if (proto->fd[newfd] > 0)
	{
		close(proto->fd[newfd]);
	}
	if (proto->maxfiles > 1)
		snprintf(proto->filename, sizeof(proto->filename) - 1, "stream_%.04d.ts", proto->fileid);
	if (faccessat(proto->rootfd, proto->filename, F_OK, 0) == 0)
	{
		unlinkat(proto->rootfd, proto->filename, 0);
	}
#ifdef O_TMPFILE
	proto->fd[newfd] = open(config->host, O_TMPFILE | O_RDWR, 0644);
#else
	proto->fd[newfd] = openat(proto->rootfd, proto->filename, O_CREAT | O_RDWR, 0644);
#endif
	if (proto->fd[newfd] < 0)
		return -1;
	proto->currentfd = newfd;
	proto->fileid++;
	proto->fileid %= proto->maxfiles;
	return 0;
}

static ssize_t proto_send(void *arg, const void *buf, size_t len, Proto_Flags_t flags)
{
	Proto_FILE_t *proto = (Proto_FILE_t *)arg;
	ssize_t ret = -1;

	if(proto->currentfd >= 0)
		ret = write(proto->fd[proto->currentfd], buf, len);
	return ret;
}

static ssize_t proto_recv(void *arg, void *buf, size_t len, Proto_Flags_t flags)
{
	Proto_FILE_t *proto = (Proto_FILE_t *)arg;
	ssize_t ret = -1;

	if(proto->currentfd >= 0)
		ret = read(proto->fd[proto->currentfd], buf, len);
	return ret;
}

static void proto_flush(void *arg)
{
	Proto_FILE_t *proto = (Proto_FILE_t *)arg;
	fsync(proto->fd[proto->currentfd]);
}

static int proto_fd(void *arg)
{
	Proto_FILE_t *proto = (Proto_FILE_t *)arg;
	return -1;
}

static size_t proto_mtu(void *arg)
{
	Proto_FILE_t *proto = (Proto_FILE_t *)arg;
	return proto->mtu;
}

static void proto_close(void *arg)
{
	Proto_FILE_t *proto = (Proto_FILE_t *)arg;

	if (proto->fd[proto->currentfd])
	{
#ifdef O_TMPFILE
		linkat(proto->fd[proto->currentfd], "", proto->fd[proto->currentfd], proto->filename, AT_EMPTY_PATH);
#endif
		close(proto->fd[proto->currentfd]);
		proto->fd[proto->currentfd] = -1;
	}
}

static void proto_destroy(void *arg)
{
	Proto_FILE_t *proto = (Proto_FILE_t *)arg;
	free(proto);
}

const Proto_t proto_file =
{
	.name = "file",
	.create = proto_create,
	.connect = proto_connect_reg,
	.close = proto_close,
	.mtu = proto_mtu,
	.fd = proto_fd,
	.send = proto_send,
	.recv = proto_recv,
	.flush = proto_flush,
	.destroy = proto_destroy,
};

const Proto_t proto_fifo =
{
	.name = "fifo",
	.create = proto_create,
	.connect = proto_connect_fifo,
	.close = proto_close,
	.mtu = proto_mtu,
	.fd = proto_fd,
	.send = proto_send,
	.recv = proto_recv,
	.flush = proto_flush,
	.destroy = proto_destroy,
};

#include <dlfcn.h>

static void __attribute__ ((constructor)) smpegts_init()
{
	fastvideo_proto_append_t _fastvideo_proto_append;
	void *hdl = dlopen(NULL, RTLD_NOW);
	_fastvideo_proto_append = dlsym(hdl, "fastvideo_proto_append");
	if (_fastvideo_proto_append)
	{
		_fastvideo_proto_append(&proto_file);
		_fastvideo_proto_append(&proto_fifo);
	}
}
