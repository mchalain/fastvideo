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
	uint32_t width;
	uint32_t height;
	uint32_t fourcc;
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
	/**
	 * if the device is a M2M device the mode contains OUTPUT and CAPTURE
	 * but to duplicate the device, the first one should be by default OUTPUT
	 * and the second should be a CAPTURE.
	 * The use of type = 0 or -1 helps to set the default value.
	 */
	if (type == 0 && (mode & MODE_OUTPUT))
	{
		if (mode & MODE_META)
			type = V4L2_BUF_TYPE_META_OUTPUT;
		else
			type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	}
	else if ((type == 0 || type == -1) && (mode & MODE_CAPTURE))
	{
		if (mode & MODE_META)
			type = V4L2_BUF_TYPE_META_CAPTURE;
		else
			type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	}
	else if (type == -1 && (mode & MODE_OUTPUT))
	{
		if (mode & MODE_META)
			type = V4L2_BUF_TYPE_META_OUTPUT;
		else
			type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
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

static int _v4l2_devicecapabilities(int fd, const char *interface, int *mode)
{
	struct v4l2_capability cap;
	if (ioctl(fd, VIDIOC_QUERYCAP, &cap) != 0)
	{
		err("device %s not video %m", interface);
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
		return -1;
	}
	dbg("%s streaming available (%#x)", interface, *mode);
	return 0;
}

uint32_t sv4l2_getpixformat(V4L2_t *dev, int (*pixformat)(void *arg, struct v4l2_fmtdesc *fmtdesc, int isset), void *cbarg)
{
	uint32_t pixelformat = 0;

	struct v4l2_format fmt;
	fmt.type = dev->type;
	if (ioctl(dev->fd, VIDIOC_G_FMT, &fmt) != 0)
	{
		err("FMT not found %m");
		return -1;
	}
	if (dev->type == V4L2_BUF_TYPE_META_CAPTURE || dev->type == V4L2_BUF_TYPE_META_OUTPUT)
		pixelformat = fmt.fmt.meta.dataformat;
	else
		pixelformat = fmt.fmt.pix.pixelformat;

	struct v4l2_fmtdesc fmtdesc = {0};
	fmtdesc.type = dev->type;
	dbg("Formats:");
	while (ioctl(dev->fd, VIDIOC_ENUM_FMT, &fmtdesc) == 0)
	{
		dbg("\t%.4s => %s", (char*)&fmtdesc.pixelformat,
				fmtdesc.description);
		fmtdesc.index++;
		if (pixformat)
			pixformat(cbarg, &fmtdesc, (fmtdesc.pixelformat == pixelformat));
	}
	return pixelformat;
}

static uint32_t _v4l2_setpixformat(int fd, enum v4l2_buf_type type, uint32_t fourcc)
{
	uint32_t pixelformat = 0;

	struct v4l2_format fmt = {0};
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
		if (fourcc != 0)
		{
			if (fmtdesc.pixelformat != fourcc)
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
	return fmt.fmt.pix.pixelformat;
}

static uint32_t _v4l2_checkframesize(int fd, uint32_t fourcc, uint32_t *width, uint32_t *height)
{
	struct v4l2_frmsizeenum video_cap = {0};
	video_cap.pixel_format = fourcc;
	video_cap.index = 0;
	if (ioctl(fd, VIDIOC_ENUM_FRAMESIZES, &video_cap) != 0)
	{
		err("framsesize enumeration error %m");
		return -1;
	}

	dbg("Frame size:");
	if (video_cap.type == V4L2_FRMSIZE_TYPE_STEPWISE)
	{
		dbg("\t%d < width  < %d, step %d", video_cap.stepwise.min_width, video_cap.stepwise.max_width, video_cap.stepwise.step_width);
		dbg("\t%d < height < %d, step %d", video_cap.stepwise.min_height, video_cap.stepwise.max_height, video_cap.stepwise.step_height);
		if (*width > video_cap.stepwise.max_width)
			*width = video_cap.stepwise.max_width;
		if (*width < video_cap.stepwise.min_width)
			*width = video_cap.stepwise.min_width;

		if (*height > video_cap.stepwise.max_height)
			*height = video_cap.stepwise.max_height;
		if (*height < video_cap.stepwise.min_height)
			*height = video_cap.stepwise.min_height;
	}
	else if (video_cap.type == V4L2_FRMSIZE_TYPE_DISCRETE)
	{
		uint32_t nbpixels = *height * *width;
		for (video_cap.index = 0; ioctl(fd, VIDIOC_ENUM_FRAMESIZES, &video_cap) != -1; video_cap.index++)
		{
			dbg("\twidth %d, height %d", video_cap.discrete.width, video_cap.discrete.height);
			if (*height <= video_cap.discrete.height)
			{
				*height = video_cap.discrete.height;
				*width = video_cap.discrete.width;
				if (nbpixels > 0 && nbpixels == video_cap.discrete.height * video_cap.discrete.width)
					break;
			}
		}
	}
	return 0;
}

int sv4l2_getframesize(V4L2_t *dev, int(*framesize)(void *arg, uint32_t width, uint32_t height, int isset), void *cbarg)
{
	struct v4l2_format fmt = {0};
	fmt.type = dev->type;
	fmt.fmt.pix.field = V4L2_FIELD_ANY;
	if (ioctl(dev->fd, VIDIOC_G_FMT, &fmt) != 0)
	{
		return -1;
	}
	struct v4l2_frmsizeenum video_cap = {0};
	video_cap.pixel_format = fmt.fmt.pix.pixelformat;
	video_cap.index = 0;
	if (ioctl(dev->fd, VIDIOC_ENUM_FRAMESIZES, &video_cap) != 0)
	{
		err("framsesize enumeration error %m");
		return -1;
	}
	if (video_cap.type == V4L2_FRMSIZE_TYPE_STEPWISE)
	{
		dbg("\t%d < width  < %d, step %d", video_cap.stepwise.min_width, video_cap.stepwise.max_width, video_cap.stepwise.step_width);
		dbg("\t%d < height < %d, step %d", video_cap.stepwise.min_height, video_cap.stepwise.max_height, video_cap.stepwise.step_height);
		if (framesize)
		{
			framesize(cbarg, video_cap.stepwise.min_width, video_cap.stepwise.min_height, 0);
			framesize(cbarg, fmt.fmt.pix.width, fmt.fmt.pix.height, 1);
			framesize(cbarg, video_cap.stepwise.max_width, video_cap.stepwise.max_height, 0);
		}
	}
	else if (video_cap.type == V4L2_FRMSIZE_TYPE_DISCRETE)
	{
		for (video_cap.index = 0; ioctl(dev->fd, VIDIOC_ENUM_FRAMESIZES, &video_cap) != -1; video_cap.index++)
		{
			dbg("\twidth %d, height %d", video_cap.discrete.width, video_cap.discrete.height);
			if (framesize)
			{
				int isset = (video_cap.discrete.width == fmt.fmt.pix.width);
				isset = isset && (video_cap.discrete.height == fmt.fmt.pix.height);
				framesize(cbarg, video_cap.discrete.width, video_cap.discrete.height, isset);
			}
		}
	}
	return 0;
}

static uint32_t _v4l2_setframesize(int fd, enum v4l2_buf_type type, uint32_t width, uint32_t height)
{
	uint32_t framesize = 0;

	struct v4l2_format fmt = {0};
	fmt.type = type;
	fmt.fmt.pix.field = V4L2_FIELD_ANY;
	if (ioctl(fd, VIDIOC_G_FMT, &fmt) != 0)
	{
		return -1;
	}

	if (height > 0 && width == 0)
	{
		width = height * 16 / 9;
	}
	if (width > 0 && height > 0
		&& !_v4l2_checkframesize(fd, fmt.fmt.pix.pixelformat, &width, &height))
	{
		fmt.fmt.pix.width = width;
		fmt.fmt.pix.height = height;
	}
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
	else if (fps >= 0 && fps != streamparm.parm.capture.timeperframe.denominator)
	{
		streamparm.parm.capture.timeperframe.denominator = fps;
		streamparm.parm.capture.timeperframe.numerator = 1;
		if (ioctl(fd, VIDIOC_S_PARM, &streamparm) == -1)
		{
			err("FPS setting error %m");
			return -1;
		}
	}
	else if (fps < 0 && -fps != streamparm.parm.capture.timeperframe.numerator)
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
		{
			ret = sv4l2_requestbuffer_mmap(dev);
			if (ret)
				break;
			int *ntargets = va_arg(ap, int *);
			void ***targets = va_arg(ap, void ***);
			size_t *size = va_arg(ap, size_t *);
			if (ntargets != NULL)
				*ntargets = dev->nbuffers;
			if (targets != NULL)
			{
				*targets = calloc(dev->nbuffers, sizeof(void *));
				for (int i = 0; i < dev->nbuffers; i++)
					(*targets)[i] = dev->buffers[i].map;
			}
			if (size != NULL)
				*size = dev->buffers[0].ops.getsize(&dev->buffers[0]);
		}
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
		case buf_type_dmabuf | buf_type_master:
		{
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
		err("sv4l2: cropping error %m");
		return -1;
	}
	dbg("sv4l2: croping requested (%d %d %d %d)", sel.r.left, sel.r.top, sel.r.width, sel.r.height);
	return 0;
}

static void * _sv4l2_control(int ctrlfd, int id, void *value, struct v4l2_queryctrl *queryctrl)
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
			err("sv4l2: control %#x setting error %m", id);
			return (void *)-1;
		}
		control.value = 0;
		if (queryctrl->type != V4L2_CTRL_TYPE_CTRL_CLASS &&
			ioctl(ctrlfd, VIDIOC_G_CTRL, &control))
		{
			err("sv4l2: device doesn't support control %#x %m", id);
			return (void *)-1;
		}
		value = (void *)control.value;
		dbg("sv4l2: control %#x => %d", id, control.value);
	}
	else
