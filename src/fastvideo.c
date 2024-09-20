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
#include "config.h"

#define MODE_DAEMONIZE 0x01
//#define DISABLE_TRANSFER

typedef DeviceConf_t * (*FastVideoDevice_createconfig_t)(void);
typedef void *(*FastVideoDevice_create_t)(const char *devicename, device_type_e type, DeviceConf_t *config);
typedef void *(*FastVideoDevice_duplicate_t)(void *dev);
typedef int (*FastVideoDevice_loadsettings_t)(void *dev, void *configentry);
typedef int (*FastVideoDevice_requestbuffer_t)(void *dev, enum buf_type_e t, ...);
typedef int (*FastVideoDevice_eventfd_t)(void *dev);
typedef int (*FastVideoDevice_start_t)(void *dev);
typedef int (*FastVideoDevice_stop_t)(void *dev);
typedef int (*FastVideoDevice_dequeue_t)(void *dev, void **mem, size_t *bytesused);
typedef int (*FastVideoDevice_queue_t)(void *dev, int index, size_t bytesused);
typedef void (*FastVideoDevice_destroy_t)(void *dev);

typedef struct FastVideoDevice_ops_s FastVideoDevice_ops_t;
struct FastVideoDevice_ops_s
{
	const char *name;
	FastVideoDevice_createconfig_t createconfig;
	FastVideoDevice_create_t create;
	FastVideoDevice_duplicate_t duplicate;
	FastVideoDevice_loadsettings_t loadsettings;
	FastVideoDevice_requestbuffer_t requestbuffer;
	FastVideoDevice_eventfd_t eventfd;
	FastVideoDevice_start_t start;
	FastVideoDevice_stop_t stop;
	FastVideoDevice_dequeue_t dequeue;
	FastVideoDevice_queue_t queue;
	FastVideoDevice_destroy_t destroy;
};

