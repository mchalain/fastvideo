#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/select.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <errno.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <string.h>
#include <stdarg.h>
#include <dlfcn.h>

#include <linux/videodev2.h>
#include <linux/v4l2-subdev.h>
#ifdef HAVE_JANSSON
#include <jansson.h>
#endif

#include "log.h"
#include "sv4l2.h"

#define MAX_BUFFERS 4

#define dbg_buffer_splane(v4l2) 		dbg("sv4l2: buf %d info:", v4l2->index); \
		dbg("\ttype: %s", (v4l2->type == V4L2_BUF_TYPE_VIDEO_CAPTURE)? "CAPTURE":"OUTPUT"); \
		dbg("\tmemory: %s", (v4l2->memory == V4L2_MEMORY_DMABUF)? "DMABUF":"MMAP"); \
		dbg("\tdmafd: %d", v4l2->m.fd); \
		dbg("\tlength: %u", v4l2->length); \
		dbg("\tbytesused: %u", v4l2->bytesused);
#define dbg_buffer_mplane(v4l2) 		dbg("sv4l2: mplane buf %d info:", v4l2->index); \
		dbg("\ttype: %s", (v4l2->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE)? "CAPTURE":"OUTPUT"); \
		dbg("\tmemory: %s", (v4l2->memory == V4L2_MEMORY_DMABUF)? "DMABUF":"MMAP"); \
		dbg("\tdmafd: %d", v4l2->m.planes[0].m.fd); \
		dbg("\tlength: %u", v4l2->m.planes[0].length); \
		dbg("\tbytesused: %u", v4l2->bytesused);
#define dbg_buffer(v4l2) if (v4l2->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE || v4l2->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) \
		{dbg_buffer_mplane(v4l2);}else{dbg_buffer_splane(v4l2);}

const char sv4l2_defaultdevice[20] = "/dev/video0";

DeviceConf_t formats[] = {
	{.fourcc = FOURCC('Y','U','Y','V')},
	{.fourcc = 0, },
};

typedef struct V4L2Buffer_s V4L2Buffer_t;
struct V4L2Buffer_s
{
	struct v4l2_buffer v4l2;
	struct v4l2_plane planes[VIDEO_MAX_PLANES];
	void *map;
	size_t length;
	struct {
		int (*getdmafd)(V4L2Buffer_t *buf);
		size_t (*getsize)(V4L2Buffer_t *buf);
		void (*setdma)(V4L2Buffer_t *buf, int fd, size_t size);
		void (*setmem)(V4L2Buffer_t *buf, void *mem, size_t size);
		void *(*mmap)(V4L2Buffer_t *buf, int fd);
	} ops;
};

#define MODE_CAPTURE 0x01
#define MODE_OUTPUT 0x02
#define MODE_MASTER 0x04
#define MODE_META 0x08
#define MODE_MEDIACTL 0x10
#define MODE_MPLANE 0x80

typedef struct V4L2_s V4L2_t;
struct V4L2_s
{
	const char *name;
	CameraConfig_t *config;
	int fd;
	int ctrlfd;
	enum v4l2_buf_type type;
	int nbuffers;
	int nplanes;
	V4L2Buffer_t *buffers;
	int mode;
	int ifd[2];
	struct {
		V4L2Buffer_t *(*createbuffers)(V4L2_t *dev, int number, enum v4l2_memory memory);
	} ops;
	int (*transfer)(void *, int id, const char *mem, size_t size);
};

static int sv4l2_subdev_open(CameraConfig_t *config);

static int _v4l2buffer_exportdmafd(V4L2Buffer_t *buf, int fd)
{
	struct v4l2_exportbuffer expbuf = {0};
	expbuf.type = buf->v4l2.type;
	expbuf.index = buf->v4l2.index;
	expbuf.flags = O_CLOEXEC | O_RDWR;;
	if (ioctl(fd, VIDIOC_EXPBUF, &expbuf) != 0)
	{
		return -1;
	}
	return expbuf.fd;
}

static int getdmafd_splane(V4L2Buffer_t *buf)
{
	return buf->v4l2.m.fd;
}

static size_t getsize_splane(V4L2Buffer_t *buf)
{
	return buf->v4l2.length;
}

static void setdma_splane(V4L2Buffer_t *buf, int fd, size_t size)
{
	buf->v4l2.m.fd = fd;
	buf->v4l2.length = size;
	buf->length = size;
}

static void setmem_splane(V4L2Buffer_t *buf, void *mem, size_t size)
{
	buf->v4l2.m.userptr = (uintptr_t)mem;
	buf->v4l2.length = size;
	buf->length = size;
}

static void *mmap_splane(V4L2Buffer_t *buf, int fd)
{
	size_t offset = buf->v4l2.m.offset;
	buf->length = buf->v4l2.length;
	buf->map = mmap(NULL, buf->length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, offset);
	return buf->map;
}

static int getdmafd_mplane(V4L2Buffer_t *buf)
{
	return buf->v4l2.m.planes[0].m.fd;
}

static size_t getsize_mplane(V4L2Buffer_t *buf)
{
	return buf->v4l2.m.planes[0].length;
}

static void setdma_mplane(V4L2Buffer_t *buf, int fd, size_t size)
{
	buf->v4l2.m.planes[0].m.fd = fd;
	buf->v4l2.m.planes[0].length = size;
	buf->length = size;
}

static void setmem_mplane(V4L2Buffer_t *buf, void *mem, size_t size)
{
	buf->v4l2.m.planes[0].m.userptr = (uintptr_t)mem;
	buf->v4l2.m.planes[0].length = size;
	buf->length = size;
}

static void *mmap_mplane(V4L2Buffer_t *buf, int fd)
{
	size_t offset = buf->v4l2.m.planes[0].m.mem_offset;
	buf->length = buf->v4l2.m.planes[0].length;
	buf->map = mmap(NULL, buf->length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, offset);
	return buf->map;
}

static V4L2Buffer_t *createbuffers_splane(V4L2_t *dev, int number, enum v4l2_memory memory)
{
	V4L2Buffer_t * buffers = calloc(number, sizeof(*dev->buffers));
	for (int i = 0; i < dev->nbuffers; i++)
	{
		buffers[i].v4l2.type = dev->type;
		buffers[i].v4l2.memory = memory;
		buffers[i].v4l2.index = i;
		buffers[i].ops.getdmafd = getdmafd_splane;
		buffers[i].ops.getsize = getsize_splane;
		buffers[i].ops.setdma = setdma_splane;
		buffers[i].ops.setmem = setmem_splane;
		buffers[i].ops.mmap = mmap_splane;
	}
	return buffers;
}

static V4L2Buffer_t *createbuffers_mplane(V4L2_t *dev, int number, enum v4l2_memory memory)
{
	V4L2Buffer_t * buffers = createbuffers_splane(dev, number, memory);
	for (int i = 0; i < dev->nbuffers; i++)
	{
		buffers[i].v4l2.m.planes = buffers[i].planes;
		buffers[i].v4l2.length = dev->nplanes;
		buffers[i].ops.getdmafd = getdmafd_mplane;
		buffers[i].ops.getsize = getsize_mplane;
		buffers[i].ops.setdma = setdma_mplane;
		buffers[i].ops.setmem = setmem_mplane;
		buffers[i].ops.mmap = mmap_mplane;
	}
	return buffers;
}

static enum v4l2_buf_type _v4l2_getbuftype(enum v4l2_buf_type type, int mode)
{
	if (type == 0 && (mode & MODE_OUTPUT))
	{
		if (mode & MODE_META)
			type = V4L2_BUF_TYPE_META_OUTPUT;
		else
			type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	}
	else if (type == 0 && (mode & MODE_CAPTURE))
	{
		if (mode & MODE_META)
			type = V4L2_BUF_TYPE_META_CAPTURE;
		else
			type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	}
	if (type == V4L2_BUF_TYPE_VIDEO_OUTPUT || type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE)
	{
		if ((mode & (MODE_OUTPUT | MODE_MPLANE)) == (MODE_MPLANE | MODE_OUTPUT))
			return V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
		if (mode & MODE_OUTPUT)
			return V4L2_BUF_TYPE_VIDEO_OUTPUT;
	}
	if (type == V4L2_BUF_TYPE_VIDEO_CAPTURE || type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE)
	{
		if ((mode & (MODE_CAPTURE | MODE_MPLANE)) == (MODE_MPLANE | MODE_CAPTURE))
			return V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
		if (mode & MODE_CAPTURE)
			return V4L2_BUF_TYPE_VIDEO_CAPTURE;
	}
	if ((type == V4L2_BUF_TYPE_META_CAPTURE || type == V4L2_BUF_TYPE_META_OUTPUT) && (mode & MODE_META))
	{
		return type;
	}
	return -1;
}

static int _v4l2_open(const char *interface, int *mode)
{
	int fd = open(interface, O_RDWR | O_NONBLOCK, 0);
	if (fd < 0)
	{
		err("open %s failed %m", interface);
		return -1;
	}
	return fd;
}