#endif
	if (queryctrl->type != V4L2_CTRL_TYPE_CTRL_CLASS)
	{
		struct v4l2_ext_control control = {0};
		char string[256] = {0};
		control.id = id;
		if (queryctrl->type == V4L2_CTRL_TYPE_INTEGER)
			control.size = sizeof(uint32_t);
		else if (queryctrl->type == V4L2_CTRL_TYPE_BOOLEAN)
			control.size = 1;
		else if (queryctrl->type == V4L2_CTRL_TYPE_MENU)
			control.size = sizeof(uint32_t);
		else if (queryctrl->type == V4L2_CTRL_TYPE_BUTTON)
			control.size = 0;
		else if (queryctrl->type == V4L2_CTRL_TYPE_INTEGER64)
			control.size = sizeof(uint64_t);
		else if (queryctrl->type == V4L2_CTRL_TYPE_STRING && value != NULL)
		{
			if (value != (void*)-1)
				control.size = strlen(value) + 1;
			else
				control.size = sizeof(string);
			control.string = string;
		}
		else
			control.size = 0;
		struct v4l2_ext_controls controls = {0};
		controls.count = 1;
		controls.controls = &control;
		if (value != (void*)-1)
		{
			control.string = value;
			if (ioctl(ctrlfd, VIDIOC_S_EXT_CTRLS, &controls))
			{
				err("sv4l2: control %#x setting error %m", id);
				return (void *)-1;
			}
		}
		/**
		 * DEBUG with valgrind.
		 * valgrind returns problem on the next ioctl
		 * if ptr is not set on a good address even with an integer control.
		 * Syscall param ioctl(VKI_V4L2_G_EXT_CTRLS).controls[].ptr[] points to unaddressable byte(s)
		 */
		control.ptr = string;
		if ((queryctrl->type != V4L2_CTRL_TYPE_BUTTON) &&
			(queryctrl->type != V4L2_CTRL_TYPE_CTRL_CLASS) &&
			ioctl(ctrlfd, VIDIOC_G_EXT_CTRLS, &controls))
		{
			err("sv4l2: control %#x getting error %m", id);
			return (void *)-1;
		}
		value = control.string;
		dbg("sv4l2: control %#x => %d", id, control.value);
	}
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
			err("sv4l2: control %#x not supported", id);
			return (void *)-1;
		}
	}
	if (queryctrl.flags & V4L2_CTRL_FLAG_DISABLED)
	{
		err("sv4l2: control %#x disabled", id);
		return 0;
	}
	/**
	 * TODO extend the controls
	 */
	if (queryctrl.flags & V4L2_CTRL_FLAG_HAS_PAYLOAD)
	{
		err("sv4l2: control %#x with payload unsupported", id);
		return 0;
	}
	return _sv4l2_control(ctrlfd, id, value, &queryctrl);
}

