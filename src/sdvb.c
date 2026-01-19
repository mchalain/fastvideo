#include <stdlib.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <string.h>
#include <stdarg.h>
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>

#include <linux/dvb/dmx.h>
#ifdef HAVE_JANSSON
#include <jansson.h>
#endif

#include "log.h"
#include "sdvb.h"

#define MAX_BUFFER 4

static const char sdvb_defaultdevice[] = "/dev/dvb/adapter0/demux0";

typedef struct DVB_s DVB_t;
struct DVB_s
{
	int fd;
	uint16_t pid;
	uint32_t framesize;
	FrameBuffer_t buffers[MAX_BUFFER];
	int nbuffers;
};

EXT_API DVB_t *sdvb_create(const char *devicename, device_type_e type, DVBConfig_t *config)
{
	DVB_t *dev = NULL;
	const char *device = devicename;
	if (config && config->device)
		device = config->device;
	int fd = open(device, O_RDWR | O_NONBLOCK, 0);
	if (fd > 0)
		dev = sdvb_create2(fd, devicename, type, config);
	return dev;
}

EXT_API DVB_t *sdvb_create2(int fd, const char *devicename, device_type_e type, DVBConfig_t *config)
{
	uint16_t pids[5] = {0};
	if (ioctl(fd, DMX_GET_PES_PIDS, pids) != 0)
		return NULL;
	struct dmx_pes_filter_params filter = {0};
	filter.pid =  pids[DMX_PES_VIDEO0];
	warn("sdvb: filter pid %#x", filter.pid);

	if (config && config->pid > pids[DMX_PES_VIDEO])
	{
		filter.pid = config->pid;
	}
	filter.input = DMX_IN_FRONTEND;
	//filter.output = DMX_OUT_TAP;
	filter.output = DMX_OUT_TS_TAP;
	//filter.output = DMX_OUT_TSDEMUX_TAP;
	//filter.pes_type = DMX_PES_VIDEO;
	filter.pes_type = DMX_PES_OTHER;
	filter.flags = DMX_IMMEDIATE_START;
	if (ioctl(fd, DMX_SET_PES_FILTER, &filter) != 0)
	{
		err("sdvb: filter pid %#x error %m", filter.pid);
		return NULL;
	}
	DVB_t *dev = calloc(1, sizeof(*dev));
	dev->fd = fd;
	switch (config->type)
	{
	case DVB_T:
		dev->framesize = (188*(4096/188));
	break;
	case DVB_S:
		dev->framesize = (188*(4096/188));
	break;
	case DVB_N:
		dev->framesize = (188*(4096/188));
	break;
	}
	return dev;
}

EXT_API int sdvb_requestbuffer(DVB_t *dev, enum buf_type_e t, ...)
{
	va_list ap;
	va_start(ap, t);

	struct dmx_requestbuffers reqbuffers = {0};
	if (t & buf_type_master)
	{
		reqbuffers.count = MAX_BUFFER;
		reqbuffers.size = dev->framesize;
		if (ioctl(dev->fd, DMX_REQBUFS, &reqbuffers) != 0)
		{
			err("sdvb: request buffers error %m");
			va_end(ap);
			return -1;
		}
		dev->nbuffers = reqbuffers.count;
	}

	switch (t)
	{
		case (buf_type_memory | buf_type_master):
		{
			for (int i = 0; i < dev->nbuffers; i++)
			{
				struct dmx_buffer buffer = {0};
				buffer.index = i;
				if (ioctl(dev->fd, DMX_QUERYBUF, &buffer) != 0)
				{
					va_end(ap);
					return -1;
				}
				dev->buffers[i].size = buffer.length;
				dev->buffers[i].mem = mmap(NULL, buffer.length, PROT_READ | PROT_WRITE, MAP_SHARED, dev->fd, buffer.offset);
			}
			int *ntargets = va_arg(ap, int *);
			void **targets = va_arg(ap, void **);
			size_t *psize = va_arg(ap, size_t *);
			if (targets != NULL)
			{
				*targets = calloc(dev->nbuffers, sizeof(void*));
				for (int i = 0; i < dev->nbuffers; i++)
				{
					targets[i] = dev->buffers[i].mem;
				}
			}
			if (ntargets != NULL)
				*ntargets = dev->nbuffers;
			if (psize != NULL)
				*psize = dev->buffers[0].size;
		}
		break;
		case buf_type_dmabuf:
		{
			int ntargets = va_arg(ap, int);
			int *targets = va_arg(ap, int *);
			size_t size = va_arg(ap, size_t);
			for (int i = 0; i < ntargets; i++)
			{
				dev->buffers[i].size = size;
				dev->buffers[i].dma_buf = targets[i];
			}
			dev->nbuffers = ntargets;
			/// the DVB device must be master
			va_end(ap);
			return -1;
		}
		break;
		case buf_type_dmabuf | buf_type_master:
		{
			for (int i = 0; i < dev->nbuffers; i++)
			{
				struct dmx_exportbuffer buffer = {0};
				buffer.index = i;
				if (ioctl(dev->fd, DMX_EXPBUF, &buffer) != 0)
				{
					va_end(ap);
					return -1;
				}
				dev->buffers[i].size = reqbuffers.size;
				dev->buffers[i].dma_buf = buffer.fd;
			}
			int *ntargets = va_arg(ap, int *);
			int **targets = va_arg(ap, int **);
			size_t *psize = va_arg(ap, size_t *);
			if (targets != NULL)
			{
				*targets = calloc(dev->nbuffers, sizeof(int));
				for (int i = 0; i < dev->nbuffers; i++)
				{
					(*targets)[i] = dev->buffers[i].dma_buf;
				}
			}
			if (ntargets != NULL)
				*ntargets = dev->nbuffers;
			if (psize != NULL)
				*psize = dev->buffers[0].size;
		}
		break;
		default:
			va_end(ap);
			return -1;
	}
	va_end(ap);

}