FastVideoDevice_ops_t sv4l2_ops = {
	.name = "v4l2",
	.createconfig = sv4l2_createconfig,
	.create = (FastVideoDevice_create_t)sv4l2_create,
	.duplicate = (FastVideoDevice_duplicate_t)sv4l2_duplicate,
	.loadsettings = (FastVideoDevice_loadsettings_t)sv4l2_loadsettings,
	.requestbuffer = (FastVideoDevice_requestbuffer_t)sv4l2_requestbuffer,
	.eventfd = (FastVideoDevice_eventfd_t)sv4l2_fd,
	.start = (FastVideoDevice_start_t)sv4l2_start,
	.stop = (FastVideoDevice_stop_t)sv4l2_stop,
	.dequeue = (FastVideoDevice_dequeue_t)sv4l2_dequeue,
	.queue = (FastVideoDevice_queue_t)sv4l2_queue,
	.destroy = (FastVideoDevice_destroy_t)sv4l2_destroy,
};
FastVideoDevice_ops_t spassthrough_ops = {
	.name = "passthrough",
	.createconfig = spassthrough_createconfig,
	.create = (FastVideoDevice_create_t)spassthrough_create,
	.duplicate = (FastVideoDevice_duplicate_t)spassthrough_duplicate,
	.loadsettings = (FastVideoDevice_loadsettings_t)spassthrough_loadsettings,
	.requestbuffer = (FastVideoDevice_requestbuffer_t)spassthrough_requestbuffer,
	.eventfd = (FastVideoDevice_eventfd_t)spassthrough_fd,
	.start = (FastVideoDevice_start_t)spassthrough_start,
	.stop = (FastVideoDevice_stop_t)spassthrough_stop,
	.dequeue = (FastVideoDevice_dequeue_t)spassthrough_dequeue,
	.queue = (FastVideoDevice_queue_t)spassthrough_queue,
	.destroy = (FastVideoDevice_destroy_t)spassthrough_destroy,
};
#ifdef HAVE_EGL
FastVideoDevice_ops_t segl_ops = {
	.name = "gpu",
	.createconfig = segl_createconfig,
	.create = (FastVideoDevice_create_t)segl_create,
	.duplicate = (FastVideoDevice_duplicate_t)NULL,
	.loadsettings = (FastVideoDevice_loadsettings_t)NULL,
	.requestbuffer = (FastVideoDevice_requestbuffer_t)segl_requestbuffer,
	.eventfd = (FastVideoDevice_eventfd_t)segl_fd,
	.start = (FastVideoDevice_start_t)segl_start,
	.stop = (FastVideoDevice_stop_t)segl_stop,
	.dequeue = (FastVideoDevice_dequeue_t)segl_dequeue,
	.queue = (FastVideoDevice_queue_t)segl_queue,
	.destroy = (FastVideoDevice_destroy_t)segl_destroy,
};
#endif
#ifdef HAVE_LIBDRM
FastVideoDevice_ops_t sdrm_ops = {
	.name = "screen",
	.createconfig = sdrm_createconfig,
	.create = (FastVideoDevice_create_t)sdrm_create,
	.duplicate = (FastVideoDevice_duplicate_t)NULL,
	.loadsettings = (FastVideoDevice_loadsettings_t)sdrm_loadsettings,
	.requestbuffer = (FastVideoDevice_requestbuffer_t)sdrm_requestbuffer,
	.eventfd = (FastVideoDevice_eventfd_t)NULL,
	.start = (FastVideoDevice_start_t)sdrm_start,
	.stop = (FastVideoDevice_stop_t)sdrm_stop,
	.dequeue = (FastVideoDevice_dequeue_t)sdrm_dequeue,
	.queue = (FastVideoDevice_queue_t)sdrm_queue,
	.destroy = (FastVideoDevice_destroy_t)sdrm_destroy,
};
#endif
FastVideoDevice_ops_t sfile_ops = {
	.name = "file",
	.createconfig = sfile_createconfig,
	.create = (FastVideoDevice_create_t)sfile_create,
	.duplicate = (FastVideoDevice_duplicate_t)NULL,
	.loadsettings = (FastVideoDevice_loadsettings_t)NULL,
	.requestbuffer = (FastVideoDevice_requestbuffer_t)sfile_requestbuffer,
	.eventfd = (FastVideoDevice_eventfd_t)NULL,
	.start = (FastVideoDevice_start_t)sfile_start,
	.stop = (FastVideoDevice_stop_t)sfile_stop,
	.dequeue = (FastVideoDevice_dequeue_t)sfile_dequeue,
	.queue = (FastVideoDevice_queue_t)sfile_queue,
	.destroy = (FastVideoDevice_destroy_t)sfile_destroy,
};

typedef struct FastVideoDevice_s FastVideoDevice_t;
struct FastVideoDevice_s
{
	DeviceConf_t *config;
	void *dev;
	FastVideoDevice_ops_t *ops;
};

FastVideoDevice_t *device_duplicate(FastVideoDevice_t *dev)
{
	FastVideoDevice_t *device = NULL;
	if (dev->ops->duplicate == NULL)
	{
		err("fastvideo: device may not be duplicated");
		return NULL;
	}
	void *ndev = NULL;
	ndev = dev->ops->duplicate(dev->dev);
	if (ndev)
	{
		device = calloc(1, sizeof(*device));
		device->config = dev->config;
		device->ops = dev->ops;
		device->dev = ndev;
	}
	return device;
}

