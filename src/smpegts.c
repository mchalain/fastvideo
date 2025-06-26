#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/select.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/dma-buf.h>
#include <time.h>

#include <arpa/inet.h>

#include "fastvideo.h"
#include "smpegts.h"
#include "config.h"
#include "log.h"

#define MAX_BUFFERS 4
#define MAX_CLIENTS 1

#define BUFFER_SIZE 16

#define SERVER_RUNNING 0x1000
#define DATA_STARTED 0x0001

#define MPEG_TS_LENGTH 188

#define DUMPDATA 0
#define PES_PTSDTS_ENABLE 0
#define PADDING_NULLPACKET 1

typedef struct MPEGHeader_s MPEGHeader_t;
struct MPEGHeader_s
{
	uint8_t sync;
	uint16_t pid2:5;
	uint16_t prio:1;
	uint16_t pusi:1;
	uint16_t tei:1;
	uint16_t pid:8;
	uint8_t cc:4;
	uint8_t afi:2;
	uint8_t tsi:2;
};

typedef struct PESHeader_s PESHeader_t;
struct PESHeader_s
{
	uint8_t sync[3];
	uint8_t str_id;
	uint16_t len;
	uint8_t orig:1;
	uint8_t dai:1;
	uint8_t cpri:1;
	uint8_t prio:1;
	uint8_t scr_ctrl:2;
	uint8_t mark:2;
	uint8_t exti:1;
	uint8_t crci:1;
	uint8_t acinf:1;
	uint8_t dsmi:1;
	uint8_t esri:1;
	uint8_t escri:1;
	uint8_t ptsi:2;
	uint8_t hlen;
	union {
		struct {
			uint8_t pts[5];
			uint8_t dts[5];
		} __attribute__ ((packed));
		uint8_t raw[10];
	} opt;
};

static uint8_t nullpacket[MPEG_TS_LENGTH] =
{
	 'G', 0x1f, 0xff, 0x10, 0xff, 0xff, 0xff, 0xff,
	 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
};

typedef struct Dev_s Dev_t;
struct Dev_s
{
	device_type_e type;
	MPEG_TSConf_t *config;
	Proto_t *proto;
	void *protoctx;
	FrameBuffer_t buffers[MAX_BUFFERS];
	int nbuffers;
	int currentid;
	MPEGHeader_t header;
	PESHeader_t pes_header;
	uint8_t packetlen;
	time_t start;
	uint32_t pcr;
};

static FrameBuffer_t *_create_buffer(DeviceConf_t *config)
{
	FrameBuffer_t *buffer = calloc(1, sizeof(*buffer));
	buffer->size = 1024;
	buffer->mem = malloc(buffer->size);
	buffer->dma_buf = 0;
	buffer->offset = 0;
	return buffer;
}

static void _destroy_buffer(FrameBuffer_t *buffer)
{
	free(buffer->mem);
}

#ifdef HAVE_JANSSON
int mpegts_loadjsonconfiguration(void *arg, void *entry);
#endif
#ifdef IPV6
static const char default_addr[] = "FF02::1:FF00:56";
#else
static const char default_addr[] = "239.0.0.14";
#endif
DeviceConf_t *mpegts_createconfig(void)
{
	MPEG_TSConf_t *config = calloc(1, sizeof(*config));
	config->parent.fourcc = FOURCC_H264;
	config->host = default_addr;
	config->port = 5014;
	config->pid = 0x41;
#ifdef HAVE_JANSSON
	config->parent.ops.loadconfiguration = mpegts_loadjsonconfiguration;
#endif
	return &config->parent;
}

static int dumpfd = 0;

#define CRC_32_RESULT_WIDTH                 32u
#define CRC_32_POLYNOMIAL                   0x04C11DB7u
#define CRC_32_INIT_VALUE                   0xFFFFFFFFu
#define CRC_32_XOR_VALUE                    0xFFFFFFFFu

static uint8_t reverse_8bits(uint8_t value)
{
	value = (value & 0xF0) >> 4u | (value & 0x0F) << 4u;
	value = (value & 0xCC) >> 2u | (value & 0x33) << 2u;
	value = (value & 0xAA) >> 1u | (value & 0x55) << 1u;

	return value;
}

