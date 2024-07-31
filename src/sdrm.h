#ifndef __SDRM_H__
#define __SDRM_H__

#include "config.h"

#define DISPLAYCONFIG(name, defaultdevice) name = { \
	.DEVICECONFIG(parent, sdrm_loadconfiguration), \
	.device = defaultdevice, \
	}

typedef struct DisplayConf_s DisplayConf_t;
struct DisplayConf_s
{
	DeviceConf_t parent;
	const char *device;
	int mode;
};

typedef struct Display_s Display_t;

DeviceConf_t * sdrm_createconfig();

Display_t *sdrm_create(const char *name, DisplayConf_t *config);
int sdrm_requestbuffer(Display_t *dev, enum buf_type_e t, ...);
int sdrm_fd(Display_t *disp);
int sdrm_queue(Display_t *disp, int id);
int sdrm_dequeue(Display_t *disp, void **mem, size_t *bytesused);
int sdrm_start(Display_t *disp);
int sdrm_stop(Display_t *disp);
void sdrm_destroy(Display_t *disp);

#ifdef HAVE_JANSSON
int sdrm_loadjsonsettings(void *dev, void *jconfig);

int sdrm_loadjsonconfiguration(void *config, void *jconfig);

#define sdrm_loadsettings sdrm_loadjsonsettings
#define sdrm_loadconfiguration sdrm_loadjsonconfiguration
#else
#define sdrm_loadsettings NULL
#define sdrm_loadconfiguration NULL
#endif

#endif
