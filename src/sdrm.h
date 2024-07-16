#ifndef __SDRM_H__
#define __SDRM_H__

#include "config.h"

#define DISPLAYCONFIG(name, defaultdevice) name = { \
	.DEVICECONFIG(parent, sdrm_loadjsonconfiguration, sdrm_loadjsonsettings), \
	.device = defaultdevice, \
	}

typedef struct DisplayConf_s DisplayConf_t;
struct DisplayConf_s
{
	DeviceConf_t parent;
	const char *device;
	uint32_t fourcc;
	int mode;
	uint32_t width;
	uint32_t height;
};

typedef struct Display_s Display_t;

Display_t *sdrm_create(const char *name, DisplayConf_t *config);
int sdrm_requestbuffer(Display_t *dev, enum buf_type_e t, ...);
int sdrm_fd(Display_t *disp);
int sdrm_queue(Display_t *disp, int id);
int sdrm_dequeue(Display_t *disp);
int sdrm_start(Display_t *disp);
int sdrm_stop(Display_t *disp);
void sdrm_destroy(Display_t *disp);

#ifdef HAVE_JANSSON
int sdrm_loadjsonsettings(void *dev, void *jconfig);
int sdrm_loadjsonconfiguration(void *config, void *jconfig);
#else
#define sdrm_loadjsonsettings NULL
#define sdrm_loadjsonconfiguration NULL
#endif

#endif