static uint32_t reverse_32bits(uint32_t value)
{
	uint32_t reversed = 0u;

	for(uint8_t i = 31u; value; )
	{
		reversed |= (value & 1u) << i;
		value >>= 1u;
		-- i;
	}

	return reversed;
}

static uint32_t crc32(const uint8_t *buffer, uint16_t length)
{
	uint32_t retVal = 0u;

	if (buffer != NULL)
	{
		retVal = CRC_32_INIT_VALUE;

		/* Do calculation procedure for each byte */
		for (int i = 0; i < length; i++)
		{
			/* XOR new byte with temp result */
			retVal ^= (reverse_8bits(buffer[i]) << (CRC_32_RESULT_WIDTH - 8u));

			/* Do calculation for current data */
			for (int j = 0; j < 8; j++)
			{
				if (retVal & (1u << (CRC_32_RESULT_WIDTH - 1u)))
				{
					retVal = (retVal << 1u) ^ CRC_32_POLYNOMIAL;
				}
				else
				{
					retVal = (retVal << 1u);
				}
			}
		}

		/* XOR result with specified value */
		retVal ^= CRC_32_XOR_VALUE;
	}

	/* Reflect result */
	retVal = reverse_32bits(retVal);

	return retVal;
}

int _client_filldata(Dev_t *dev, size_t mtu)
{
	int ret = 1;
	int length = 0;
#if PADDING_NULLPACKET
	while (ret > 0 && mtu > dev->packetlen)
	{
		length += ret;
		int flags = MSG_MORE;
		if (mtu < 2 * dev->packetlen)
				flags = 0;
		ret = dev->proto->send(dev->protoctx, nullpacket, dev->packetlen, flags);
		if (ret != dev->packetlen)
			break;
		nullpacket[3] += 1;
		nullpacket[3] %= 0x0f;
		mtu -= ret;
	}
	length += ret;
#else
	if (ret > 0 mtu > dev->packetlen)
	{
		length += ret;
		dev->proto->flush(dev->protoctx);
	}
#endif
	if (ret > 0)
		return length;
	return ret;
}