static int _sv4l2_treecontrols(int ctrlfd, int (*cb)(void *arg, struct v4l2_queryctrl *ctrl), void * arg)
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
			cb(arg, &qctrl);
		qctrl.id |= V4L2_CTRL_FLAG_NEXT_CTRL;
	}
	if (ret && errno != EINVAL)
	{
		nbctrls = ret;
	}
	return nbctrls;
}

int sv4l2_treecontrols(V4L2_t *dev, int (*cb)(void *arg, struct v4l2_queryctrl *ctrl), void * arg)
{
	int nbctrls = 0;
	int ret;
	ret = _sv4l2_treecontrols(dev->ctrlfd, cb, arg);
	nbctrls += ret;
	if (dev->fd != dev->ctrlfd)
	{
		ret = _sv4l2_treecontrols(dev->fd, cb, arg);
		nbctrls += ret;
	}
	if (ret < 0)
		err("sv4l2: %s query controls error %m", dev->name);
	return nbctrls;
}

int _sv4l2_treecontrolmenu(int ctrlfd, struct v4l2_queryctrl *ctrl, int (*cb)(void *arg, struct v4l2_querymenu *ctrl), void * arg)
{
	struct v4l2_querymenu querymenu = {0};
	querymenu.id = ctrl->id;
	for (querymenu.index = ctrl->minimum; querymenu.index <= ctrl->maximum; querymenu.index++ )
	{
		if (ioctl(ctrlfd, VIDIOC_QUERYMENU, &querymenu) != 0)
		{
			err("sv4l2: query menu error %m");
			return -1;
		}
		if (cb)
			cb(arg, &querymenu);
	}
	return ctrl->maximum - ctrl->minimum;
}

int sv4l2_treecontrolmenu(V4L2_t *dev, struct v4l2_queryctrl *ctrl, int (*cb)(void *arg, struct v4l2_querymenu *ctrl), void * arg)
{
	return _sv4l2_treecontrolmenu(sv4l2_fd(dev), ctrl, cb, arg);
}

