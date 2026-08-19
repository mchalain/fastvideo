#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/timerfd.h>
#include <fcntl.h>
#include <dlfcn.h>

#include "fastvideo.h"
#include "log.h"
#include "daemonize.h"
#include "sv4l2.h"
#include "spassthrough.h"
#include "sdrm.h"
#include "segl.h"
#include "sfile.h"
#include "sdvb.h"
#include "sconfig.h"

#define MODE_DAEMONIZE 0x01
#define MODE_INITIALIZE 0x02
#define MODE_VERBOSE 0x04
//#define DISABLE_TRANSFER
unsigned int _mode = 0;

#define MAX_DRAIN_PER_ITERATION 16

#define verbose_warn(f,...) do{if ((_mode & MODE_VERBOSE) != 0) warn(f,  ##__VA_ARGS__);} while(0)

int scommon_loadlibrary(const char *path)
{
	void *hd = NULL;
	hd = dlopen(path, RTLD_NOW);
	return (hd)?0:-1;
}

typedef struct FastVideoPipe_s FastVideoPipe_t;
struct FastVideoPipe_s
{
	FastVideoDevice_t *input;
	FastVideoDevice_t *output;
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
	for (FastVideoDevice_ops_t *ops = fastvideodevice_ops_next(NULL);
		ops != NULL; ops = fastvideodevice_ops_next(ops))
	{
		if (! strcmp(ops->name, type))
		{
			DeviceConf_t *devconfig = NULL;
			devconfig = config_create(fastvideo->name, ops, config);
			if (devconfig)
			{
				if (devconfig->ops.loadconfiguration)
					devconfig->ops.loadconfiguration(devconfig, config);
				fastvideo->device = calloc(1, sizeof(*fastvideo->device));
				fastvideo->device->config = devconfig;
				fastvideo->device->ops = ops;
			}
			break;
		}
	}
	return 0;
}

FastVideoDevice_t *config_createdevice(const char *name, const char *configfile)
{
	FastVideoDevice_t *device = NULL;
	FastVideo_t fastvideo = {0};
	fastvideo.name = name;
	if (configfile != NULL &&
		scommon_parseconfigfile(configfile, _config_createdevice, &fastvideo) == 0)
	{
		device =  fastvideo.device;
	}
	return device;
}

int choice_config(DeviceConf_t *inconfig, DeviceConf_t *outconfig)
{
	sconfig_mergedefinition(outconfig, inconfig);
	sconfig_mergedefinition(inconfig, outconfig);
	dbg("input %s size %u %u", inconfig->name, inconfig->width, inconfig->height);
	dbg("output %s size %u %u", outconfig->name, outconfig->width, outconfig->height);
	return 0;
}

/**
 * return value convention (used by main_loop() to decide whether to keep
 * draining a pipe within the same select() wakeup, instead of just one
 * hop per pipe per iteration):
 *   -1 : real error - caller kills the daemon
 *    0 : no progress (nothing was available, or the output pushed the
 *        buffer straight back to the input because it wasn't ready) -
 *        caller stops looping on this pipe for this wakeup
 *    1 : one buffer was genuinely relayed end to end - caller may retry
 *        immediately, there could be more ready right now
 */
static int main_transferbuffer(FastVideoDevice_t *input, FastVideoDevice_t *output)
{
	int index = 0;
	size_t bytesused = 0;
	void *mem = NULL;
	int flags = 0;
	/// reset errno for the new loop
	errno = 0;

	if ((index = input->ops->dequeue(input->dev, &mem, &bytesused, &flags)) < 0)
	{
		if (errno == EAGAIN)
		{
			return 0;
		}
		if (errno)
			err("%s buffer dequeuing error %m", input->config->name);
		return -1;
	}
	//dbg("transfer (%d) %s => %s %lu bytes", index, input->config->name, output->config->name, bytesused);

	if (output->ops->queue(output->dev, index, mem, bytesused, flags) < 0)
	{
		if (errno != EAGAIN)
		{
			err("%s buffer queuing error %m", output->config->name);
			return -1;
		}
		dbg("buffer lost from %s", output->config->name);
		/// push back the buffer to the input device because the ouput is not ready to manage it
		input->ops->queue(input->dev, index, mem, bytesused, flags);
		errno = 0;
		return 0;
	}
	return 1;
}

