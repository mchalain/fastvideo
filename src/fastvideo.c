#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/timerfd.h>
#include <fcntl.h>

#include "log.h"
#include "daemonize.h"
#include "sv4l2.h"
#include "spassthrough.h"
#include "sdrm.h"
#include "segl.h"
#include "sfile.h"
#include "sdvb.h"
#include "config.h"

#define MODE_DAEMONIZE 0x01
#define MODE_INITIALIZE 0x02
//#define DISABLE_TRANSFER

typedef struct FastVideoDevice_s FastVideoDevice_t;
struct FastVideoDevice_s
{
	DeviceConf_t *config;
	void *dev;
	FastVideoDevice_ops_t *ops;
};

typedef struct FastVideoPipe_s FastVideoPipe_t;
struct FastVideoPipe_s
{
	FastVideoDevice_t *input;
	FastVideoDevice_t *output;
};

typedef struct FastVideoList_s FastVideoList_t;
struct FastVideoList_s
{
	void *entity;
	FastVideoList_t *next;
	FastVideoList_t *previous;
	FastVideoList_t *last;
	int fd;
};

FastVideoList_t *fastvideolist_append(FastVideoList_t *list, void *device)
{
	FastVideoList_t *entry = NULL;
	entry = calloc(1, sizeof(*entry));
	entry->entity = device;
	entry->next = list;
	if (list == NULL)
		entry->last = entry;
	else
	{
		list->previous = entry;
		entry->last = list->last;
	}
	return entry;
}

FastVideoList_t *fastvideolist_insert(FastVideoList_t *list, void *device)
{
	FastVideoList_t *entry = NULL;
	entry = calloc(1, sizeof(*entry));
	entry->entity = device;
	if (list)
	{
		entry->previous = list->last;
		list->last->next = entry;
	}
	else
	{
		list = entry;
	}
	list->last = entry;
	return list;
}

FastVideoDevice_t *device_duplicate(FastVideoDevice_t *dev)
{
	FastVideoDevice_t *device = NULL;
	if (dev->ops->duplicate == NULL)
	{
		err("fastvideo: device may not be duplicated");
		return NULL;
	}
	void *ndev = NULL;
	DeviceConf_t *config = dev->config;
	ndev = dev->ops->duplicate(dev->dev, &config);
	if (ndev)
	{
		device = calloc(1, sizeof(*device));
		device->config = config;
		device->ops = dev->ops;
		device->dev = ndev;
	}
	return device;
}

typedef struct FastVideo_s FastVideo_t;
struct FastVideo_s
{
	FastVideoDevice_ops_t **ops;
	FastVideoDevice_t *device;
	const char *name;
};

static int _config_createdevice(void *data, const char *name, const char *type, void *config)
{
	FastVideo_t *fastvideo = data;

	/// This part allows to use option with argument
	/// cam:width=640
	char tmpname[256] = {0};
	const char *end = strchr(fastvideo->name, ':');
	int length = strlen(fastvideo->name);
	if (end)
		length = end - fastvideo->name;
	if (length > 255)
		return -1;
	strncpy(tmpname, fastvideo->name, length);

	if (strcmp(tmpname, name))
		return -1;
	for (int i = 0; fastvideo->ops[i] != NULL; i++)
	{
		if (! strcmp(fastvideo->ops[i]->name, type))
		{
			DeviceConf_t *devconfig = NULL;
			devconfig = fastvideo->ops[i]->createconfig();
			if (devconfig)
			{
				devconfig->name = name;
				devconfig->type = type;
				devconfig->entry = config;
				if (devconfig->ops.loadconfiguration)
					devconfig->ops.loadconfiguration(devconfig, config);
				fastvideo->device = calloc(1, sizeof(*fastvideo->device));
				fastvideo->device->config = devconfig;
				fastvideo->device->ops = fastvideo->ops[i];
			}
			break;
		}
	}
	return 0;
}

