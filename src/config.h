#ifndef __FASTVIDEO_CONFIG_H__
#define __FASTVIDEO_CONFIG_H__

#ifdef HAVE_JANSSON
# include <jansson.h>
#endif

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
	buf_type_sv4l2_master = 1 |  0x80,
	buf_type_memory_master = 2 |  0x80,
	buf_type_dmabuf_master = 3 |  0x80,
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
	uint32_t fps;
	uint64_t modifiers;
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
typedef struct FastVideoDevice_ops_s FastVideoDevice_ops_t;
DeviceConf_t *config_create(const char *name, FastVideoDevice_ops_t *ops, void *entry);

int config_loaddefinition(DeviceConf_t *config, json_t *definition);
int config_mergedefinition(DeviceConf_t *dest, DeviceConf_t *src);

int config_isnamed(DeviceConf_t *devconfig, const char *name);

int config_parseconfigfile(const char *configfile, int (*loaddevice)(void *data, const char *name, const char *type, void *config), void *data);
json_t *config_getdevices(json_t *jconfig);
int config_loaddevice(json_t *jconfig, int (*cb)(void *data, const char *name, const char *type, void *config), void *data);

/**
 * default configuration callback for the devices
 */
int scommon_loadconfiguration(void *arg, void *entry);

int scommon_isnamed(json_t *jdevice, const char *name);
#else
inline int config_parseconfigfile(const char *name, const char *configfile, DeviceConf_t *devconfig) {return -1;};
#endif

#endif