static int _client_pushdata(Dev_t *dev, int bufferid)
{
	int ret = 0;
	dev->header.pusi = 1;
	dev->header.afi = 1;

	size_t length = dev->buffers[bufferid].bytesused;
	void *buffer = dev->buffers[bufferid].mem;
#if DUMPDATA
	if (dumpfd > 0)
	{
		write(dumpfd, buffer, length);
	}
#endif
#ifdef UDP_CORK
	int value = 1;
	setsockopt(dev->serverfd, IPPROTO_UDP, UDP_CORK, &value, sizeof(value));
#endif
	uint32_t pcr = 0;
	struct timespec tp;
	if (clock_gettime(CLOCK_TAI, &tp) == 0)
	{
		uint32_t C90kHz = 0;
		/// a precision of 27MHz is useless here
		if (dev->start == 0)
		{
			dev->start = tp.tv_sec;
			C90kHz = ((tp.tv_nsec / 1000) * 90) / 1000;
		}
		else
		{
			C90kHz =  tp.tv_sec - dev->start;
			C90kHz *= 90000;
			///compute pcr 33bits here
			C90kHz += ((tp.tv_nsec / 1000) * 90) / 1000;
		}
#define __NB_TICKS_FOR_40ms (40 * 90)
		//if (C90kHz > (dev->pcr + __NB_TICKS_FOR_40ms))
		{
			pcr = C90kHz;
			dev->header.afi |= 0x2;
		}
	}
	ssize_t mtu = dev->proto->mtu(dev->protoctx);
	while (length > 0 && ret >= 0)
	{
		int paddinglength = 0;
		int flags = MSG_MORE;
		size_t buflength; /// the length of buffer to send with this ts packet
		/// the packet must contain 188 bytes even when the payload is smaller
		buflength = dev->packetlen;
		if (buflength - sizeof(dev->header) > length)
		{
			/// need padding to complete the ts packet
			paddinglength = buflength - sizeof(dev->header) - length;
			/// request the adaptation field to add this padding
			dev->header.afi |= 0x2;
		}
		ret = dev->proto->send(dev->protoctx, &dev->header, sizeof(dev->header), flags);
		/// add the adaptation field
		if (ret > 0 && dev->header.afi & 0x02)
		{
			buflength -= ret;
			mtu -= ret;
			/// adaptation field buffer with only pcr
			uint8_t adaptfield[8] = {0xff};
			size_t adaptfieldlength = 1;
			/// the adapation field may contain pcr
			if (pcr)
			{
				/// bits field indicated the pcr
				adaptfield[1] = 0x10;
				if (dev->buffers[bufferid].bytesused > dev->proto->mtu(dev->protoctx))
					adaptfield[1] |= 0x40;
				/// pcr over 33bits
				adaptfield[2] = pcr >> 25 & 0xff;
				adaptfield[3] = pcr >> 17 & 0xff;
				adaptfield[4] = pcr >> 9 & 0xff;
				adaptfield[5] = pcr >> 1 & 0xff;
				adaptfield[6] = (pcr & 0x01) << 7;
				adaptfield[7] = 0;
				adaptfieldlength += 7;
			}
			/// the adaptation field contains padding (or stuffing)
			paddinglength -= adaptfieldlength;
			if (paddinglength > 0)
			{
				adaptfieldlength += paddinglength;
			}
			else
				paddinglength = 0;

			/// Adaptation field length - length's byte
			adaptfield[0] = adaptfieldlength - 1;
			ret = dev->proto->send(dev->protoctx, adaptfield, adaptfieldlength - paddinglength, flags);
			for (int i = 0; ret > 0 && i < paddinglength; i++)
			{
				buflength -= ret;
				mtu -= ret;
				if (mtu < 0)
					break;
				uint8_t padding = 0xff;
				ret = dev->proto->send(dev->protoctx, &padding, 1, flags);
			}
			paddinglength = 0;
		}
		/// the first part of an es block (buffer) must start with
		/// a PES header
		if (ret > 0 && dev->header.pusi)
		{
			buflength -= ret;
			mtu -= ret;
#if PES_PTSDTS_ENABLE
			dev->pes_header.opt.dts[0] = ((dev->pcr >> 30 & 0x07) << 1) | 0x11;
			dev->pes_header.opt.dts[1] = (dev->pcr >> 22 & 0x7f);
			dev->pes_header.opt.dts[2] = ((dev->pcr >> 15 & 0x7f) << 1) | 0x01;
			dev->pes_header.opt.dts[3] = (dev->pcr >> 7 & 0x7f);
			dev->pes_header.opt.dts[4] = ((dev->pcr & 0x7f) << 1) | 0x01;
#endif
#if PES_PTSDTS_ENABLE
			dev->pes_header.opt.pts[0] = ((pcr >> 30 & 0x07) << 1) | 0x31;
			dev->pes_header.opt.pts[1] = (pcr >> 22 & 0x7f);
			dev->pes_header.opt.pts[2] = ((pcr >> 15 & 0x7f) << 1) | 0x01;
			dev->pes_header.opt.pts[3] = (pcr >> 7 & 0x7f);
			dev->pes_header.opt.pts[4] = ((pcr & 0x7f) << 1) | 0x01;
#endif
			/// sizeof(dev->pes_header) returns 20 instead 19 (alignment error)
			//ret = dev->proto->send(dev->protoctx, &dev->pes_header, sizeof(dev->pes_header), flags);
			ret = dev->proto->send(dev->protoctx, &dev->pes_header, 19, flags);
		}
		/// the part of the buffer
		if (ret > 0)
		{
			buflength -= ret;
			mtu -= ret;
			if (mtu < 2 * dev->packetlen)
				flags = 0;
			ret = dev->proto->send(dev->protoctx, buffer, buflength, flags);
		}
		if (ret > 0)
		{
			buflength -= ret;
			if (buflength != 0)
				err("mpegts: format error");
			mtu -= ret;
			if (flags == 0)
				mtu = dev->proto->mtu(dev->protoctx);
			if (length > ret)
				length -= ret;
			else
				length = 0;
			buffer += ret;
			/// next part comes from the same buffer
			dev->header.pusi = 0;
			/// ts packet contains only payload by default
			dev->header.afi = 0x1;
			dev->header.cc++;
			if (pcr > 0)
				dev->pcr = pcr;
			/// the pcr must be only inside the first ts packet of the buffer
			pcr = 0;
		}
		else if (errno == EAGAIN)
			ret = 0;
	}
	if (ret > 0)
	{
		ret = _client_filldata(dev, mtu);
		mtu -= ret;
	}
	if (ret < 0)
	{
		dev->proto->close(dev->protoctx);
		err("mpegts: send error %m");
	}
	else if (ret > 0 && length == 0)
	{
		dev->buffers[bufferid].state = ready;
		ret = 0;
	}
	return ret;
}

