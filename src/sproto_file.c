#define _GNU_SOURCE
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <time.h>

#include "fastvideo.h"
#include "sconfig.h"
#include "smpegts.h"
#include "log.h"

#define HLS_HEADER "#EXTM3U\n\
#EXT-X-VERSION:3\n\
#EXT-X-TARGETDURATION:%f\n"
#define HLS_ENTRY "#EXTINF:%ld.%ld\n"
#define HLS_FOOTER "#EXT-X-ENDLIST\n"

#define SPROTO_FILE_TMPFILE 1
#define SPROTO_FILA_NAME_LENGTH 128

#define Proto_FILE_Hls 0x010000
#define Proto_FILE_Static 0x020000
#define Proto_FILE_Loop 0x030000
typedef struct Proto_FILE_s Proto_FILE_t;
struct Proto_FILE_s
{
	Proto_Config_t *config;
	char *root;
	int rootfd;
	int fd[2];
	int maxfiles;
	int currentfd;
	char filename[SPROTO_FILA_NAME_LENGTH];
	const char *ext;
	int fileid;
	size_t mtu;
	int mode;
	int hlsfd;
	struct timespec hlstp;
};

static const char ext_ts[] = "ts";
static const char ext_jpeg[] = "jpg";
static const char ext_h264[] = "h264";
static const char ext_pam[] = "pam";
static const char ext_raw[] = "raw";
static const char str_hiddenfile[] = ".tmp.part";

struct timespec *timespec_subs( struct timespec *a, struct timespec *b)
{
	a->tv_sec -= b->tv_sec;
	a->tv_nsec -= b->tv_nsec;
	if (a->tv_nsec < 0)
	{
		a->tv_nsec = 1000000000 - a->tv_nsec;
		a->tv_sec--;
	}
	return a;
}

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
			err("file: host (%s) must contain at least a directory", host);
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
		err("file: directory %s not found", host);
		free(host);
		return NULL;
	}
	Proto_FILE_t *proto = calloc(1, sizeof(*proto));
	proto->config = config;
	proto->mtu = mtu;
	proto->rootfd = rootfd;
	proto->root = host;
	if (config->mode && strstr(config->mode, "static"))
		proto->mode |= Proto_FILE_Static;
	if (config->mode && strstr(config->mode, "loop"))
		proto->mode |= Proto_FILE_Loop;
	switch (config->parent.fourcc)
	{
	case FOURCC_JPEG:
		proto->ext = ext_jpeg;
	break;
	case FOURCC_MPTS:
		if (config->mode && strstr(config->mode, "hls"))
		{
			proto->mode |= Proto_FILE_Hls;
		}
		proto->ext = ext_ts;
	break;
	case FOURCC_H264:
		proto->ext = ext_h264;
	break;
	default:
		if (config->mode && strstr(config->mode, "pam"))
			proto->ext = ext_pam;
		else
			proto->ext = ext_raw;
	}
	if (proto->mode & Proto_FILE_Hls)
	{
		int fd = 0;
		const char* path = "stream.m3u8";
		if (filename && strstr(filename, ".m3u"))
			path = filename;
		if (!faccessat(proto->rootfd, path, F_OK, AT_EACCESS))
			unlinkat(proto->rootfd, filename, 0);
		fd = openat(proto->rootfd, path, O_CREAT | O_RDWR, 0664);
		if (fd > 0)
		{
			proto->maxfiles = config->maxclients;
			proto->hlsfd = fd;
			clock_gettime(CLOCK_TAI, &proto->hlstp);
			dprintf(proto->hlsfd, HLS_HEADER, 10.0);
		}
		else
			proto->mode &= ~Proto_FILE_Hls;
	}
	else if (filename)
	{
		snprintf(proto->filename, sizeof(proto->filename) - 1, "%s", filename);
		proto->maxfiles = 1;
	}
	else
	{
		proto->maxfiles = config->maxclients;
	}
	return proto;
}

static int proto_connect_fifo(void *arg)
{
	Proto_FILE_t *proto = (Proto_FILE_t *)arg;
	if (faccessat(proto->rootfd, proto->filename, F_OK, 0) < 0 &&
			mkfifoat(proto->rootfd, proto->filename, 0664))
	{
		err("file: fifo %s creation error %m", proto->filename);
	}
	struct stat sb;
	if (fstatat(proto->rootfd, proto->filename, &sb, 0) &&
		((sb.st_mode & S_IFMT) != S_IFIFO))
	{
		err("file: file %s is not a named pipe", proto->filename);
		return -1;
	}
	warn("file: wait fifo %s", proto->filename);
	proto->fd[0] = openat(proto->rootfd, proto->filename, O_TRUNC | O_RDWR, 0644);
	if (proto->fd[0] < 0)
		return -1;
	proto->currentfd = 0;
	proto->mode &= ~Proto_FILE_Static;
	return 0;
}

