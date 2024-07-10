#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

#include "log.h"
#include "sv4l2.h"
#include "sdrm.h"
#include "segl.h"
#include "config.h"

int main_loop(V4L2_t *cam, EGL_t *gpu)
{
	int run = 1;
	sv4l2_start(cam);
	segl_start(gpu);
	int camfd = sv4l2_fd(cam);
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
		int maxfd = camfd;

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

			if (segl_queue(gpu, index, bytesused) < 0)
			{
				err("gpu buffer queuing error %m");
				if (errno == EAGAIN)
					continue;
				run = 0;
				break;
			}
			if ((index = segl_dequeue(gpu)) < 0)
			{
				err("display buffer dequeuing error %m");
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
		}
	}
	sv4l2_stop(cam);
	segl_stop(gpu);
	return 0;
}

int main(int argc, char * const argv[])
{
	const char *configfile = NULL;
	const char *input = "cam";
	const char *gpuname = "gpu";
	const char *output = "screen";

	CameraConfig_t CAMERACONFIG(configcam, "/dev/video0");

	EGLConfig_t EGLCONFIG(configgpu, unknown);

	int opt;
	do
	{
		opt = getopt(argc, argv, "i:o:g:j:Iw:h:");
		switch (opt)
		{
			case 'i':
				input = optarg;
			break;
			case 'o':
				output = optarg;
			break;
			case 'g':
				gpuname = optarg;
			break;
			case 'j':
				configfile = optarg;
			break;
			case 'v':
				configcam.mode |= MODE_VERBOSE;
			break;
			case 'I':
				configcam.mode |= MODE_INTERACTIVE;
			break;
			case 'w':
				configcam.width = strtol(optarg, NULL, 10);
			break;
			case 'h':
				configcam.height = strtol(optarg, NULL, 10);
			break;
		}
	} while(opt != -1);

	if (configfile)
	{
		config_parseconfigfile(input, configfile, &configcam.parent);
		config_parseconfigfile(gpuname, configfile, &configgpu.parent);
	}

	V4L2_t *cam = sv4l2_create(configcam.device, V4L2_BUF_TYPE_VIDEO_CAPTURE, &configcam);
	if (!cam)
	{
		err("camera not available");
		return -1;
	}

	configgpu.texture.name = "vTexture";
	configgpu.texture.width = configcam.width;
	configgpu.texture.height = configcam.height;
	configgpu.texture.stride = configcam.stride;
	configgpu.texture.fourcc = configcam.fourcc;
	EGL_t *gpu = segl_create(&configgpu);
	if (!gpu)
	{
		err("gpu not available");
		return -1;
	}

	if (configfile)
	{
		config_parseconfigfile(input, configfile, &configcam.parent);
		config_parseconfigfile(gpuname, configfile, &configgpu.parent);
	}

	int *dma_bufs = {0};
	size_t size = 0;
	int nbbufs = 0;
	if (sv4l2_requestbuffer(cam, buf_type_dmabuf | buf_type_master, &nbbufs, &dma_bufs, &size, NULL) < 0)
	{
		err("drm dma buffer not allowed");
		return -1;
	}
	if (segl_requestbuffer(gpu, buf_type_dmabuf, nbbufs, dma_bufs, size, NULL) < 0)
	{
		err("segl dma buffers not linked");
		return -1;
	}
	main_loop(cam, gpu);

	sv4l2_destroy(cam);
	segl_destroy(gpu);
	return 0;
}
