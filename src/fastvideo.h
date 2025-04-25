#ifndef __FASTVIDEO_DEVICE_H__
#define __FASTVIDEO_DEVICE_H__

#include <stdint.h>

#include "config.h"

typedef DeviceConf_t * (*FastVideoDevice_createconfig_t)(void);
typedef void *(*FastVideoDevice_create_t)(const char *devicename, device_type_e type, DeviceConf_t *config);
typedef void *(*FastVideoDevice_duplicate_t)(void *dev, DeviceConf_t **pconfig);
typedef int (*FastVideoDevice_loadsettings_t)(void *dev, void *configentry);
typedef int (*FastVideoDevice_capabilities_t)(void *dev, void *capabilities, int all);
typedef int (*FastVideoDevice_requestbuffer_t)(void *dev, enum buf_type_e t, ...);
typedef int (*FastVideoDevice_eventfd_t)(void *dev);
typedef int (*FastVideoDevice_start_t)(void *dev);
typedef int (*FastVideoDevice_stop_t)(void *dev);
typedef int (*FastVideoDevice_dequeue_t)(void *dev, void **mem, size_t *bytesused);
typedef int (*FastVideoDevice_queue_t)(void *dev, int index, size_t bytesused);
typedef void (*FastVideoDevice_destroy_t)(void *dev);

typedef struct FastVideoDevice_ops_s FastVideoDevice_ops_t;
struct FastVideoDevice_ops_s
{
	const char *name;
	FastVideoDevice_createconfig_t createconfig;
	FastVideoDevice_create_t create;
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

typedef void (*fastvideodevice_ops_append_t)(FastVideoDevice_ops_t *ops);
void fastvideodevice_ops_append(FastVideoDevice_ops_t *ops);
FastVideoDevice_ops_t *fastvideodevice_ops_next(FastVideoDevice_ops_t *ops);

typedef struct FastVideoList_s FastVideoList_t;

FastVideoList_t *fastvideolist_append(FastVideoList_t *list, void *device);
FastVideoList_t *fastvideolist_insert(FastVideoList_t *list, void *device);
FastVideoList_t *fastvideolist_last(FastVideoList_t *list);
void *fastvideolist_next(FastVideoList_t *list);
void *fastvideolist_previous(FastVideoList_t *list);
int fastvideolist_islast(FastVideoList_t *list, void *entity);
int fastvideolist_isfirst(FastVideoList_t *list, void *entity);

#endif