static int _v4l2_devicecapabilities(int fd, const char *interface, int *mode)
{
	struct v4l2_capability cap;
	if (ioctl(fd, VIDIOC_QUERYCAP, &cap) != 0)
	{
		err("device %s not video %m", interface);
		close(fd);
		return -1;
	}
#ifdef DEBUG
	dbg("device %s capabilities %#X", interface, cap.device_caps);
	if(cap.device_caps & V4L2_CAP_VIDEO_CAPTURE)
		dbg("device %s capture (camera)", interface);
	if(cap.device_caps & V4L2_CAP_VIDEO_CAPTURE_MPLANE)
		dbg("device %s capture mplane", interface);
	if(cap.device_caps & V4L2_CAP_VIDEO_OUTPUT)
		dbg("device %s output", interface);
	if(cap.device_caps & V4L2_CAP_VIDEO_OUTPUT_MPLANE)
		dbg("device %s output mplane", interface);
	if(cap.device_caps & V4L2_CAP_VIDEO_OVERLAY)
		dbg("device %s overlay", interface);
	if(cap.device_caps & V4L2_CAP_VIDEO_M2M)
		dbg("device %s memory to memory", interface);
	if(cap.device_caps & V4L2_CAP_VIDEO_M2M_MPLANE)
		dbg("device %s memory to memory mplane", interface);
	if(cap.device_caps & V4L2_CAP_AUDIO)
		dbg("device %s audio", interface);
	if(cap.device_caps & V4L2_CAP_VBI_CAPTURE)
		dbg("device %s vbi", interface);
	if(cap.device_caps & V4L2_CAP_RADIO)
		dbg("device %s radio", interface);
	if(cap.device_caps & V4L2_CAP_IO_MC)
		dbg("device %s media control available", interface);
#endif
	if (cap.device_caps & V4L2_CAP_VIDEO_CAPTURE ||
		cap.device_caps & V4L2_CAP_VIDEO_CAPTURE_MPLANE ||
		cap.device_caps & V4L2_CAP_VIDEO_M2M ||
		cap.device_caps & V4L2_CAP_VIDEO_M2M_MPLANE)
		*mode |= MODE_CAPTURE;
	if (cap.device_caps & V4L2_CAP_VIDEO_OUTPUT ||
		cap.device_caps & V4L2_CAP_VIDEO_OUTPUT_MPLANE ||
		cap.device_caps & V4L2_CAP_VIDEO_M2M ||
		cap.device_caps & V4L2_CAP_VIDEO_M2M_MPLANE)
		*mode |= MODE_OUTPUT;
	if (cap.device_caps & V4L2_CAP_VIDEO_CAPTURE_MPLANE ||
		cap.device_caps & V4L2_CAP_VIDEO_OUTPUT_MPLANE ||
		cap.device_caps & V4L2_CAP_VIDEO_M2M_MPLANE)
		*mode |= MODE_MPLANE;
	if (cap.device_caps & V4L2_CAP_META_CAPTURE ||
		cap.device_caps & V4L2_CAP_META_OUTPUT)
		*mode |= MODE_META;
	if (cap.device_caps & V4L2_CAP_IO_MC)
		*mode |= MODE_MEDIACTL;

	if (!(cap.device_caps & V4L2_CAP_STREAMING))
	{
		err("device %s not camera", interface);
		close(fd);
		return -1;
	}
	dbg("%s streaming available (%#x)", interface, *mode);
	return 0;
}

static uint32_t _v4l2_setpixformat(int fd, enum v4l2_buf_type type, CameraConfig_t *config)
{
	uint32_t pixelformat = 0;

	struct v4l2_format fmt;
	fmt.type = type;
	if (ioctl(fd, VIDIOC_G_FMT, &fmt) != 0)
	{
		err("FMT not found %m");
		return -1;
	}
	if (type == V4L2_BUF_TYPE_META_CAPTURE || type == V4L2_BUF_TYPE_META_OUTPUT)
		pixelformat = fmt.fmt.meta.dataformat;
	else
		pixelformat = fmt.fmt.pix.pixelformat;

	struct v4l2_fmtdesc fmtdesc = {0};
	fmtdesc.type = type;
	dbg("Formats:");
	while (ioctl(fd, VIDIOC_ENUM_FMT, &fmtdesc) == 0)
	{
		dbg("\t%.4s => %s", (char*)&fmtdesc.pixelformat,
				fmtdesc.description);
		fmtdesc.index++;
		if (config->parent.fourcc != 0)
		{
			if (fmtdesc.pixelformat != config->parent.fourcc)
				continue;
			pixelformat = fmtdesc.pixelformat;
		}
		int i = 0;
		while (formats[i].fourcc != 0 && formats[i].fourcc != fmtdesc.pixelformat) i++;
		if (formats[i].fourcc != 0)
		{
			pixelformat = formats[i].fourcc;
		}
	}
	if (type == V4L2_BUF_TYPE_META_CAPTURE || type == V4L2_BUF_TYPE_META_OUTPUT)
		fmt.fmt.meta.dataformat = pixelformat;
	else
		fmt.fmt.pix.pixelformat = pixelformat;
	if (ioctl(fd, VIDIOC_S_FMT, &fmt) != 0)
	{
		return -1;
	}
	dbg("V4l2 settings: %.4s", (char*)&fmt.fmt.pix.pixelformat);
	config->parent.fourcc = fmt.fmt.pix.pixelformat;
	return pixelformat;
}

static uint32_t _v4l2_setframesize(int fd, enum v4l2_buf_type type, CameraConfig_t *config)
{
	uint32_t framesize = 0;
	struct v4l2_frmsizeenum video_cap = {0};
	video_cap.pixel_format = config->parent.fourcc;
	video_cap.index = 0;
	if (ioctl(fd, VIDIOC_ENUM_FRAMESIZES, &video_cap) != 0)
	{
		err("framsesize enumeration error %m");
		return -1;
	}
	struct v4l2_format fmt = {0};
	fmt.type = type;
	fmt.fmt.pix.field = V4L2_FIELD_ANY;

	dbg("Frame size:");
	if (video_cap.type == V4L2_FRMSIZE_TYPE_STEPWISE)
	{
		dbg("\t%d < width  < %d, step %d", video_cap.stepwise.min_width, video_cap.stepwise.max_width, video_cap.stepwise.step_width);
		dbg("\t%d < height < %d, step %d", video_cap.stepwise.min_height, video_cap.stepwise.max_height, video_cap.stepwise.step_height);
		if (config->parent.width < (video_cap.stepwise.max_width + 1) && config->parent.width > (video_cap.stepwise.min_width - 1))
			fmt.fmt.pix.width = config->parent.width;
		else if (config->parent.width > (video_cap.stepwise.max_width))
			fmt.fmt.pix.width = video_cap.stepwise.max_width;
		else
			fmt.fmt.pix.width = video_cap.stepwise.min_width;

		if (config->parent.height < (video_cap.stepwise.max_height + 1) && config->parent.height > (video_cap.stepwise.min_height - 1))
			fmt.fmt.pix.height = config->parent.height;
		else if (config->parent.height > (video_cap.stepwise.max_height))
			fmt.fmt.pix.height = video_cap.stepwise.max_height;
		else
			fmt.fmt.pix.height = video_cap.stepwise.min_height;
	}
	else if (video_cap.type == V4L2_FRMSIZE_TYPE_DISCRETE)
	{
		uint32_t nbpixels = config->parent.height * config->parent.width;
		if (config->parent.height == 0 && config->parent.width > 0)
			config->parent.height = (config->parent.width * 9) / 16;
		for (video_cap.index = 0; ioctl(fd, VIDIOC_ENUM_FRAMESIZES, &video_cap) != -1; video_cap.index++)
		{
			dbg("\twidth %d, height %d", video_cap.discrete.width, video_cap.discrete.height);
			if (config->parent.height > 0 && config->parent.height <= video_cap.discrete.height)
			{
				fmt.fmt.pix.height = video_cap.discrete.height;
				fmt.fmt.pix.width = video_cap.discrete.width;
				if (nbpixels == video_cap.discrete.height * video_cap.discrete.width)
					break;
			}
		}
	}

	fmt.fmt.pix.pixelformat = config->parent.fourcc;
	if (ioctl(fd, VIDIOC_S_FMT, &fmt) != 0)
	{
		return -1;
	}

	return framesize;
}

static int _v4l2_setfps(int fd, enum v4l2_buf_type type, int fps)
{
	struct v4l2_streamparm streamparm = {0};
	streamparm.type = type;
	if (ioctl(fd, VIDIOC_G_PARM, &streamparm) == -1)
	{
		err("FPS info not available %m");
		return -1;
	}

	if (fps == -1)
	{
		fps = streamparm.parm.capture.timeperframe.denominator /
				streamparm.parm.capture.timeperframe.numerator;
		dbg("Frame per second:");
		dbg("\t%d / %d %u", streamparm.parm.capture.timeperframe.denominator,
					streamparm.parm.capture.timeperframe.numerator, fps);
	}
	else if (fps >= 0 & fps != streamparm.parm.capture.timeperframe.denominator)
	{
		streamparm.parm.capture.timeperframe.denominator = fps;
		streamparm.parm.capture.timeperframe.numerator = 1;
		if (ioctl(fd, VIDIOC_S_PARM, &streamparm) == -1)
		{
			err("FPS setting error %m");
			return -1;
		}
	}
	else if (fps < 0 & -fps != streamparm.parm.capture.timeperframe.numerator)
	{
		streamparm.parm.capture.timeperframe.denominator = 1;
		streamparm.parm.capture.timeperframe.numerator = fps;
		if (ioctl(fd, VIDIOC_S_PARM, &streamparm) == -1)
		{
			err("FPS setting error %m");
			return -1;
		}
	}
	return fps;
}

static int _v4l2_getbufferfd(V4L2_t *dev, int i)
{
	if (dev->nbuffers <= i)
		return -1;
	int dma_fd = dev->buffers[i].ops.getdmafd(&dev->buffers[i]);
	if (dev->buffers[i].v4l2.memory == V4L2_MEMORY_DMABUF && dma_fd > 0)
	{
		return dma_fd;
	}
	return _v4l2buffer_exportdmafd(&dev->buffers[i], dev->fd);
}

int sv4l2_requestbuffer_mmap(V4L2_t *dev)
{
	int ret = 0;
	int count = MAX_BUFFERS;
	if (dev->buffers && dev->buffers[0].v4l2.memory == V4L2_MEMORY_MMAP)
		return 0;
	if (dev->buffers)
	{
		count = dev->nbuffers;
		struct v4l2_requestbuffers req = {0};
		req.type = dev->buffers[0].v4l2.type;
		req.memory = dev->buffers[0].v4l2.memory;
		req.count = 0;
		if (ioctl(dev->fd, VIDIOC_REQBUFS, &req) == -1)
		{
			err("sv4l2: Release buffer for mmap error %m");
			return -1;
		}
		free(dev->buffers);
	}
	struct v4l2_requestbuffers req = {0};
	req.count = count;
	req.type = dev->type;
	req.memory = V4L2_MEMORY_MMAP;
	if (ioctl(dev->fd, VIDIOC_REQBUFS, &req) != 0)
	{
		err("sv4l2: Request buffer for mmap error %m");
		return -1;
	}
	if (req.capabilities & V4L2_BUF_CAP_SUPPORTS_DMABUF)
	{
		dbg("buffer supports DMABUF too");
	}
	dev->nbuffers = req.count;
	dev->buffers = dev->ops.createbuffers(dev, dev->nbuffers, V4L2_MEMORY_MMAP);

	dbg("request %d buffers", req.count);
	for (int i = 0; i < dev->nbuffers; i++)
	{
		if (ioctl(dev->fd, VIDIOC_QUERYBUF, &dev->buffers[i].v4l2) != 0)
		{
			err("sv4l2: Query buffer for mmap error %m");
			dev->nbuffers = i;
			return -1;
		}
		if (dev->buffers[i].ops.mmap(&dev->buffers[i], dev->fd) == MAP_FAILED)
		{
			err("sv4l2: buffer mmap error %m");
			ret = -1;
			break;
		}
	}

	return ret;
}