int main_loop(FastVideoList_t *pipes)
{
	int maxfd = 0;
	for(FastVideoPipe_t *pipe = fastvideolist_next(pipes);
			pipe != NULL; pipe = fastvideolist_next(pipes))
	{
		if (pipe->output->ops->start(pipe->output->dev) == -1)
			return -1;
		warn("stream %s started", pipe->output->config->name);
		if (pipe->output->ops->eventfd)
		{
			int fd = pipe->output->ops->eventfd(pipe->output->dev, 0);
			maxfd = (fd > maxfd)?fd:maxfd;
		}
		if (pipe->input->ops->start(pipe->input->dev) == -1)
			return -1;
		warn("stream %s started", pipe->input->config->name);
		if (pipe->input->ops->eventfd)
		{
			int fd = pipe->input->ops->eventfd(pipe->input->dev, 0);
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
		for(FastVideoPipe_t *pipe = fastvideolist_next(pipes);
				pipe != NULL; pipe = fastvideolist_next(pipes))
		{
			if (pipe->output->ops->eventfd)
			{
				int fd;
				fd = pipe->output->ops->eventfd(pipe->output->dev, 2);
				if (fd > 0)
					FD_SET(fd, &rfds);
				fd = pipe->output->ops->eventfd(pipe->output->dev, 1);
				if (fd > 0)
					FD_SET(fd, &wfds);
			}
			if (pipe->input->ops->eventfd)
			{
				int fd;
				fd = pipe->input->ops->eventfd(pipe->input->dev, 0);
				if (fd > 0)
					FD_SET(fd, &rfds);
				fd = pipe->input->ops->eventfd(pipe->input->dev, 1);
				if (fd > 0)
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
				verbose_warn("fastvideo(%d): %d fps", getpid(), count);
				count = 0;
			}
			ret--;
		}
		if (ret == 0)
		{
			continue;
		}
		ret = 0;
		for(FastVideoPipe_t *pipe = fastvideolist_next(pipes);
				pipe != NULL; pipe = fastvideolist_next(pipes))
		{
			int infd = -1;
			if (pipe->input->ops->eventfd)
				infd = pipe->input->ops->eventfd(pipe->input->dev, 0);
			if (infd < 0 ||
				(infd > 0 && FD_ISSET(infd, &rfds)))
			{
				int drained = 0;
				do
				{
					ret = main_transferbuffer(pipe->input, pipe->output);
					if (ret == -1 && (infd > 0 || ! errno))
					{
						killdaemon(NULL);
						break;
					}
					if (ret == 1 && fastvideolist_islast(pipes, pipe))
						count++;
				} while (ret == 1 && ++drained < MAX_DRAIN_PER_ITERATION);
				if (ret == -1 && (infd > 0 || ! errno))
					break;
			}
		}

		for (FastVideoPipe_t *pipe = fastvideolist_previous(pipes);
					pipe != NULL; pipe = fastvideolist_previous(pipes))
		{
			int outfd = -1;
			if (pipe->output->ops->eventfd)
				outfd = pipe->output->ops->eventfd(pipe->output->dev, 1);
			if (outfd < 0)
				outfd = pipe->output->ops->eventfd(pipe->output->dev, 2);
			if (outfd < 0 ||
				(outfd > 0 && FD_ISSET(outfd, &rfds)) ||
				(outfd > 0 && FD_ISSET(outfd, &wfds)))
			{
				int drained = 0;
				do
				{
					ret = main_transferbuffer(pipe->output, pipe->input);
					if (ret == -1 && (outfd > 0 || ! errno))
					{
						killdaemon(NULL);
						break;
					}
				} while (ret == 1 && ++drained < MAX_DRAIN_PER_ITERATION);
				if (ret == -1 && (outfd > 0 || ! errno))
					break;
			}
		}
	}
	for(FastVideoPipe_t *pipe = fastvideolist_next(pipes);
			pipe != NULL; pipe = fastvideolist_next(pipes))
	{

		pipe->output->ops->stop(pipe->output->dev);
		pipe->input->ops->stop(pipe->input->dev);
	}
	return 0;
}

FastVideoDevice_t *main_createdevice(const char *name, const char *configfile, device_type_e type, DeviceConf_t *choiceconfig)
{
	if (configfile == NULL)
	{
		err("load json file first");
		return NULL;
	}
	FastVideoDevice_t *device = NULL;
	device = config_createdevice(name, configfile);
	if (!device)
	{
		err("device %s not available", name);
		return NULL;
	}
	if (choiceconfig)
		choice_config(choiceconfig, device->config);

	device->dev = device->ops->create(name, type, device->config);
	if (device->dev == NULL)
		return NULL;
	if (device->ops->loadsettings && device->config->entry)
	{
		dbg("loadsettings");
		device->ops->loadsettings(device->dev, device->config->entry);
	}
	return device;
}

FastVideoPipe_t *main_createinput(const char *name, const char *configfile)
{
	FastVideoPipe_t *pipe = NULL;
	pipe = calloc(1, sizeof(*pipe));
	FastVideoDevice_t *indev = NULL;
	indev = main_createdevice(name, configfile, device_input, NULL);
	if (!indev)
	{
		free(pipe);
		return NULL;
	}
	pipe->input = indev;
	return pipe;
}

FastVideoPipe_t *main_createtransfer(const char *name, const char *configfile, FastVideoPipe_t *pipe)
{
	FastVideoDevice_t *transferdev = NULL;
	transferdev = main_createdevice(name, configfile, device_transfer, pipe->input->config);
	if (!transferdev)
	{
		return NULL;
	}
	pipe->output = transferdev;

	pipe = calloc(1, sizeof(*pipe));
	FastVideoDevice_t *transferdevD = NULL;
	transferdevD = device_duplicate(transferdev);
	if (!transferdevD)
	{
		err("%s not duplicated", transferdev->config->name);
		free(pipe);
		return NULL;
	}
	pipe->input = transferdevD;
	return pipe;
}

int main_createoutput(const char *name, const char *configfile, FastVideoPipe_t *pipe)
{
	FastVideoDevice_t *outdev = NULL;
	outdev = main_createdevice(name, configfile, device_output, pipe->input->config);
	if (!outdev)
	{
		return -1;
	}
	pipe->output = outdev;
	return 0;
}

void main_pipedestroy(void *arg)
{
	FastVideoPipe_t *pipe = (FastVideoPipe_t *)arg;
	pipe->input->ops->destroy(pipe->input->dev);
	free(pipe->input);
	pipe->output->ops->destroy(pipe->output->dev);
	free(pipe->output);
	free(pipe);
}

int main(int argc, char * const argv[])
{
	const char *owner = NULL;
	const char *pidfile= NULL;
	const char *configfile = NULL;
	const char *logfile = NULL;
	const char *cwd = PKG_DATADIR;
	FastVideoList_t *pipes = NULL;
	FastVideoPipe_t *pipe = NULL;

	fastvideodevice_ops_append(&spassthrough_ops);

	opterr = 0;
	int opt;
	do
	{
		opt = getopt(argc, argv, "+L:W:DP:Ivj:l:");
		switch (opt)
		{
			case 'D':
				_mode |= MODE_DAEMONIZE;
			break;
			case 'I':
				_mode |= MODE_INITIALIZE;
			break;
			case 'v':
				_mode |= MODE_VERBOSE;
			break;
			case 'L':
				logfile = optarg;
			break;
			case 'P':
				pidfile = optarg;
			break;
			case 'W':
				cwd = optarg;
			break;
			case 'j':
				configfile = optarg;
			break;
			case 'l':
				scommon_loadlibrary(optarg);
			break;
		}
	} while(opt != -1);

	if (logfile)
		daemon_setlogfile(logfile);

	if (cwd  && chdir(cwd) != 0)
		err("main: working directory %m");

	optind = 0;
	do
	{
		opt = getopt(argc, argv, "i:o:t:");
		switch (opt)
		{
			case 'i':
				pipe = main_createinput(optarg, configfile);
				if (pipe == NULL)
				{
					return -1;
				}
			break;
			case 'o':
				if (main_createoutput(optarg, configfile, pipe))
				{
					return -1;
				}
				pipes = fastvideolist_insert(pipes, pipe);
			break;
			case 't':
				pipes = fastvideolist_insert(pipes, pipe);
				pipe = main_createtransfer(optarg, configfile, pipe);
				if (pipe == NULL)
				{
					return -1;
				}
			break;
		}
	} while(opt != -1);

	for(FastVideoPipe_t *pipe = fastvideolist_next(pipes);
			pipe != NULL; pipe = fastvideolist_next(pipes))
	{
		int *dma_bufs = {0};
		size_t size = 0;
		int nbbufs = 0;
		FastVideoDevice_t *input = pipe->input;
		FastVideoDevice_t *output = pipe->output;
		struct
		{
			FastVideoDevice_t *master;
			FastVideoDevice_t *slave;
			enum buf_type_e buf_type;
		} device_list[] = {
			{
				.master = input,
				.slave = output,
				.buf_type = buf_type_dmabuf,
			},
			{
				.master = input,
				.slave = output,
				.buf_type = buf_type_memory,
			},
			{
				.master = output,
				.slave = input,
				.buf_type = buf_type_dmabuf,
			},
			{
				.master = output,
				.slave = input,
				.buf_type = buf_type_memory,
			},
		};
		int ret = -1;
		for (int i = 0; i < sizeof(device_list)/sizeof(*device_list); i++)
		{
			enum buf_type_e buf_type = device_list[i].buf_type;
			FastVideoDevice_t *master = device_list[i].master;
			if (master->ops->requestbuffer(master->dev, buf_type | buf_type_master, &nbbufs, &dma_bufs, &size, NULL) < 0)
			{
				err("%s buffer type(%d) not allowed", master->config->name, buf_type);
				continue;
			}
			FastVideoDevice_t *slave = device_list[i].slave;
			if (slave->ops->requestbuffer(slave->dev, buf_type, nbbufs, dma_bufs, size, NULL) < 0)
			{
				err("%s buffer type(%d) not linked", slave->config->name, buf_type);
				continue;
			}
			ret = 0;
			break;
		}
		if (ret)
			return -1;
		verbose_warn("pipe %s => %s ready", input->config->name, output->config->name);
	}

	daemonize((_mode & MODE_DAEMONIZE) == MODE_DAEMONIZE, NULL, pidfile, owner, NULL);

	if ((_mode & MODE_INITIALIZE) == 0)
		main_loop(pipes);

	killdaemon(pidfile);
	fastvideolist_destroy(pipes, main_pipedestroy);
	return 0;
}
