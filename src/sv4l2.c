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
#include "sv4l2_subdev.h"

#define sv4l2_dbg(...)

/**
 * TODO split this file
 */
#define MAX_BUFFERS 4
#define TEST_FORMATMODIFIERS 0
#define V4L2_DEQUEUE_NONBLOCKED 0
#ifndef V4L2_TRYRATIO
#define V4L2_TRYRATIO 40
#endif

#ifdef V4L2_SUBDEV
#define ADD_SUBDEVICES 1
#endif

#define dbg_buffer_splane(v4l2) 		dbg("sv4l2: buf %d info:", v4l2->index); \
		dbg("\ttype: %s", (v4l2->type == V4L2_BUF_TYPE_VIDEO_CAPTURE)? "CAPTURE":"OUTPUT"); \
		dbg("\tmemory: %s", (v4l2->memory == V4L2_MEMORY_DMABUF)? "DMABUF":(v4l2->memory == V4L2_MEMORY_MMAP)?"MMAP":"USERPTR"); \
		dbg("\tdmafd: %d", v4l2->m.fd); \
		dbg("\tlength: %u", v4l2->length); \
		dbg("\tbytesused: %u", v4l2->bytesused);
#define dbg_buffer_mplane(v4l2) 		dbg("sv4l2: mplane buf %d info:", v4l2->index); \
		dbg("\ttype: %s", (v4l2->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE)? "CAPTURE":"OUTPUT"); \
		dbg("\tmemory: %s", (v4l2->memory == V4L2_MEMORY_DMABUF)? "DMABUF":(v4l2->memory == V4L2_MEMORY_MMAP)?"MMAP":"USERPTR"); \
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
	void *map[VIDEO_MAX_PLANES];
	size_t length;
	struct {
		int (*getdmafd)(V4L2Buffer_t *buf, int plane);
		void *(*getmem)(V4L2Buffer_t *buf, int plane);
		size_t (*getsize)(V4L2Buffer_t *buf, int plane);
		void (*setdma)(V4L2Buffer_t *buf, int plane, int fd, size_t size);
		void (*setmem)(V4L2Buffer_t *buf, int plane, void *mem, size_t size);
		void *(*mmap)(V4L2Buffer_t *buf, int plane, int fd);
	} ops;
};

#define MODE_CAPTURE 0x01
#define MODE_OUTPUT 0x02
#define MODE_MASTER 0x04
#define MODE_META 0x08
#define MODE_MEDIACTL 0x10
#define MODE_MPLANE 0x80

static int _v4l2buffer_exportdmafd(V4L2Buffer_t *buf, int plane, int fd)
{
	struct v4l2_exportbuffer expbuf = {0};
	expbuf.type = buf->v4l2.type;
	expbuf.index = buf->v4l2.index;
	expbuf.flags = O_CLOEXEC | O_RDWR;;
	if (ioctl(fd, VIDIOC_EXPBUF, &expbuf) != 0)
	{
		err("sv4l2: dmabuf export failed %m");
		return -1;
	}
	return expbuf.fd;
}

static int getdmafd_splane(V4L2Buffer_t *buf, int plane)
{
	return buf->v4l2.m.fd;
}

static void *getmem_splane(V4L2Buffer_t *buf, int plane)
{
	return buf->map[0];
}

static size_t getsize_splane(V4L2Buffer_t *buf, int plane)
{
	return buf->v4l2.length;
}

static void setdma_splane(V4L2Buffer_t *buf, int plane, int fd, size_t size)
{
	buf->v4l2.m.fd = fd;
	buf->v4l2.length = size;
	buf->length = size;
}

static void setmem_splane(V4L2Buffer_t *buf, int plane, void *mem, size_t size)
{
	buf->v4l2.m.userptr = (uintptr_t)mem;
	buf->v4l2.length = size;
	buf->length = size;
}

static void *mmap_splane(V4L2Buffer_t *buf, int plane, int fd)
{
	size_t offset = buf->v4l2.m.offset;
	buf->length = buf->v4l2.length;
	buf->map[0] = mmap(NULL, buf->length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, offset);
	return buf->map[0];
}

static int getdmafd_mplane(V4L2Buffer_t *buf, int plane)
{
	return buf->v4l2.m.planes[plane].m.fd;
}

static void *getmem_mplane(V4L2Buffer_t *buf, int plane)
{
	return buf->map[plane];
}

static size_t getsize_mplane(V4L2Buffer_t *buf, int plane)
{
	return buf->v4l2.m.planes[plane].length;
}

static void setdma_mplane(V4L2Buffer_t *buf, int plane, int fd, size_t size)
{
	buf->v4l2.m.planes[plane].m.fd = fd;
	buf->v4l2.m.planes[plane].length = size;
	buf->length += size;
}

static void setmem_mplane(V4L2Buffer_t *buf, int plane, void *mem, size_t size)
{
	buf->v4l2.m.planes[plane].m.userptr = (uintptr_t)mem;
	buf->v4l2.m.planes[plane].length = size;
	buf->length += size;
}

static void *mmap_mplane(V4L2Buffer_t *buf, int plane, int fd)
{
	size_t offset = buf->v4l2.m.planes[plane].m.mem_offset;
	size_t length = buf->v4l2.m.planes[plane].length;
	buf->map[plane] = mmap(NULL,length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, offset);
	return buf->map[plane];
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
		buffers[i].ops.getmem = getmem_splane;
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
		buffers[i].ops.getmem = getmem_mplane;
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
#ifdef V4L2_HAS_META
		if (mode & MODE_META)
			type = V4L2_BUF_TYPE_META_OUTPUT;
		else
#endif
			type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	}
	else if ((type == 0 || type == -1) && (mode & MODE_CAPTURE))
	{
#ifdef V4L2_HAS_META
		if (mode & MODE_META)
			type = V4L2_BUF_TYPE_META_CAPTURE;
		else
#endif
			type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	}
	else if (type == -1 && (mode & MODE_OUTPUT))
	{
#ifdef V4L2_HAS_META
		if (mode & MODE_META)
			type = V4L2_BUF_TYPE_META_OUTPUT;
		else
#endif
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
#ifdef V4L2_HAS_META
	if ((type == V4L2_BUF_TYPE_META_CAPTURE || type == V4L2_BUF_TYPE_META_OUTPUT) && (mode & MODE_META))
	{
		return type;
	}
#endif
	return -1;
}

static int _v4l2_devicecapabilities(int fd, char interface[32], int *mode, device_type_e type)
{
	struct v4l2_capability cap = {0};
	if (ioctl(fd, VIDIOC_QUERYCAP, &cap) != 0)
	{
		err("sv4l2: device %s not video %m", interface);
		return -1;
	}
	if (interface)
		memcpy(interface, cap.card, sizeof(cap.card));

	uint32_t caps = cap.capabilities;
#ifdef DEBUG
	if (caps & (V4L2_CAP_META_CAPTURE | V4L2_CAP_META_OUTPUT))
		dbg("sv4l2: media has Metadata capabilities");
#endif
	if (caps & V4L2_CAP_DEVICE_CAPS)
	{
		dbg("sv4l2: device capabilities available on %s %#x", cap.card, cap.capabilities);
		caps = cap.device_caps;
	}
#ifdef V4L2_HAS_META
	/**
	 * Some device may have two stream (data, meta)
	 * by default the data is supported
	 * and meta must be set inside the configuration.
	 * We unset meta if the device may not support meta.
	 */
	if (!(caps & (V4L2_CAP_META_CAPTURE | V4L2_CAP_META_OUTPUT)))
	{
		if (*mode & MODE_META)
		{
			err("sv4l2: device Metadata not available");
			return -1;
		}
	}
	else if (!(*mode & MODE_META) && !(caps & (V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_VIDEO_OUTPUT)))
	{
		warn("sv4l2: force enable Metadata");
		*mode |= MODE_META;
	}
	else
	{
		dbg("sv4l2: meta data available on %s", cap.card);
	}
#endif

#ifdef DEBUG
	dbg("sv4l2: device %s capabilities %#X", interface, cap.device_caps);
	if(caps & V4L2_CAP_VIDEO_CAPTURE)
		dbg("sv4l2: device %s capture (camera)", cap.card);
	if(caps & V4L2_CAP_VIDEO_CAPTURE_MPLANE)
		dbg("sv4l2: device %s capture mplane", cap.card);
	if(caps & V4L2_CAP_VIDEO_OUTPUT)
		dbg("sv4l2: device %s output", cap.card);
	if(caps & V4L2_CAP_VIDEO_OUTPUT_MPLANE)
		dbg("sv4l2: device %s output mplane", cap.card);
	if(caps & V4L2_CAP_VIDEO_OVERLAY)
		dbg("sv4l2: device %s overlay", cap.card);
	if(caps & V4L2_CAP_VIDEO_M2M)
		dbg("sv4l2: device %s memory to memory", cap.card);
	if(caps & V4L2_CAP_VIDEO_M2M_MPLANE)
		dbg("sv4l2: device %s memory to memory mplane", cap.card);
	if(caps & V4L2_CAP_AUDIO)
		dbg("sv4l2: device %s audio", cap.card);
	if(caps & V4L2_CAP_VBI_CAPTURE)
		dbg("sv4l2: device %s vbi", cap.card);
	if(caps & V4L2_CAP_RADIO)
		dbg("sv4l2: device %s radio", cap.card);
	if(caps & V4L2_CAP_EXT_PIX_FORMAT)
		dbg("sv4l2: device %s Pixformat extension available", cap.card);
	if(caps & V4L2_CAP_IO_MC)
		dbg("sv4l2: device %s media control available", cap.card);
#endif
	if ((caps & V4L2_CAP_VIDEO_CAPTURE ||
		caps & V4L2_CAP_META_CAPTURE ||
		caps & V4L2_CAP_VIDEO_CAPTURE_MPLANE) &&
		(type == device_input || type == device_control))
		*mode |= MODE_CAPTURE;
	if ((caps & V4L2_CAP_VIDEO_M2M ||
		caps & V4L2_CAP_VIDEO_M2M_MPLANE))
		*mode |= (MODE_CAPTURE | MODE_OUTPUT);
	if ((caps & V4L2_CAP_VIDEO_OUTPUT ||
		caps & V4L2_CAP_META_OUTPUT ||
		caps & V4L2_CAP_VIDEO_OUTPUT_MPLANE) &&
		(type == device_output || type == device_control))
		*mode |= MODE_OUTPUT;
	if (caps & V4L2_CAP_VIDEO_CAPTURE_MPLANE ||
		caps & V4L2_CAP_VIDEO_OUTPUT_MPLANE ||
		caps & V4L2_CAP_VIDEO_M2M_MPLANE)
		*mode |= MODE_MPLANE;
#ifdef V4L2_CAP_IO_MC
	if (caps & V4L2_CAP_IO_MC)
		*mode |= MODE_MEDIACTL;
#endif

	if ((*mode & (MODE_CAPTURE | MODE_OUTPUT)) == 0)
	{
		err("sv4l2: %s bad device type %#x", cap.card, cap.capabilities);
		return -1;
	}
	if (!(caps & V4L2_CAP_STREAMING))
	{
		err("sv4l2: device %s not camera", cap.card);
		return -1;
	}
	dbg("sv4l2: %s streaming available (%#x)", cap.card, *mode);
	return 0;
}