int sv4l2_requestbuffer_dmabuf(V4L2_t *dev)
{
	int count = MAX_BUFFERS;
	if (dev->buffers && dev->buffers[0].v4l2.memory == V4L2_MEMORY_DMABUF)
		return 0;
	V4L2Buffer_t *oldbuffers = NULL;
	if (dev->mode & MODE_MASTER)
	{
		/// master request MMAP first to export the DMA in a second time
		if (sv4l2_requestbuffer_mmap(dev) < 0)
		{
			err("mmap error");
			return -1;
		}
		oldbuffers = dev->buffers;
		count = dev->nbuffers;
		for (int i = 0; i < dev->nbuffers; i++)
		{
			int dma_fd = _v4l2_getbufferfd(dev, i);
			size_t size = dev->buffers[i].ops.getsize(&dev->buffers[i]);
			dev->buffers[i].ops.setdma(&dev->buffers[i], dma_fd, size);
		}
		struct v4l2_requestbuffers req = {0};
		req.type = dev->buffers[0].v4l2.type;
		req.memory = dev->buffers[0].v4l2.memory;
		req.count = 0;
		if (ioctl(dev->fd, VIDIOC_REQBUFS, &req) == -1)
		{
			err("Free buffer for dma error %m");
			return -1;
		}
	}

	struct v4l2_requestbuffers req = {0};
	req.count = count;
	req.type = dev->type;
	req.memory = V4L2_MEMORY_DMABUF;
	if (ioctl(dev->fd, VIDIOC_REQBUFS, &req) != 0)
	{
		err("device doesn't allow DMABUF %m");
		return -1;
	}
	dev->nbuffers = req.count;
	dev->buffers = calloc(dev->nbuffers, sizeof(*dev->buffers));
	dev->buffers = dev->ops.createbuffers(dev, dev->nbuffers, V4L2_MEMORY_DMABUF);

	dbg("request %d buffers", req.count);
	for (int i = 0; i < dev->nbuffers; i++)
	{
		if (oldbuffers)
		{
			int dma_fd = oldbuffers[i].ops.getdmafd(&oldbuffers[i]);
			size_t size = oldbuffers[i].ops.getsize(&oldbuffers[i]);
			dev->buffers[i].ops.setdma(&dev->buffers[i], dma_fd, size);
		}
	}
	if (oldbuffers)
		free(oldbuffers);
	return 0;
}

int sv4l2_requestbuffer_userptr(V4L2_t *dev, int nmems, void *mems[], size_t size)
{
	int count = MAX_BUFFERS;
	if (dev->buffers && dev->buffers[0].v4l2.memory == V4L2_MEMORY_USERPTR)
		return 0;
	if (dev->buffers)
	{
		count = dev->nbuffers;
		struct v4l2_requestbuffers req = {0};
		req.type = dev->buffers[0].v4l2.type;
		req.memory = dev->buffers[0].v4l2.memory;
		req.count = 0;
		if (ioctl(dev->fd, VIDIOC_REQBUFS, &req) == -1)
		{
			err("sv4l2: Relsease buffer for mmap error %m");
			return -1;
		}
		free(dev->buffers);
	}
	if (count > nmems)
		count = nmems;

	struct v4l2_requestbuffers req = {0};
	req.count = count;
	req.type = dev->type;
	req.memory = V4L2_MEMORY_USERPTR;
	if (ioctl(dev->fd, VIDIOC_REQBUFS, &req) != 0)
	{
		if (errno == EINVAL)
			err("sv4l2: UserPtr memory not supported");
		else
			err("Request buffer for mmap error %m");
		return -1;
	}
	dev->nbuffers = req.count;
	dev->buffers = dev->ops.createbuffers(dev, dev->nbuffers, V4L2_MEMORY_USERPTR);
	if (dev->nbuffers > nmems)
		err("Not enougth memory buffers");

	dbg("request %d buffers", req.count);
	count = (dev->nbuffers > nmems)? nmems:dev->nbuffers;
	for (int i = 0; i < count; i++)
	{
		dev->buffers[i].ops.setmem(&dev->buffers[i], mems[i], size);
	}
	return 0;
}

int sv4l2_linkv4l2(V4L2_t *dev, V4L2_t *target)
{
	for (int i = 0; i < dev->nbuffers; i++)
	{
		if (dev->buffers[i].v4l2.memory != V4L2_MEMORY_DMABUF)
			return -1;
		int dma_fd = _v4l2_getbufferfd(target, i);
		size_t size = target->buffers[i].ops.getsize(&target->buffers[i]);
		dev->buffers[i].ops.setdma(&dev->buffers[i], dma_fd, size);
		if (dma_fd == -1)
		{
			dev->nbuffers = i;
			return -1;
		}
	}
	return 0;
}

int sv4l2_linkdma(V4L2_t *dev, int ntargets, int targets[], size_t size)
{
	for (int i = 0; i < dev->nbuffers; i++)
	{
		if (dev->buffers[i].v4l2.memory != V4L2_MEMORY_DMABUF)
			return -1;
		if (i == ntargets)
			return -1;
		int dma_fd = targets[i];
		dev->buffers[i].ops.setdma(&dev->buffers[i], dma_fd, size);
	}
	return 0;
}

int sv4l2_requestbuffer(V4L2_t *dev, enum buf_type_e t, ...)
{
	int ret = 0;
	if (t & buf_type_master)
		dev->mode |= MODE_MASTER;
	va_list ap;
	va_start(ap, t);
	switch (t)
	{
		case buf_type_sv4l2 | buf_type_master:
			ret = sv4l2_requestbuffer_dmabuf(dev);
		break;
		case buf_type_sv4l2:
		{
			V4L2_t *master = va_arg(ap, V4L2_t *);
			if ((ret = sv4l2_requestbuffer_dmabuf(dev)) == 0)
				ret = sv4l2_linkv4l2(dev, master);
		}
		break;
		case buf_type_memory:
		{
			int nmem = va_arg(ap, int);
			void **mems = va_arg(ap, void **);
			size_t size = va_arg(ap, size_t);
			ret = sv4l2_requestbuffer_userptr(dev, nmem, mems, size);
		}
		break;
		case (buf_type_memory | buf_type_master):
			ret = sv4l2_requestbuffer_mmap(dev);
		break;
		case buf_type_dmabuf | buf_type_master:
			ret = sv4l2_requestbuffer_dmabuf(dev);
			if (ret)
				break;
			int *ntargets = va_arg(ap, int *);
			int **targets = va_arg(ap, int **);
			size_t *size = va_arg(ap, size_t *);
			if (ntargets != NULL)
				*ntargets = dev->nbuffers;
			if (targets != NULL)
			{
				*targets = calloc(dev->nbuffers, sizeof(int));
				for (int i = 0; i < dev->nbuffers; i++)
					(*targets)[i] = dev->buffers[i].ops.getdmafd(&dev->buffers[i]);
			}
			if (size != NULL)
				*size = dev->buffers[0].ops.getsize(&dev->buffers[0]);
		break;
		case buf_type_dmabuf:
		{
			int ntargets = va_arg(ap, int);
			int *targets = va_arg(ap, int *);
			size_t size = va_arg(ap, size_t);
			if ((ret = sv4l2_requestbuffer_dmabuf(dev)) == 0)
			{
				ret = sv4l2_linkdma(dev, ntargets, targets, size);
			}
		}
		break;
		default:
			err("sv4l2: unkonwn buffer type");
			va_end(ap);
			return -1;
	}
	va_end(ap);
#ifdef DEBUG
	for (int i = 0; i < dev->nbuffers; i++)
	{
		dbg_buffer((&dev->buffers[i].v4l2));
	}
#endif
	return ret;
}

int sv4l2_crop(V4L2_t *dev, struct v4l2_rect *r)
{
	struct v4l2_selection sel = {0};
	sel.type = dev->type;
	sel.target = V4L2_SEL_TGT_CROP_DEFAULT;
	if (r != NULL)
	{
		sel.target = V4L2_SEL_TGT_CROP;
		memcpy(&sel.r, r, sizeof(sel.r));
	}
	sel.flags = V4L2_SEL_FLAG_GE | V4L2_SEL_FLAG_LE;
	if (ioctl(dev->fd, VIDIOC_S_SELECTION, &sel))
	{
		err("cropping error %m");
		return -1;
	}
	dbg("sv4l2: croping requested (%d %d %d %d)", sel.r.left, sel.r.top, sel.r.width, sel.r.height);
	return 0;
}

