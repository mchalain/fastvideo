#ifndef __SDVB_H__
#define __SDVB_H__

#include "config.h"

typedef struct DVB_s DVB_t;

typedef enum
{
	DVB_T,
	DVB_S,
	DVB_N,
} DVB_TYPE_e;
typedef struct DVBConfig_s DVBConfig_t;
struct DVBConfig_s
{
	DeviceConf_t parent;
	const char *device;
	DVB_TYPE_e type;
	uint16_t pid;
};

DVB_t *sdvb_create(const char *devicename, device_type_e type, DVBConfig_t *config);
DVB_t *sdvb_create2(int fd, const char *devicename, device_type_e type, DVBConfig_t *config);
int sdvb_requestbuffer(DVB_t *dev, enum buf_type_e t, ...);
int sdvb_fd(DVB_t *dev);
int sdvb_start(DVB_t *dev);
int sdvb_stop(DVB_t *dev);
int sdvb_dequeue(DVB_t *dev, void **mem, size_t *bytesused);
int sdvb_queue(DVB_t *dev, int index, size_t bytesused);
void sdvb_destroy(DVB_t *dev);
DeviceConf_t * sdvb_createconfig();

#ifdef HAVE_JANSSON
int sdvb_loadjsonsettings(DVB_t *dev, void *jconfig);
int sdvb_loadjsonconfiguration(void *config, void *jconfig);
#define sdvb_loadsettings sdvb_loadjsonsettings
#define sdvb_loadconfiguration sdvb_loadjsonconfiguration
#else
#define sdvb_loadsettings NULL
#define sdvb_loadconfiguration NULL
#endif

extern FastVideoDevice_ops_t sdvb_ops;

#endif
