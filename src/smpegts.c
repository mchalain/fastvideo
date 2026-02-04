#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/select.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/dma-buf.h>
#include <time.h>
#include <stdlib.h>

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
#define PES_PTSDTS_ENABLE 1
#define PADDING_NULLPACKET 1
#define PSI_SDTPACKET 1
#define GLOBAL_PCR 1

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
typedef struct MPEGPAT_s MPEGPAT_t;
struct MPEGPAT_s
{
	uint8_t tbid;
	union {
		struct {
			uint16_t len:12;
			uint16_t res1:3;
			uint16_t si:1;
		};
		uint16_t rlen __attribute__ ((packed));
	} __attribute__ ((packed));
	uint16_t tsid __attribute__ ((packed));
	uint8_t cni:1;
	uint8_t ver:5;
	uint8_t res2:2;
	uint8_t secn;
	uint8_t lsecn;
	struct {
		uint16_t id __attribute__ ((packed));
		uint8_t unused:5;
		uint8_t re1:3;
		uint8_t pmtid;
	} prog1;
	uint32_t crc __attribute__ ((packed));
};

typedef struct MPEGPMT_s MPEGPMT_t;
struct MPEGPMT_s
{
	uint8_t tbid;
	union {
		struct {
			uint16_t len:12;
			uint16_t res1:3;
			uint16_t si:1;
		};
		uint16_t rlen __attribute__ ((packed));
	} __attribute__ ((packed));
	uint16_t progid __attribute__ ((packed));
	uint8_t cni:1;
	uint8_t ver:5;
	uint8_t res2:2;
	uint8_t secn;
	uint8_t lsecn;
	uint8_t res3:3;
	uint8_t pcr_pid_h:5;
	uint8_t pcr_pid;
	uint8_t res4:4;
	uint8_t prinf_h:4;
	uint8_t prinflen;
	struct {
		uint8_t type;
		uint8_t unused1:5;
		uint8_t re1:3;
		uint8_t esid;
		uint8_t unused2:4;
		uint8_t re2:4;
		uint8_t esinfolen;
	} es1;
	uint32_t crc __attribute__ ((packed));
};