static void * _sv4l2_control(int ctrlfd, int id, void *value, struct v4l2_queryctrl queryctrl)
{
#if 1
	if (V4L2_CTRL_ID2CLASS(id) == V4L2_CTRL_CLASS_USER)
	{
		uint32_t ivalue = (uint32_t) value;
		struct v4l2_control control = {0};
		control.id = id;
		control.value = ivalue;
		if (value != (void*)-1 && ioctl(ctrlfd, VIDIOC_S_CTRL, &control))
		{
			err("control %#x setting error %m", id);
			return (void *)-1;
		}
		control.value = 0;
		if (ioctl(ctrlfd, VIDIOC_G_CTRL, &control))
		{
			err("device doesn't support control %#x %m", id);
			return (void *)-1;
		}
		value = (void *)control.value;
		dbg("control %#x => %d", id, control.value);
	}
	else
#endif
	if (value != (void*)-1 &&
		(queryctrl.type != V4L2_CTRL_TYPE_CTRL_CLASS))
	{
		struct v4l2_ext_control control = {0};
		control.id = id;
		if (queryctrl.type == V4L2_CTRL_TYPE_INTEGER)
			control.size = sizeof(uint32_t);
		else if (queryctrl.type == V4L2_CTRL_TYPE_BOOLEAN)
			control.size = 1;
		else if (queryctrl.type == V4L2_CTRL_TYPE_MENU)
			control.size = sizeof(uint32_t);
		else if (queryctrl.type == V4L2_CTRL_TYPE_BUTTON)
			control.size = 0;
		else if (queryctrl.type == V4L2_CTRL_TYPE_INTEGER64)
			control.size = sizeof(uint64_t);
		else if (queryctrl.type == V4L2_CTRL_TYPE_STRING && value != NULL)
			control.size = strlen(value) + 1;
		else
			control.size = 0;
		control.string = value;
		struct v4l2_ext_controls controls = {0};
		controls.count = 1;
		controls.controls = &control;
		if (ioctl(ctrlfd, VIDIOC_S_EXT_CTRLS, &controls))
		{
			err("control %#x setting error %m", id);
			return (void *)-1;
		}
	}
	struct v4l2_ext_control control = {0};
	control.id = id;
	char string[256];
	if (queryctrl.type == V4L2_CTRL_TYPE_INTEGER)
		control.size = sizeof(uint32_t);
	else if (queryctrl.type == V4L2_CTRL_TYPE_BOOLEAN)
		control.size = 1;
	else if (queryctrl.type == V4L2_CTRL_TYPE_MENU)
		control.size = sizeof(uint32_t);
	else if (queryctrl.type == V4L2_CTRL_TYPE_BUTTON)
		control.size = 0;
	else if (queryctrl.type == V4L2_CTRL_TYPE_INTEGER64)
		control.size = sizeof(uint64_t);
	else if (queryctrl.type == V4L2_CTRL_TYPE_STRING)
	{
		control.size = sizeof(string);
		control.string = string;
	}
	else
		control.size = 0;
	struct v4l2_ext_controls controls = {0};
	controls.count = 1;
	controls.controls = &control;
	if ((queryctrl.type != V4L2_CTRL_TYPE_BUTTON) &&
		(queryctrl.type != V4L2_CTRL_TYPE_CTRL_CLASS) &&
		ioctl(ctrlfd, VIDIOC_G_EXT_CTRLS, &controls))
	{
		err("control %#x setting error %m", id);
		return (void *)-1;
	}
	value = control.string;
	return value;
}

void * sv4l2_control(V4L2_t *dev, int id, void *value)
{
	int ctrlfd = dev->ctrlfd;
	struct v4l2_queryctrl queryctrl = {0};
	queryctrl.id = id;
	int ret = ioctl (ctrlfd, VIDIOC_QUERYCTRL, &queryctrl);
	if (ret != 0)
	{
		if (dev->ctrlfd != dev->fd)
		{
			ctrlfd = dev->fd;
			ret = ioctl (ctrlfd, VIDIOC_QUERYCTRL, &queryctrl);
		}
		if (ret != 0)
		{
			err("control %#x not supported", id);
			return (void *)-1;
		}
	}
	if (queryctrl.flags & V4L2_CTRL_FLAG_DISABLED)
	{
		err("control %#x disabled", id);
		return 0;
	}
	return _sv4l2_control(ctrlfd, id, value, queryctrl);
}

static int _sv4l2_treecontrols(int ctrlfd, V4L2_t *dev, int (*cb)(void *arg, struct v4l2_queryctrl *ctrl, V4L2_t *dev), void * arg)
{
	int nbctrls = 0;
	struct v4l2_queryctrl qctrl = {0};
	qctrl.id = V4L2_CTRL_FLAG_NEXT_CTRL;
	int ret;
	for (nbctrls = 0; (ret = ioctl(ctrlfd, VIDIOC_QUERYCTRL, &qctrl)) == 0; nbctrls++)
	{
		if (qctrl.flags & V4L2_CTRL_FLAG_DISABLED)
		{
			dbg("control %s %#x disabled", qctrl.name, qctrl.id);
			qctrl.id |= V4L2_CTRL_FLAG_NEXT_CTRL;
			continue;
		}
		dbg("sv4l2: control %s id %#x", qctrl.name, qctrl.id);
		if (cb)
			cb(arg, &qctrl, dev);
		qctrl.id |= V4L2_CTRL_FLAG_NEXT_CTRL;
	}
	if (ret)
	{
		err("sv4l2: %s query controls error %m", dev->config->parent.name);
	}
	return nbctrls;
}

int sv4l2_treecontrols(V4L2_t *dev, int (*cb)(void *arg, struct v4l2_queryctrl *ctrl, V4L2_t *dev), void * arg)
{
	int nbctrls = 0;
	nbctrls += _sv4l2_treecontrols(dev->ctrlfd, dev, cb, arg);
	if (dev->fd != dev->ctrlfd)
		nbctrls += _sv4l2_treecontrols(dev->fd, dev, cb, arg);
	return nbctrls;
}

int sv4l2_treecontrolmenu(V4L2_t *dev, int id, int (*cb)(void *arg, struct v4l2_querymenu *ctrl, V4L2_t *dev), void * arg)
{
	struct v4l2_querymenu querymenu = {0};
	querymenu.id = id;
	for (querymenu.index = 0; ioctl(sv4l2_fd(dev), VIDIOC_QUERYMENU, &querymenu) == 0; querymenu.index++)
	{
		if (dev->config->mode & MODE_VERBOSE)
		{
			dbg("menu[%d] %s", querymenu.index, querymenu.name);
		}
		if (cb)
			cb(arg, &querymenu, dev);
	}
}

V4L2_t *sv4l2_create(const char *devicename, CameraConfig_t *config)
{
	enum v4l2_buf_type type = 0;
	int mode = 0;
	if (config && config->mode)
		mode = config->mode;
	const char *device = devicename;
	if (config && config->device)
		device = config->device;
	int fd = config->fd;
	if (fd == 0)
		fd = _v4l2_open(device, &mode);
	if (fd == -1)
		return NULL;
	if (_v4l2_devicecapabilities(fd, device, &mode))
		return NULL;
	int ctrlfd = fd;

	type = _v4l2_getbuftype(type, mode);
	if (config->mode & MODE_VERBOSE)
		warn("SV4l2 create %s", devicename);

	if (_v4l2_setpixformat(fd, type, config) == -1)
	{
		err("pixel format error %m");
		close(fd);
		return NULL;
	}

	if (!(mode & MODE_META) &&
		_v4l2_setframesize(fd, type, config) == -1)
	{
		err("frame size error %m");
		close(fd);
		return NULL;
	}

	_v4l2_setfps(fd, type, config->fps);

	struct v4l2_format fmt;
	fmt.type = type;
	fmt.fmt.pix.field = V4L2_FIELD_ANY;
	if (ioctl(fd, VIDIOC_G_FMT, &fmt) != 0)
	{
		err("FMT not found %m");
		close(fd);
		return NULL;
	}
	uint32_t bytesperline = 0;
	uint32_t sizeimage = 0;
	if (mode & MODE_MPLANE)
	{
		config->parent.width = fmt.fmt.pix_mp.width;
		config->parent.height = fmt.fmt.pix_mp.height;
		config->parent.fourcc = fmt.fmt.pix_mp.pixelformat;
		bytesperline = fmt.fmt.pix_mp.plane_fmt[0].bytesperline;
		sizeimage = fmt.fmt.pix_mp.plane_fmt[0].sizeimage;
	}
	else
	{
		config->parent.width = fmt.fmt.pix.width;
		config->parent.height = fmt.fmt.pix.height;
		config->parent.fourcc = fmt.fmt.pix.pixelformat;
		bytesperline = fmt.fmt.pix.bytesperline;
		sizeimage = fmt.fmt.pix.sizeimage;
	}

	if (bytesperline)
		config->parent.stride = bytesperline;
	else
		config->parent.stride = sizeimage / config->parent.height;

	if (mode & MODE_MEDIACTL)
	{
		int fd = sv4l2_subdev_open(config);
		if (fd > 0)
			ctrlfd = fd;
	}

	V4L2_t *dev = calloc(1, sizeof(*dev));
	dev->name = devicename;
	dev->config = config;
	dev->fd = fd;
	dev->ctrlfd = ctrlfd;
	dev->type = type;
	dev->mode = mode;
	dev->transfer = config->transfer;

	dev->ops.createbuffers = createbuffers_splane;
	dev->nplanes = 1;
	if (mode & MODE_MPLANE)
	{
		dev->ops.createbuffers = createbuffers_mplane;
		dev->nplanes = fmt.fmt.pix_mp.num_planes;
	}
	if (config->mode & MODE_INTERACTIVE && pipe(dev->ifd))
	{
		err("interactive is disabled %m");
	}
	dbg("V4l2 settings: %dx%d, %.4s", config->parent.width, config->parent.height, (char*)&config->parent.fourcc);
	config->parent.dev = dev;
	return dev;
}

int sv4l2_fd(V4L2_t *dev)
{
	return dev->fd;
}

int sv4l2_type(V4L2_t *dev)
{
	return dev->type;
}

int sv4l2_interactive(V4L2_t *dev, const char *json, size_t length)
{
	return write(dev->ifd[1], json, length);
}

int sv4l2_start(V4L2_t *dev)
{
	enum v4l2_buf_type type = dev->type;
	if (dev->type == V4L2_BUF_TYPE_VIDEO_CAPTURE ||
		dev->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE)
	{
		dbg("sv4l2: start buffers enqueuing");
		for (int i = 0; i < dev->nbuffers; i++)
		{
			dbg_buffer((&dev->buffers[i].v4l2));
			if (sv4l2_queue(dev, i, 0))
				return -1;
		}
	}
	if (ioctl(dev->fd, VIDIOC_STREAMON, &type) != 0)
		return -1;
	dbg("sv4l2: starting");
	return 0;
}

int sv4l2_stop(V4L2_t *dev)
{
	enum v4l2_buf_type type = dev->type;
	if (ioctl(dev->fd, VIDIOC_STREAMOFF, &type) != 0)
		return -1;
	return 0;
}