uint32_t sv4l2_getpixformat(V4L2_t *dev, int (*pixformat)(void *arg, struct v4l2_fmtdesc *fmtdesc, int isset), void *cbarg)
{
	uint32_t pixelformat = 0;

	struct v4l2_format fmt = {0};
	fmt.type = dev->type;
	if (ioctl(dev->fd, VIDIOC_G_FMT, &fmt) != 0)
	{
		err("sv4l2: FMT not found %m");
		return -1;
	}
#ifdef V4L2_HAS_META
	if (dev->type == V4L2_BUF_TYPE_META_CAPTURE || dev->type == V4L2_BUF_TYPE_META_OUTPUT)
		pixelformat = fmt.fmt.meta.dataformat;
	else
#endif
		pixelformat = fmt.fmt.pix.pixelformat;
	if (dev->mode & MODE_MPLANE)
	{
		dev->width = fmt.fmt.pix_mp.width;
		dev->height = fmt.fmt.pix_mp.height;
		dev->fourcc = fmt.fmt.pix_mp.pixelformat;
		dev->stride = fmt.fmt.pix_mp.plane_fmt[0].bytesperline;
		dev->nplanes = fmt.fmt.pix_mp.num_planes;
	}
	else
	{
		dev->width = fmt.fmt.pix.width;
		dev->height = fmt.fmt.pix.height;
		dev->fourcc = fmt.fmt.pix.pixelformat;
		dev->stride = fmt.fmt.pix.bytesperline;
		dev->nplanes = 1;
	}

	struct v4l2_fmtdesc fmtdesc = {0};
	fmtdesc.type = dev->type;
	sv4l2_dbg("sv4l2: Formats:");
	while (pixformat && ioctl(dev->fd, VIDIOC_ENUM_FMT, &fmtdesc) == 0)
	{
		sv4l2_dbg("\t%.4s => %s %#x", (char*)&fmtdesc.pixelformat, fmtdesc.description,
				fmtdesc.flags);
		fmtdesc.index++;
		pixformat(cbarg, &fmtdesc, (fmtdesc.pixelformat == pixelformat));
	}
	return pixelformat;
}