EXT_API int sdvb_fd(DVB_t *dev, int writer)
{
	return dev->fd;
}

EXT_API int sdvb_start(DVB_t *dev)
{
	if (ioctl(dev->fd, DMX_START, 0) != 0)
		return -1;
	dbg("sdvb: starting");
	return 0;
}

EXT_API int sdvb_stop(DVB_t *dev)
{
	if (ioctl(dev->fd, DMX_STOP, 0) != 0)
		return -1;
	dbg("sdvb: stoping");
	return 0;
}

EXT_API int sdvb_dequeue(DVB_t *dev, void **mem, size_t *bytesused, int *flags)
{
	struct dmx_buffer buffer;
	if (ioctl(dev->fd, DMX_DQBUF, &buffer) != 0)
		return -1;
	if (bytesused)
		*bytesused = buffer.bytesused;
	if (flags)
		*flags = buffer.flags;
	if (mem && dev->buffers[buffer.index].mem)
	{
		*mem = dev->buffers[buffer.index].mem + buffer.offset;
	}
	return buffer.index;
}

EXT_API int sdvb_queue(DVB_t *dev, int index, void *mem, size_t bytesused, int flags)
{
	struct dmx_buffer buffer;
	buffer.index = index;
	buffer.bytesused = bytesused;
	buffer.flags = flags;
	if (ioctl(dev->fd, DMX_QBUF, &buffer) != 0)
		return -1;
	return 0;
}
EXT_API void sdvb_destroy(DVB_t *dev)
{
	close(dev->fd);
	free(dev);
}

DeviceConf_t * sdvb_createconfig(const char *name)
{
	DVBConfig_t *devconfig = NULL;
	devconfig = calloc(1, sizeof(DVBConfig_t));
	devconfig->device = sdvb_defaultdevice;
#ifdef HAVE_JANSSON
	devconfig->parent.ops.loadconfiguration = sdvb_loadjsonconfiguration;
#endif
	return (DeviceConf_t *)devconfig;
}

#ifdef HAVE_JANSSON
int sdvb_loadjsonsettings(DVB_t *dev, void *jconfig)
{
	return 0;
}

int sdvb_loadjsonconfiguration(void *arg, void *entry)
{
	json_t *jconfig = entry;

	DVBConfig_t *config = (DVBConfig_t *)arg;
	json_t *device = json_object_get(jconfig, "device");
	if (device && json_is_string(device))
	{
		const char *value = json_string_value(device);
		config->device = value;
	}
	json_t *type = json_object_get(jconfig, "subtype");
	if (type && json_is_string(type))
	{
		const char *value = json_string_value(type);
		if (value && ! strcasecmp(value, "DVB_T"))
			config->type = DVB_T;
		if (value && ! strcasecmp(value, "DVB_S"))
			config->type = DVB_S;
		if (value && ! strcasecmp(value, "DVB_N"))
			config->type = DVB_N;
	}
	json_t *pid = json_object_get(jconfig, "pid");
	if (pid && json_is_integer(pid))
	{
		config->pid = json_integer_value(pid) & 0xFFFF;
	}
	json_t *definition = json_object_get(jconfig, "definition");
	scommon_loaddefinition(&config->parent, definition);
	return 0;
}
#endif

const FastVideoDevice_ops_t sdvb_ops = {
	.name = "dvb",
	.createconfig = sdvb_createconfig,
	.create = (FastVideoDevice_create_t)sdvb_create,
	.duplicate = (FastVideoDevice_duplicate_t)NULL,
	.loadsettings = (FastVideoDevice_loadsettings_t)sdvb_loadsettings,
	.requestbuffer = (FastVideoDevice_requestbuffer_t)sdvb_requestbuffer,
	.eventfd = (FastVideoDevice_eventfd_t)sdvb_fd,
	.start = (FastVideoDevice_start_t)sdvb_start,
	.stop = (FastVideoDevice_stop_t)sdvb_stop,
	.dequeue = (FastVideoDevice_dequeue_t)sdvb_dequeue,
	.queue = (FastVideoDevice_queue_t)sdvb_queue,
	.destroy = (FastVideoDevice_destroy_t)sdvb_destroy,
};

#include <dlfcn.h>

static void __attribute__ ((constructor)) sdvd_init()
{
	fastvideodevice_ops_append_t _fastvideodevice_ops_append;
	void *hdl = dlopen(NULL, RTLD_NOW);
	_fastvideodevice_ops_append = dlsym(hdl, "fastvideodevice_ops_append");
	if (_fastvideodevice_ops_append)
	{
		_fastvideodevice_ops_append(&sdvd_ops);
	}
}