static int _client_flushdata(Dev_t *dev, int bufferid)
{
	dev->proto->flush(dev->protoctx);
	dev->buffers[bufferid].state = dequeued;
	return 0;
}

EXT_API Dev_t *mpegts_create(const char *devicename, device_type_e type, MPEG_TSConf_t *config)
{
	if (type != device_output)
		return NULL;

	Proto_t *proto = &proto_udp;
	void *protoctx = proto->create(config);
	if (protoctx == NULL)
		return NULL;

	Dev_t *dev = calloc(1, sizeof(*dev));
	dev->type = type;
	dev->config = config;
	dev->proto = proto;
	dev->protoctx = protoctx;

	dev->header.sync = 'G';
	dev->header.pid = config->pid;
	dev->header.afi = 1;

	dev->pes_header.sync[2] = 0x01;
	dev->pes_header.str_id = 0xe0;
	dev->pes_header.mark = 0x2;
#if PES_PTSDTS_ENABLE || 1
	dev->pes_header.ptsi = 0x3;
#endif
	dev->pes_header.hlen = sizeof(dev->pes_header.opt);

	dev->packetlen = MPEG_TS_LENGTH;

#if DUMPDATA
	dumpfd = open("/tmp/dump.h264", O_RDWR | O_CREAT);
	err("dumpfd %d %m", dumpfd);
#endif
	return dev;
}

EXT_API int mpegts_requestbuffer(Dev_t *dev, enum buf_type_e t, ...)
{
	va_list ap;
	va_start(ap, t);
	switch (t)
	{
		case (buf_type_memory):
		{
			int ntargets = va_arg(ap, int);
			void **targets = va_arg(ap, void **);
			size_t size = va_arg(ap, size_t);
			dev->nbuffers = ntargets > MAX_BUFFERS? MAX_BUFFERS:ntargets;

			for (int i = 0; i < dev->nbuffers; i++)
			{
				FrameBuffer_t *buffer = &dev->buffers[i];
				buffer->id = i;
				buffer->size = size;
				buffer->mem = targets[i];
			}
		}
		break;
		case buf_type_dmabuf:
		{
			if (dev->type == device_input)
				return -1;
			int ntargets = va_arg(ap, int);
			int *targets = va_arg(ap, int *);
			size_t size = va_arg(ap, size_t);
			dev->nbuffers = ntargets > MAX_BUFFERS? MAX_BUFFERS:ntargets;
			for (int i = 0; i < ntargets; i++)
			{
				FrameBuffer_t *buffer = &dev->buffers[i];
				buffer->id = i;
				buffer->size = size;
				buffer->dma_buf = targets[i];
			}
		}
		break;
		default:
			va_end(ap);
			return -1;
	}
	va_end(ap);

	return 0;
}

EXT_API int mpegts_fd(Dev_t *dev, int writer)
{
	if (writer && dev->currentid == -1)
		return 0;
	return dev->proto->fd(dev->protoctx);
}

EXT_API int mpegts_queue(Dev_t *dev, int id, void *mem, size_t size)
{
	if (id < 0 || id > dev->nbuffers)
		return -1;
	FrameBuffer_t *buffer = &dev->buffers[id];
	buffer->bytesused = size;
	if (buffer->dma_buf > 0)
	{
		if (buffer->mem)
			warn("mpegts: memory already locked");
		struct dma_buf_sync sync = { 0 };
		sync.flags = DMA_BUF_SYNC_READ | DMA_BUF_SYNC_START;
		ioctl(buffer->dma_buf, DMA_BUF_IOCTL_SYNC, &sync);
		mem = mmap(NULL, buffer->size, PROT_READ, MAP_SHARED, buffer->dma_buf, 0 );
	}
	if (mem)
	{
		buffer->mem = mem;
	}

	if (dev->currentid != -1)
	{
		errno = EAGAIN;
		return -1;
	}
	dev->currentid = id;
	buffer->state = queued;
	if (_client_pushdata(dev, id) < 0)
	{
		return -1;
	}
	return 0;
}

