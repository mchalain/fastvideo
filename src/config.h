#ifndef __FASTVIDEO_CONFIG_H__
#define __FASTVIDEO_CONFIG_H__

#ifdef HAVE_JANSSON
# include <jansson.h>
#endif

#ifndef FOURCC
#define FOURCC(a,b,c,d)	((a << 0) | (b << 8) | (c << 16) | (d << 24))
#endif

#define FOURCC_AB24		FOURCC('A','B','2','4')
#define FOURCC_XB24		FOURCC('X','B','2','4')
#define FOURCC_AR24		FOURCC('A','R','2','4')
#define FOURCC_XR24		FOURCC('X','R','2','4')
#define FOURCC_BG24		FOURCC('B','G','2','4')
#define FOURCC_RG24		FOURCC('R','G','2','4')
#define FOURCC_RGBA		FOURCC('R','G','B','A')
#define FOURCC_RGBP		FOURCC('R','G','B','P')
#define FOURCC_RG16		FOURCC('R','G','1','6')
#define FOURCC_R8		FOURCC('R','8',' ',' ')
#define FOURCC_YUYV		FOURCC('Y','U','Y','V')
#define FOURCC_YUY2		FOURCC('Y','U','Y','2')
#define FOURCC_U008		FOURCC('U','0','0','8')
#define FOURCC_JPEG		FOURCC('J','P','E','G')
#define FOURCC_MJPG		FOURCC('M','J','P','G')
#define FOURCC_H264		FOURCC('H','2','6','4')

#define BPP_TO_BYTE(_bpp)	(((_bpp) + 7) / 8)

/**
 * @brief buffers to communicate with another V4L2_t object.
 *
 * @param (buf_type_sv4l2 | buf_type_master) creates a master object.
 * @param buf_type_sv4l2 creates a slave object, it needs a master object as argument.
 * @param (buf_type_memory | buf_type_master) creates a master memory sharing.
 * @param buf_type_memory not yet supported.
 *  - int nmem the number of buffers
 *  - void *mems a table of memory pointers to use
 *  - size_t size the size of each memory spaces
 * @param (buf_type_dmabuf | buf_type_master) creates a master object.
 * @param buf_type_dmabuf creates a slave object, it needs 3 arguments:
 * 	- int nbuffers the number of buffers
 *  - int buffers[*] a table of dmabuf to use
 *  - size_t size the size of each dmabuf
 */
typedef enum buf_type_e
{
	buf_type_sv4l2 = 1,
	buf_type_memory = 2,
	buf_type_dmabuf = 3,
	buf_type_master = 0x80,
} buf_type_e;

typedef enum device_type_e
{
	device_input,
	device_output,
	device_transfer,
	device_control,
} device_type_e;

typedef struct ImageDefinition_s ImageDefinition_t;
struct ImageDefinition_s
{
	uint32_t fourcc;
	uint32_t width;
	uint32_t height;
	uint32_t stride;
};

typedef struct DeviceConf_s DeviceConf_t;
struct DeviceConf_s
{
	void *dev;
#ifdef HAVE_JANSSON
	json_t *entry;
#else
	void *entry;
#endif
	const char *name;
	const char *type;
	uint32_t fourcc;
	uint32_t width;
	uint32_t height;
	uint32_t stride;
	struct
	{
		int (*loadconfiguration)(void *storage, void *config);
	} ops;
};

#define DEVICECONFIG(_config, _name, _loadconfig) \
	_config = { \
		.name = #_name, \
		.dev = 0, \
		.ops.loadconfiguration = _loadconfig, \
	}

#ifdef HAVE_JANSSON
/**
 * this function is currently defined inside sv4l2.c
 */
int scommon_loaddefinition(DeviceConf_t *config, json_t *definition);
int scommon_parsedevices(const char *name, json_t *jconfig, DeviceConf_t *devconfig);

int config_parseconfigfile(const char *configfile, int (*loaddevice)(void *data, const char *name, const char *type, void *config), void *data);
json_t *config_getdevices(json_t *jconfig);
int config_loaddevice(json_t *jconfig, int (*cb)(void *data, const char *name, const char *type, void *config), void *data);

json_t *scommon_getdevice(const char *name);
int scommon_isnamed(json_t *jdevice, const char *name);
#else
inline int config_parseconfigfile(const char *name, const char *configfile, DeviceConf_t *devconfig) {return -1;};
#endif

/**
 * @brief share a same definition of buffer for the devices
 */
typedef struct FrameBuffer_s FrameBuffer_t;
struct FrameBuffer_s
{
	int id;
	void *mem;
	size_t offset;
	int dma_buf;
	size_t size;
	size_t bytesused;
	int flags;
	enum {
		invalid,
		dequeued,
		ready,
		queued,
	} state;
	void *private;
	FrameBuffer_t *next;
};

#endif