static int proto_generatename(Proto_FILE_t *proto, char name[SPROTO_FILA_NAME_LENGTH])
{
	time_t now = time(NULL);
	struct tm *tm_info = localtime(&now);
	int ret;
	char filename[SPROTO_FILA_NAME_LENGTH];
	ret = strftime(filename, SPROTO_FILA_NAME_LENGTH, "stream_%Y-%m-%d_%H-%M-%S.%%s", tm_info);
	ret = snprintf(name, SPROTO_FILA_NAME_LENGTH, filename, proto->ext);
	if (ret > 0)
		return 0;
	return -1;
}

static int proto_connect_reg(void *arg)
{
	Proto_FILE_t *proto = (Proto_FILE_t *)arg;

	int newfd = proto->currentfd + 1;
	newfd %= (sizeof(proto->fd) / sizeof(*proto->fd));
#if 0
	if (faccessat(proto->rootfd, proto->filename, F_OK, 0) == 0)
	{
		unlinkat(proto->rootfd, proto->filename, 0);
	}
#endif
	if (proto->mode & Proto_FILE_Hls)
	{
		struct timespec tp;
		clock_gettime(CLOCK_TAI, &tp);
		timespec_subs(&tp, &proto->hlstp);
		dprintf(proto->hlsfd, HLS_ENTRY, tp.tv_sec, tp.tv_nsec / 10000000);
		dprintf(proto->hlsfd, "%s\n", proto->filename);
		clock_gettime(CLOCK_TAI, &proto->hlstp);
	}
#if SPROTO_FILE_TMPFILE
	unlinkat(proto->rootfd, str_hiddenfile, 0);
	proto->fd[newfd] = openat(proto->rootfd, str_hiddenfile, O_CREAT | O_RDWR, 0664);
#else
#ifdef O_TMPFILE
	proto->fd[newfd] = open(proto->root, O_TMPFILE | O_RDWR, 0644);
#else
	proto->fd[newfd] = openat(proto->rootfd, proto->filename, O_CREAT | O_RDWR, 0664);
#endif
#endif
	if (proto->fd[newfd] < 0)
	{
		err("sproto: file %s/%s opening error %m", proto->root, proto->filename);
		return -1;
	}
	if (proto->fd[proto->currentfd] > 0)
	{
		close(proto->fd[proto->currentfd]);
	}
	warn("sproto: new %s file(%d)", proto->filename, proto->fd[newfd]);
	proto->currentfd = newfd;
	proto->fileid++;
	if (proto->fileid > proto->maxfiles)
		proto->fileid = 0;

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
	if (ret > 0 && proto->mode & Proto_FILE_Static)
		lseek(proto->fd[proto->currentfd], 0, SEEK_SET);
	if (ret == 0 && proto->mode & Proto_FILE_Loop)
	{
		lseek(proto->fd[proto->currentfd], 0, SEEK_SET);
		ret = read(proto->fd[proto->currentfd], buf, len);
	}
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
	return proto->fd[proto->currentfd];
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
		warn("sproto: close file (%d)", proto->fd[proto->currentfd]);
		off_t length = lseek(proto->fd[proto->currentfd], 0, SEEK_CUR);
		if (proto->filename[0] == '\0')
		{
			proto_generatename(proto, proto->filename);
		}
#if SPROTO_FILE_TMPFILE
		close(proto->fd[proto->currentfd]);
		if (length)
			renameat2(proto->rootfd, str_hiddenfile, proto->rootfd, proto->filename, RENAME_NOREPLACE);
#else
#ifdef O_TMPFILE
		int ret = 0
		if (length)
			ret = linkat(proto->fd[proto->currentfd], "", , proto->filename, AT_EMPTY_PATH);
		if (ret < 0)
			err("sproto: file %s link error %m", proto->filename);
#endif
		close(proto->fd[proto->currentfd]);
#endif
		proto->fd[proto->currentfd] = -1;
		proto->filename[0] = '\0';
	}
}

static void proto_destroy(void *arg)
{
	Proto_FILE_t *proto = (Proto_FILE_t *)arg;
	if (proto->hlsfd)
	{
		close(proto->hlsfd);
	}
	if (proto->fd[proto->currentfd])
	{
		close(proto->fd[proto->currentfd]);
	}
	unlinkat(proto->rootfd, str_hiddenfile, 0);
	close(proto->rootfd);
	free(proto->root);
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