FastVideoDevice_t *config_createdevice(const char *name, const char *configfile, FastVideoDevice_ops_t *ops[])
{
	FastVideoDevice_t *device = NULL;
	FastVideo_t fastvideo = {0};
	fastvideo.ops = ops;
	fastvideo.name = name;
	if (configfile != NULL &&
		config_parseconfigfile(configfile, _config_createdevice, &fastvideo) == 0)
	{
		device =  fastvideo.device;
	}
	return device;
}

int choice_config(DeviceConf_t *inconfig, DeviceConf_t *outconfig)
{
	if (inconfig->width)
		outconfig->width = inconfig->width;
	else if (outconfig->width)
		inconfig->width = outconfig->width;
	else
	{
		inconfig->width = outconfig->width = 640;
	}
	if (inconfig->height)
		outconfig->height = inconfig->height;
	else if (outconfig->height)
		inconfig->height = outconfig->height;
	else
	{
		inconfig->height = outconfig->height = 480;
	}
	if (inconfig->fourcc)
		outconfig->fourcc = inconfig->fourcc;
	else if (outconfig->fourcc)
		inconfig->fourcc = outconfig->fourcc;
	else
		inconfig->fourcc = outconfig->fourcc = FOURCC('A','B','2','4');
	return 0;
}

static int main_transferbuffer(FastVideoDevice_t *input, FastVideoDevice_t *output)
{
	int index = 0;
	size_t bytesused = 0;
	if ((index = input->ops->dequeue(input->dev, NULL, &bytesused)) < 0)
	{
		if (errno == EAGAIN)
			return 0;
		if (errno)
			err("%s buffer dequeuing error %m", input->config->name);
		return -1;
	}
	//dbg("transfer (%d) %s => %s", index, input->config->name, output->config->name);

	if (output->ops->queue(output->dev, index, bytesused) < 0)
	{
		if (errno == EAGAIN)
			return 0;
		if (errno)
			err("%s buffer queuing error %m", output->config->name);
		return -1;
	}
	return 0;
}