int sv4l2_dequeue(V4L2_t *dev, void **mem, size_t *bytesused)
{
	int ret = 0;
	struct v4l2_buffer buf = {0};
	buf.type = dev->type;
	buf.memory = dev->buffers[0].v4l2.memory;
	struct v4l2_plane planes[VIDEO_MAX_PLANES] = {0};
	if (dev->mode & MODE_MPLANE)
	{
		buf.m.planes = planes;
		buf.length = dev->nplanes;
	}
	ret = ioctl(dev->fd, VIDIOC_DQBUF, &buf);
	if (ret)
	{
		err("sv4l2: %s dequeueing error %m", dev->config->parent.name);
		dbg_buffer((&buf));
		return -1;
	}
	if (!ret && bytesused)
	{
		*bytesused = buf.bytesused;
		if (dev->mode & MODE_MPLANE)
		{
			*bytesused = buf.m.planes[0].bytesused;
		}
	}
	if (!ret && mem)
		*mem = dev->buffers[buf.index].map;
	return buf.index;
}

int sv4l2_queue(V4L2_t *dev, int index, size_t bytesused)
{
	int ret = 0;
	if (bytesused > 0)
		dev->buffers[index].v4l2.bytesused = bytesused;
	ret = ioctl(dev->fd, VIDIOC_QBUF, &dev->buffers[index].v4l2);
	if (ret)
	{
		dbg_buffer((&dev->buffers[index].v4l2));
		err("sv4l2: %s queueing error %m", dev->config->parent.name);
	}
	return ret;
}

int sv4l2_loop(V4L2_t *dev, int (*transfer)(void *, int id, const char *mem, size_t size), void *transferarg)
{
	if (transfer == NULL)
		transfer = dev->transfer;
	if (transfer == NULL)
	{
		err("transfer function is unset");
		return -1;
	}
	int run = 1;
	sv4l2_start(dev);
	while (run)
	{
		fd_set rfds;
		FD_ZERO(&rfds);
		FD_SET(dev->fd, &rfds);
		struct timeval timeout = {
			.tv_sec = 2,
			.tv_usec = 0,
		};
		int ret;
		int maxfd = dev->fd;
		if (dev->ifd[0] > 0)
		{
			FD_SET(dev->ifd[0], &rfds);
			maxfd = (maxfd > dev->ifd[0])? maxfd:dev->ifd[0];
		}

		ret = select(maxfd + 1, &rfds, NULL, NULL, &timeout);
		if (ret == -1 && errno == EINTR)
			continue;
		if (ret == 0)
			warn("frame timeout");
		if (ret > 0 && FD_ISSET(dev->fd, &rfds))
		{
			int index = 0;
			void *mem = NULL;
			size_t bytesused = 0;
			if ((index = sv4l2_dequeue(dev, &mem, &bytesused)) < 0)
			{
				if (errno == EAGAIN)
					continue;
				run = 0;
				err("sv4l2: dequeuing error %m");
				break;
			}

			transfer(transferarg, index, mem, bytesused);
			if (sv4l2_queue(dev, index, 0) < 0)
			{
				run = 0;
				err("sv4l2: queuing error %m");
				break;
			}
			ret--;
		}
#ifdef HAVE_JANSSON
		if (ret > 0 && FD_ISSET(dev->ifd[0], &rfds))
		{
			json_error_t error;
			json_t *jconfig = json_loadfd(dev->ifd[0], JSON_DISABLE_EOF_CHECK, &error);
			if (jconfig == NULL)
			{
				err("interactive error %s", error.text);
			}
			else if (json_is_string(jconfig))
			{
				if (!strcmp("stop", json_string_value(jconfig)))
				{
					warn("sv4l2: stop requested");
					run = 0;
				}
			}
			else
				sv4l2_loadjsonsettings(dev, jconfig);
			ret--;
		}
#endif
	}
	sv4l2_stop(dev);
	return 0;
}

int sv4l2_transfer(V4L2_t *dev, V4L2_t *link)
{
	if (dev->buffers[0].v4l2.memory != link->buffers[0].v4l2.memory)
	{
		err("bad memory trnasfer type, change to %#x", dev->buffers[0].v4l2.memory);
		return -1;
	}
	int run = 1;
	sv4l2_start(dev);
	int infd = dev->fd;
	int outfd = link->fd;
	while (run)
	{
		fd_set rfds;
		fd_set wfds;
		FD_ZERO(&rfds);
		FD_ZERO(&wfds);
		FD_SET(infd, &rfds);
		FD_SET(outfd, &wfds);
		struct timeval timeout = {
			.tv_sec = 2,
			.tv_usec = 0,
		};
		int ret;
		int maxfd = (infd > outfd)? infd: outfd;
		if (dev->ifd[0] > 0)
		{
			FD_SET(dev->ifd[0], &rfds);
			maxfd = (maxfd > dev->ifd[0])? maxfd:dev->ifd[0];
		}

		ret = select(maxfd + 1, &rfds, &wfds, NULL, &timeout);
		if (ret == -1 && errno == EINTR)
			continue;
		if (ret == 0)
		{
			err("camera timeout");
			continue;
		}
		if (ret > 0 && FD_ISSET(infd, &rfds))
		{
			int index = 0;
			if ((index = sv4l2_dequeue(dev, NULL, NULL)) < 0)
			{
				err("input buffer dequeuing error %m");
				if (errno == EAGAIN)
					continue;
				run = 0;
				break;
			}

			if (sv4l2_queue(link, index, 0) < 0)
			{
				err("input buffer queuing error %m");
				if (errno == EAGAIN)
					continue;
				run = 0;
				break;
			}
			ret--;
		}
		if (ret > 0 && FD_ISSET(outfd, &wfds))
		{
			int index;
			if ((index = sv4l2_dequeue(link, NULL, NULL)) < 0)
			{
				err("output buffer dequeuing error %m");
				if (errno == EAGAIN)
					continue;
				run = 0;
				break;
			}

			if (sv4l2_queue(dev, index, 0) < 0)
			{
				err("output buffer queuing error %m");
				if (errno == EAGAIN)
					continue;
				run = 0;
				break;
			}
			ret--;
		}
#ifdef HAVE_JANSSON
		if (ret > 0 && FD_ISSET(dev->ifd[0], &rfds))
		{
			json_error_t error;
			json_t *jconfig = json_loadfd(dev->ifd[0], JSON_DISABLE_EOF_CHECK, &error);
			if (jconfig == NULL)
			{
				err("interactive error %s", error.text);
			}
			else if (json_is_string(jconfig))
			{
				if (!strcmp("stop", json_string_value(jconfig)))
					run = 0;
			}
			else
				sv4l2_loadjsonsettings(dev, jconfig);
			ret--;
		}
#endif
	}
	sv4l2_stop(dev);
	return 0;
}

void sv4l2_destroy(V4L2_t *dev)
{
	for (int i = 0; i < dev->nbuffers; i++)
	{
		if (dev->buffers[i].map)
			munmap(dev->buffers[i].map, dev->buffers[i].length);
	}
	free(dev->buffers);
	close(dev->fd);
	free(dev);
}

#ifdef VIDIOC_SUBDEV_S_FMT

static uint32_t sv4l2_subdev_translate_fmtbus(int ctrlfd, uint32_t fourcc)
{
	uint32_t ret = -1;
	uint32_t code = -1;
	switch (fourcc)
	{
	case V4L2_PIX_FMT_SGBRG10:
	case V4L2_PIX_FMT_SGBRG10P:
		code = V4L2_MBUS_FMT_SGBRG10_1X10;
	break;
	case V4L2_PIX_FMT_SBGGR10:
	case V4L2_PIX_FMT_SBGGR10P:
		code = V4L2_MBUS_FMT_SBGGR10_1X10;
	break;
	case V4L2_PIX_FMT_SRGGB12:
	case V4L2_PIX_FMT_SRGGB12P:
		code = V4L2_MBUS_FMT_SRGGB12_1X12;
	break;
	};
	dbg("smedia: format request %#x", code);
	for (int i = 0; ; i++)
	{
		struct v4l2_subdev_mbus_code_enum mbusEnum = {0};
		mbusEnum.pad = 0;
		mbusEnum.index = i;
		mbusEnum.which = V4L2_SUBDEV_FORMAT_ACTIVE;

		if (ioctl(ctrlfd, VIDIOC_SUBDEV_ENUM_MBUS_CODE, &mbusEnum) != 0)
		{
			dbg("smedia: %d supported formats", i);
			break;
		}
		dbg("smedia: format supported %#x", mbusEnum.code);
		if (mbusEnum.code == code)
		{
			warn("smedia: bus format found");
			ret = code;
		}
	}
	return ret;
}

static int sv4l2_subdev_setpixformat(int ctrlfd, CameraConfig_t *config)
{
	struct v4l2_subdev_format ffs = {0};
	ffs.pad = 0;
	ffs.which = V4L2_SUBDEV_FORMAT_ACTIVE;
	ffs.format.width = config->parent.width;
	ffs.format.height = config->parent.height;
	ffs.format.code = sv4l2_subdev_translate_fmtbus(ctrlfd, config->parent.fourcc);
	if (ioctl(ctrlfd, VIDIOC_SUBDEV_S_FMT, &ffs) != 0)
	{
		err("smedia: subdev set format error %m");
		close(ctrlfd);
		return -1;
	}
	return 0;
}

uint32_t _subdev_getpixformat(int ctrlfd, uint32_t (*busformat)(void *arg, struct v4l2_subdev_format *ffs), void *cbarg)
{
	struct v4l2_subdev_format ffs = {0};
	ffs.pad = 0;
	ffs.which = V4L2_SUBDEV_FORMAT_ACTIVE;
	if (ioctl(ctrlfd, VIDIOC_SUBDEV_G_FMT, &ffs) != 0)
	{
		err("smedia: subdev get format error %m");
		close(ctrlfd);
		return -1;
	}
	dbg("smedia: current subdev %lu x %lu %#X", ffs.format.width, ffs.format.height, ffs.format.code);
	if (busformat)
		return busformat(cbarg, &ffs);
	return 0;
}

