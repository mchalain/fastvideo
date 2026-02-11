#ifndef __FASTVIDEO_DEVICE_H__
#define __FASTVIDEO_DEVICE_H__

#include <stdint.h>

#include "config.h"
#include "sformats.h"

#define EXT_API static

typedef struct FastVideoDevice_ops_s FastVideoDevice_ops_t;
typedef struct FastVideoDevice_s FastVideoDevice_t;
struct FastVideoDevice_s
{
	DeviceConf_t *config;
	const char *name;
	int id;
	void *dev;
	FastVideoDevice_ops_t *ops;
};

typedef DeviceConf_t * (*FastVideoDevice_createconfig_t)(const char *name);
typedef void *(*FastVideoDevice_create_t)(const char *devicename, device_type_e type, DeviceConf_t *config);
typedef void *(*FastVideoDevice_create2_t)(int fd, const char *name, device_type_e type, DeviceConf_t *config);
typedef void *(*FastVideoDevice_duplicate_t)(void *dev, DeviceConf_t **pconfig);
typedef int (*FastVideoDevice_loadsettings_t)(void *dev, void *configentry);
typedef int (*FastVideoDevice_capabilities_t)(void *dev, void *capabilities, int all);
typedef int (*FastVideoDevice_requestbuffer_t)(void *dev, enum buf_type_e t, ...);
typedef int (*FastVideoDevice_eventfd_t)(void *dev, int writer);
typedef int (*FastVideoDevice_start_t)(void *dev);
typedef int (*FastVideoDevice_stop_t)(void *dev);
typedef int (*FastVideoDevice_dequeue_t)(void *dev, void **mem, size_t *bytesused, int *flags);
typedef int (*FastVideoDevice_queue_t)(void *dev, int index, void *mem, size_t bytesused, int flags);
typedef void (*FastVideoDevice_destroy_t)(void *dev);

struct FastVideoDevice_ops_s
{
	const char *name;
	FastVideoDevice_createconfig_t createconfig;
	FastVideoDevice_create_t create;
	FastVideoDevice_create2_t create2;
	FastVideoDevice_duplicate_t duplicate;
	FastVideoDevice_loadsettings_t loadsettings;
	FastVideoDevice_capabilities_t capabilities;
	FastVideoDevice_requestbuffer_t requestbuffer;
	FastVideoDevice_eventfd_t eventfd;
	FastVideoDevice_start_t start;
	FastVideoDevice_stop_t stop;
	FastVideoDevice_dequeue_t dequeue;
	FastVideoDevice_queue_t queue;
	FastVideoDevice_destroy_t destroy;
};

typedef void (*fastvideodevice_ops_append_t)(const FastVideoDevice_ops_t *ops);
void fastvideodevice_ops_append(const FastVideoDevice_ops_t *ops);
FastVideoDevice_ops_t *fastvideodevice_ops_next(FastVideoDevice_ops_t *ops);

typedef struct FastVideoList_s FastVideoList_t;

FastVideoList_t *fastvideolist_append(FastVideoList_t *list, void *device);
FastVideoList_t *fastvideolist_insert(FastVideoList_t *list, void *device);
void fastvideolist_reset(FastVideoList_t *list);
FastVideoList_t *fastvideolist_first(FastVideoList_t *list);
FastVideoList_t *fastvideolist_last(FastVideoList_t *list);
FastVideoList_t *fastvideolist_current(FastVideoList_t *list);
FastVideoList_t *fastvideolist_up(FastVideoList_t *list);
FastVideoList_t *fastvideolist_down(FastVideoList_t *list);
FastVideoList_t *fastvideolist_push(FastVideoList_t *list, FastVideoList_t *newentry);
FastVideoList_t *fastvideolist_poplast(FastVideoList_t *list, FastVideoList_t **entry);
FastVideoList_t *fastvideolist_pop(FastVideoList_t *list, FastVideoList_t **entry);
void *fastvideolist_get(FastVideoList_t *list);
void *fastvideolist_next(FastVideoList_t *list);
void *fastvideolist_previous(FastVideoList_t *list);
int fastvideolist_islast(FastVideoList_t *list, void *entity);
int fastvideolist_isfirst(FastVideoList_t *list, void *entity);
void fastvideolist_destroy(FastVideoList_t *list, void(*destroy)(void *));

/**
 * @brief share a same definition of buffer for the devices
 */
enum
{
	FB_FLAGS_KEYFRAME = 0x00000008, /// V4L2_BUF_FLAG_KEYFRAME
	FB_FLAGS_MODIFIER = 0x00000001,
};

typedef enum {
	invalid,
	dequeued,
	ready,
	queued,
} FrameBuffer_state_e;

typedef struct FrameBuffer_s FrameBuffer_t;
struct FrameBuffer_s
{
	int id;
	void *mem;
	off_t map_offset;
	int dma_buf;
	int nplanes;
	struct {
		size_t size;
		uint32_t strides[4];
		uint32_t offsets[4];
		int bpp;
	};
	size_t bytesused;
	struct {
		uint32_t width;
		uint32_t height;
	};
	int flags;
	FrameBuffer_state_e state;
	void *private;
	FrameBuffer_t *next;
};

typedef struct Proto_Config_s Proto_Config_t;
struct Proto_Config_s
{
	DeviceConf_t parent;
	const char *host;
	int port;
	int maxclients;
	const char *mode;
};

typedef enum Proto_Flags_e Proto_Flags_t;
enum Proto_Flags_e
{
	Proto_Flush = 0,
	Proto_More,
};

typedef struct Proto_s Proto_t;
struct Proto_s
{
	const char *name;
	void *(*create)(Proto_Config_t *config);
	int (*connect)(void *arg);
	void (*close)(void *arg);
	size_t (*mtu)(void *arg);
	int (*fd)(void *arg);
	ssize_t (*send)(void *arg, const void *buf, size_t len, Proto_Flags_t flags);
	ssize_t (*recv)(void *arg, void *buf, size_t len, Proto_Flags_t flags);
	void (*flush)(void *arg);
	void (*destroy)(void *arg);
};

typedef void (*fastvideo_proto_append_t)(const Proto_t *proto);
extern const Proto_t *_protos[10];
#endif