EXT_API int mpegts_dequeue(Dev_t *dev, void **mem, size_t *bytesused)
{
	int id = dev->currentid;
	if (id == -1)
	{
		errno = EAGAIN;
		return -1;
	}
	FrameBuffer_t *buffer = NULL;
	buffer = &dev->buffers[dev->currentid];
	dev->currentid = -1;

	if (buffer->state == ready)
	{
		_client_flushdata(dev, id);
		if (buffer->dma_buf > 0)
		{
			struct dma_buf_sync sync = { 0 };
			munmap(buffer->mem, buffer->size);
			buffer->mem = NULL;
			sync.flags = DMA_BUF_SYNC_READ | DMA_BUF_SYNC_END;
			ioctl(buffer->dma_buf, DMA_BUF_IOCTL_SYNC, &sync);
		}
	}
	if (buffer->state != dequeued)
	{
		return -1;
	}
	if (*mem)
		*mem = buffer->mem;
	if (*bytesused)
		*bytesused = buffer->size;
	buffer->state = dequeued;
	return id;
}

EXT_API int mpegts_start(Dev_t *dev)
{
	dev->proto->connect(dev->protoctx);
	dev->currentid = -1;
	return 0;
}

EXT_API int mpegts_stop(Dev_t *dev)
{
	dev->proto->close(dev->protoctx);
	return 0;
}

EXT_API void mpegts_destroy(Dev_t *dev)
{
	dev->proto->destroy(dev->protoctx);
#if DUMPDATA
	if (dumpfd > 0)
		close(dumpfd);
#endif
	free(dev);
}


#ifdef HAVE_JANSSON
int mpegts_loadjsonconfiguration(void *arg, void *entry)
{
	json_t *jconfig = entry;

	MPEG_TSConf_t *config = (MPEG_TSConf_t *)arg;
	json_t *host = json_object_get(jconfig, "host");
	if (host && json_is_string(host))
	{
		const char *value = json_string_value(host);
		config->host = value;
	}
	json_t *port = json_object_get(jconfig, "port");
	if (port && json_is_integer(port))
	{
		int value = json_integer_value(port);
		config->port = value;
	}
	json_t *pid = json_object_get(jconfig, "pid");
	if (pid && json_is_integer(pid))
	{
		int value = json_integer_value(pid);
		config->pid = value;
	}
library_end:
	return 0;
}
#endif

FastVideoDevice_ops_t smpegts_ops = {
	.name = "mpegts",
	.createconfig = mpegts_createconfig,
	.create = (FastVideoDevice_create_t)mpegts_create,
	.duplicate = (FastVideoDevice_duplicate_t)NULL,
	.loadsettings = (FastVideoDevice_loadsettings_t)NULL,
	.requestbuffer = (FastVideoDevice_requestbuffer_t)mpegts_requestbuffer,
	.eventfd = (FastVideoDevice_eventfd_t)mpegts_fd,
	.start = (FastVideoDevice_start_t)mpegts_start,
	.stop = (FastVideoDevice_stop_t)mpegts_stop,
	.dequeue = (FastVideoDevice_dequeue_t)mpegts_dequeue,
	.queue = (FastVideoDevice_queue_t)mpegts_queue,
	.destroy = (FastVideoDevice_destroy_t)mpegts_destroy,
};

#include <dlfcn.h>

static void __attribute__ ((constructor)) smpegts_init()
{
	fastvideodevice_ops_append_t _fastvideodevice_ops_append;
	void *hdl = dlopen(NULL, RTLD_NOW);
	_fastvideodevice_ops_append = dlsym(hdl, "fastvideodevice_ops_append");
	if (_fastvideodevice_ops_append)
	{
		_fastvideodevice_ops_append(&smpegts_ops);
	}
}