static uint32_t _set_config(void *arg, struct v4l2_subdev_format *ffs)
{
	CameraConfig_t *config = arg;
	uint32_t fourcc = 0xFFFFFFFF;
	switch (ffs->format.code)
	{
	case V4L2_MBUS_FMT_SGBRG10_1X10:
		fourcc = V4L2_PIX_FMT_SGBRG10;
	break;
	case V4L2_MBUS_FMT_SBGGR10_1X10:
		fourcc = V4L2_PIX_FMT_SBGGR10;
	break;
	case V4L2_MBUS_FMT_SRGGB12_1X12:
		fourcc = V4L2_PIX_FMT_SRGGB12;
	break;
	};
	config->parent.fourcc = fourcc;
	config->parent.width = ffs->format.width;
	config->parent.height = ffs->format.height;
	return fourcc;
}

static uint32_t sv4l2_subdev_getpixformat(int ctrlfd, CameraConfig_t *config)
{
	if (config)
		return _subdev_getpixformat(ctrlfd, _set_config, config);
	return _subdev_getpixformat(ctrlfd, NULL, NULL);
}

static int sv4l2_subdev_open(CameraConfig_t *config)
{
	const char *subdevice = config->subdevice;
	if (subdevice == NULL)
	{
		err("sv4l2: error mediactl support without subdev");
		return -1;
	}
	int ctrlfd = open(subdevice, O_RDWR, 0);
	if (ctrlfd < 0)
	{
		err("smedia: subdevice %s not exist", subdevice);
		return -1;
	}
	struct v4l2_subdev_capability caps;
	if (ioctl(ctrlfd, VIDIOC_SUBDEV_QUERYCAP, &caps) != 0)
	{
		err("smedia: subdev control error %m");
		close(ctrlfd);
		return -1;
	}
#ifdef V4L2_SUBDEV_CAP_STREAMS
	if (caps.capabilities & V4L2_SUBDEV_CAP_STREAMS)
	{
		struct v4l2_subdev_client_capability clientCaps;
		clientCaps.capabilities = V4L2_SUBDEV_CLIENT_CAP_STREAMS;

		if (ioctl(ctrlfd, VIDIOC_SUBDEV_S_CLIENT_CAP, &clientCaps) != 0)
		{
			err("smedia: subdev control error %m");
			close(ctrlfd);
			return -1;
		}
		warn("smedia: client streams capabilities");
	}
#endif
	if (caps.capabilities & V4L2_SUBDEV_CAP_RO_SUBDEV)
	{
		warn("smedia: subdev read-only");
	}
	else
		sv4l2_subdev_setpixformat(ctrlfd, config);
	sv4l2_subdev_getpixformat(ctrlfd, config);
	return ctrlfd;
}

#else
static int sv4l2_subdev_open(CameraConfig_t *config)
{
	err("sv4l2: subdev is not supported");
	return -1;
}
#endif

DeviceConf_t * sv4l2_createconfig()
{
	CameraConfig_t *devconfig = NULL;
	devconfig = calloc(1, sizeof(CameraConfig_t));
	devconfig->device = sv4l2_defaultdevice;
	devconfig->parent.ops.loadconfiguration = sv4l2_loadjsonconfiguration;
	return (DeviceConf_t *)devconfig;
}

#ifdef HAVE_JANSSON

static int _sv4l2_loadjsonsetting(void *arg, struct v4l2_queryctrl *ctrl, V4L2_t *dev)
{
	json_t *jconfig = (json_t *)arg;
	json_t *jvalue = NULL;
	if (json_is_object(jconfig))
	{
		/**
		 * json format:
		 * {"Gain":1000,"Exposure":1}
		 */
		jvalue = json_object_get(jconfig, ctrl->name);
	}
	else if (json_is_array(jconfig))
	{
		/**
		 * json format:
		 * [ {"name":"Gain","value":1000},{"name":"Exposure","value":1}]
		 */
		int index = 0;
		json_t *jcontrol = NULL;
		json_array_foreach(jconfig, index, jcontrol)
		{
			if (json_is_object(jcontrol))
			{
				json_t *jname = json_object_get(jcontrol, "name");
				if (jname && json_is_string(jname) &&
					!strcmp(json_string_value(jname),ctrl->name))
				{
					jvalue = json_object_get(jcontrol, "value");
					break;
				}
			}
		}
	}
	if (jvalue == NULL)
		return 0;
	if (ctrl->type == V4L2_CTRL_TYPE_INTEGER && json_is_integer(jvalue))
	{
		int value = json_integer_value(jvalue);
		value = (int)sv4l2_control(dev, ctrl->id, (void*)value);
		if (value != -1)
			warn("%s => %d", ctrl->name, value);
		return value;
	}
	else if (ctrl->type == V4L2_CTRL_TYPE_BOOLEAN && json_is_boolean(jvalue))
	{
		int value = (int)sv4l2_control(dev, ctrl->id, (void*)json_is_true(jvalue));
		if (value != -1)
			warn("%s => %s", ctrl->name, value?"on":"off");
		return value;
	}
	else if (ctrl->type == V4L2_CTRL_TYPE_MENU && json_is_integer(jvalue))
	{
		int value = json_integer_value(jvalue);
		value = (int)sv4l2_control(dev, ctrl->id, (void*)value);
		if (value != -1)
			warn("%s => %d", ctrl->name, value);
		return value;
	}
	else if (ctrl->type == V4L2_CTRL_TYPE_BUTTON)
	{
		int value = (int)sv4l2_control(dev, ctrl->id, (void*)0);
		if (value != -1)
			warn("%s => done", ctrl->name);
		return value;
	}
	else if (ctrl->type == V4L2_CTRL_TYPE_MENU && json_is_string(jvalue))
	{
		const char *value = json_string_value(jvalue);
		struct v4l2_querymenu querymenu = {0};
		querymenu.id = ctrl->id;
		for (querymenu.index = 0; ioctl(dev->fd, VIDIOC_QUERYMENU, &querymenu) == 0; querymenu.index++)
		{
			if (!strcmp(querymenu.name, value))
			{
				if (sv4l2_control(dev, ctrl->id, (void*)querymenu.index) != (void *)-1)
					warn("%s => %s", ctrl->name, value);
				return (int)value;
			}
		}
	}
	else if (ctrl->type == V4L2_CTRL_TYPE_STRING && json_is_string(jvalue))
	{
		const char *value = json_string_value(jvalue);
		value = sv4l2_control(dev, ctrl->id, (void*)value);
		if (value != (void *)-1)
			warn("%s => %s", ctrl->name, value);
		return (int)value;
	}
	else
	{
		int value = json_integer_value(jvalue);
		value = (int)sv4l2_control(dev, ctrl->id, (void*)value);
		if (value != -1)
			warn("%s => %d", ctrl->name, value);
		return value;
	}
	return -1;
}
int sv4l2_loadjsonsettings(V4L2_t *dev, void *entry)
{
	json_t *jconfig = entry;

	json_t *crop = json_object_get(jconfig, "crop");
	if (crop && json_is_object(crop))
	{
		int disable = 0;
		json_t *top = json_object_get(jconfig, "top");
		json_t *left = json_object_get(jconfig, "left");
		json_t *width = json_object_get(jconfig, "width");
		json_t *height = json_object_get(jconfig, "height");
		struct v4l2_rect r = {0};
		if (top && json_is_integer(top))
			r.top = json_integer_value(top);
		if (left && json_is_integer(left))
			r.left = json_integer_value(left);
		if (width && json_is_integer(width))
			r.width = json_integer_value(width);
		else
			disable = 1;
		if (height && json_is_integer(height))
			r.height = json_integer_value(height);
		else
			disable = 1;
		if (disable)
			sv4l2_crop(dev, NULL);
		else
			sv4l2_crop(dev, &r);
	}
	if (crop && json_is_boolean(crop) && !json_is_true(crop))
		sv4l2_crop(dev, NULL);

	json_t *jcontrols = json_object_get(jconfig,"controls");
	if (jcontrols && (json_is_array(jcontrols) || json_is_object(jcontrols)))
		jconfig = jcontrols;
#if 1
	return sv4l2_treecontrols(dev, _sv4l2_loadjsonsetting, jconfig);
#else
	json_t *brightness = json_object_get(jconfig, "brightness");
	if (brightness && json_is_integer(brightness))
	{
		int value = json_integer_value(brightness);
		sv4l2_control(dev, V4L2_CID_BRIGHTNESS, value);
	}
	json_t *contrast = json_object_get(jconfig, "contrast");
	if (contrast && json_is_integer(contrast))
	{
		int value = json_integer_value(contrast);
		sv4l2_control(dev, V4L2_CID_CONTRAST, value);
	}
	json_t *color = json_object_get(jconfig, "color");
	if (color && json_is_object(color))
	{
		json_t *saturation = json_object_get(color, "saturation");
		if (saturation && json_is_integer(saturation))
		{
			int value = json_integer_value(saturation);
			sv4l2_control(dev, V4L2_CID_SATURATION, value);
		}
		json_t *gamma = json_object_get(color, "gamma");
		if (gamma && json_is_integer(gamma))
		{
			int value = json_integer_value(gamma);
			sv4l2_control(dev, V4L2_CID_GAMMA, value);
		}
		json_t *hue = json_object_get(color, "hue");
		if (hue && json_is_integer(hue))
		{
			int value = json_integer_value(hue);
			sv4l2_control(dev, V4L2_CID_HUE, value);
		}
		else if (hue && json_is_object(hue))
		{
			if (json_is_true(json_object_get(hue, "auto")))
				sv4l2_control(dev, V4L2_CID_HUE_AUTO, 1);
			else
			{
				sv4l2_control(dev, V4L2_CID_HUE_AUTO, 0);
				int value = json_integer_value(json_object_get(hue, "value"));
				sv4l2_control(dev, V4L2_CID_HUE, value);
			}
		}
		json_t *wb = json_object_get(color, "wb");
		if (wb && json_is_object(wb))
		{
			sv4l2_control(dev, V4L2_CID_DO_WHITE_BALANCE, 0);
			if (json_is_true(json_object_get(wb, "auto")))
				sv4l2_control(dev, V4L2_CID_AUTO_WHITE_BALANCE, 1);
			else
				sv4l2_control(dev, V4L2_CID_AUTO_WHITE_BALANCE, 0);
			if (json_is_integer(json_object_get(wb, "temperature")))
			{
				int value = json_integer_value(json_object_get(wb, "temperature"));
				sv4l2_control(dev, V4L2_CID_WHITE_BALANCE_TEMPERATURE, value);
			}
		}
		json_t *red = json_object_get(color, "red");
		if (red && json_is_integer(red))
		{
			int value = json_integer_value(red);
			sv4l2_control(dev, V4L2_CID_RED_BALANCE, value);
		}
		json_t *blue = json_object_get(color, "blue");
		if (blue && json_is_integer(blue))
		{
			int value = json_integer_value(blue);
			sv4l2_control(dev, V4L2_CID_BLUE_BALANCE, value);
		}
		json_t *effect = json_object_get(color, "effect");
		if (effect && json_is_string(effect))
		{
			const char *value = json_string_value(effect);
			if (!strcmp(value, "bw"))
				sv4l2_control(dev, V4L2_CID_COLORFX, V4L2_COLORFX_BW);
			else if (!strcmp(value, "sepia"))
				sv4l2_control(dev, V4L2_CID_COLORFX, V4L2_COLORFX_SEPIA);
			else if (!strcmp(value, "none"))
				sv4l2_control(dev, V4L2_CID_COLORFX, V4L2_COLORFX_NONE);
			else
			{
				err("effect %s unknown", value);
				sv4l2_control(dev, V4L2_CID_COLORFX, V4L2_COLORFX_NONE);
			}
		}
	}
	json_t *transformation = json_object_get(jconfig, "transformation");
	if (transformation && json_is_object(transformation))
	{
		if (json_is_true(json_object_get(transformation, "hflip")))
			sv4l2_control(dev, V4L2_CID_HFLIP, 1);
		if (json_is_true(json_object_get(transformation, "vflip")))
			sv4l2_control(dev, V4L2_CID_VFLIP, 1);
	}
	json_t *exposure = json_object_get(jconfig, "exposure");
	if (exposure && json_is_object(exposure))
	{
		if (json_is_true(json_object_get(exposure, "auto")))
			sv4l2_control(dev, V4L2_CID_AUTOGAIN, 1);
		else
		{
			sv4l2_control(dev, V4L2_CID_AUTOGAIN, 0);
			int value = json_integer_value(json_object_get(exposure, "value"));
			sv4l2_control(dev, V4L2_CID_EXPOSURE, value);
		}
	}
	json_t *gain = json_object_get(jconfig, "gain");
	if (gain && json_is_object(gain))
	{
		if (json_is_true(json_object_get(gain, "auto")))
			sv4l2_control(dev, V4L2_CID_AUTOGAIN, 1);
		else
		{
			sv4l2_control(dev, V4L2_CID_AUTOGAIN, 0);
			int value = json_integer_value(json_object_get(gain, "value"));
			sv4l2_control(dev, V4L2_CID_GAIN, value);
		}
	}
#endif
	return 0;
}