typedef struct MPEGDST_s MPEGDST_t;
struct MPEGDST_s
{
	uint8_t tbid;
	union {
		struct {
			uint16_t len:12;
			uint16_t res1:3;
			uint16_t si:1;
		};
		uint16_t rlen __attribute__ ((packed));
	} __attribute__ ((packed));
	uint16_t extension __attribute__ ((packed));
	uint8_t cni:1;
	uint8_t ver:5;
	uint8_t res2:2;
	uint8_t secn;
	uint8_t lsecn;
	uint16_t netid __attribute__ ((packed));
	uint8_t ff;
	struct {
		uint16_t id __attribute__ ((packed));
		uint8_t sched:1;
		uint8_t present:1;
		uint8_t fc:6;
		uint8_t status:3;
		uint8_t freeca:1;
		uint8_t desclooplen_h:4;
		uint8_t desclooplen;
		uint8_t tag;
		uint8_t desclen;
		uint8_t type;
		uint8_t provlen;
		uint8_t provider[9];
		uint8_t namelen;
		uint8_t name[9];
	} service;
	uint32_t crc __attribute__ ((packed));
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

static const uint8_t default_pat[MPEG_TS_LENGTH] = {
	  'G', 0x40, 0x00, 0x10, 0x00, 0x00, 0xb0, 0x0d, 0x00, 0x01,
	 0xc1, 0x00, 0x00, 0x00, 0x01, 0xe0, 0x40, 0xaa, 0xaa, 0xaa,
	 0xaa, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
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
	 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
};

static const uint8_t default_pmt[MPEG_TS_LENGTH] = {
	  'G', 0x40, 0x41, 0x10, 0x00, 0x02, 0xb0, 0x12, 0x00, 0x01,
	 0xc1, 0x00, 0x00, 0xe0, 0x42, 0xf0, 0x00, 0x03, 0xe0, 0x42,
	 0xf0, 0x00, 0xaa, 0xaa, 0xaa, 0xaa, 0xff, 0xff, 0xff, 0xff,
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
	 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
};

#if PSI_SDTPACKET
static const uint8_t default_sdt[MPEG_TS_LENGTH] = {
	  'G', 0x40, 0x11, 0x10, 0x00, 0x42, 0xf0, 0x28, 0x00, 0x01,
	 0xc1, 0x00, 0x00, 0x00, 0x01, 0xff, 0x00, 0x01, 0xfc, 0x80,
	 0x16, 0x48, 0x14, 0x0A, 0x09,  'F',  'a',  's',  't',  'V',
	  'i',  'd',  'e',  'o', 0x09, 0x00, 0x00, 0x00, 0x00, 0x00,
	 0x00, 0x00, 0x00, 0x00, 0xaa, 0xaa, 0xaa, 0xaa, 0xff, 0xff,
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
	 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
};
#endif

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

static uint8_t *padding = nullpacket + 5;

typedef struct Dev_s Dev_t;
struct Dev_s
{
	device_type_e type;
	MPEG_TSConf_t *config;
	const Proto_t *proto;
	void *protoctx;
	uint32_t frames;
	FrameBuffer_t buffers[MAX_BUFFERS];
	int nbuffers;
	int currentid;
	MPEGHeader_t header;
	PESHeader_t pes_header;
	union
	{
		struct {
			MPEGHeader_t header;
			uint8_t pointer;
			MPEGPAT_t pat;
		};
		uint8_t raw[MPEG_TS_LENGTH];
	} pat;
	union
	{
		struct {
			MPEGHeader_t header;
			uint8_t pointer;
			MPEGPMT_t pmt;
		};
		uint8_t raw[MPEG_TS_LENGTH];
	} pmt;
#if PSI_SDTPACKET
	union
	{
		struct {
			MPEGHeader_t header;
			uint8_t pointer;
			MPEGDST_t sdt;
		};
		uint8_t raw[MPEG_TS_LENGTH];
	} sdt;
#endif
	uint8_t packetlen;
	time_t start;
	uint32_t pcr;
	int periodic;
};

static FrameBuffer_t *_create_buffer(DeviceConf_t *config)
{
	FrameBuffer_t *buffer = calloc(1, sizeof(*buffer));
	buffer->size = 1024;
	buffer->mem = malloc(buffer->size);
	buffer->dma_buf = 0;
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
DeviceConf_t *mpegts_createconfig(const char *name)
{
	MPEG_TSConf_t *config = calloc(1, sizeof(*config));
	config->parent.fourcc = FOURCC_H264;
	config->host = default_addr;
	config->port = 1024;
	config->pid = 0x41;
	config->maxclients = 5;
	config->proto = _protos[0];
#ifdef HAVE_JANSSON
	config->parent.ops.loadconfiguration = mpegts_loadjsonconfiguration;
#endif
	return &config->parent;
}

static int dumpfd = 0;

#if 0

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
#else
static uint32_t crc32_table[256] =
{
  0x00000000, 0x04c11db7, 0x09823b6e, 0x0d4326d9,
  0x130476dc, 0x17c56b6b, 0x1a864db2, 0x1e475005,
  0x2608edb8, 0x22c9f00f, 0x2f8ad6d6, 0x2b4bcb61,
  0x350c9b64, 0x31cd86d3, 0x3c8ea00a, 0x384fbdbd,
  0x4c11db70, 0x48d0c6c7, 0x4593e01e, 0x4152fda9,
  0x5f15adac, 0x5bd4b01b, 0x569796c2, 0x52568b75,
  0x6a1936c8, 0x6ed82b7f, 0x639b0da6, 0x675a1011,
  0x791d4014, 0x7ddc5da3, 0x709f7b7a, 0x745e66cd,
  0x9823b6e0, 0x9ce2ab57, 0x91a18d8e, 0x95609039,
  0x8b27c03c, 0x8fe6dd8b, 0x82a5fb52, 0x8664e6e5,
  0xbe2b5b58, 0xbaea46ef, 0xb7a96036, 0xb3687d81,
  0xad2f2d84, 0xa9ee3033, 0xa4ad16ea, 0xa06c0b5d,
  0xd4326d90, 0xd0f37027, 0xddb056fe, 0xd9714b49,
  0xc7361b4c, 0xc3f706fb, 0xceb42022, 0xca753d95,
  0xf23a8028, 0xf6fb9d9f, 0xfbb8bb46, 0xff79a6f1,
  0xe13ef6f4, 0xe5ffeb43, 0xe8bccd9a, 0xec7dd02d,
  0x34867077, 0x30476dc0, 0x3d044b19, 0x39c556ae,
  0x278206ab, 0x23431b1c, 0x2e003dc5, 0x2ac12072,
  0x128e9dcf, 0x164f8078, 0x1b0ca6a1, 0x1fcdbb16,
  0x018aeb13, 0x054bf6a4, 0x0808d07d, 0x0cc9cdca,
  0x7897ab07, 0x7c56b6b0, 0x71159069, 0x75d48dde,
  0x6b93dddb, 0x6f52c06c, 0x6211e6b5, 0x66d0fb02,
  0x5e9f46bf, 0x5a5e5b08, 0x571d7dd1, 0x53dc6066,
  0x4d9b3063, 0x495a2dd4, 0x44190b0d, 0x40d816ba,
  0xaca5c697, 0xa864db20, 0xa527fdf9, 0xa1e6e04e,
  0xbfa1b04b, 0xbb60adfc, 0xb6238b25, 0xb2e29692,
  0x8aad2b2f, 0x8e6c3698, 0x832f1041, 0x87ee0df6,
  0x99a95df3, 0x9d684044, 0x902b669d, 0x94ea7b2a,
  0xe0b41de7, 0xe4750050, 0xe9362689, 0xedf73b3e,
  0xf3b06b3b, 0xf771768c, 0xfa325055, 0xfef34de2,
  0xc6bcf05f, 0xc27dede8, 0xcf3ecb31, 0xcbffd686,
  0xd5b88683, 0xd1799b34, 0xdc3abded, 0xd8fba05a,
  0x690ce0ee, 0x6dcdfd59, 0x608edb80, 0x644fc637,
  0x7a089632, 0x7ec98b85, 0x738aad5c, 0x774bb0eb,
  0x4f040d56, 0x4bc510e1, 0x46863638, 0x42472b8f,
  0x5c007b8a, 0x58c1663d, 0x558240e4, 0x51435d53,
  0x251d3b9e, 0x21dc2629, 0x2c9f00f0, 0x285e1d47,
  0x36194d42, 0x32d850f5, 0x3f9b762c, 0x3b5a6b9b,
  0x0315d626, 0x07d4cb91, 0x0a97ed48, 0x0e56f0ff,
  0x1011a0fa, 0x14d0bd4d, 0x19939b94, 0x1d528623,
  0xf12f560e, 0xf5ee4bb9, 0xf8ad6d60, 0xfc6c70d7,
  0xe22b20d2, 0xe6ea3d65, 0xeba91bbc, 0xef68060b,
  0xd727bbb6, 0xd3e6a601, 0xdea580d8, 0xda649d6f,
  0xc423cd6a, 0xc0e2d0dd, 0xcda1f604, 0xc960ebb3,
  0xbd3e8d7e, 0xb9ff90c9, 0xb4bcb610, 0xb07daba7,
  0xae3afba2, 0xaafbe615, 0xa7b8c0cc, 0xa379dd7b,
  0x9b3660c6, 0x9ff77d71, 0x92b45ba8, 0x9675461f,
  0x8832161a, 0x8cf30bad, 0x81b02d74, 0x857130c3,
  0x5d8a9099, 0x594b8d2e, 0x5408abf7, 0x50c9b640,
  0x4e8ee645, 0x4a4ffbf2, 0x470cdd2b, 0x43cdc09c,
  0x7b827d21, 0x7f436096, 0x7200464f, 0x76c15bf8,
  0x68860bfd, 0x6c47164a, 0x61043093, 0x65c52d24,
  0x119b4be9, 0x155a565e, 0x18197087, 0x1cd86d30,
  0x029f3d35, 0x065e2082, 0x0b1d065b, 0x0fdc1bec,
  0x3793a651, 0x3352bbe6, 0x3e119d3f, 0x3ad08088,
  0x2497d08d, 0x2056cd3a, 0x2d15ebe3, 0x29d4f654,
  0xc5a92679, 0xc1683bce, 0xcc2b1d17, 0xc8ea00a0,
  0xd6ad50a5, 0xd26c4d12, 0xdf2f6bcb, 0xdbee767c,
  0xe3a1cbc1, 0xe760d676, 0xea23f0af, 0xeee2ed18,
  0xf0a5bd1d, 0xf464a0aa, 0xf9278673, 0xfde69bc4,
  0x89b8fd09, 0x8d79e0be, 0x803ac667, 0x84fbdbd0,
  0x9abc8bd5, 0x9e7d9662, 0x933eb0bb, 0x97ffad0c,
  0xafb010b1, 0xab710d06, 0xa6322bdf, 0xa2f33668,
  0xbcb4666d, 0xb8757bda, 0xb5365d03, 0xb1f740b4
};

static uint32_t crc32(const uint8_t *buffer, uint16_t length)
{
	uint32_t crc = 0xffffffff;
	const uint8_t *it = buffer;
	while (it < buffer + length)
	{
		crc = (crc << 8) ^ crc32_table[(crc >>24) ^ (*it)];
		it++;
	}
	return crc;
}
#endif

int _client_filldata(Dev_t *dev, size_t mtu)
{
	int ret = 1;
	int length = 0;
	if (ret > 0 && mtu > dev->packetlen)
	{
		/**
		 * send PMT packet
		 */
		int flags = Proto_More;
		if (mtu < 2 * dev->packetlen)
				flags = 0;
		uint8_t cc = dev->pmt.header.cc;
		dev->pmt.header.cc = (cc + 1) & 0x0f;
		ret = dev->proto->send(dev->protoctx, dev->pmt.raw, dev->packetlen, flags);
		if (ret == dev->packetlen)
			mtu -= ret;
	}
	if (ret > 0 && mtu > dev->packetlen)
	{
		/**
		 * send PAT packet
		 */
		length += ret;
		Proto_Flags_t flags = Proto_More;
		if (mtu < 2 * dev->packetlen)
				flags = 0;
		uint8_t cc = dev->pat.header.cc;
		dev->pat.header.cc = (cc + 1) & 0x0f;
		ret = dev->proto->send(dev->protoctx, dev->pat.raw, dev->packetlen, flags);
		if (ret == dev->packetlen)
			mtu -= ret;
	}
#if PSI_SDTPACKET
	if (ret > 0 && mtu > dev->packetlen)
	{
		/**
		 * send SDT packet
		 */
		length += ret;
		Proto_Flags_t flags = Proto_More;
		if (mtu < 2 * dev->packetlen)
				flags = 0;
		uint8_t cc = dev->sdt.header.cc;
		dev->sdt.header.cc = (cc + 1) & 0x0f;
		ret = dev->proto->send(dev->protoctx, dev->sdt.raw, dev->packetlen, flags);
		if (ret == dev->packetlen)
			mtu -= ret;
	}
#endif
#if PADDING_NULLPACKET
	while (ret > 0 && mtu > dev->packetlen)
	{
		length += ret;
		Proto_Flags_t flags = Proto_More;
		if (mtu < 2 * dev->packetlen)
				flags = 0;
		ret = dev->proto->send(dev->protoctx, nullpacket, dev->packetlen, flags);
		if (ret != dev->packetlen)
			break;
		nullpacket[3] += 1;
		nullpacket[3] %= 0x0f;
		mtu -= ret;
	}
#else
	if (ret > 0 && mtu > dev->packetlen)
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
	int ret = 1;
	dev->header.pusi = 1;
	dev->header.afi = 1;
	uint8_t randomaccess = 0;

	size_t length = dev->buffers[bufferid].bytesused;
	void *buffer = dev->buffers[bufferid].mem;
	if (dev->buffers[bufferid].flags & FB_FLAGS_KEYFRAME)
	{
		randomaccess = 0x40;
	}
	if (dev->config->periodic && dev->periodic == dev->config->periodic)
	{
		dev->periodic = 0;
		randomaccess = 0x40;
	}
	else
		dev->periodic++;
#if DUMPDATA
	if (dumpfd > 0)
	{
		write(dumpfd, buffer, length);
	}
#endif
	uint32_t pcr = 0;
#if GLOBAL_PCR
	struct timespec tp;
	if (clock_gettime(CLOCK_TAI, &tp) == 0)
	{
		uint32_t C90kHz = 0;
		/// a precision of 27MHz is useless here
		if (dev->start == 0)
		{
			dev->start = tp.tv_sec;
			C90kHz = ((tp.tv_nsec / 1000000) * 90);
		}
		else
		{
			C90kHz =  tp.tv_sec - dev->start;
			C90kHz *= 90000;
			///compute pcr 33bits here
			C90kHz += ((tp.tv_nsec / 1000000) * 90);
		}
#define __NB_TICKS_FOR_40ms (40 * 90)
		//if (C90kHz > (dev->pcr + __NB_TICKS_FOR_40ms))
		{
			pcr = C90kHz;
			dev->header.afi |= 0x2;
		}
	}
#else
	/// frame rate 30 fps clock 90kHz
	pcr = dev->pcr + 90000 / dev->config->periodic;
	dev->header.afi |= 0x2;
#endif
	ssize_t mtu = dev->proto->mtu(dev->protoctx);
	while (length > 0 && ret > 0)
	{
		int paddinglength = 0;
		Proto_Flags_t flags = Proto_More;
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
			size_t adaptfieldlength = 2;
			/// the adapation field may contain pcr
			if (pcr)
			{
				/// bits field indicated the pcr
				adaptfield[1] = 0x10;
				adaptfield[1] |= randomaccess;
				if (pcr < dev->pcr)
					adaptfield[1] |= 0x80;
				/// pcr over 33bits
				adaptfield[2] = (pcr >> 25) & 0xff;
				adaptfield[3] = (pcr >> 17) & 0xff;
				adaptfield[4] = (pcr >> 9) & 0xff;
				adaptfield[5] = (pcr >> 1) & 0xff;
				adaptfield[6] = ((pcr & 0x01) << 7) | 0x7e;
				adaptfield[7] = 0x00;
				adaptfieldlength += 6;
			}
			else
				adaptfield[1] = 0x00;
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
			if (ret > 0 && paddinglength > 0)
			{
				buflength -= ret;
				mtu -= ret;
				if (mtu < 2 * dev->packetlen)
					flags = 0;
				if (paddinglength > dev->packetlen - sizeof(dev->header) - 1) /// see the declaration of "padding"
				{
					err("smpegts: padding length overflow the pecket");
					paddinglength = dev->packetlen - sizeof(dev->header) - 1;
				}
				ret = dev->proto->send(dev->protoctx, padding, paddinglength, flags);
			}
		}
		/// the first part of an es block (buffer) must start with
		/// a PES header
		if (ret > 0 && dev->header.pusi)
		{
			buflength -= ret;
			mtu -= ret;
#if PES_PTSDTS_ENABLE
			/// this extend the latency in all cases ?
			pcr += 90; /// 90 ticks means 1ms
			int nibble = 0x00;
			if (dev->pes_header.ptsi & 0x02)
			{
				nibble |= 0x01;
				dev->pes_header.opt.dts[0] = (nibble << 4) | ((dev->pcr >> 30 & 0x07) << 1) | 0x1;
				dev->pes_header.opt.dts[1] = (dev->pcr >> 22 & 0xff);
				dev->pes_header.opt.dts[2] = ((dev->pcr >> 15 & 0x7f) << 1) | 0x1;
				dev->pes_header.opt.dts[3] = (dev->pcr >> 7 & 0xff);
				dev->pes_header.opt.dts[4] = ((dev->pcr & 0x7f) << 1) | 0x1;
			}
			if (dev->pes_header.ptsi & 0x01)
			{
				nibble |= 0x10;
				dev->pes_header.opt.pts[0] = (nibble << 4) | ((pcr >> 30 & 0x07) << 1) | 0x1;
				dev->pes_header.opt.pts[1] = (pcr >> 22 & 0xff);
				dev->pes_header.opt.pts[2] = ((pcr >> 15 & 0x7f) << 1) | 0x1;
				dev->pes_header.opt.pts[3] = (pcr >> 7 & 0xff);
				dev->pes_header.opt.pts[4] = ((pcr & 0x7f) << 1) | 0x1;
			}
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
		{
			ret = 0;
			length = 0;
		}
	}
	if (ret > 0)
	{
		ret = _client_filldata(dev, mtu);
		mtu -= ret;
	}
	if (ret < 0 && errno != EAGAIN)
	{
		dev->proto->close(dev->protoctx);
		err("mpegts: send error %m");
	}
	else
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

	const Proto_t *proto = &proto_udp;
	if (config && config->proto)
		proto = config->proto;
	void *protoctx = proto->create(&config->protoconf);
	if (protoctx == NULL)
		return NULL;

	Dev_t *dev = calloc(1, sizeof(*dev));
	dev->type = type;
	dev->config = config;
	dev->proto = proto;
	dev->protoctx = protoctx;

	dev->header.sync = 'G';
	dev->header.pid = config->pid + 1;
	dev->header.afi = 1;

	dev->pes_header.sync[2] = 0x01;
	dev->pes_header.str_id = 0xe0;
	dev->pes_header.mark = 0x2;
#if PES_PTSDTS_ENABLE
	dev->pes_header.ptsi = 0x3;
#endif
	dev->pes_header.hlen = sizeof(dev->pes_header.opt);

	/// setup the pat packet
	{
		memcpy(&dev->pat, default_pat, sizeof(dev->pat));
		dev->pat.pat.prog1.pmtid = config->pid & 0x00ff;
		int start = sizeof(dev->pat.header) + sizeof(dev->pat.pointer);
		/// patlen contains the pointer length + the data of psi
		/// crc is calculated on the psi (without the pointer)
		uint16_t patlen = htons(dev->pat.pat.len & 0xff0f);
		/// the bit fields structure doesn't work correctly
		patlen = ((dev->pat.raw[6] & 0x0f) << 8) | dev->pat.raw[7];
		patlen -= sizeof(dev->pat.pointer);
		uint32_t crc = crc32(&dev->pat.raw[start], patlen);
		dev->pat.pat.crc = htonl(crc);
	}
	/// setup the pmt packet
	{
		memcpy(&dev->pmt, default_pmt, sizeof(dev->pmt));
		/// default 0x03 is mpeg1l3
		if (config->parent.fourcc == FOURCC_H264)
			dev->pmt.pmt.es1.type = 0x1b;
		dev->pmt.header.pid = config->pid & 0x00ff;
		dev->pmt.pmt.progid = htons(1); /// value for the TV programm
		dev->pmt.pmt.pcr_pid = config->pid + 1;
		dev->pmt.pmt.es1.esid = config->pid + 1;
		int start = sizeof(dev->pmt.header) + sizeof(dev->pmt.pointer);
		uint16_t pmtlen = htons(dev->pmt.pmt.len & 0xff0f);
		/// the bit fields structure doesn't work correctly
		pmtlen = ((dev->pmt.raw[6] & 0x0f) << 8) | dev->pmt.raw[7];
		pmtlen -= sizeof(dev->pmt.pointer);
		uint32_t crc = crc32(&dev->pmt.raw[start], pmtlen);
		dev->pmt.pmt.crc = htonl(crc);
	}
#if PSI_SDTPACKET
	/// setup the sdt packet
	{
		memcpy(&dev->sdt, default_sdt, sizeof(dev->sdt));
		memcpy(dev->sdt.sdt.service.name, config->parent.name, 9);
		if (config->parent.fourcc == FOURCC_H264)
			dev->sdt.sdt.service.type = 0x1b;
		/// default 0x03 is mpeg1l3
		int start = sizeof(dev->sdt.header) + sizeof(dev->sdt.pointer);
		uint16_t sdtlen = htons(dev->sdt.sdt.len & 0xff0f);
		/// the bit fields structure doesn't work correctly
		sdtlen = ((dev->sdt.raw[6] & 0x0f) << 8) | dev->sdt.raw[7];
		sdtlen -= sizeof(dev->sdt.pointer);
		uint32_t crc = crc32(&dev->sdt.raw[start], sdtlen);
		dev->sdt.sdt.crc = htonl(crc);
	}
#endif
	dev->packetlen = MPEG_TS_LENGTH;

#if DUMPDATA
	dumpfd = open("/tmp/dump.h264", O_RDWR | O_CREAT);
	err("dumpfd %d %m", dumpfd);
#endif
	warn("smpegts: stream %s out to %s", dev->proto->name , config->host);
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
	if (!writer && dev->type == device_input)
	{
		int ret = dev->proto->fd(dev->protoctx);
		for (int i = 0; i < dev->nbuffers; i++)
		{
			if (dev->buffers[i].state == queued)
			{
				ret = -1;
				break;
			}
		}
		return ret;
	}
	return -1;
}

EXT_API int mpegts_queue(Dev_t *dev, int id, void *mem, size_t size, int flags)
{
	if (id < 0 || id > dev->nbuffers)
		return -1;
	FrameBuffer_t *buffer = &dev->buffers[id];
	if (buffer->state != dequeued && buffer->state != invalid)
	{
		errno = EAGAIN;
		return -1;
	}
	buffer->bytesused = size;
	buffer->flags = flags;
	if (buffer->dma_buf > 0)
	{
		if (buffer->mem)
		{
			warn("mpegts: memory already locked");
			errno = ENOMEM;
			return -1;
		}
		struct dma_buf_sync sync = { 0 };
		sync.flags = DMA_BUF_SYNC_READ | DMA_BUF_SYNC_START;
		ioctl(buffer->dma_buf, DMA_BUF_IOCTL_SYNC, &sync);
		mem = mmap(NULL, buffer->size, PROT_READ, MAP_SHARED, buffer->dma_buf, 0 );
	}
	if (mem)
	{
		buffer->mem = mem;
	}

	buffer->state = queued;
	if (_client_pushdata(dev, id) < 0)
	{
		return -1;
	}
	return 0;
}

EXT_API int mpegts_dequeue(Dev_t *dev, void **mem, size_t *bytesused, int *flags)
{
	int id = 0;
	for (int i = 0; i < dev->nbuffers; i++)
	{
		id = dev->currentid + i;
		id %= dev->nbuffers;
		if (dev->buffers[id].state == ready)
			break;
		id = -1;
	}
	if (id == -1)
	{
		errno = EAGAIN;
		return -1;
	}
	FrameBuffer_t *buffer = NULL;
	buffer = &dev->buffers[id];
	dev->currentid = id;

	if (buffer->state == ready)
	{
		_client_flushdata(dev, id);
		dev->frames++;
		if (dev->frames == dev->config->maxframes)
		{
			dev->frames = 0;
			dev->proto->close(dev->protoctx);
			dev->proto->connect(dev->protoctx);
		}
		if (dev->frames == UINT32_MAX)
			dev->frames = 0;
	}
	if ((buffer->state == dequeued) && (buffer->dma_buf > 0))
	{
		struct dma_buf_sync sync = { 0 };
		munmap(buffer->mem, buffer->size);
		buffer->mem = NULL;
		sync.flags = DMA_BUF_SYNC_READ | DMA_BUF_SYNC_END;
		ioctl(buffer->dma_buf, DMA_BUF_IOCTL_SYNC, &sync);
	}
	if (buffer->state == queued)
	{
		errno = EAGAIN;
		return -1;
	}
	if (*mem)
		*mem = buffer->mem;
	if (*bytesused)
		*bytesused = buffer->size;
	return id;
}

EXT_API int mpegts_start(Dev_t *dev)
{
	dev->currentid = 0;
	return dev->proto->connect(dev->protoctx);
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
	json_t *periodic = json_object_get(jconfig, "periodic");
	if (periodic && json_is_integer(periodic))
	{
		int value = json_integer_value(periodic);
		config->periodic = value;
	}
	json_t *maxframes = json_object_get(jconfig, "maxframes");
	if (maxframes && json_is_integer(maxframes))
	{
		uint32_t value = json_integer_value(maxframes);
		config->maxframes = value;
	}
	json_t *maxclients = json_object_get(jconfig, "maxclients");
	if (maxclients && json_is_integer(maxclients))
	{
		uint32_t value = json_integer_value(maxclients);
		config->maxclients = value;
	}
	json_t *proto = json_object_get(jconfig, "proto");
	if (proto == NULL)
		proto = json_object_get(jconfig, "protocol");
	if (proto && json_is_string(proto))
	{
		const char *value = json_string_value(proto);
		for (int i = 0; i < (sizeof(_protos)/sizeof(*_protos)); i++)
		{
			if (_protos[i] && !strcasecmp(value, _protos[i]->name))
			{
				config->proto = _protos[i];
				break;
			}
		}
	}
library_end:
	return 0;
}
#endif

const FastVideoDevice_ops_t smpegts_ops = {
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