static int _sv4l2_prepare(int fd, enum v4l2_buf_type *type, int mode, CameraConfig_t *config)
{
	*type = _v4l2_getbuftype(*type, mode);

	uint32_t fourcc = 0;
	if (config)
		fourcc = config->parent.fourcc;
	if (_v4l2_setpixformat(fd, *type, fourcc) == -1)
	{
		err("pixel format error %m");
		return -1;
	}

	uint32_t width = 0;
	uint32_t height = 0;
	if (config)
	{
		width = config->parent.width;
		height = config->parent.height;
		dbg("sv4l2: device requesting %dx%d %.4s", width, height, &fourcc);
	}
	if (!(mode & MODE_META) &&
		_v4l2_setframesize(fd, *type, width, height) == -1)
	{
		err("frame size error %m");
		return -1;
	}

	int fps = -1;
	if (config)
		fps = config->fps;
	_v4l2_setfps(fd, *type, fps);
	return 0;
}

V4L2_t *sv4l2_create2(int fd, const char *devicename, CameraConfig_t *config)
{
	enum v4l2_buf_type type = 0;
	int mode = 0;
	if (config && config->mode)
		mode = config->mode;
	if (_v4l2_devicecapabilities(fd, devicename, &mode))
	{
		return NULL;
	}
	int ctrlfd = fd;

	if (_sv4l2_prepare(fd, &type, mode, config))
	{
		return NULL;
	}

	struct v4l2_format fmt = {0};
	fmt.type = type;
	fmt.fmt.pix.field = V4L2_FIELD_ANY;
	if (ioctl(fd, VIDIOC_G_FMT, &fmt) != 0)
	{
		err("FMT not found %m");
		return NULL;
	}
	uint32_t width = 0;
	uint32_t height = 0;
	uint32_t fourcc = 0;
	uint32_t bytesperline = 0;
	uint32_t sizeimage = 0;
	if (mode & MODE_MPLANE)
	{
		width = fmt.fmt.pix_mp.width;
		height = fmt.fmt.pix_mp.height;
		fourcc = fmt.fmt.pix_mp.pixelformat;
		bytesperline = fmt.fmt.pix_mp.plane_fmt[0].bytesperline;
		sizeimage = fmt.fmt.pix_mp.plane_fmt[0].sizeimage;
	}
	else
	{
		width = fmt.fmt.pix.width;
		height = fmt.fmt.pix.height;
		fourcc = fmt.fmt.pix.pixelformat;
		bytesperline = fmt.fmt.pix.bytesperline;
		sizeimage = fmt.fmt.pix.sizeimage;
	}

	if (!bytesperline && sizeimage)
		bytesperline = sizeimage / config->parent.height;

	V4L2_t *dev = calloc(1, sizeof(*dev));
	dev->name = devicename;
	dev->config = config;
	dev->fd = fd;
	dev->ctrlfd = ctrlfd;
	dev->type = type;
	dev->mode = mode;
	dev->width = width;
	dev->height = height;
	dev->fourcc = fourcc;

	if (mode & MODE_MEDIACTL && config)
	{
		int fd = sv4l2_subdev_open(&config->subdevices[0]);
		if (fd > 0)
		{
			ctrlfd = fd;
			sv4l2_subdev_setpixformat(ctrlfd, dev->fourcc, dev->width, dev->height);
#if 0
			sv4l2_subdev_getpixformat(ctrlfd, _v4l2_subdev_set_config, dev->config);
#endif
		}
	}

	if (mode & MODE_VERBOSE)
		warn("sv4l2: create %s", devicename);
	dev->ops.createbuffers = createbuffers_splane;
	dev->nplanes = 1;
	if (mode & MODE_MPLANE)
	{
		dev->ops.createbuffers = createbuffers_mplane;
		dev->nplanes = fmt.fmt.pix_mp.num_planes;
	}
	dbg("V4l2 settings: %dx%d, %.4s", dev->width, dev->height, (char*)&dev->fourcc);
	if (config)
		config->parent.dev = dev;
	return dev;
}

V4L2_t *sv4l2_create(const char *devicename, CameraConfig_t *config)
{
	const char *device = devicename;
	if (config && config->device)
		device = config->device;
	int fd = open(device, O_RDWR | O_NONBLOCK, 0);
	if (fd < 0)
	{
		err("sv4l2: open %s failed %m", device);
		return NULL;
	}

	V4L2_t *dev = sv4l2_create2(fd, devicename, type, config);
	if (dev == NULL)
		close(fd);
	return dev;
}