int sv4l2_loadjsonconfiguration(void *arg, void *entry)
{
	json_t *jconfig = entry;

	CameraConfig_t *config = (CameraConfig_t *)arg;
	json_t *device = json_object_get(jconfig, "device");
	if (device && json_is_string(device))
	{
		const char *value = json_string_value(device);
		config->device = value;
	}
	json_t *subdevice = json_object_get(jconfig, "subdevice");
	if (subdevice && json_is_string(subdevice))
	{
		const char *value = json_string_value(subdevice);
		config->subdevice = value;
	}
	json_t *fps = NULL;
	json_t *mode = NULL;
	json_t *definition = json_object_get(jconfig, "definition");
	if (definition && json_is_array(definition))
	{
		json_t *field = NULL;
		int index = 0;
		json_array_foreach(definition, index, field)
		{
			if (json_is_object(field))
			{
				json_t *name = json_object_get(field, "name");
				if (name && json_is_string(name) &&
					!strcmp(json_string_value(name), "fps"))
				{
					fps = json_object_get(field, "value");
				}
				if (name && json_is_string(name) &&
					!strcmp(json_string_value(name), "mode"))
				{
					mode = json_object_get(field, "value");
				}
			}
		}
	}
	else if (definition && json_is_object(definition))
	{
		fps = json_object_get(definition, "fps");
		mode = json_object_get(definition, "mode");
	}
	else
	{
		fps = json_object_get(jconfig, "fps");
		mode = json_object_get(jconfig, "mode");
	}
	if (fps && json_is_integer(fps))
	{
		int value = json_integer_value(fps);
		config->fps = value;
	}
	if (mode && json_is_string(mode))
	{
		const char *value = json_string_value(mode);
		if (strstr(value,"capture"))
			config->mode |= MODE_CAPTURE;
		if (strstr(value,"output"))
			config->mode |= MODE_OUTPUT;
	}
	json_t *interactive = json_object_get(jconfig, "interactive");
	if (interactive && json_is_boolean(interactive) && json_is_true(interactive))
	{
		config->mode |= MODE_INTERACTIVE;
	}
	json_t *library = json_object_get(jconfig, "library");
	if (library && json_is_string(library))
	{
		const char *value = json_string_value(library);
		void *handle;
		handle = dlopen(value, RTLD_LAZY);
		if (!handle)
		{
			err("library %s opening error %s", value, dlerror());
			goto library_end;
		}
		config->transfer = dlsym(handle, "transfer");
		if (config->transfer == NULL)
		{
			err("library %s symbol \"transfer\" error %s", value, dlerror());
			goto library_end;
		}
		
	}
library_end:
	return 0;
}

static const char *CTRLTYPE(enum v4l2_ctrl_type type)
{
	switch (type)
	{
	case V4L2_CTRL_TYPE_INTEGER:
		return "integer";
	case V4L2_CTRL_TYPE_BOOLEAN:
		return "boolean";
	case V4L2_CTRL_TYPE_MENU:
		return "menu";
	case V4L2_CTRL_TYPE_BUTTON:
		return "button";
	case V4L2_CTRL_TYPE_INTEGER64:
		return "large integer";
	case V4L2_CTRL_TYPE_STRING:
		return "string";
	case V4L2_CTRL_TYPE_CTRL_CLASS:
		return "control class";
	}
	dbg("type %d", type);
	return "unknown";
}

static const char *CTRLNAME(uint32_t id)
{
	switch (id)
	{
	case V4L2_CID_BRIGHTNESS:
		return "V4L2_CID_BRIGHTNESS";
	case V4L2_CID_CONTRAST:
		return "V4L2_CID_CONTRAST";
	case V4L2_CID_SATURATION:
		return "V4L2_CID_SATURATION";
	case V4L2_CID_HUE:
		return "V4L2_CID_HUE";
	case V4L2_CID_AUDIO_VOLUME:
		return "V4L2_CID_AUDIO_VOLUME";
	case V4L2_CID_AUDIO_BALANCE:
		return "V4L2_CID_AUDIO_BALANCE";
	case V4L2_CID_BLACK_LEVEL:
		return "V4L2_CID_BLACK_LEVEL";
	case V4L2_CID_AUTO_WHITE_BALANCE:
		return "V4L2_CID_AUTO_WHITE_BALANCE";
	case V4L2_CID_DO_WHITE_BALANCE:
		return "V4L2_CID_DO_WHITE_BALANCE";
	case V4L2_CID_RED_BALANCE:
		return "V4L2_CID_RED_BALANCE";
	case V4L2_CID_BLUE_BALANCE:
		return "V4L2_CID_BLUE_BALANCE";
	case V4L2_CID_GAMMA:
		return "V4L2_CID_GAMMA";
	case V4L2_CID_EXPOSURE:
		return "V4L2_CID_EXPOSURE";
	case V4L2_CID_AUTOGAIN:
		return "V4L2_CID_AUTOGAIN";
	case V4L2_CID_GAIN:
		return "V4L2_CID_GAIN";
	case V4L2_CID_HFLIP:
		return "V4L2_CID_HFLIP";
	case V4L2_CID_VFLIP:
		return "V4L2_CID_VFLIP";
	case V4L2_CID_HUE_AUTO:
		return "V4L2_CID_HUE_AUTO";
	case V4L2_CID_WHITE_BALANCE_TEMPERATURE:
		return "V4L2_CID_WHITE_BALANCE_TEMPERATURE";
	case V4L2_CID_SHARPNESS:
		return "V4L2_CID_SHARPNESS";
	case V4L2_CID_BACKLIGHT_COMPENSATION:
		return "V4L2_CID_BACKLIGHT_COMPENSATION";
	case V4L2_CID_CHROMA_AGC:
		return "V4L2_CID_CHROMA_AGC";
	case V4L2_CID_COLOR_KILLER:
		return "V4L2_CID_COLOR_KILLER";
	case V4L2_CID_COLORFX:
		return "V4L2_CID_COLORFX";
	case V4L2_CID_LASTP1:
		return "V4L2_CID_LASTP1";
	case V4L2_CID_EXPOSURE_AUTO:
		return "V4L2_CID_EXPOSURE_AUTO";
	case V4L2_CID_EXPOSURE_ABSOLUTE:
		return "V4L2_CID_EXPOSURE_ABSOLUTE";
	case V4L2_CID_EXPOSURE_AUTO_PRIORITY:
		return "V4L2_CID_EXPOSURE_AUTO_PRIORITY";
	case V4L2_CID_PAN_RELATIVE:
		return "V4L2_CID_PAN_RELATIVE";
	case V4L2_CID_TILT_RELATIVE:
		return "V4L2_CID_TILT_RELATIVE";
	case V4L2_CID_PAN_RESET:
		return "V4L2_CID_PAN_RESET";
	case V4L2_CID_TILT_RESET:
		return "V4L2_CID_TILT_RESET";
	case V4L2_CID_PAN_ABSOLUTE:
		return "V4L2_CID_PAN_ABSOLUTE";
	case V4L2_CID_TILT_ABSOLUTE:
		return "V4L2_CID_TILT_ABSOLUTE";
	case V4L2_CID_FOCUS_ABSOLUTE:
		return "V4L2_CID_FOCUS_ABSOLUTE";
	case V4L2_CID_FOCUS_RELATIVE:
		return "V4L2_CID_FOCUS_RELATIVE";
	case V4L2_CID_FOCUS_AUTO:
		return "V4L2_CID_FOCUS_AUTO";
	case V4L2_CID_ZOOM_ABSOLUTE:
		return "V4L2_CID_ZOOM_ABSOLUTE";
	case V4L2_CID_ZOOM_RELATIVE:
		return "V4L2_CID_ZOOM_RELATIVE";
	case V4L2_CID_ZOOM_CONTINUOUS:
		return "V4L2_CID_ZOOM_CONTINUOUS";
	case V4L2_CID_PRIVACY:
		return "V4L2_CID_PRIVACY";
	case V4L2_CID_BAND_STOP_FILTER:
		return "V4L2_CID_BAND_STOP_FILTER";
	}
	if (id && V4L2_CID_CAMERA_CLASS)
		return "CAMERA_CLASS";
	return "V4L2_CID???";
}