static uint32_t _v4l2_setpixformat(int fd, enum v4l2_buf_type type, uint32_t fourcc, uint64_t modifiers)
{
	uint32_t pixelformat = 0;

	struct v4l2_format fmt = {0};
	fmt.type = type;
	if (ioctl(fd, VIDIOC_G_FMT, &fmt) != 0)
	{
		err("sv4l2: FMT not found %m");
		return -1;
	}
#ifdef V4L2_HAS_META
	int ismeta = (type == V4L2_BUF_TYPE_META_CAPTURE || type == V4L2_BUF_TYPE_META_OUTPUT);
	if (ismeta)
		pixelformat = fmt.fmt.meta.dataformat;
#endif

	struct v4l2_fmtdesc fmtdesc = {0};
	fmtdesc.type = type;
	sv4l2_dbg("sv4l2: Formats:");
	while (ioctl(fd, VIDIOC_ENUM_FMT, &fmtdesc) == 0)
	{
		sv4l2_dbg("\t%.4s => %s", (char*)&fmtdesc.pixelformat,
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
		if (!pixelformat && formats[i].fourcc != 0)
		{
			pixelformat = formats[i].fourcc;
		}
	}
#ifdef V4L2_HAS_META
	if ((pixelformat) && ismeta)
		fmt.fmt.meta.dataformat = pixelformat;
	else
#endif
	if (pixelformat)
		fmt.fmt.pix.pixelformat = pixelformat;
	if (ioctl(fd, VIDIOC_S_FMT, &fmt) != 0)
	{
		err("sv4l2: FMT setting error %m on device type %d", type);
		return -1;
	}
#ifdef V4L2_HAS_META
	if (ismeta)
		dbg("sv4l2: settings: %.4s %u", (char*)&fmt.fmt.meta.dataformat, fmt.fmt.meta.buffersize);
	else
#endif
	dbg("sv4l2: settings: %.4s stride: %u/%u field:%#x", (char*)(fmt.fmt.pix.pixelformat?&fmt.fmt.pix.pixelformat:&pixelformat), fmt.fmt.pix.bytesperline, fmt.fmt.pix.width, fmt.fmt.pix.field);
	return fmt.fmt.pix.pixelformat;
}

static uint32_t _v4l2_checkframesize(int fd, uint32_t fourcc, uint32_t *width, uint32_t *height)
{
	struct v4l2_frmsizeenum video_cap = {0};
	video_cap.pixel_format = fourcc;
	video_cap.index = 0;
	if (ioctl(fd, VIDIOC_ENUM_FRAMESIZES, &video_cap) != 0)
	{
		err("sv4l2: framsesize enumeration error %m");
		return -1;
	}

	sv4l2_dbg("sv4l2: Frame size:");
	if (video_cap.type == V4L2_FRMSIZE_TYPE_STEPWISE)
	{
		sv4l2_dbg("\t%d < width  < %d, step %d", video_cap.stepwise.min_width, video_cap.stepwise.max_width, video_cap.stepwise.step_width);
		sv4l2_dbg("\t%d < height < %d, step %d", video_cap.stepwise.min_height, video_cap.stepwise.max_height, video_cap.stepwise.step_height);
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
			sv4l2_dbg("\twidth %d, height %d", video_cap.discrete.width, video_cap.discrete.height);
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
		err("sv4l2: framsesize enumeration error %m");
		return -1;
	}
	sv4l2_dbg("sv4l2: Frame size:");
	if (video_cap.type == V4L2_FRMSIZE_TYPE_STEPWISE)
	{
		sv4l2_dbg("\t%d < width  < %d, step %d", video_cap.stepwise.min_width, video_cap.stepwise.max_width, video_cap.stepwise.step_width);
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
			sv4l2_dbg("\twidth %d, height %d", video_cap.discrete.width, video_cap.discrete.height);
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

static uint32_t _v4l2_setframesize(int fd, enum v4l2_buf_type type, uint32_t *width, uint32_t *height)
{
	uint32_t framesize = 0;

	struct v4l2_format fmt = {0};
	fmt.type = type;
	fmt.fmt.pix.field = V4L2_FIELD_ANY;
	if (ioctl(fd, VIDIOC_G_FMT, &fmt) != 0)
	{
		return -1;
	}

	uint8_t pdepth = fmt.fmt.pix.bytesperline / fmt.fmt.pix.width;
	if (*height > 0 && *width == 0)
	{
		*width = *height * 16 / 9;
	}
	if (*width > 0 && *height > 0
		&& !_v4l2_checkframesize(fd, fmt.fmt.pix.pixelformat, width, height))
	{
		fmt.fmt.pix.width = *width;
		fmt.fmt.pix.height = *height;
		fmt.fmt.pix.bytesperline = *width * pdepth;
	}
	if (ioctl(fd, VIDIOC_S_FMT, &fmt) != 0)
	{
		return -1;
	}
	*width = fmt.fmt.pix.width;
	*height = fmt.fmt.pix.height;
	framesize = fmt.fmt.pix.sizeimage;
	return framesize;
}

static int _v4l2_setfps_vblank(int ctrlfd, uint32_t width, uint32_t height, int fps)
{
	struct v4l2_ext_control control = {0};
	struct v4l2_ext_controls controls = {0};
	controls.count = 1;
	controls.controls = &control;

	errno = 0;
	control.id = V4L2_CID_PIXEL_RATE;
	control.value = 0;
	if (ioctl(ctrlfd, VIDIOC_G_EXT_CTRLS, &controls))
	{
		err("sv4l2: fps access error %m");
		return -1;
	}
	uint32_t pixelrate = control.value;

	control.id = V4L2_CID_HBLANK;
	control.value = 0;
	ioctl(ctrlfd, VIDIOC_G_EXT_CTRLS, &controls);
	uint32_t hblank = control.value;

	control.id = V4L2_CID_VBLANK;
	control.value = 0;
	ioctl(ctrlfd, VIDIOC_G_EXT_CTRLS, &controls);
	uint32_t vblank = control.value;

	if (fps != -1)
	{
		if (fps > 0)
			vblank = pixelrate / fps;
		else
			vblank = pixelrate * fps;
		vblank /= width + hblank;
		vblank -= height;
		control.id = V4L2_CID_VBLANK;
		control.value = vblank;
		if (ioctl(ctrlfd, VIDIOC_S_EXT_CTRLS, &controls))
		{
			err("sv4l2: unable to set vblank %m");
			fps = -1;
		}
		else
		{
			vblank = control.value;
			dbg("sv4l2: new vertical blank %u", vblank);
		}
	}
	else
	{
		fps = vblank + height;
		fps *= width + hblank;
		if (fps)
			fps = pixelrate / fps;
	}
	if (fps != -1)
		warn("sv4l2: Frame rate: %d/%d fps vertical blank %u",
			(fps > 0)?fps:1, (fps > 0)?1:-fps, vblank);
	return fps;
}

static int _v4l2_setfps_param(int fd, enum v4l2_buf_type type, int fps)
{
	if (type < V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -1;
	struct v4l2_streamparm streamparm = {0};
	streamparm.type = type;
	if (ioctl(fd, VIDIOC_G_PARM, &streamparm) == -1)
	{
		err("sv4l2: parameter not available %m");
		return -1;
	}

	struct v4l2_captureparm *parm = &streamparm.parm.capture;
	if (type == V4L2_BUF_TYPE_VIDEO_OUTPUT || type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE)
		parm = (struct v4l2_captureparm *)&streamparm.parm.output;
	if (fps == -1)
	{
		fps = parm->timeperframe.denominator /
				parm->timeperframe.numerator;
	}
	else if (fps >= 0 && fps != parm->timeperframe.denominator)
	{
		parm->timeperframe.denominator = fps;
		parm->timeperframe.numerator = 1;
		if (ioctl(fd, VIDIOC_S_PARM, &streamparm) == -1)
		{
			err("sv4l2: parameter setting error %m");
			return -1;
		}
	}
	else if (fps < 0 && -fps != parm->timeperframe.numerator)
	{
		parm->timeperframe.denominator = 1;
		parm->timeperframe.numerator = -fps;
		if (ioctl(fd, VIDIOC_S_PARM, &streamparm) == -1)
		{
			err("sv4l2: parameter setting error %m");
			return -1;
		}
	}
	dbg("sv4l2: Frame rate: %d/%d fps (request %d/%d)",
			parm->timeperframe.numerator,
			parm->timeperframe.denominator,
			(fps > 0)?1:-fps, (fps > 0)?fps:1);
	return fps;
}

int sv4l2_fps(V4L2_t *dev, int fps)
{
	int ret = _v4l2_setfps_param(dev->fd, dev->type, fps);
	if (ret == -1)
		ret = _v4l2_setfps_vblank(dev->fd, dev->width, dev->height, fps);
	return ret;
}

static int _v4l2_getbufferfd(V4L2_t *dev, int i, int plane)
{
	if (dev->nbuffers <= i)
		return -1;
	int dma_fd = dev->buffers[i].ops.getdmafd(&dev->buffers[i], plane);
	if (dev->buffers[i].v4l2.memory == V4L2_MEMORY_DMABUF && dma_fd > 0)
	{
		return dma_fd;
	}
	return _v4l2buffer_exportdmafd(&dev->buffers[i], plane, dev->fd);
}

int sv4l2_requestbuffer_mmap(V4L2_t *dev, int count)
{
	int ret = 0;
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
#ifdef V4L2_BUF_CAP_SUPPORTS_DMABUF
	if (req.capabilities & V4L2_BUF_CAP_SUPPORTS_DMABUF)
	{
		dbg("sv4l2: buffer supports DMABUF too");
	}
#endif
	dev->nbuffers = req.count;
	dev->buffers = dev->ops.createbuffers(dev, dev->nbuffers, V4L2_MEMORY_MMAP);

	dbg("sv4l2: request %d buffers", req.count);
	for (int i = 0; i < dev->nbuffers; i++)
	{
		if (ioctl(dev->fd, VIDIOC_QUERYBUF, &dev->buffers[i].v4l2) != 0)
		{
			err("sv4l2: Query buffer for mmap error %m");
			dev->nbuffers = i;
			return -1;
		}
		for (int j = 0; j < dev->nplanes; j ++)
		{
			if (dev->buffers[i].ops.mmap(&dev->buffers[i], j, dev->fd) == MAP_FAILED)
			{
				err("sv4l2: buffer mmap error %m");
				return -1;
			}
		}
	}

	return ret;
}

int sv4l2_requestbuffer_dmabuf(V4L2_t *dev, int count)
{
	if (dev->buffers && dev->buffers[0].v4l2.memory == V4L2_MEMORY_DMABUF)
		return 0;
	V4L2Buffer_t *oldbuffers = NULL;
	if (dev->mode & MODE_MASTER)
	{
		/// master request MMAP first to export the DMA in a second time
		if (sv4l2_requestbuffer_mmap(dev, count) < 0)
		{
			err("sv4l2: mmap error");
			return -1;
		}
		oldbuffers = dev->buffers;
		for (int i = 0; i < dev->nbuffers; i++)
		{
			int dma_fd = _v4l2_getbufferfd(dev, i, 0);
			if (dma_fd == -1)
			{
				dev->nbuffers = i;
				return -1;
			}
			for (int j = 0; j < dev->nplanes; j++)
			{
				size_t size = dev->buffers[i].ops.getsize(&dev->buffers[i], j);
				dev->buffers[i].ops.setdma(&dev->buffers[i], j, dma_fd, size);
				for (int k = 0 ; k < dev->nplanes; k++)
				{
					munmap(dev->buffers[i].map[k], dev->buffers[i].ops.getsize(&dev->buffers[i], k));
					dev->buffers[i].map[k] = NULL;
				}
			}
		}
		struct v4l2_requestbuffers req = {0};
		req.type = dev->buffers[0].v4l2.type;
		req.memory = dev->buffers[0].v4l2.memory;
		req.count = 0;
		if (ioctl(dev->fd, VIDIOC_REQBUFS, &req) == -1)
		{
			err("sv4l2: Free buffer for dma error %m");
			return -1;
		}
	}

	struct v4l2_requestbuffers req = {0};
	req.count = count;
	req.type = dev->type;
	req.memory = V4L2_MEMORY_DMABUF;
	if (ioctl(dev->fd, VIDIOC_REQBUFS, &req) != 0)
	{
		err("sv4l2: device doesn't allow DMABUF %m");
		return -1;
	}
	dev->nbuffers = req.count;
	dev->buffers = dev->ops.createbuffers(dev, dev->nbuffers, V4L2_MEMORY_DMABUF);

	dbg("sv4l2: request %d buffers", req.count);
	for (int i = 0; i < dev->nbuffers; i++)
	{
		if (oldbuffers)
		{
			for (int j = 0; j < dev->nplanes; j++)
			{
				int dma_fd = oldbuffers[i].ops.getdmafd(&oldbuffers[i], j);
				size_t size = oldbuffers[i].ops.getsize(&oldbuffers[i], j);
				dev->buffers[i].ops.setdma(&dev->buffers[i], j, dma_fd, size);
			}
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
			err("sv4l2: UserPtr memory not supported by device");
		else
			err("sv4l2: Request buffer for mmap error %m");
		return -1;
	}
	dev->nbuffers = req.count;
	dev->buffers = dev->ops.createbuffers(dev, dev->nbuffers, V4L2_MEMORY_USERPTR);
	if (dev->nbuffers > nmems)
		err("sv4l2: Not enougth memory buffers");

	dbg("sv4l2: request %d buffers", req.count);
	count = (dev->nbuffers > nmems)? nmems:dev->nbuffers;
	for (int i = 0; i < count; i++)
	{
		for (int j = 0; j < dev->nplanes; j++)
			dev->buffers[i].ops.setmem(&dev->buffers[i], j, mems[i], size);
	}
	return 0;
}

int sv4l2_linkv4l2(V4L2_t *dev, V4L2_t *target)
{
	for (int i = 0; i < dev->nbuffers; i++)
	{
		if (dev->buffers[i].v4l2.memory != V4L2_MEMORY_DMABUF)
			return -1;
		int dma_fd = _v4l2_getbufferfd(target, i, 0);
		if (dma_fd == -1)
		{
			dev->nbuffers = i;
			return -1;
		}
		for (int j = 0; j < dev->nplanes; j++)
		{
			size_t size = target->buffers[i].ops.getsize(&target->buffers[i], j);
			dev->buffers[i].ops.setdma(&dev->buffers[i], j, dma_fd, size);
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
		for (int j = 0; j < dev->nplanes; j++)
		{
			int dma_fd = targets[i + j];
			dev->buffers[i].ops.setdma(&dev->buffers[i], j, dma_fd, size);
		}
	}
	return 0;
}

int sv4l2_requestbuffer(V4L2_t *dev, enum buf_type_e t, ...)
{
	int ret = 0;
	if (dev->ops.createbuffers == NULL)
		return -1;
	if (t & buf_type_master)
		dev->mode |= MODE_MASTER;
	va_list ap;
	va_start(ap, t);
	switch (t)
	{
		case buf_type_sv4l2_master:
			ret = sv4l2_requestbuffer_dmabuf(dev, MAX_BUFFERS);
		break;
		case buf_type_sv4l2:
		{
			V4L2_t *master = va_arg(ap, V4L2_t *);
			if ((ret = sv4l2_requestbuffer_dmabuf(dev, MAX_BUFFERS)) == 0)
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
		case (buf_type_memory_master):
		{
			ret = sv4l2_requestbuffer_mmap(dev, MAX_BUFFERS);
			if (ret)
				break;
			int *ntargets = va_arg(ap, int *);
			void ***targets = va_arg(ap, void ***);
			size_t *size = va_arg(ap, size_t *);
			if (ntargets != NULL)
				*ntargets = dev->nbuffers;
			if (targets != NULL)
			{
				*targets = calloc(dev->nbuffers * dev->nplanes, sizeof(void *));
				for (int i = 0; i < dev->nbuffers; i++)
					for (int j = 0; j < dev->nplanes; j++)
					{
						(*targets)[i + j] = dev->buffers[i].ops.getmem(&dev->buffers[i], j);
					}
				dev->arraybuffers = *targets;
			}
			if (size != NULL)
				*size = dev->buffers[0].ops.getsize(&dev->buffers[0], 0);
		}
		break;
		case buf_type_dmabuf:
		{
			if (dev->config->parent.modifiers)
			{
				err("sv4l2: format currently doesn't support modifiers");
#if TEST_FORMATMODIFIERS
				return -1;
#endif
			}
			int ntargets = va_arg(ap, int);
			int *targets = va_arg(ap, int *);
			size_t size = va_arg(ap, size_t);
			if ((ret = sv4l2_requestbuffer_dmabuf(dev, ntargets)) == 0)
			{
				ret = sv4l2_linkdma(dev, ntargets, targets, size);
			}
		}
		break;
		case buf_type_dmabuf_master:
		{
			ret = sv4l2_requestbuffer_dmabuf(dev, MAX_BUFFERS);
			if (ret)
				break;
			int *ntargets = va_arg(ap, int *);
			int **targets = va_arg(ap, int **);
			size_t *size = va_arg(ap, size_t *);
			if (ntargets != NULL)
				*ntargets = dev->nbuffers;
			if (targets != NULL)
			{
				*targets = calloc(dev->nbuffers * dev->nplanes, sizeof(int));
				for (int i = 0; i < dev->nbuffers; i++)
					for (int j = 0; j < dev->nplanes; j++)
						(*targets)[i + j] = dev->buffers[i].ops.getdmafd(&dev->buffers[i], j);
				dev->arraybuffers = *targets;
			}
			if (size != NULL)
				*size = dev->buffers[0].ops.getsize(&dev->buffers[0], 0);
		}
		break;
		default:
			err("sv4l2: unkonwn buffer type");
			va_end(ap);
			return -1;
	}
	va_end(ap);
#if 0
	for (int i = 0; i < dev->nbuffers; i++)
	{
		dbg_buffer((&dev->buffers[i].v4l2));
	}
#endif
	dbg("sv4l2: %s %dx%d, %.4s %u", dev->name, dev->width, dev->height, (char*)&dev->fourcc, (dev->buffers)?dev->buffers[0].length:0);
	return ret;
}

static int _v4l2_transform(V4L2_t *dev, struct v4l2_rect *r, int target)
{
	struct v4l2_selection sel = {0};
	sel.type = dev->type;
	if (r != NULL)
	{
		sel.target = target;
		memcpy(&sel.r, r, sizeof(sel.r));
	}
	else
		sel.target = target | 0x01; /// 0x01 => <target>_DEFAULT
	sel.flags = V4L2_SEL_FLAG_GE | V4L2_SEL_FLAG_LE;
	if (ioctl(dev->fd, VIDIOC_S_SELECTION, &sel))
	{
		err("sv4l2: cropping error %m");
		return -1;
	}
	dbg("sv4l2: croping requested (%d %d %d %d)", sel.r.left, sel.r.top, sel.r.width, sel.r.height);
	return 0;
}

int sv4l2_crop(V4L2_t *dev, struct v4l2_rect *r)
{
	return _v4l2_transform(dev, r, V4L2_SEL_TGT_CROP);
}

int sv4l2_compose(V4L2_t *dev, struct v4l2_rect *r)
{
	return _v4l2_transform(dev, r, V4L2_SEL_TGT_COMPOSE);
}

static void * _sv4l2_control(int ctrlfd, int id, void *value, struct v4l2_query_ext_ctrl *queryctrl)
{
#if 0
	if (V4L2_CTRL_ID2CLASS(id) == V4L2_CTRL_CLASS_USER)
	{
		uint32_t ivalue = (uint32_t) value;
		struct v4l2_control control = {0};
		control.id = id;
		control.value = ivalue;
		if (value != (void*)-1 && ioctl(ctrlfd, VIDIOC_S_CTRL, &control))
		{
			err("sv4l2: control %d(%#d) setting error %m", id, id);
			return (void *)-1;
		}
		control.value = 0;
		if (queryctrl->type != V4L2_CTRL_TYPE_CTRL_CLASS &&
			ioctl(ctrlfd, VIDIOC_G_CTRL, &control))
		{
			err("sv4l2: device doesn't support control %d(%#d) %m", id, id);
			return (void *)-1;
		}
		value = (void *)control.value;
		dbg("sv4l2: control %d(%#d) => %d", id, id, control.value);
	}
	else
#endif
	struct v4l2_ext_control control = {0};
	char string[256] = {0};
	control.id = id;
	if (queryctrl->type == V4L2_CTRL_TYPE_INTEGER)
	{
		control.value = (int32_t)(long)value;
		control.size = sizeof(int32_t);
	}
	else if (queryctrl->type == V4L2_CTRL_TYPE_BOOLEAN)
	{
		if (value)
			control.value = 1;
		control.size = 1;
	}
	else if (queryctrl->type == V4L2_CTRL_TYPE_MENU)
	{
		control.value = (uint32_t)(long)value;
		control.size = sizeof(uint32_t);
	}
	else if (queryctrl->type == V4L2_CTRL_TYPE_BUTTON)
	{
		control.size = 0;
	}
	else if (queryctrl->type == V4L2_CTRL_TYPE_INTEGER64)
	{
		control.value64 = (int64_t)(long)value;
		control.size = sizeof(int64_t);
	}
	else if (queryctrl->type == V4L2_CTRL_TYPE_STRING && value != NULL)
	{
		if (value != (void*)-1)
		{
			control.size = strlen(value) + 1;
			control.string = value;
		}
		else
		{
			control.size = sizeof(string);
			control.string = string;
		}
	}
	else if (queryctrl->type == V4L2_CTRL_TYPE_U8 ||
			queryctrl->type == V4L2_CTRL_TYPE_U16 ||
			queryctrl->type == V4L2_CTRL_TYPE_U32)
	{
		if (value != (void*)-1)
			control.size = queryctrl->elem_size * queryctrl->elems;
		else
			control.size = sizeof(string);
		/// fix error on some drivers (raspberry as example)
		if (control.size == 0)
		{
			for (int i = 0; i < queryctrl->nr_of_dims; i++)
			{
				control.size += queryctrl->dims[i];
			}
		}
		if (control.size == 0)
		{
			control.size = queryctrl->nr_of_dims * ( 1 + (queryctrl->type - V4L2_CTRL_TYPE_U8) * 2);
		}
		control.p_u8 = value;
	}
	if (control.size == 0)
	{
		return 0;
	}
	struct v4l2_ext_controls controls = {0};
	controls.count = 1;
	controls.controls = &control;
	if (value != (void *)(long)-1)
	{
		if (queryctrl->flags & V4L2_CTRL_FLAG_READ_ONLY)
		{
			err("sv4l2: control %d(%#x) read-only", id, id);
		}
		else if (ioctl(ctrlfd, VIDIOC_S_EXT_CTRLS, &controls))
		{
			err("sv4l2: control %d(%#x) %s setting error %m", id, id, queryctrl->name);
			return (void *)-1;
		}
	}
	/**
	 * DEBUG with valgrind.
	 * valgrind returns problem on the next ioctl
	 * if ptr is not set on a good address even with an integer control.
	 * Syscall param ioctl(VKI_V4L2_G_EXT_CTRLS).controls[].ptr[] points to unaddressable byte(s)
	 */
	control.value = 0;
	if (queryctrl->flags & V4L2_CTRL_FLAG_HAS_PAYLOAD)
		control.p_u8 = (unsigned char *)string;
	if ((queryctrl->type != V4L2_CTRL_TYPE_BUTTON) &&
		(queryctrl->type != V4L2_CTRL_TYPE_CTRL_CLASS) &&
		ioctl(ctrlfd, VIDIOC_G_EXT_CTRLS, &controls))
	{
		err("sv4l2: control %d(%#x) %s getting error %m", id, id, queryctrl->name);
		return (void *)(long)-1;
	}
	value = control.ptr;
	switch (queryctrl->type)
	{
	case V4L2_CTRL_TYPE_BOOLEAN:
	case V4L2_CTRL_TYPE_INTEGER:
		warn("sv4l2: control %d(%#x) %s => %d", id, id, queryctrl->name, control.value);
	break;
	case V4L2_CTRL_TYPE_STRING:
		warn("sv4l2: control %d(%#x) %s => %s", id, id, queryctrl->name, (const char *)control.ptr);
	break;
	case V4L2_CTRL_TYPE_INTEGER64:
		warn("sv4l2: control %d(%#x) %s => array", id, id, queryctrl->name);
	break;
	case V4L2_CTRL_TYPE_U8:
	case V4L2_CTRL_TYPE_U16:
	case V4L2_CTRL_TYPE_U32:
		warn("sv4l2: control %d %s => array", id, queryctrl->name);
	break;
	case V4L2_CTRL_TYPE_MENU:
	{
		struct v4l2_querymenu querymenu = {0};
		querymenu.id = control.id;
		querymenu.index = control.value;
		ioctl(ctrlfd, VIDIOC_QUERYMENU, &querymenu);
		warn("sv4l2: control %d(%#x) %s => %s", id, id, queryctrl->name, querymenu.name);
	}
	break;
	default:
		warn("sv4l2: control %d(%#x) %s => type(%d)", id, id, queryctrl->name, queryctrl->type);
	}
	if (value == (void *)(long)-1)
		err("sv4l2: control %d(%#x) %s => not set", id, id, queryctrl->name);
	return value;
}

void * sv4l2_control(V4L2_t *dev, int id, void *value)
{
	int ctrlfd = dev->fd;
	struct v4l2_query_ext_ctrl queryctrl = {0};
	queryctrl.id = id;
	void *retvalue = NULL;
	int ret = ioctl(ctrlfd, VIDIOC_QUERY_EXT_CTRL, &queryctrl);
	if (ret != 0)
	{
		retvalue = (void *)(long)-1;
#if ADD_SUBDEVICES
		for (int i = 0; retvalue == (void *)(long)-1 && i < (sizeof(dev->subdevs) / sizeof(*(dev->subdevs))); i++)
		{
			if (dev->subdevs[i])
				retvalue = sv4l2_control(dev->subdevs[i], id, value);
		}
#endif
	}
	if (retvalue == (void *)(long)-1)
	{
		err("sv4l2: control %d(%#x) not supported on %s %d", id, id, dev->config->parent.name, ctrlfd);
		return (void *)-1;
	}

	if (queryctrl.flags & V4L2_CTRL_FLAG_DISABLED)
	{
		err("sv4l2: control %d(%#x) disabled", id, id);
		return 0;
	}
	retvalue =  _sv4l2_control(ctrlfd, id, value, &queryctrl);
	return retvalue;
}

static int _v4l2_periodiccontrol(V4L2_t *dev, int bufferid)
{
	int ctrlfd = dev->fd;
	struct v4l2_ext_control control = {0};
	control.id = dev->config->periodiccontrol;
	struct v4l2_ext_controls controls = {0};
	controls.count = 1;
	controls.controls = &control;
	if (ioctl(ctrlfd, VIDIOC_S_EXT_CTRLS, &controls))
	{
		err("sv4l2: periodic control error %m");
		dev->config->periodiccontrol = 0;
		dev->periodicfunc = NULL;
		return -1;
	}
	return 0;
}

static int _sv4l2_treecontrols(int ctrlfd, int (*cb)(void *arg, struct v4l2_query_ext_ctrl *ctrl), void * arg)
{
	int nbctrls = 0;
#if 0
	struct v4l2_queryctrl qctrl = {0};
	qctrl.id = V4L2_CTRL_FLAG_NEXT_CTRL;
	int ret;
	for (nbctrls = 0; (ret = ioctl(ctrlfd, VIDIOC_QUERYCTRL, &qctrl)) == 0; nbctrls++)
	{
		if (qctrl.flags & V4L2_CTRL_FLAG_DISABLED)
		{
			dbg("sv4l2: control %s %d(%#d) disabled", qctrl.name, qctrl.id, qctrl.id);
			qctrl.id |= V4L2_CTRL_FLAG_NEXT_CTRL | V4L2_CTRL_FLAG_NEXT_COMPOUND;
			continue;
		}
		dbg("sv4l2: control %s id %d(%#d)", qctrl.name, qctrl.id, qctrl.id);
		if (cb)
		{
			struct v4l2_query_ext_ctrl qectrl = {0};
			qectrl.id = qctrl.id;
			qectrl.type = qctrl.type;
			memcpy(qectrl.name, qctrl.name, 32);
			qectrl.minimum = qctrl.minimum;
			qectrl.maximum = qctrl.maximum;
			qectrl.step = qctrl.step;
			qectrl.default_value = qctrl.default_value;
			qectrl.flags = qctrl.flags;
			cb(arg, &qectrl);
		}
		qctrl.id |= V4L2_CTRL_FLAG_NEXT_CTRL | V4L2_CTRL_FLAG_NEXT_COMPOUND;
	}
#else
	struct v4l2_query_ext_ctrl qctrl = {0};
	qctrl.id = V4L2_CTRL_FLAG_NEXT_CTRL | V4L2_CTRL_FLAG_NEXT_COMPOUND;
	int ret;
	for (nbctrls = 0; (ret = ioctl(ctrlfd, VIDIOC_QUERY_EXT_CTRL, &qctrl)) == 0; nbctrls++)
	{
		if (qctrl.flags & V4L2_CTRL_FLAG_DISABLED)
		{
			dbg("sv4l2: control %s %d(%#x) disabled", qctrl.name, qctrl.id, qctrl.id);
			qctrl.id |= V4L2_CTRL_FLAG_NEXT_CTRL | V4L2_CTRL_FLAG_NEXT_COMPOUND;
			continue;
		}
		dbg("sv4l2: control %s id %d(%#x)", qctrl.name, qctrl.id, qctrl.id);
		if (cb)
			cb(arg, &qctrl);
		qctrl.id |= V4L2_CTRL_FLAG_NEXT_CTRL | V4L2_CTRL_FLAG_NEXT_COMPOUND;
	}
#endif
	if (ret && errno != EINVAL)
	{
		nbctrls = ret;
	}
	return nbctrls;
}

int sv4l2_treecontrols(V4L2_t *dev, int (*cb)(void *arg, struct v4l2_query_ext_ctrl *ctrl), void * arg)
{
	int ret;
	ret = _sv4l2_treecontrols(dev->fd, cb, arg);
	return ret;
}

int _sv4l2_treecontrolmenu(int ctrlfd, struct v4l2_query_ext_ctrl *ctrl, int (*cb)(void *arg, struct v4l2_querymenu *ctrl), void * arg)
{
	struct v4l2_querymenu querymenu = {0};
	querymenu.id = ctrl->id;
	for (querymenu.index = ctrl->minimum; querymenu.index <= ctrl->maximum; querymenu.index++ )
	{
		if (ioctl(ctrlfd, VIDIOC_QUERYMENU, &querymenu) != 0)
			return -1;
		if (cb)
			cb(arg, &querymenu);
	}
	return ctrl->maximum - ctrl->minimum;
}

int sv4l2_treecontrolmenu(V4L2_t *dev, struct v4l2_query_ext_ctrl *ctrl, int (*cb)(void *arg, struct v4l2_querymenu *ctrl), void * arg)
{
	return _sv4l2_treecontrolmenu(sv4l2_fd(dev, 0), ctrl, cb, arg);
}

static uint32_t _sv4l2_getfourcc(int fd, enum v4l2_buf_type type, uint32_t fourcc)
{
	int ret = -1;
	struct v4l2_fmtdesc fmtdesc = {0};
	fmtdesc.type = type;
	while (ioctl(fd, VIDIOC_ENUM_FMT, &fmtdesc) == 0)
	{
		if (fmtdesc.pixelformat == fourcc)
		{
			ret = 0;
			break;
		}
		fmtdesc.index++;
	}
	if (!ret)
		return fourcc;

	switch (fourcc)
	{
		case FOURCC_XB24:
			fourcc = _sv4l2_getfourcc(fd, type, FOURCC_AB24);
		break;
		case FOURCC_XR24:
		case FOURCC_AR24:
			fourcc = _sv4l2_getfourcc(fd, type, FOURCC_BGR4);
		break;
	}
	return fourcc;
}

static int _sv4l2_prepare(int fd, enum v4l2_buf_type *type, int mode, V4l2Config_t *config)
{
	*type = _v4l2_getbuftype(*type, mode);

	uint32_t fourcc = 0;
	uint64_t modifiers = 0;
	if (config)
	{
		fourcc = _sv4l2_getfourcc(fd, *type, config->parent.fourcc);
		modifiers = config->parent.modifiers;
	}
	if (_v4l2_setpixformat(fd, *type, fourcc, modifiers) == -1)
	{
		if (errno != EBUSY)
			return -1;
	}

	uint32_t width = 0;
	uint32_t height = 0;
	if (config)
	{
		width = config->parent.width;
		height = config->parent.height;
	}
	if (!(mode & MODE_META) &&
		_v4l2_setframesize(fd, *type, &width, &height) == (uint32_t)-1)
	{
		err("sv4l2: frame size error %m");
		if (errno != EBUSY)
			return -1;
	}

	int fps = -1;
	if (config)
		fps = config->parent.fps;
	if (_v4l2_setfps_param(fd, *type, fps) == -1)
		_v4l2_setfps_vblank(fd, width, height, fps);
	return 0;
}

V4L2_t *sv4l2_create2(int fd, const char *name, device_type_e dtype, V4l2Config_t *config)
{
	enum v4l2_buf_type type = 0;
	int mode = 0;
	if (config)
		mode = config->mode;
	char devicename[32];
	memcpy(devicename, name, sizeof(devicename));
	if (_v4l2_devicecapabilities(fd, devicename, &mode, dtype))
	{
		return NULL;
	}

	if (dtype != device_control && _sv4l2_prepare(fd, &type, mode, config))
	{
		err("sv4l2: create device %s failed", name);
		return NULL;
	}
	else
		type = _v4l2_getbuftype(type, mode);

	V4L2_t *dev = calloc(1, sizeof(*dev));
	dev->name = name;
	strncpy(dev->devicename, devicename, sizeof(dev->devicename));
	dev->config = config;
	dev->fd = fd;
	dev->type = type;
	dev->mode = mode;
	dev->ops.createbuffers = createbuffers_splane;
#if ADD_SUBDEVICES
	/// the subdevices must be intialized, even if they are not used after
	for (int i = 0; config && i < (sizeof(dev->subdevs) / sizeof(*(dev->subdevs))) &&
			i < (sizeof(config->subdev_entries) / sizeof(*(config->subdev_entries))); i++)
	{
		if (config->subdev_entries[i])
		{
			dev->subdevs[i] = subdev_ops.create(config->subdev_entries[i]->parent.name, dtype, (DeviceConf_t *)config->subdev_entries[i]);
		}
	}
#endif
	if (mode & MODE_MPLANE)
	{
		dev->ops.createbuffers = createbuffers_mplane;
	}
	sv4l2_getpixformat(dev, NULL, NULL);
	warn("sv4l2: create %s(%s), %s %ux%u %.4s", name, devicename, config?config->device:"",
				dev->width, dev->height, (char*)&dev->fourcc);

	return dev;
}

V4L2_t *sv4l2_create(const char *devicename, device_type_e type, V4l2Config_t *config)
{
	const char *device = devicename;
	if (config && config->device)
		device = config->device;
	int fd = open(device, O_RDWR | O_NONBLOCK, 0);
	if (fd < 0)
	{
		dbg("sv4l2: try device %s %d", device, fd);
		return NULL;
	}

	dbg("sv4l2: try device %s", device);
	V4L2_t *dev = sv4l2_create2(fd, devicename, type, config);
	if (dev == NULL)
	{
		close(fd);
		return NULL;
	}
	config->parent.dev = dev;
	config->parent.width = dev->width;
	config->parent.height = dev->height;
	config->parent.fourcc = dev->fourcc;
	config->parent.stride = dev->stride;
	if (!(dev->mode & MODE_OUTPUT) && dev->config->periodic)
	{
		dev->periodicfunc = _v4l2_periodiccontrol;
	}
	sv4l2_fps(dev, config->parent.fps);

	return dev;
}

V4L2_t *sv4l2_duplicate(V4L2_t *dev, V4l2Config_t **pconfig)
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
	dup->mode &= ~MODE_OUTPUT;
	dup->type = -1;
	*pconfig = dup->config = malloc(sizeof(*dev->config));
	memmove(dup->config, dev->config, sizeof(*dev->config));
	if (!dev->config->transfer.width)
		dup->config->parent.width = dev->config->parent.width;
	if (!dev->config->transfer.height)
		dup->config->parent.height = dev->config->parent.height;
	if (!dev->config->transfer.fourcc)
		dup->config->parent.fourcc = dev->config->parent.fourcc;
	if ((dup->mode & MODE_CAPTURE) && dup->config->periodic)
	{
		dup->periodicfunc = _v4l2_periodiccontrol;
	}

	sv4l2_fps(dup, dup->config->parent.fps);
	if (_sv4l2_prepare(dup->fd, &dup->type, dup->mode, dup->config))
	{
		close(dup->fd);
		return NULL;
	}

	sv4l2_getpixformat(dup, NULL, NULL);
	warn("sv4l2: %s output  %dx%d, %.4s", dup->name, dup->width, dup->height, (char*)&dup->fourcc);

	return dup;
}

int sv4l2_fd(V4L2_t *dev, int writer)
{
	if (writer)
		return -1;
	return dev->fd;
}

int sv4l2_type(V4L2_t *dev)
{
	return dev->type;
}

int sv4l2_start(V4L2_t *dev)
{
	enum v4l2_buf_type type = dev->type;
	if (dev->type == V4L2_BUF_TYPE_VIDEO_CAPTURE ||
		dev->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE ||
		dev->type == V4L2_BUF_TYPE_META_CAPTURE)
	{
		dbg("sv4l2: %s start buffers enqueuing", dev->name);
		for (int i = 0; i < dev->nbuffers; i++)
		{
			if (sv4l2_queue(dev, i, NULL, 0, 0))
				return -1;
		}
	}
	if (ioctl(dev->fd, VIDIOC_STREAMON, &type) != 0)
	{
		err("sv4l2: %s(%s) starting error (%d)%m", dev->name, dev->mode & MODE_OUTPUT?"output":"capture", errno);
		return -1;
	}
	dbg("sv4l2: %s starting", dev->name);
	return 0;
}

int sv4l2_stop(V4L2_t *dev)
{
	enum v4l2_buf_type type = dev->type;
	if (ioctl(dev->fd, VIDIOC_STREAMOFF, &type) != 0)
		return -1;
	return 0;
}

int sv4l2_dequeue(V4L2_t *dev, void **mem, size_t *bytesused, int *flags)
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
#if V4L2_DEQUEUE_NONBLOCKED
		if (errno == EAGAIN && dev->config->parent.fps != 0)
		{
			/**
			 * with this waiting the stream seems faster
			 */
			useconds_t usec = -dev->config->parent.fps * 1000000;
			if (dev->config->parent.fps > 0)
				usec = 1000000 / dev->config->parent.fps;
			usec /= V4L2_TRYRATIO; /// we don't want to be late.
			usleep(usec);
		}
#endif
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
		*mem = dev->buffers[buf.index].map[0];
	if (flags && (buf.flags & V4L2_BUF_FLAG_KEYFRAME))
		*flags |= FB_FLAGS_KEYFRAME;
	return buf.index;
}

int sv4l2_queue(V4L2_t *dev, int index, void *mem, size_t bytesused, int flags)
{
	int ret = 0;
	if (flags & FB_FLAGS_MODIFIER && !dev->config->parent.modifiers)
		err("sv4l2: input format required not supported modifier");
	if (bytesused > 0)
		dev->buffers[index].v4l2.bytesused = bytesused;
	if (mem && dev->buffers[0].v4l2.memory == V4L2_MEMORY_USERPTR)
		dev->buffers[index].ops.setmem(&dev->buffers[index], 0, mem, bytesused);
	if (dev->config->periodic && dev->periodicfunc &&
		dev->periodic == dev->config->periodic)
	{
		dev->periodic = 0;
		dev->periodicfunc(dev, index);
	}
	else
		dev->periodic ++;
	/**
	 * the management of flags is currently unclear
	 * it should be used to pass the format modifiers (see dma_buf kernel documentation)
	 * but v4l2 driver doesn't support it.
	 */
	// dev->buffers[index].v4l2.flags = flags;
	ret = ioctl(dev->fd, VIDIOC_QBUF, &dev->buffers[index].v4l2);
	if (ret && errno != EAGAIN)
	{
		dbg("sv4l2: %s(%s[%d]) queueing error %m", dev->name, (dev->mode & MODE_OUTPUT)?"output":"capture", index);
		dbg_buffer((&dev->buffers[index].v4l2));
	}
	return ret;
}

void sv4l2_destroy(V4L2_t *dev)
{
	for (int i = 0; i < dev->nbuffers; i++)
	{
		for (int j = 0; j < dev->nplanes; j++)
			if (dev->buffers[i].map[j])
				munmap(dev->buffers[i].map[j], dev->buffers[i].length);
	}
	if (dev->arraybuffers)
		free(dev->arraybuffers);
#if ADD_SUBDEVICES
	for (int i = 0; i < (sizeof(dev->subdevs) / sizeof(*(dev->subdevs))); i++)
	{
		if (dev->subdevs[i])
			subdev_ops.destroy(dev->subdevs[i]);
	}
#endif
	free(dev->buffers);
	if (dev->config)
		free(dev->config);
	close(dev->fd);
	free(dev);
}

DeviceConf_t * sv4l2_createconfig(const char *name)
{
	V4l2Config_t *devconfig = NULL;
	devconfig = calloc(1, sizeof(V4l2Config_t));
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

static json_t *_sv4l2_getjsonvalue(json_t *jconfig, const char *name, int id)
{
	json_t *jvalue = NULL;
	if (json_is_object(jconfig))
	{
		/**
		 * json format:
		 * {"Gain":1000,"Exposure":1}
		 */
		jvalue = json_object_get(jconfig, name);
		json_object_del(jconfig, name);
	}
	else if (json_is_array(jconfig))
	{
		/**
		 * json format:
		 * [ {"name":"Gain","value":1000},{"id":9963790,"value":1}]
		 */
		int index = 0;
		json_t *jcontrol = NULL;
		json_array_foreach(jconfig, index, jcontrol)
		{
			if (json_is_object(jcontrol))
			{
				json_t *jname = json_object_get(jcontrol, "name");
				if (jname && json_is_string(jname) &&
					!strcmp(json_string_value(jname), name))
				{
					jvalue = json_object_get(jcontrol, "value");
					break;
				}
				json_t *jid = json_object_get(jcontrol, "id");
				if (jid && json_is_integer(jid) &&
					json_integer_value(jid) == id)
				{
					jvalue = json_object_get(jcontrol, "value");
					break;
				}
				json_t *jitems = json_object_get(jcontrol, "items");
				if (jitems && json_is_array(jitems))
				{
					jvalue = _sv4l2_getjsonvalue(jitems, name, id);
					if (jvalue)
						break;
				}
			}
		}
		if (jvalue)
			json_array_remove(jconfig, index);
	}
	return jvalue;
}

static int _sv4l2_loadjsonsetting(void *arg, struct v4l2_query_ext_ctrl *ctrl)
{
	_SV4L2_Setting_t *setting = (_SV4L2_Setting_t *)arg;
	json_t *jconfig = setting->jconfig;
	V4L2_t *dev = setting->dev;
	json_t *jvalue = _sv4l2_getjsonvalue(jconfig, ctrl->name, ctrl->id);
	if (jvalue == NULL)
		return 0;
	if (ctrl->type == V4L2_CTRL_TYPE_INTEGER && json_is_integer(jvalue))
	{
		int value = json_integer_value(jvalue);
		value = (long)sv4l2_control(dev, ctrl->id, (void*)(long)value);
		if (value != -1)
			warn("%s => %d", ctrl->name, value);
		return value;
	}
	else if (ctrl->type == V4L2_CTRL_TYPE_BOOLEAN && json_is_boolean(jvalue))
	{
		int value = (long)sv4l2_control(dev, ctrl->id, (void*)(long)json_is_true(jvalue));
		if (value != -1)
			warn("%s => %s", ctrl->name, value?"on":"off");
		return value;
	}
	else if (ctrl->type == V4L2_CTRL_TYPE_MENU && json_is_integer(jvalue))
	{
		int value = json_integer_value(jvalue);
		value = (long)sv4l2_control(dev, ctrl->id, (void*)(long)value);
		if (value != -1)
			warn("%s => %d", ctrl->name, value);
		return value;
	}
	else if (ctrl->type == V4L2_CTRL_TYPE_BUTTON)
	{
		int value = (long)sv4l2_control(dev, ctrl->id, NULL);
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
			if (!strcmp((const char *)querymenu.name, value))
			{
				if (sv4l2_control(dev, ctrl->id, (void*)(long)querymenu.index) != (void *)-1)
					warn("%s => %s", ctrl->name, value);
				return (long)value;
			}
		}
	}
	else if (ctrl->type == V4L2_CTRL_TYPE_STRING && json_is_string(jvalue))
	{
		const char *value = json_string_value(jvalue);
		value = sv4l2_control(dev, ctrl->id, (void*)value);
		if (value != (void *)(long)-1)
			warn("%s => %s", ctrl->name, value);
		return (long)value;
	}
	else if (ctrl->type == V4L2_CTRL_TYPE_U8 && json_is_array(jvalue))
	{
		uint8_t *u8 = calloc(ctrl->elems, sizeof(uint8_t));
		int index;
		json_t *ju8;
		json_array_foreach(jvalue, index, ju8)
		{
			if (index == ctrl->elems)
				break;
			u8[index] = (uint8_t)json_integer_value(ju8);
		}
		void *value = sv4l2_control(dev, ctrl->id, (void*)u8);
		if (value != (void *)(long)-1)
			warn("%s => array", ctrl->name);
		free(u8);
		return (long)value;
	}
	else if (ctrl->type == V4L2_CTRL_TYPE_U16 && json_is_array(jvalue))
	{
		uint16_t *u16 = calloc(ctrl->elems, sizeof(uint16_t));
		int index;
		json_t *ju16;
		json_array_foreach(jvalue, index, ju16)
		{
			if (index == ctrl->elems)
				break;
			u16[index] = (uint16_t)json_integer_value(ju16);
		}
		void *value = sv4l2_control(dev, ctrl->id, (void*)u16);
		if (value != (void *)(long)-1)
			warn("%s => array", ctrl->name);
		free(u16);
		return (long)value;
	}
	else if (ctrl->type == V4L2_CTRL_TYPE_U32 && json_is_array(jvalue))
	{
		uint32_t *u32 = calloc(ctrl->elems, sizeof(uint32_t));
		int index;
		json_t *ju32;
		json_array_foreach(jvalue, index, ju32)
		{
			if (index == ctrl->elems)
				break;
			u32[index] = (uint32_t)json_integer_value(ju32);
		}
		void *value = sv4l2_control(dev, ctrl->id, (void*)u32);
		if (value != (void *)(long)-1)
			warn("%s => array", ctrl->name);
		free(u32);
		return (long)value;
	}
	else
	{
		int value = json_integer_value(jvalue);
		value = (long)sv4l2_control(dev, ctrl->id, (void*)(long)value);
		if (value != -1)
			warn("%s (%#x) => %d", ctrl->name, ctrl->type, value);
		else
			warn("%s (%#x) not supported", ctrl->name, ctrl->type);
		return value;
	}
	return -1;
}

static int _v4l2_loadjsontransformation(V4L2_t *dev, json_t *transformation)
{
	int disable = 0;
	json_t *type = json_object_get(transformation, "name");
	json_t *top = NULL;
	json_t *left = NULL;
	json_t *width = NULL;
	json_t *height = NULL;
	json_t *bounds = json_object_get(transformation, "bounds");
	if (bounds && json_is_array(bounds))
	{
		top = json_array_get(bounds, 0);
		left = json_array_get(bounds, 1);
		width = json_array_get(bounds, 2);
		height = json_array_get(bounds, 3);
	}
	if (bounds == NULL || !json_is_object(bounds))
		bounds = transformation;
	if (width == NULL && json_is_object(bounds))
	{
		top = json_object_get(bounds, "top");
		left = json_object_get(bounds, "left");
		width = json_object_get(bounds, "width");
		height = json_object_get(bounds, "height");
	}
	if (width == NULL)
		return -1;
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
	{
		sv4l2_crop(dev, NULL);
		sv4l2_compose(dev, NULL);
	}
	else if (type && !strcmp("compose", json_string_value(type)))
	{
		sv4l2_compose(dev, &r);
	}
	else
	{
		sv4l2_crop(dev, &r);
	}
	return 0;
}

/**
 * @brief callback for sv4l2_treecontrols.
 * it fills a json_t object with controls information
 *
 * @param arg a pointer on json_object of jansson library.
 * @param ctrl the control cf the standard v4l2 dpcumentation.
 *
 * @return -1 on error, 0 otherwise.
 */
static int _v4l2_loadjsoncontrol(V4L2_t *dev, json_t *control)
{
	json_t *jdisable = json_object_get(control, "disable");
	if (json_is_true(jdisable))
		return 1;
	json_t *jrdonly = json_object_get(control, "read-only");
	if (json_is_true(jrdonly))
		return 1;
	json_t *jid = json_object_get(control, "id");
	if (!jid || !json_is_integer(jid))
		return -1;
	int ret = 0;
	json_t *jvalue = json_object_get(control, "value");
	if (jvalue && json_is_number(jvalue))
	{
		double value = json_number_value(jvalue);
		if (sv4l2_control(dev, json_integer_value(jid), (void*)(long)value) != (void*)(long)-1)
		{
			ret = 1;
		}
	}
	else if (jvalue && json_is_integer(jvalue))
	{
		int32_t value = json_integer_value(jvalue);
		if (sv4l2_control(dev, json_integer_value(jid), (void*)(long)value) != (void*)(long)-1)
		{
			ret = 1;
		}
	}
	else if (jvalue && json_is_string(jvalue))
	{
		const char *value = json_string_value(jvalue);
		if (sv4l2_control(dev, json_integer_value(jid), (void*)value) != (void*)(long)-1)
		{
			ret = 1;
		}
	}
	else if (jvalue && json_is_boolean(jvalue))
	{
		int value = json_is_true(jvalue);
		if (sv4l2_control(dev, json_integer_value(jid), (void*)(long)value) != (void*)(long)-1)
		{
			ret = 1;
		}
	}
	else if (jvalue && json_is_array(jvalue) && json_array_size(jvalue) < 1024)
	{
		char value[1024];
		int index = 0;
		json_t *jentry = NULL;
		json_array_foreach(jvalue, index, jentry)
		{
			if (!json_is_integer(jentry))
				break;
			value[index] = json_integer_value(jentry) & 0xFF;
		}

		if (index && sv4l2_control(dev, json_integer_value(jid), (void*)value) != (void*)(long)-1)
		{
			ret = 1;
		}
	}
	return ret;
}

static int _v4l2_loadjsoncontrols(V4L2_t *dev, json_t *controls)
{
	if (json_is_array(controls))
	{
		int ncontrols = json_array_size(controls);
		for (int index = 0; index < json_array_size(controls); index++)
		{
			json_t *control = json_array_get(controls, index);
			if (_v4l2_loadjsoncontrol(dev, control) >= 0)
			{
				// remove each control designed by an ID
				json_array_remove(controls, index);
				ncontrols--;
				// roolback the list because the number of elements changed
				index--;
			}
		}
	}
	if (json_array_size(controls) == 0)
		return 0;
	_SV4L2_Setting_t setting;
	setting.dev = dev;
	setting.jconfig = controls;
	return sv4l2_treecontrols(dev, _sv4l2_loadjsonsetting, &setting);
}

int sv4l2_loadjsonsettings(V4L2_t *dev, void *entry)
{
	json_t *jconfig = entry;

	json_t *jname = json_object_get(jconfig, "name");
	if (jname && json_is_array(jname))
	{
		int index = 0;
		json_t *jentry = NULL;
		json_array_foreach(jname, index, jentry)
		{
			if (dev->config && config_isnamed(&dev->config->parent, json_string_value(jentry)))
			{
				jname = jentry;
				break;
			}
		}
	}
	if (jname && json_is_string(jname) &&
			!config_isnamed(&dev->config->parent, json_string_value(jname)))
	{
		return -1;
	}

	json_t *transformations = json_object_get(jconfig, "transformation");
	if (transformations && json_is_array(transformations))
	{
		int index;
		json_t *transformation;
		json_array_foreach(transformations, index, transformation)
		{
			_v4l2_loadjsontransformation(dev, transformation);
		}
	}
	else if (transformations && json_is_object(transformations))
	{
		_v4l2_loadjsontransformation(dev, transformations);
	}

	json_t *disable = json_object_get(jconfig, "disable");
	if (json_is_true(disable))
		return -1;

	json_t *jcontrols = json_object_get(jconfig,"controls");
	if (jcontrols && (json_is_array(jcontrols) || json_is_object(jcontrols)))
	{
		jconfig = jcontrols;
	}
	return _v4l2_loadjsoncontrols(dev, jconfig);
}

static int _v4l2_parsedefinition(json_t *definition, V4l2Config_t *config)
{
	int ret = -1;
	ret = scommon_loaddefinition(&config->parent, definition);

	json_t *fps = NULL;
	json_t *mode = NULL;
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
		mode = json_object_get(definition, "mode");
	}
	if (mode && json_is_string(mode))
	{
		const char *value = json_string_value(mode);
		if (value && strstr(value,"capture"))
			config->mode |= MODE_CAPTURE;
		if (value && strstr(value,"output"))
			config->mode |= MODE_OUTPUT;
#ifdef V4L2_HAS_META
		if (value && strstr(value,"meta"))
			config->mode |= MODE_META;
#endif
	}
	if (mode && json_is_array(mode))
	{
		json_t *field = NULL;
		int index = 0;
		json_array_foreach(mode, index, field)
		{
			const char *value = json_string_value(field);
			if (value == NULL)
				continue;

			if (!strncmp(value,"capture", 7))
				config->mode |= MODE_CAPTURE;
			if (!strncmp(value,"output", 6))
				config->mode |= MODE_OUTPUT;
#ifdef V4L2_HAS_META
			if (!strncmp(value,"meta", 4))
				config->mode |= MODE_META;
#endif
		}
	}
	return ret;
}

#if ADD_SUBDEVICES
/**
 * the subdevices should be useless for video
 * It is enought to manage the subdevices independently for the controls
 */
int _v4l2_addsubdevices(V4l2Config_t *config, json_t *subdevices, const char *name)
{
	int subdev_id = 0;
	if (subdevices && json_is_array(subdevices))
	{
		size_t namelen = 0;
		const char* options = strchr(name, ':');
		if (options)
			namelen = options - name;
		json_t *subdevice = NULL;
		int index = 0;
		json_t *jlastname = NULL;
		json_array_foreach(subdevices, index, subdevice)
		{
			if (json_is_object(subdevice))
			{
				json_t *jname = json_object_get(subdevice, "name");
				if (jname && json_is_array(jname))
				{
					jlastname = json_array_get(jname, json_array_size(jname) - 1);
					json_t *it = NULL;
					int i = 0;
					json_array_foreach(jname, i, it)
					{
						if (json_is_string(it) &&
							((namelen && !strncmp(json_string_value(it), name, namelen)) ||
							!strcmp(json_string_value(it), name)))
						{
							jname = it;
							break;
						}
					}
					if (json_is_array(jname))
						warn("sv4l2: %s entry not found into %s", name, json_string_value(json_array_get(jname,0)));
				}
				if (jname && json_is_string(jname) &&
					((namelen && !strncmp(json_string_value(jname), name, namelen)) ||
					!strcmp(json_string_value(jname), name)) &&
					json_is_object(subdevice))
				{
					json_t *jdisable = json_object_get(subdevice, "disable");
					if (json_is_true(jdisable))
					{
						warn("sv4l2: subdev %s is disabled", name);
						continue;
					}
					json_t *definition = json_object_get(subdevice, "definition");
					_v4l2_parsedefinition(definition, config);
					if (subdev_id >= (sizeof(config->subdev_entries) / sizeof(*config->subdev_entries)))
						break;

					config->subdev_entries[subdev_id] = (V4l2Config_t *)subdev_ops.createconfig(name);
					memcpy(&config->subdev_entries[subdev_id]->parent, &config->parent, sizeof(config->parent));
					config->subdev_entries[subdev_id]->parent.entry = subdevice;
					config->subdev_entries[subdev_id]->parent.ops.loadconfiguration(config->subdev_entries[subdev_id], subdevice);
					if (jlastname && json_is_string(jlastname))
						config->subdev_entries[subdev_id]->parent.name = json_string_value(jlastname);
					subdev_id++;
				}
			}
		}
	}
	return 0;
}
#endif

static int _v4l2_addaction(V4l2Config_t *config, json_t *action)
{
	if (action && json_is_object(action))
	{
		json_t *periodic = json_object_get(action, "periodic");
		if (periodic && json_is_integer(periodic))
		{
			config->periodic = json_integer_value(periodic);
		}
		json_t *periodiccontrol = json_object_get(action, "periodiccontrol");
		if (periodiccontrol && json_is_integer(periodiccontrol))
		{
			config->periodiccontrol = json_integer_value(periodiccontrol);
		}
	}
	return 0;
}

int sv4l2_loadjsonconfiguration(void *arg, void *entry)
{
	json_t *jconfig = entry;

	V4l2Config_t *config = (V4l2Config_t *)arg;
	json_t *device = json_object_get(jconfig, "device");
	if (device && json_is_string(device))
	{
		const char *value = json_string_value(device);
		config->device = value;
	}
	json_t *definition = json_object_get(jconfig, "definition");
	_v4l2_parsedefinition(definition, config);

	json_t *transfer = json_object_get(jconfig, "transfer");
	scommon_loaddefinition(&config->transfer, transfer);
	if (config->transfer.width == 0)
		config->transfer.width = config->parent.width;
	if (config->transfer.height == 0)
		config->transfer.height = config->parent.height;
	if (config->transfer.fourcc == 0)
		config->transfer.fourcc = config->parent.fourcc;
	if (config->transfer.fps == 0)
		config->transfer.fps = config->parent.fps;
	if (config->transfer.modifiers == 0)
		config->transfer.modifiers = config->parent.modifiers;

#if ADD_SUBDEVICES
	json_t *subdevice = json_object_get(jconfig, "subdevices");
	_v4l2_addsubdevices(config, subdevice, config->parent.name);
#endif

	json_t *action = json_object_get(jconfig, "action");
	_v4l2_addaction(config, action);

	return 0;
}

const char *sv4l2_CTRLTYPE(enum v4l2_ctrl_type type)
{
	switch (type)
	{
	case V4L2_CTRL_TYPE_INTEGER:
		return "integer";
	case V4L2_CTRL_TYPE_BOOLEAN:
		return "boolean";
	case V4L2_CTRL_TYPE_MENU:
		return "menu";
	case V4L2_CTRL_TYPE_INTEGER_MENU:
		return "menu";
	case V4L2_CTRL_TYPE_BUTTON:
		return "button";
	case V4L2_CTRL_TYPE_INTEGER64:
		return "large integer";
	case V4L2_CTRL_TYPE_STRING:
		return "string";
	case V4L2_CTRL_TYPE_U8:
		return "u8array";
	case V4L2_CTRL_TYPE_U16:
		return "u16array";
	case V4L2_CTRL_TYPE_U32:
		return "u32array";
	case V4L2_CTRL_TYPE_CTRL_CLASS:
		return "control class";
	default:
		return "unknown";
	}
	dbg("sv4l2: control type %#x not supported", type);
	return "unknown";
}

const char *sv4l2_CTRLNAME(uint32_t id)
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
		json_object_set_new(control, "value_show", json_string((const char *)querymenu->name));
	return 0;
}

static int menuprintall(void *arg, struct v4l2_querymenu *querymenu)
{
	json_t *control = (json_t *)arg;
	json_t *items = json_object_get(control, "items");
	json_array_append_new(items, json_string((const char *)querymenu->name));
	return menuprint(arg, querymenu);
}

static int intmenuprint(void *arg, struct v4l2_querymenu *querymenu)
{
	json_t *control = (json_t *)arg;
	json_t *value = json_object_get(control, "value");
	if (value && querymenu->index == json_integer_value(value))
		json_object_set_new(control, "value_show", json_integer(querymenu->value));
	return 0;
}

static int intmenuprintall(void *arg, struct v4l2_querymenu *querymenu)
{
	json_t *control = (json_t *)arg;
	json_t *items = json_object_get(control, "items");
	json_array_append_new(items, json_integer(querymenu->value));
	return intmenuprint(arg, querymenu);
}

typedef struct _JSONControl_Arg_s _JSONControl_Arg_t;
struct _JSONControl_Arg_s
{
	json_t *controls;
	int ctrlfd;
	int all;
};

int sv4l2_jsoncontrol_cb(void *arg, struct v4l2_query_ext_ctrl *ctrl)
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
					!strcmp(json_string_value(classtype), sv4l2_CTRLTYPE(V4L2_CTRL_TYPE_CTRL_CLASS)))
				{
					ctrlclass = json_object_get(control, "items");
				}
			}
		}
	}
	void *value = _sv4l2_control(ctrlfd, ctrl->id, (void*)-1, ctrl);
	if (value == (void*)(long)-1)
		return -1;
	json_t *control = json_object();
	json_object_set_new(control, "name", json_string(ctrl->name));
	json_object_set_new(control, "id", json_integer(ctrl->id));
	if (jsoncontrol_arg->all)
	{
		json_t *type = json_string(sv4l2_CTRLTYPE(ctrl->type));
		json_object_set_new(control, "type", type);
		if (ctrl->flags & V4L2_CTRL_FLAG_READ_ONLY)
			json_object_set_new(control, "read-only", json_true());
	}

	switch (ctrl->type)
	{
	case V4L2_CTRL_TYPE_INTEGER:
	{
		json_object_set_new(control, "value", json_integer((long) value));
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
		json_object_set_new(control, "value", json_boolean((long) value));
		if (jsoncontrol_arg->all)
		{
			json_object_set_new(control, "default_value", json_integer(ctrl->default_value));
		}
	break;
	case V4L2_CTRL_TYPE_MENU:
	{
		json_t *jvalue = json_integer((long) value);
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
	case V4L2_CTRL_TYPE_INTEGER_MENU:
	{
		json_t *jvalue = json_integer((long) value);
		json_object_set(control, "value", jvalue);
		if (jsoncontrol_arg->all)
		{
			json_t *items = json_array();
			json_object_set(control, "items", items);
			_sv4l2_treecontrolmenu(ctrlfd, ctrl, intmenuprintall, control);
			json_decref(items);
			json_object_set_new(control, "default_value", json_integer(ctrl->default_value));
		}
		else
			_sv4l2_treecontrolmenu(ctrlfd, ctrl, intmenuprint, control);
		json_decref(jvalue);
	}
	break;
	case V4L2_CTRL_TYPE_BUTTON:
	break;
	case V4L2_CTRL_TYPE_INTEGER64:
		json_object_set_new(control, "value", json_integer((long) value));
		if (jsoncontrol_arg->all)
		{
			json_object_set_new(control, "default_value", json_integer(ctrl->default_value));
		}
	break;
	case V4L2_CTRL_TYPE_STRING:
		json_object_set_new(control, "value", json_string(value));
	break;
	case V4L2_CTRL_TYPE_U8:
	{
		json_t *jvalue = json_array();
		for (int i = 0; i < ctrl->elems; i++)
		{
			json_array_append_new(jvalue, json_integer(((uint8_t*)value)[i]));
		}
		json_object_set_new(control, "value", jvalue);
	}
	break;
	case V4L2_CTRL_TYPE_U16:
	{
		json_t *jvalue = json_array();
		for (int i = 0; i < ctrl->elems; i++)
		{
			json_array_append_new(jvalue, json_integer(((uint16_t*)value)[i]));
		}
		json_object_set_new(control, "value", jvalue);
	}
	break;
	case V4L2_CTRL_TYPE_U32:
	{
		json_t *jvalue = json_array();
		for (int i = 0; i < ctrl->elems; i++)
		{
			json_array_append_new(jvalue, json_integer(((uint32_t*)value)[i]));
		}
		json_object_set_new(control, "value", jvalue);
	}
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

static int _v4l2_capabilities_transform(V4L2_t *dev, json_t *transformations, int all, int target)
{
	json_t *top = json_object();
	json_object_set_new(top, "name", json_string("top"));
	json_t *left = json_object();
	json_object_set_new(left, "name", json_string("left"));
	json_t *width = json_object();
	json_object_set_new(width, "name", json_string("width"));
	json_t *height = json_object();
	json_object_set_new(height, "name", json_string("height"));
	struct v4l2_selection sel = {0};
	if (all)
	{
		sel.type = sv4l2_type(dev);
		sel.target = target | 0x02; // <target>_BOUNDS
		if (ioctl(sv4l2_fd(dev, 0), VIDIOC_G_SELECTION, &sel) == 0)
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
			json_decref(top);
			json_decref(left);
			json_decref(width);
			json_decref(height);
			return -1;
		}
		memset(&sel, 0, sizeof(sel));
		sel.type = sv4l2_type(dev);
		sel.target = target | 0x01; // <target>_DEFAULT
		if (ioctl(sv4l2_fd(dev, 0), VIDIOC_G_SELECTION, &sel) == 0)
		{
			json_object_set_new(top, "value_default", json_integer(sel.r.top));
			json_object_set_new(left, "value_default", json_integer(sel.r.left));
			json_object_set_new(width, "value_default", json_integer(sel.r.width));
			json_object_set_new(height, "value_default", json_integer(sel.r.height));
		}
	}
	sel.type = sv4l2_type(dev);
	sel.target = target;
	if (ioctl(sv4l2_fd(dev, 0), VIDIOC_G_SELECTION, &sel) == 0)
	{
		json_object_set_new(top, "value", json_integer(sel.r.top));
		json_object_set_new(left, "value", json_integer(sel.r.left));
		json_object_set_new(width, "value", json_integer(sel.r.width));
		json_object_set_new(height, "value", json_integer(sel.r.height));
	}
	else if (!all)
	{
		json_decref(top);
		json_decref(left);
		json_decref(width);
		json_decref(height);
		return -1;
	}
	json_t *transformation = json_object();
	if (target == V4L2_SEL_TGT_CROP)
		json_object_set_new(transformation, "name", json_string("crop"));
	if (target == V4L2_SEL_TGT_COMPOSE)
		json_object_set_new(transformation, "name", json_string("compose"));
	if (all)
	{
		json_object_set_new(transformation, "type", json_string("rectangle"));
	}
	json_t *bounds = json_array();
	json_array_append_new(bounds, top);
	json_array_append_new(bounds, left);
	json_array_append_new(bounds, width);
	json_array_append_new(bounds, height);
	json_object_set_new(transformation, "bounds", bounds);
	json_array_append_new(transformations, transformation);
	return 0;
}

static int _v4l2_capabilities_fps(V4L2_t *dev, json_t *definition, int all)
{
	json_t *fps = json_object();
	json_object_set_new(fps, "name", json_string("fps"));
	if (all)
		json_object_set_new(fps, "type", json_string(sv4l2_CTRLTYPE(V4L2_CTRL_TYPE_INTEGER)));
	struct v4l2_streamparm streamparm = {0};
	streamparm.type = sv4l2_type(dev);
	if (ioctl(sv4l2_fd(dev, 0), VIDIOC_G_PARM, &streamparm) == 0)
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

static int _v4l2_capabilities_metaformat(V4L2_t *dev, json_t *definition, int all)
{
	struct v4l2_format fmt = {0};
	fmt.type = sv4l2_type(dev);
	fmt.fmt.pix.field = V4L2_FIELD_ANY;
	json_t *metaformat = NULL;
	json_t *size = NULL;
	if (ioctl(sv4l2_fd(dev, 0), VIDIOC_G_FMT, &fmt) == 0)
	{
		if (all || fmt.fmt.meta.dataformat != 0)
		{
			metaformat = json_object();
			json_object_set_new(metaformat, "name", json_string("fourcc"));
			json_object_set_new(metaformat, "value", json_stringn((char*)&fmt.fmt.meta.dataformat, 4));
		}
		if (all || fmt.fmt.meta.buffersize > 0)
		{
			size = json_object();
			json_object_set_new(size, "name", json_string("size"));
			json_object_set_new(size, "value", json_integer(fmt.fmt.meta.buffersize));
		}
	}

	if (!all)
	{
		if (metaformat)
			json_array_append_new(definition, metaformat);
		if (size)
			json_array_append_new(definition, size);
		return 0;
	}

	json_object_set_new(metaformat, "type", json_string(sv4l2_CTRLTYPE(V4L2_CTRL_TYPE_STRING)));
	json_object_set_new(size, "type", json_string(sv4l2_CTRLTYPE(V4L2_CTRL_TYPE_INTEGER)));
	json_t *items = json_array();
	struct v4l2_fmtdesc fmtdesc = {0};
	fmtdesc.type = sv4l2_type(dev);
	while (ioctl(sv4l2_fd(dev, 0), VIDIOC_ENUM_FMT, &fmtdesc) == 0)
	{
		json_array_append_new(items, json_stringn((char*)&fmtdesc.pixelformat, 4));
		fmtdesc.index++;
	}
	json_object_set_new(metaformat, "items", items);
	json_object_set_new(size, "read-only", json_true());

	json_array_append_new(definition, metaformat);
	json_array_append_new(definition, size);
	return 0;
}

static int _v4l2_capabilities_imageformat(V4L2_t *dev, json_t *definition, int all)
{
	json_t *pixelformat = NULL;
	json_t *width = NULL;
	json_t *height = NULL;

	struct v4l2_format fmt = {0};
	fmt.type = sv4l2_type(dev);
	fmt.fmt.pix.field = V4L2_FIELD_ANY;
	if (ioctl(sv4l2_fd(dev, 0), VIDIOC_G_FMT, &fmt) == 0)
	{
		if (fmt.fmt.pix.pixelformat)
		{
			pixelformat = json_object();
			json_object_set_new(pixelformat, "name", json_string("fourcc"));
			json_t *jvalue = json_stringn((char*)&fmt.fmt.pix.pixelformat, 4);
			json_object_set_new(pixelformat, "value", jvalue);

		}
		if (fmt.fmt.pix.width)
		{
			json_t *width = json_object();
			json_object_set_new(width, "name", json_string("width"));
			json_object_set_new(width, "value", json_integer(fmt.fmt.pix.width));
		}
		if (fmt.fmt.pix.height)
		{
			json_t *height = json_object();
			json_object_set_new(height, "name", json_string("height"));
			json_object_set_new(height, "value", json_integer(fmt.fmt.pix.height));
		}
	}

	if (!all)
	{
		if (pixelformat)
			json_array_append_new(definition, pixelformat);
		if (width)
			json_array_append_new(definition, width);
		if (height)
			json_array_append_new(definition, height);
		return 0;
	}

	json_object_set_new(pixelformat, "type", json_string(sv4l2_CTRLTYPE(V4L2_CTRL_TYPE_STRING)));
	json_object_set_new(width, "type", json_string(sv4l2_CTRLTYPE(V4L2_CTRL_TYPE_INTEGER)));
	json_object_set_new(height, "type", json_string(sv4l2_CTRLTYPE(V4L2_CTRL_TYPE_INTEGER)));
	json_t *items = json_array();
	struct v4l2_fmtdesc fmtdesc = {0};
	fmtdesc.type = sv4l2_type(dev);
	while (ioctl(sv4l2_fd(dev, 0), VIDIOC_ENUM_FMT, &fmtdesc) == 0)
	{
		json_array_append_new(items, json_stringn((char*)&fmtdesc.pixelformat, 4));
		fmtdesc.index++;
	}
	json_object_set_new(pixelformat, "items", items);

	struct v4l2_frmsizeenum video_cap = {0};
	video_cap.pixel_format = fmt.fmt.pix.pixelformat;
	video_cap.type = V4L2_FRMSIZE_TYPE_STEPWISE;
	if(ioctl(sv4l2_fd(dev, 0), VIDIOC_ENUM_FRAMESIZES, &video_cap) == 0)
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

int sv4l2_capabilities_definition(V4L2_t *dev, json_t *definition, int all)
{
	return _v4l2_capabilities_imageformat(dev, definition, all);
}

int sv4l2_capabilities(V4L2_t *dev, json_t *capabilities, int all)
{
	json_t *definition = json_array();
	if (dev->mode & MODE_META)
		_v4l2_capabilities_metaformat(dev, definition, all);
	else
		_v4l2_capabilities_imageformat(dev, definition, all);
	_v4l2_capabilities_fps(dev, definition, all);
	if (json_array_size(definition) > 0)
		json_object_set_new(capabilities, "definition", definition);
	json_t *transformations = json_array();
	_v4l2_capabilities_transform(dev, transformations, all, V4L2_SEL_TGT_CROP);
	_v4l2_capabilities_transform(dev, transformations, all, V4L2_SEL_TGT_COMPOSE);
	if (json_array_size(transformations) > 0)
		json_object_set(capabilities, "transformation", transformations);
	json_decref(transformations);
	_JSONControl_Arg_t arg = {0};
	arg.controls = json_array();
	arg.all = all;
	arg.ctrlfd = sv4l2_fd(dev, 0);
	int ret;
	ret = sv4l2_treecontrols(dev, sv4l2_jsoncontrol_cb, &arg);
	if (ret > 0)
		json_object_set(capabilities, "controls", arg.controls);
	json_decref(arg.controls);
	return 0;
}

#endif

const FastVideoDevice_ops_t sv4l2_ops = {
	.name = "v4l2",
	.createconfig = sv4l2_createconfig,
	.create = (FastVideoDevice_create_t)sv4l2_create,
	.create2 = (FastVideoDevice_create2_t)sv4l2_create2,
	.duplicate = (FastVideoDevice_duplicate_t)sv4l2_duplicate,
	.loadsettings = (FastVideoDevice_loadsettings_t)sv4l2_loadsettings,
	.capabilities = (FastVideoDevice_capabilities_t)sv4l2_capabilities,
	.requestbuffer = (FastVideoDevice_requestbuffer_t)sv4l2_requestbuffer,
	.eventfd = (FastVideoDevice_eventfd_t)sv4l2_fd,
	.start = (FastVideoDevice_start_t)sv4l2_start,
	.stop = (FastVideoDevice_stop_t)sv4l2_stop,
	.dequeue = (FastVideoDevice_dequeue_t)sv4l2_dequeue,
	.queue = (FastVideoDevice_queue_t)sv4l2_queue,
	.destroy = (FastVideoDevice_destroy_t)sv4l2_destroy,
};

#include <dlfcn.h>

static void __attribute__ ((constructor)) sskeleton_init()
{
	fastvideodevice_ops_append_t _fastvideodevice_ops_append;
	void *hdl = dlopen(NULL, RTLD_NOW | RTLD_GLOBAL);
	_fastvideodevice_ops_append = dlsym(hdl, "fastvideodevice_ops_append");
	if (_fastvideodevice_ops_append)
	{
		_fastvideodevice_ops_append(&sv4l2_ops);
	}
}