int main_loop(FastVideoList_t *pipes)
{
	int maxfd = 0;
	for(FastVideoList_t *entry = pipes; entry != NULL; entry = entry->next)
	{
		FastVideoPipe_t *pipe = entry->entity;
		if (pipe->output->ops->start(pipe->output->dev) == -1)
			return -1;
		if (pipe->output->ops->eventfd)
		{
			int fd = pipe->output->ops->eventfd(pipe->output->dev);
			maxfd = (fd > maxfd)?fd:maxfd;
		}
		if (pipe->input->ops->start(pipe->input->dev) == -1)
			return -1;
		if (pipe->input->ops->eventfd)
		{
			int fd = pipe->input->ops->eventfd(pipe->input->dev);
			maxfd = (fd > maxfd)?fd:maxfd;
		}
	}
	int timerfd = timerfd_create(CLOCK_REALTIME, 0);
	struct itimerspec timeout = {
		.it_interval = {.tv_sec = 1, .tv_nsec = 0},
		.it_value = {.tv_sec = 1, .tv_nsec = 0},
	};
	timerfd_settime(timerfd, TFD_TIMER_CANCEL_ON_SET, &timeout, NULL);
	maxfd = (maxfd > timerfd)?maxfd:timerfd;

	unsigned int count = 0;
	while (isrunning())
	{
		fd_set rfds;
		fd_set wfds;
		FD_ZERO(&rfds);
		FD_ZERO(&wfds);
		for(FastVideoList_t *entry = pipes; entry != NULL; entry = entry->next)
		{
			FastVideoPipe_t *pipe = entry->entity;
			if (pipe->output->ops->eventfd)
			{
				int fd = pipe->output->ops->eventfd(pipe->output->dev);
				FD_SET(fd, &rfds);
				FD_SET(fd, &wfds);
			}
			if (pipe->input->ops->eventfd)
			{
				int fd = pipe->input->ops->eventfd(pipe->input->dev);
				FD_SET(fd, &rfds);
				FD_SET(fd, &wfds);
			}
		}
		if (timerfd > 0)
			FD_SET(timerfd, &rfds);

		int ret;
		ret = select(maxfd + 1, &rfds, &wfds, NULL, NULL);
		if (ret == -1 && errno == EINTR)
			continue;
		if (ret > 0 && FD_ISSET(timerfd, &rfds))
		{
			uint64_t exp = 0;
			int nread = read(timerfd, &exp, sizeof(uint64_t));
			if (nread == sizeof(uint64_t))
			{
				warn("fastvideo(%d): %d fps", getpid(), count);
				count = 0;
			}
			ret--;
		}
		if (ret == 0)
		{
			continue;
		}
		ret = 0;
		for(FastVideoList_t *entry = pipes; entry != NULL; entry = entry->next)
		{
			FastVideoPipe_t *pipe = entry->entity;
			int infd = -1;
			if (pipe->input->ops->eventfd)
				infd = pipe->input->ops->eventfd(pipe->input->dev);
			if (infd < 0 ||
				FD_ISSET(infd, &rfds))
			{
				ret = main_transferbuffer(pipe->input, pipe->output);
				if (ret && infd > 0)
				{
					killdaemon(NULL);
					break;
				}
			}
		}
		for(FastVideoList_t *entry = pipes->last; entry != NULL; entry = entry->previous)
		{
			FastVideoPipe_t *pipe = entry->entity;
			int outfd = -1;
			if (pipe->output->ops->eventfd)
				outfd = pipe->output->ops->eventfd(pipe->output->dev);
			if (outfd < 0 ||
				FD_ISSET(outfd, &wfds) ||
				FD_ISSET(outfd, &rfds))
			{
				ret = main_transferbuffer(pipe->output, pipe->input);
				if (ret && outfd > 0)
				{
					killdaemon(NULL);
					break;
				}
				if (!ret && entry == pipes->last)
					count++;
			}

		}
	}
	for(FastVideoList_t *entry = pipes; entry != NULL; entry = entry->next)
	{
		FastVideoPipe_t *pipe = entry->entity;
		pipe->output->ops->stop(pipe->output->dev);
		pipe->input->ops->stop(pipe->input->dev);
	}
	return 0;
}