static int menuprint(void *arg, struct v4l2_querymenu *querymenu, V4L2_t *dev)
{
	json_t *control = (json_t *)arg;
	json_t *items = json_object_get(control, "items");
	json_t *value = json_object_get(control, "value");
	if (querymenu->index == json_integer_value(value))
		json_object_set(control, "value_str", json_string(querymenu->name));
	json_array_append_new(items, json_string(querymenu->name));
	return 0;
}

int sv4l2_jsoncontrol_cb(void *arg, struct v4l2_queryctrl *ctrl, V4L2_t *dev)
{
	json_t *controls = (json_t *)arg;
	if (!json_is_object(controls) && !json_is_array(controls))
		return -1;
	json_t *ctrlclass = NULL;
	if (json_is_array(controls))
	{
		int lastindex = json_array_size(controls);
		if (lastindex > 0)
		{
			json_t *control = json_array_get(controls, lastindex - 1);
			if (json_is_object(control))
			{
				json_t *classtype = json_object_get(control, "type");
				if (classtype && json_is_string(classtype) &&
					!strcmp(json_string_value(classtype), CTRLTYPE(V4L2_CTRL_TYPE_CTRL_CLASS)))
				{
					ctrlclass = json_object_get(control, "items");
				}
			}
		}
	}
	void *value = sv4l2_control(dev, ctrl->id, (void*)-1);
	json_t *control = json_object();
	json_object_set_new(control, "name", json_string(ctrl->name));
	json_object_set_new(control, "id", json_integer(ctrl->id));
	json_t *type = json_string(CTRLTYPE(ctrl->type));
	json_object_set_new(control, "type", type);

	switch (ctrl->type)
	{
	case V4L2_CTRL_TYPE_INTEGER:
	{
		json_object_set(control, "value", json_integer((int) value));
		json_object_set(control, "minimum", json_integer(ctrl->minimum));
		json_object_set(control, "maximum", json_integer(ctrl->maximum));
		json_object_set(control, "step", json_integer(ctrl->step));
		json_object_set(control, "default_value", json_integer(ctrl->default_value));
	}
	break;
	case V4L2_CTRL_TYPE_BOOLEAN:
		json_object_set(control, "value", json_boolean((int) value));
		json_object_set(control, "default_value", json_integer(ctrl->default_value));
	break;
	case V4L2_CTRL_TYPE_MENU:
	{
		json_object_set(control, "value", json_integer((int) value));
		json_t *items = json_array();
		json_object_set(control, "items", items);
		sv4l2_treecontrolmenu(dev, ctrl->id, menuprint, control);
		json_object_set(control, "default_value", json_integer(ctrl->default_value));
	}
	break;
	case V4L2_CTRL_TYPE_BUTTON:
	break;
	case V4L2_CTRL_TYPE_INTEGER64:
		json_object_set(control, "value", json_integer((int) value));
		json_object_set(control, "default_value", json_integer(ctrl->default_value));
	break;
	case V4L2_CTRL_TYPE_STRING:
		json_object_set(control, "value", json_string(value));
	break;
	case V4L2_CTRL_TYPE_CTRL_CLASS:
	{
		json_t *clas_ = json_array();
		json_object_set(control, "items", clas_);
		if (json_is_object(controls))
			json_object_set(controls, ctrl->name, control);
		else if (json_is_array(controls))
			json_array_append(controls, control);
		return 0;
	}
	break;
	}

	if (ctrlclass && json_is_array(ctrlclass))
		json_array_append(ctrlclass, control);
	else if (json_is_array(controls))
		json_array_append(controls, control);
	return 0;
}

static int _v4l2_capabilities_crop(V4L2_t *dev, json_t *transformation)
{
	struct v4l2_selection sel = {0};
	sel.type = sv4l2_type(dev);
	sel.target = V4L2_SEL_TGT_CROP_BOUNDS;
	if (ioctl(sv4l2_fd(dev), VIDIOC_G_SELECTION, &sel) == 0)
	{
		json_t *crop = json_object();
		json_object_set_new(crop, "name", json_string("crop"));
		json_object_set_new(crop, "type", json_string("rectangle"));
		json_t *rect = json_object();
		json_t *top = json_object();
		json_object_set_new(top, "minimum", json_integer(0));
		json_object_set_new(top, "maximum", json_integer(sel.r.height - 32));
		json_object_set_new(rect, "top", top);
		json_t *left = json_object();
		json_object_set_new(left, "minimum", json_integer(0));
		json_object_set_new(left, "maximum", json_integer(sel.r.width - 32));
		json_object_set_new(rect, "left", left);
		json_t *width = json_object();
		json_object_set_new(width, "minimum", json_integer(32));
		json_object_set_new(width, "maximum", json_integer(sel.r.width));
		json_object_set_new(rect, "width", width);
		json_t *height = json_object();
		json_object_set_new(height, "minimum", json_integer(32));
		json_object_set_new(height, "maximum", json_integer(sel.r.height));
		json_object_set_new(rect, "height", height);
		json_object_set(crop, "bounds", rect);
		json_array_append(transformation, crop);
	}
	return 0;
}

static int _v4l2_capabilities_fps(V4L2_t *dev, json_t *definition)
{
	json_t *fps = json_object();
	json_object_set_new(fps, "name", json_string("fps"));
	json_object_set_new(fps, "type", json_string(CTRLTYPE(V4L2_CTRL_TYPE_INTEGER)));
	struct v4l2_streamparm streamparm = {0};
	streamparm.type = sv4l2_type(dev);
	if (ioctl(sv4l2_fd(dev), VIDIOC_G_PARM, &streamparm) == 0)
	{
		if (streamparm.parm.capture.timeperframe.denominator > streamparm.parm.capture.timeperframe.numerator)
			json_object_set_new(fps, "value", json_integer((int)(streamparm.parm.capture.timeperframe.denominator / streamparm.parm.capture.timeperframe.numerator)));
		else if (streamparm.parm.capture.timeperframe.denominator > 0)
			json_object_set_new(fps, "value", json_integer( -1 * ((int) (streamparm.parm.capture.timeperframe.numerator / streamparm.parm.capture.timeperframe.denominator))));
	}
	json_array_append(definition, fps);
	return 0;
}

static int _v4l2_capabilities_imageformat(V4L2_t *dev, json_t *definition)
{
	json_t *pixelformat = json_object();
	json_object_set_new(pixelformat, "name", json_string("fourcc"));
	json_object_set_new(pixelformat, "type", json_string(CTRLTYPE(V4L2_CTRL_TYPE_STRING)));

	json_t *items = json_array();
	struct v4l2_fmtdesc fmtdesc = {0};
	fmtdesc.type = sv4l2_type(dev);
	while (ioctl(sv4l2_fd(dev), VIDIOC_ENUM_FMT, &fmtdesc) == 0)
	{
		json_array_append_new(items, json_stringn((char*)&fmtdesc.pixelformat, 4));
		fmtdesc.index++;
	}

	json_t *width = json_object();
	json_object_set_new(width, "name", json_string("width"));
	json_object_set_new(width, "type", json_string(CTRLTYPE(V4L2_CTRL_TYPE_INTEGER)));

	json_t *height = json_object();
	json_object_set_new(height, "name", json_string("height"));
	json_object_set_new(height, "type", json_string(CTRLTYPE(V4L2_CTRL_TYPE_INTEGER)));

	struct v4l2_format fmt;
	fmt.type = sv4l2_type(dev);
	fmt.fmt.pix.field = V4L2_FIELD_ANY;
	if (ioctl(sv4l2_fd(dev), VIDIOC_G_FMT, &fmt) == 0)
	{
		json_object_set_new(pixelformat, "value", json_stringn((char*)&fmt.fmt.pix.pixelformat, 4));
		json_object_set_new(width, "value", json_integer(fmt.fmt.pix.width));
		json_object_set_new(height, "value", json_integer(fmt.fmt.pix.width));
	}

	struct v4l2_frmsizeenum video_cap = {0};
	video_cap.pixel_format = fmt.fmt.pix.pixelformat;
	video_cap.type = V4L2_FRMSIZE_TYPE_STEPWISE;
	if(ioctl(sv4l2_fd(dev), VIDIOC_ENUM_FRAMESIZES, &video_cap) == 0)
	{
		json_object_set(width, "minimum", json_integer(video_cap.stepwise.min_width));
		json_object_set(width, "maximum", json_integer(video_cap.stepwise.max_width));
		json_object_set(width, "step", json_integer(video_cap.stepwise.step_width));

		json_object_set(height, "minimum", json_integer(video_cap.stepwise.min_height));
		json_object_set(height, "maximum", json_integer(video_cap.stepwise.max_height));
		json_object_set(height, "step", json_integer(video_cap.stepwise.step_height));
	}

	json_object_set(pixelformat, "items", items);

	json_array_append(definition, pixelformat);
	json_array_append(definition, width);
	json_array_append(definition, height);
	return 0;
}

int sv4l2_capabilities(V4L2_t *dev, json_t *capabilities)
{
	json_t *definition = json_array();
	_v4l2_capabilities_imageformat(dev, definition);
	_v4l2_capabilities_fps(dev, definition);
	json_object_set(capabilities, "definition", definition);
	json_t *transformation = json_array();
	_v4l2_capabilities_crop(dev, transformation);
	json_object_set(capabilities, "transformation", transformation);
	json_t *controls = json_array();
	sv4l2_treecontrols(dev, sv4l2_jsoncontrol_cb, controls);
	json_object_set(capabilities, "controls", controls);
	return 0;
}
#endif