FastVideoDevice_t *config_createdevice(const char *name, const char *configfile, FastVideoDevice_ops_t *ops[])
{
	FastVideoDevice_t *device = NULL;
	DeviceConf_t devconfig = {0};
#ifdef HAVE_JANSSON
	if (configfile != NULL)
	{
		config_parseconfigfile(name, configfile, &devconfig);
	}
#endif
	if (devconfig.type == NULL)
	{
		devconfig.type = name;
	}

	for (int i = 0; ops[i] != NULL; i++)
	{
		if (! strcmp(ops[i]->name, devconfig.type))
		{
			DeviceConf_t *config = ops[i]->createconfig();
			config->name = name;
			config_parseconfigfile(name, configfile, config);
			device = calloc(1, sizeof(*device));
			device->config = config;
			device->ops = ops[i];
			break;
		}
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

int main_loop(FastVideoDevice_t *input, FastVideoDevice_t *intr,
			FastVideoDevice_t *outtr, FastVideoDevice_t *output)
{
	output->ops->start(output->dev);
#ifndef DISABLE_TRANSFER
	outtr->ops->start(outtr->dev);
	intr->ops->start(intr->dev);
#endif
	input->ops->start(input->dev);
	int maxfd = 0;
	int infd = -1;
	if (input->ops->eventfd)
	{
		infd = input->ops->eventfd(input->dev);
		maxfd = (infd > maxfd)?infd:maxfd;
	}
#ifndef DISABLE_TRANSFER
	int intrfd = -1;
	if (intr->ops->eventfd)
	{
		intrfd = intr->ops->eventfd(intr->dev);
		maxfd = (intrfd > maxfd)?intrfd:maxfd;
	}
	int outtrfd = -1;
	if (outtr->ops->eventfd)
	{
		outtrfd = outtr->ops->eventfd(outtr->dev);
		maxfd = (outtrfd > maxfd)?outtrfd:maxfd;
	}
#else
	int intrfd = -1;
	int outtrfd = -1;
#endif
	int outfd = -1;
	if (output->ops->eventfd)
	{
		outfd = output->ops->eventfd(output->dev);
		maxfd = (outfd > maxfd)?outfd:maxfd;
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
		if (infd > 0)
			FD_SET(infd, &rfds);
		if (intrfd > 0)
			FD_SET(intrfd, &wfds);
		if (outtrfd > 0)
			FD_SET(outtrfd, &rfds);
		if (outfd > 0)
		{
			FD_SET(outfd, &rfds);
			FD_SET(outfd, &wfds);
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
		if (infd < 0 ||
			FD_ISSET(infd, &rfds))
		{
#ifndef DISABLE_TRANSFER
			if (main_transferbuffer(input, intr))
#else
			if (main_transferbuffer(input, output))
#endif
			{
				killdaemon(NULL);
				break;
			}
#ifndef DISABLE_TRANSFER
			ret = 1;
		}
		if ((outtrfd < 0 && ret == 1) ||
			FD_ISSET(outtrfd, &rfds))
		{
			if (main_transferbuffer(outtr, output))
			{
				killdaemon(NULL);
				break;
			}
#endif
			ret = 2;
		}
		if ((outfd < 0 && ret == 2) ||
			FD_ISSET(outfd, &wfds) ||
			FD_ISSET(outfd, &rfds))
		{
#ifndef DISABLE_TRANSFER
			if (main_transferbuffer(output, outtr))
#else
			if (main_transferbuffer(output, input))
#endif
			{
				killdaemon(NULL);
				break;
			}
			count++;
#ifndef DISABLE_TRANSFER
			ret = 3;
		}
		if ((intrfd < 0 && ret == 3) ||
			FD_ISSET(intrfd, &wfds))
		{
			if (main_transferbuffer(intr, input))
			{
				killdaemon(NULL);
				break;
			}
#endif
		}
	}
	input->ops->stop(input->dev);
#ifndef DISABLE_TRANSFER
	intr->ops->stop(intr->dev);
	outtr->ops->stop(outtr->dev);
#endif
	output->ops->stop(output->dev);
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

	int opt;
	do
	{
		opt = getopt(argc, argv, "i:o:t:j:w:h:DL:");
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
			case 'L':
				logfile = optarg;
			break;
		}
	} while(opt != -1);

	FastVideoDevice_ops_t *fastVideoDevice_ops[] =
	{
		&sv4l2_ops,
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

	FastVideoDevice_t *indev = NULL;
	indev = config_createdevice(input, configfile, fastVideoDevice_ops);
	if (!indev || !indev->ops)
	{
		err("input not available");
		return -1;
	}

#ifndef DISABLE_TRANSFER
	FastVideoDevice_t *transferdev = NULL;
	transferdev = config_createdevice(transfer, configfile, fastVideoDevice_ops);
	if (!transferdev || !transferdev->ops)
	{
		err("transfer not available");
		return -1;
	}
#else
	FastVideoDevice_t *transferdev = NULL;
#endif

	FastVideoDevice_t *outdev = NULL;
	outdev = config_createdevice(output, configfile, fastVideoDevice_ops);
	if (!outdev || !outdev->ops)
	{
		err("output not available");
		return -1;
	}

#ifndef DISABLE_TRANSFER
	choice_config(indev->config, transferdev->config);
	choice_config(transferdev->config, outdev->config);
#else
	choice_config(indev->config, outdev->config);
#endif

	indev->dev = indev->ops->create(input, device_input, indev->config);
	if (indev->dev == NULL)
		return -1;
	if (indev->ops->loadsettings && indev->config->entry)
	{
		dbg("loadsettings");
		indev->ops->loadsettings(indev->dev, indev->config->entry);
	}

#ifndef DISABLE_TRANSFER
	transferdev->dev = transferdev->ops->create(transfer, device_transfer, transferdev->config);
	if (transferdev->dev == NULL)
		return -1;
	if (transferdev->ops->loadsettings && transferdev->config->entry)
	{
		dbg("loadsettings");
		transferdev->ops->loadsettings(transferdev->dev, transferdev->config->entry);
	}

	FastVideoDevice_t *transferdevD = NULL;
	transferdevD = device_duplicate(transferdev);
	if (!transferdevD)
	{
		err("%s mot duplicated", transferdev->config->name);
		return -1;
	}
#else
	FastVideoDevice_t *transferdevD = NULL;
#endif

	outdev->dev = outdev->ops->create(output, device_output, outdev->config);
	if (outdev->dev == NULL)
		return -1;
	if (outdev->ops->loadsettings && outdev->config->entry)
	{
		dbg("loadsettings");
		outdev->ops->loadsettings(outdev->dev, outdev->config->entry);
	}

	int *dma_bufs = {0};
	size_t size = 0;
	int nbbufs = 0;
	if (indev->ops->requestbuffer(indev->dev, buf_type_dmabuf | buf_type_master, &nbbufs, &dma_bufs, &size, NULL) < 0)
	{
		err("%s dma buffer not allowed", indev->config->name);
		return -1;
	}
#ifndef DISABLE_TRANSFER
	if (transferdev->ops->requestbuffer(transferdev->dev, buf_type_dmabuf, nbbufs, dma_bufs, size, NULL) < 0)
	{
		err("%s dma buffers not linked", transferdev->config->name);
		return -1;
	}
	if (transferdevD->ops->requestbuffer(transferdevD->dev, buf_type_dmabuf | buf_type_master, &nbbufs, &dma_bufs, &size, NULL) < 0)
	{
		err("%s dma buffer not allowed", transferdevD->config->name);
		return -1;
	}
#endif
	if (outdev->ops->requestbuffer(outdev->dev, buf_type_dmabuf, nbbufs, dma_bufs, size, NULL) < 0)
	{
		err("%s dma buffers not linked", outdev->config->name);
		return -1;
	}

	daemonize((mode & MODE_DAEMONIZE) == MODE_DAEMONIZE, pidfile, owner);

	main_loop(indev, transferdev, transferdevD, outdev);

	killdaemon(pidfile);
	indev->ops->destroy(indev->dev);
	outdev->ops->destroy(outdev->dev);
	return 0;
}