int main(int argc, char * const argv[])
{
	const char *owner = NULL;
	const char *pidfile= NULL;
	const char *configfile = NULL;
	const char *input = "v4l2";
	const char *output = "gpu";
	const char *transfer = "passthrough";
	int width = 640;
	int height = 480;
	unsigned int mode = 0;
	const char *logfile = "-";
	const char *cwd = NULL;
	FastVideoList_t *pipes = NULL;

	int opt;
	do
	{
		opt = getopt(argc, argv, "i:o:t:j:w:h:DL:W:I");
		switch (opt)
		{
			case 'i':
				input = optarg;
			break;
			case 'o':
				output = optarg;
			break;
			case 't':
				transfer = optarg;
			break;
			case 'j':
				configfile = optarg;
			break;
			case 'w':
				width = strtol(optarg, NULL, 10);
			break;
			case 'h':
				height = strtol(optarg, NULL, 10);
			break;
			case 'D':
				mode |= MODE_DAEMONIZE;
			break;
			case 'I':
				mode |= MODE_INITIALIZE;
			break;
			case 'L':
				logfile = optarg;
			break;
			case 'W':
				cwd = optarg;
			break;
		}
	} while(opt != -1);

	FastVideoDevice_ops_t *fastVideoDevice_ops[] =
	{
		&sv4l2_ops,
#ifdef SDVB
		&sdvb_ops,
#endif
#ifdef HAVE_EGL
		&segl_ops,
#endif
#ifdef HAVE_LIBDRM
		&sdrm_ops,
#endif
		&sfile_ops,
		&spassthrough_ops,
		NULL
	};

	if (strcmp(logfile,"-"))
	{
		int logfd = open(logfile, O_WRONLY | O_CREAT | O_TRUNC, 00644);
		if (logfd > 0)
		{
			dup2(logfd, 1);
			dup2(logfd, 2);
			close(logfd);
		}
		else
			err("log file error %m");
	}

	if (cwd != NULL && chdir(cwd) != 0)
		err("main: working directory %m");

	FastVideoDevice_t *indev = NULL;
	indev = config_createdevice(input, configfile, fastVideoDevice_ops);
	if (!indev || !indev->ops)
	{
		err("input not available");
		return -1;
	}
	indev->dev = indev->ops->create(input, device_input, indev->config);
	if (indev->dev == NULL)
		return -1;
	if (indev->ops->loadsettings && indev->config->entry)
	{
		dbg("loadsettings");
		indev->ops->loadsettings(indev->dev, indev->config->entry);
	}

	FastVideoPipe_t *pipe = NULL;
	pipe = calloc(1, sizeof(*pipe));
	pipe->input = indev;

#ifndef DISABLE_TRANSFER
	FastVideoDevice_t *transferdev = NULL;
	transferdev = config_createdevice(transfer, configfile, fastVideoDevice_ops);
	if (!transferdev || !transferdev->ops)
	{
		transferdev = calloc(1, sizeof(*transferdev));
		transferdev->config = spassthrough_createconfig();
		transferdev->ops = &spassthrough_ops;
	}
	choice_config(pipe->input->config, transferdev->config);

	transferdev->dev = transferdev->ops->create(transfer, device_transfer, transferdev->config);
	if (transferdev->dev == NULL)
		return -1;
	if (transferdev->ops->loadsettings && transferdev->config->entry)
	{
		dbg("loadsettings");
		transferdev->ops->loadsettings(transferdev->dev, transferdev->config->entry);
	}
	pipe->output = transferdev;
	pipes = fastvideolist_insert(pipes, pipe);

	FastVideoDevice_t *transferdevD = NULL;
	transferdevD = device_duplicate(transferdev);
	if (!transferdevD)
	{
		err("%s mot duplicated", transferdev->config->name);
		return -1;
	}
	pipe = calloc(1, sizeof(*pipe));
	pipe->input = transferdevD;
#endif

	FastVideoDevice_t *outdev = NULL;
	outdev = config_createdevice(output, configfile, fastVideoDevice_ops);
	if (!outdev || !outdev->ops)
	{
		err("output not available");
		return -1;
	}
	choice_config(pipe->input->config, outdev->config);

	outdev->dev = outdev->ops->create(output, device_output, outdev->config);
	if (outdev->dev == NULL)
		return -1;
	if (outdev->ops->loadsettings && outdev->config->entry)
	{
		dbg("loadsettings");
		outdev->ops->loadsettings(outdev->dev, outdev->config->entry);
	}
	pipe->output = outdev;
	pipes = fastvideolist_insert(pipes, pipe);

	for(FastVideoList_t *entry = pipes; entry != NULL; entry = entry->next)
	{
		FastVideoPipe_t *pipe = entry->entity;
		int *dma_bufs = {0};
		size_t size = 0;
		int nbbufs = 0;
		FastVideoDevice_t *input = pipe->input;
		FastVideoDevice_t *output = pipe->output;
		if (input->ops->requestbuffer(input->dev, buf_type_dmabuf | buf_type_master, &nbbufs, &dma_bufs, &size, NULL) < 0)
		{
			err("%s dma buffer not allowed", input->config->name);
			return -1;
		}
		if (output->ops->requestbuffer(output->dev, buf_type_dmabuf, nbbufs, dma_bufs, size, NULL) < 0)
		{
			err("%s dma buffers not linked", output->config->name);
			return -1;
		}
	}

	daemonize((mode & MODE_DAEMONIZE) == MODE_DAEMONIZE, pidfile, owner);

	if ((mode & MODE_INITIALIZE) == 0)
		main_loop(pipes);

	killdaemon(pidfile);
	indev->ops->destroy(indev->dev);
	outdev->ops->destroy(outdev->dev);
	return 0;
}