V4L2_t *sv4l2_duplicate(V4L2_t *dev)
{
	V4L2_t *dup = NULL;
	if ((dev->mode & (MODE_OUTPUT | MODE_CAPTURE)) !=  (MODE_OUTPUT | MODE_CAPTURE))
	{
		err("sv4l2: device may not support duplication");
		return NULL;
	}
	dup = malloc(sizeof(*dup));
	if (!dup)
		return NULL;
	memcpy(dup, dev, sizeof(*dup));
	enum v4l2_buf_type type = -1;
	if (_sv4l2_prepare(dup->fd, &type, dev->mode, dev->config))
	{
		close(dup->fd);
		return NULL;
	}
	dup->type = type;

	struct v4l2_format fmt;
	fmt.type = dup->type;
	fmt.fmt.pix.field = V4L2_FIELD_ANY;
	if (ioctl(dup->fd, VIDIOC_G_FMT, &fmt) != 0)
	{
		err("sv4l2: FMT not found %m");
		close(dup->fd);
		return NULL;
	}
	dbg("sv4l2: duplicated settings: %dx%d, %.4s", fmt.fmt.pix_mp.width,
												fmt.fmt.pix_mp.height,
												&fmt.fmt.pix_mp.pixelformat);

	return dup;
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

static int _v4l2_subdev_fmtbus(void *arg, struct v4l2_subdev_mbus_code_enum *mbus_code)
{
	uint32_t code = *(uint32_t *)arg;
	if (code == mbus_code->code)
		return 0;
	return -1;
}

uint32_t sv4l2_subdev_getfmtbus(int ctrlfd, int(*fmtbus)(void *arg, struct v4l2_subdev_mbus_code_enum *mbuscode), void *cbarg)
{
	uint32_t ret = 0;
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
		if (fmtbus)
		{
			if (!fmtbus(cbarg, &mbusEnum))
				ret = mbusEnum.code;
		}
	}
	return ret;
}

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
	dbg("sv4l2: format request %#x", code);
	ret = sv4l2_subdev_getfmtbus(ctrlfd, _v4l2_subdev_fmtbus, &code);
	return ret;
}

int sv4l2_subdev_setpixformat(int ctrlfd, uint32_t fourcc, uint32_t width, uint32_t height)
{
	struct v4l2_subdev_format ffs = {0};
	ffs.pad = 0;
	ffs.which = V4L2_SUBDEV_FORMAT_ACTIVE;
	ffs.format.width = width;
	ffs.format.height = height;
	ffs.format.code = sv4l2_subdev_translate_fmtbus(ctrlfd, fourcc);
	if (ioctl(ctrlfd, VIDIOC_SUBDEV_S_FMT, &ffs) != 0)
	{
		err("sv4l2: subdev set format error %m");
		return -1;
	}
	return 0;
}

uint32_t sv4l2_subdev_getpixformat(int ctrlfd, int (*busformat)(void *arg, struct v4l2_subdev_format *ffs), void *cbarg)
{
	struct v4l2_subdev_format ffs = {0};
	ffs.pad = 0;
	ffs.which = V4L2_SUBDEV_FORMAT_ACTIVE;
	if (ioctl(ctrlfd, VIDIOC_SUBDEV_G_FMT, &ffs) != 0)
	{
		err("sv4l2: subdev get format error %m");
		return 0;
	}
	dbg("sv4l2: current subdev %lu x %lu %#X", ffs.format.width, ffs.format.height, ffs.format.code);
	if (busformat)
		return busformat(cbarg, &ffs);
	return 0;
}

#if 0
static uint32_t _v4l2_subdev_set_config(void *arg, struct v4l2_subdev_format *ffs)
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
#endif

int sv4l2_subdev_open(SubDevConfig_t *config)
{
	int ctrlfd = open(config->device, O_RDWR, 0);
	if (ctrlfd < 0)
	{
		err("sv4l2: subdevice %s not exist", config->device);
		return -1;
	}
	struct v4l2_subdev_capability caps = {0};
	if (ioctl(ctrlfd, VIDIOC_SUBDEV_QUERYCAP, &caps) != 0)
	{
		warn("sv4l2: subdev control error %m");
	}
#ifdef V4L2_SUBDEV_CAP_STREAMS
	if (caps.capabilities & V4L2_SUBDEV_CAP_STREAMS)
	{
		struct v4l2_subdev_client_capability clientCaps;
		clientCaps.capabilities = V4L2_SUBDEV_CLIENT_CAP_STREAMS;

		if (ioctl(ctrlfd, VIDIOC_SUBDEV_S_CLIENT_CAP, &clientCaps) != 0)
		{
			err("sv4l2: subdev control error %m");
			close(ctrlfd);
			return -1;
		}
		warn("sv4l2: client streams capabilities");
	}
#endif
	if (caps.capabilities & V4L2_SUBDEV_CAP_RO_SUBDEV)
	{
		warn("sv4l2: subdev read-only");
		close(ctrlfd);
		return -1;
	}
	return ctrlfd;
}

#else
static int sv4l2_subdev_open(SubDevConfig_t *config)
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
#ifdef HAVE_JANSSON
	devconfig->parent.ops.loadconfiguration = sv4l2_loadjsonconfiguration;
#endif
	return (DeviceConf_t *)devconfig;
}

#ifdef HAVE_JANSSON
typedef struct _SV4L2_Setting_s _SV4L2_Setting_t;
struct _SV4L2_Setting_s
{
	V4L2_t *dev;
	json_t *jconfig;
};

