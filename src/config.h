#ifndef __FASTVIDEO_CONFIG_H__
#define __FASTVIDEO_CONFIG_H__

#ifdef HAVE_JANSSON
# include <jansson.h>
#endif

#ifndef FOURCC
#define FOURCC(a,b,c,d)	((a << 0) | (b << 8) | (c << 16) | (d << 24))
#endif

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
enum buf_type_e
{
	buf_type_sv4l2 = 1,
	buf_type_memory = 2,
	buf_type_dmabuf = 3,
	buf_type_master = 0x80,
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
int config_parseconfigfile(const char *name, const char *configfile, DeviceConf_t *devconfig);
#else
inline int config_parseconfigfile(const char *name, const char *configfile, DeviceConf_t *devconfig) {return -1;};
#endif

#endif
