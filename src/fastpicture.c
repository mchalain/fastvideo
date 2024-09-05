#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

#include "log.h"
#include "sv4l2.h"
#include "sfile.h"
#include "config.h"

int main_loop(V4L2_t *cam, File_t *file)
{
	int run = 1;
	sv4l2_start(cam);
	sfile_start(file);
	int camfd = sv4l2_fd(cam);
	int filefd = sfile_fd(file);
	int maxfd = camfd;
	while (run)
	{
		fd_set rfds;
		fd_set wfds;
		FD_ZERO(&rfds);
		FD_ZERO(&wfds);
		FD_SET(camfd, &rfds);
		struct timeval timeout = {
			.tv_sec = 2,
			.tv_usec = 0,
		};
		int ret;

		ret = select(maxfd + 1, &rfds, &wfds, NULL, &timeout);
		if (ret == -1 && errno == EINTR)
			continue;
		if (ret == 0)
		{
			err("camera timeout");
			continue;
		}
		if (ret > 0 && FD_ISSET(camfd, &rfds))
		{
			int index = 0;
			size_t bytesused = 0;
			if ((index = sv4l2_dequeue(cam, NULL, &bytesused)) < 0)
			{
				err("camera buffer dequeuing error %m");
				if (errno == EAGAIN)
					continue;
				run = 0;
				break;
			}

			if (sfile_queue(file, index, bytesused) < 0)
			{
				err("file buffer queuing error %m");
				if (errno == EAGAIN)
					continue;
				run = 0;
				break;
			}
			if ((index = sfile_dequeue(file, NULL, NULL)) < 0)
			{
				err("file buffer dequeuing error %m");
				if (errno == EAGAIN)
					continue;
				run = 0;
				break;
			}
			if (sv4l2_queue(cam, index, 0) < 0)
			{
				err("camera buffer queuing error %m");
				if (errno == EAGAIN)
					continue;
				run = 0;
				break;
			}
			ret--;
			run = 0; //one shot only
		}
	}
	sv4l2_stop(cam);
	sfile_stop(file);
	return 0;
}

int main(int argc, char * const argv[])
{
	const char *configfile = NULL;
	const char *input = "v4l2";
	const char *output = "file:screem.unkown";

	CameraConfig_t CAMERACONFIG(inconfig, "/dev/video0");

	FileConfig_t FILECONFIG(outconfig, unknown);

	int opt;
	do
	{
		opt = getopt(argc, argv, "i:o:j:w:h:");
		switch (opt)
		{
			case 'i':
				input = optarg;
			break;
			case 'o':
				output = optarg;
			break;
			case 'j':
				configfile = optarg;
			break;
			case 'w':
				inconfig.parent.width = strtol(optarg, NULL, 10);
			break;
			case 'h':
				inconfig.parent.height = strtol(optarg, NULL, 10);
			break;
		}
	} while(opt != -1);

	if (configfile)
	{
		config_parseconfigfile(input, configfile, &inconfig.parent);
		config_parseconfigfile(output, configfile, &outconfig.parent);
	}

	V4L2_t *cam = sv4l2_create(inconfig.device, device_input, &inconfig);
	if (!cam)
	{
		err("camera not available");
		return -1;
	}

	outconfig.parent.width = inconfig.parent.width;
	outconfig.parent.height = inconfig.parent.height;
	outconfig.parent.fourcc = inconfig.parent.fourcc;
	outconfig.parent.stride = inconfig.parent.stride;
	outconfig.direction = File_Input_e;
	File_t *file = sfile_create(outconfig.filename, device_output, &outconfig);
	if (!file)
	{
		err("file not available");
		return -1;
	}

	int (*_sv4l2_loadsettings)(V4L2_t *dev, void *jconfig) = sv4l2_loadsettings;
	if (_sv4l2_loadsettings && inconfig.parent.entry)
		_sv4l2_loadsettings(cam, inconfig.parent.entry);

	int *dma_bufs = {0};
	size_t size = 0;
	int nbbufs = 0;
	if (sv4l2_requestbuffer(cam, buf_type_dmabuf | buf_type_master, &nbbufs, &dma_bufs, &size, NULL) < 0)
	{
		err("camera dma buffer not allowed");
		return -1;
	}
	if (sfile_requestbuffer(file, buf_type_dmabuf, nbbufs, dma_bufs, size, NULL) < 0)
	{
		err("file dma buffers not linked");
		return -1;
	}
	main_loop(cam, file);

	sv4l2_destroy(cam);
	sfile_destroy(file);
	return 0;
}