static int _sv4l2_loadjsonsetting(void *arg, struct v4l2_queryctrl *ctrl)
{
	_SV4L2_Setting_t *setting = (_SV4L2_Setting_t *)arg;
	json_t *jconfig = setting->jconfig;
	V4L2_t *dev = setting->dev;
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

static int _v4l2_loadjsontransformation(V4L2_t *dev, json_t *transformation)
{
	int disable = 0;
	json_t *bounds = json_object_get(transformation, "bounds");
	if (bounds == NULL || !json_is_object(bounds))
		bounds = transformation;
	json_t *top = json_object_get(bounds, "top");
	json_t *left = json_object_get(bounds, "left");
	json_t *width = json_object_get(bounds, "width");
	json_t *height = json_object_get(bounds, "height");
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
	return 0;
}

static int _v4l2_loadjsoncontrols(V4L2_t *dev, json_t *controls)
{
	_SV4L2_Setting_t setting;
	setting.dev = dev;
	setting.jconfig = controls;
	return sv4l2_treecontrols(dev, _sv4l2_loadjsonsetting, &setting);
}

int sv4l2_loadjsonsettings(V4L2_t *dev, void *entry)
{
	json_t *jconfig = entry;

	json_t *transformation = json_object_get(jconfig, "transformation");
	if (transformation && json_is_object(transformation))
	{
		_v4l2_loadjsontransformation(dev, transformation);
	}

	json_t *jcontrols = json_object_get(jconfig,"controls");
	if (jcontrols && (json_is_array(jcontrols) || json_is_object(jcontrols)))
		jconfig = jcontrols;
	return _v4l2_loadjsoncontrols(dev,jconfig);
}

int sv4l2_subdev_loadjsonconfiguration(void *arg, void *entry)
{
	int ret = 0;
	json_t *subdevice = entry;
	SubDevConfig_t *config = (SubDevConfig_t *)arg;

	if (subdevice && json_is_object(subdevice))
	{
		subdevice = json_object_get(subdevice, "device");
	}
	if (subdevice && json_is_string(subdevice))
	{
		const char *value = json_string_value(subdevice);
		config->device = value;
		ret = 0;
	}
	return ret;
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
	json_t *subdevices = json_object_get(jconfig, "subdevice");
	if (subdevices && json_is_array(subdevices))
	{
		config->subdevices = calloc(json_array_size(subdevices), sizeof(*config->subdevices));
		int index;
		json_t *subdevice;
		json_array_foreach(subdevices, index, subdevice)
		{
			sv4l2_subdev_loadjsonconfiguration(&config->subdevices[index], subdevice);
		}
	}
	else if (subdevices)
	{
		config->subdevices = calloc(1, sizeof(*config->subdevices));
		sv4l2_subdev_loadjsonconfiguration(&config->subdevices[0], subdevices);
		config->nsubdevices = 1;
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
	dbg("sv4l2: control type %d unsupported", type);
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

static int menuprint(void *arg, struct v4l2_querymenu *querymenu)
{
	json_t *control = (json_t *)arg;
	json_t *value = json_object_get(control, "value");
	if (value && querymenu->index == json_integer_value(value))
		json_object_set_new(control, "value_str", json_string(querymenu->name));
	return 0;
}

static int menuprintall(void *arg, struct v4l2_querymenu *querymenu)
{
	json_t *control = (json_t *)arg;
	json_t *items = json_object_get(control, "items");
	json_array_append_new(items, json_string(querymenu->name));
	return menuprint(arg, querymenu);
}

typedef struct _JSONControl_Arg_s _JSONControl_Arg_t;
struct _JSONControl_Arg_s
{
	json_t *controls;
	int ctrlfd;
	int all;
};

static int _sv4l2_jsoncontrol_cb(void *arg, struct v4l2_queryctrl *ctrl)
{
	_JSONControl_Arg_t *jsoncontrol_arg = (_JSONControl_Arg_t *)arg;
	json_t *controls = jsoncontrol_arg->controls;
	int ctrlfd = jsoncontrol_arg->ctrlfd;
	if (!json_is_object(controls) && !json_is_array(controls))
		return -1;
	json_t *ctrlclass = NULL;
	if (json_is_array(controls))
	{
		/**
		 * sort the controls into theire class
		 */
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
	void *value = _sv4l2_control(ctrlfd, ctrl->id, (void*)-1, ctrl);
	json_t *control = json_object();
	json_object_set_new(control, "name", json_string(ctrl->name));
	if (jsoncontrol_arg->all)
	{
		json_object_set_new(control, "id", json_integer(ctrl->id));
		json_t *type = json_string(CTRLTYPE(ctrl->type));
		json_object_set_new(control, "type", type);
	}

	switch (ctrl->type)
	{
	case V4L2_CTRL_TYPE_INTEGER:
	{
		json_object_set_new(control, "value", json_integer((int) value));
		if (jsoncontrol_arg->all)
		{
			json_object_set_new(control, "minimum", json_integer(ctrl->minimum));
			json_object_set_new(control, "maximum", json_integer(ctrl->maximum));
			json_object_set_new(control, "step", json_integer(ctrl->step));
			json_object_set_new(control, "default_value", json_integer(ctrl->default_value));
		}
	}
	break;
	case V4L2_CTRL_TYPE_BOOLEAN:
		json_object_set_new(control, "value", json_boolean((int) value));
		if (jsoncontrol_arg->all)
		{
			json_object_set_new(control, "default_value", json_integer(ctrl->default_value));
		}
	break;
	case V4L2_CTRL_TYPE_MENU:
	{
		json_t *jvalue = json_integer((int) value);
		json_object_set(control, "value", jvalue);
		if (jsoncontrol_arg->all)
		{
			json_t *items = json_array();
			json_object_set(control, "items", items);
			_sv4l2_treecontrolmenu(ctrlfd, ctrl, menuprintall, control);
			json_decref(items);
			json_object_set_new(control, "default_value", json_integer(ctrl->default_value));
		}
		else
			_sv4l2_treecontrolmenu(ctrlfd, ctrl, menuprint, control);
		json_decref(jvalue);
	}
	break;
	case V4L2_CTRL_TYPE_BUTTON:
	break;
	case V4L2_CTRL_TYPE_INTEGER64:
		json_object_set_new(control, "value", json_integer((int) value));
		if (jsoncontrol_arg->all)
		{
			json_object_set_new(control, "default_value", json_integer(ctrl->default_value));
		}
	break;
	case V4L2_CTRL_TYPE_STRING:
		json_object_set_new(control, "value", json_string(value));
	break;
	case V4L2_CTRL_TYPE_CTRL_CLASS:
	{
		if (!jsoncontrol_arg->all)
		{
			json_decref(control);
			return 0;
		}
		json_t *clas_ = json_array();
		json_object_set_new(control, "items", clas_);
		if (json_is_object(controls))
			json_object_set_new(controls, ctrl->name, control);
		else if (json_is_array(controls))
			json_array_append_new(controls, control);
		return 0;
	}
	break;
	}

	if (ctrlclass && json_is_array(ctrlclass))
		json_array_append_new(ctrlclass, control);
	else if (json_is_array(controls))
		json_array_append_new(controls, control);
	return 0;
}

static int _v4l2_capabilities_crop(V4L2_t *dev, json_t *transformation, int all)
{
	json_t *crop = json_object();
	json_object_set_new(crop, "name", json_string("crop"));
	if (all)
		json_object_set_new(crop, "type", json_string("rectangle"));
	json_t *rect = json_object();
	json_t *top = json_object();
	json_t *left = json_object();
	json_t *width = json_object();
	json_t *height = json_object();
	struct v4l2_selection sel = {0};
	if (all)
	{
		sel.type = sv4l2_type(dev);
		sel.target = V4L2_SEL_TGT_CROP_BOUNDS;
		if (ioctl(sv4l2_fd(dev), VIDIOC_G_SELECTION, &sel) == 0)
		{
			json_object_set_new(top, "type", json_string("integer"));
			json_object_set_new(top, "minimum", json_integer(0));
			json_object_set_new(top, "maximum", json_integer(sel.r.height - 32));
			json_object_set_new(left, "type", json_string("integer"));
			json_object_set_new(left, "minimum", json_integer(0));
			json_object_set_new(left, "maximum", json_integer(sel.r.width - 32));
			json_object_set_new(width, "type", json_string("integer"));
			json_object_set_new(width, "minimum", json_integer(32));
			json_object_set_new(width, "maximum", json_integer(sel.r.width));
			json_object_set_new(height, "type", json_string("integer"));
			json_object_set_new(height, "minimum", json_integer(32));
			json_object_set_new(height, "maximum", json_integer(sel.r.height));
		}
		else
		{
			json_decref(crop);
			json_decref(rect);
			json_decref(top);
			json_decref(left);
			json_decref(width);
			json_decref(height);
			return -1;
		}
		memset(&sel, 0, sizeof(sel));
	}
	sel.type = sv4l2_type(dev);
	sel.target = V4L2_SEL_TGT_CROP_DEFAULT;
	if (ioctl(sv4l2_fd(dev), VIDIOC_G_SELECTION, &sel) == 0)
	{
		json_object_set_new(top, "value", json_integer(sel.r.top));
		json_object_set_new(left, "value", json_integer(sel.r.left));
		json_object_set_new(width, "value", json_integer(sel.r.width));
		json_object_set_new(height, "value", json_integer(sel.r.height));
	}
	else
	{
		json_decref(crop);
		json_decref(rect);
		json_decref(top);
		json_decref(left);
		json_decref(width);
		json_decref(height);
		return -1;
	}
	json_object_set_new(rect, "top", top);
	json_object_set_new(rect, "left", left);
	json_object_set_new(rect, "width", width);
	json_object_set_new(rect, "height", height);
	json_object_set_new(crop, "bounds", rect);
	json_array_append_new(transformation, crop);
	return 0;
}

static int _v4l2_capabilities_fps(V4L2_t *dev, json_t *definition, int all)
{
	json_t *fps = json_object();
	json_object_set_new(fps, "name", json_string("fps"));
	if (all)
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
	else
	{
		json_decref(fps);
		return -1;
	}
	json_array_append_new(definition, fps);
	return 0;
}

static int _v4l2_capabilities_imageformat(V4L2_t *dev, json_t *definition, int all)
{
	json_t *pixelformat = json_object();
	json_object_set_new(pixelformat, "name", json_string("fourcc"));

	json_t *width = json_object();
	json_object_set_new(width, "name", json_string("width"));

	json_t *height = json_object();
	json_object_set_new(height, "name", json_string("height"));

	struct v4l2_format fmt = {0};
	fmt.type = sv4l2_type(dev);
	fmt.fmt.pix.field = V4L2_FIELD_ANY;
	if (ioctl(sv4l2_fd(dev), VIDIOC_G_FMT, &fmt) == 0)
	{
		json_object_set_new(pixelformat, "value", json_stringn((char*)&fmt.fmt.pix.pixelformat, 4));
		json_object_set_new(width, "value", json_integer(fmt.fmt.pix.width));
		json_object_set_new(height, "value", json_integer(fmt.fmt.pix.height));
	}

	if (!all)
	{
		json_array_append_new(definition, pixelformat);
		json_array_append_new(definition, width);
		json_array_append_new(definition, height);
		return 0;
	}

	json_object_set_new(pixelformat, "type", json_string(CTRLTYPE(V4L2_CTRL_TYPE_STRING)));
	json_object_set_new(width, "type", json_string(CTRLTYPE(V4L2_CTRL_TYPE_INTEGER)));
	json_object_set_new(height, "type", json_string(CTRLTYPE(V4L2_CTRL_TYPE_INTEGER)));
	json_t *items = json_array();
	struct v4l2_fmtdesc fmtdesc = {0};
	fmtdesc.type = sv4l2_type(dev);
	while (ioctl(sv4l2_fd(dev), VIDIOC_ENUM_FMT, &fmtdesc) == 0)
	{
		json_array_append_new(items, json_stringn((char*)&fmtdesc.pixelformat, 4));
		fmtdesc.index++;
	}
	json_object_set_new(pixelformat, "items", items);

	struct v4l2_frmsizeenum video_cap = {0};
	video_cap.pixel_format = fmt.fmt.pix.pixelformat;
	video_cap.type = V4L2_FRMSIZE_TYPE_STEPWISE;
	if(ioctl(sv4l2_fd(dev), VIDIOC_ENUM_FRAMESIZES, &video_cap) == 0)
	{
		if (video_cap.type == V4L2_FRMSIZE_TYPE_STEPWISE)
		{
			json_object_set_new(width, "minimum", json_integer(video_cap.stepwise.min_width));
			json_object_set_new(width, "maximum", json_integer(video_cap.stepwise.max_width));
			json_object_set_new(width, "step", json_integer(video_cap.stepwise.step_width));

			json_object_set_new(height, "minimum", json_integer(video_cap.stepwise.min_height));
			json_object_set_new(height, "maximum", json_integer(video_cap.stepwise.max_height));
			json_object_set_new(height, "step", json_integer(video_cap.stepwise.step_height));
		}
		else if (video_cap.type == V4L2_FRMSIZE_TYPE_DISCRETE)
		{
			json_t *items1 = json_array();
			json_t *items2 = json_array();
			for (video_cap.index = 0; ioctl(dev->fd, VIDIOC_ENUM_FRAMESIZES, &video_cap) != -1; video_cap.index++)
			{
				json_array_append_new(items1, json_integer(video_cap.discrete.height));
				json_array_append_new(items2, json_integer(video_cap.discrete.width));
			}
			json_object_set_new(height, "items", items1);
			json_object_set_new(width, "items", items2);
		}
	}

	json_array_append_new(definition, pixelformat);
	json_array_append_new(definition, width);
	json_array_append_new(definition, height);
	return 0;
}

int sv4l2_capabilities(V4L2_t *dev, json_t *capabilities, int all)
{
	json_t *definition = json_array();
	_v4l2_capabilities_imageformat(dev, definition, all);
	_v4l2_capabilities_fps(dev, definition, all);
	json_object_set_new(capabilities, "definition", definition);
	json_t *transformation = json_array();
	if (_v4l2_capabilities_crop(dev, transformation, all) == 0)
		json_object_set(capabilities, "transformation", transformation);
	json_decref(transformation);
	_JSONControl_Arg_t arg = {0};
	arg.controls = json_array();
	arg.all = all;
	arg.ctrlfd = sv4l2_fd(dev);
	int ret = sv4l2_treecontrols(dev, _sv4l2_jsoncontrol_cb, &arg);
	if (ret > 0)
		json_object_set(capabilities, "controls", arg.controls);
	json_decref(arg.controls);
	return 0;
}

int sv4l2_subdev_capabilities(int ctrlfd, json_t *capabilities, int all)
{
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
	}
#endif
	if (caps.capabilities & V4L2_SUBDEV_CAP_RO_SUBDEV)
	{
		warn("smedia: subdev read-only");
		close(ctrlfd);
		return -1;
	}
	_JSONControl_Arg_t arg = {0};
	arg.controls = json_array();
	arg.all = all;
	arg.ctrlfd = ctrlfd;
	int ret = _sv4l2_treecontrols(ctrlfd, _sv4l2_jsoncontrol_cb, &arg);
	if (ret > 0)
		json_object_set(capabilities, "controls", arg.controls);
	json_decref(arg.controls);
	return 0;
}

#endif
