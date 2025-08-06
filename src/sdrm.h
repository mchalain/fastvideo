#ifndef __SDRM_H__
#define __SDRM_H__

#include "fastvideo.h"
#include "config.h"

#define DISPLAYCONFIG(name, defaultdevice) name = { \
	.DEVICECONFIG(parent, sdrm_loadconfiguration), \
	.device = defaultdevice, \
	}

typedef struct DisplayConf_s DisplayConf_t;
struct DisplayConf_s
{
	DeviceConf_t parent;
	DeviceConf_t transfer;
	const char *device;
	int mode;
};

typedef struct Display_s Display_t;

#ifdef HAVE_JANSSON
int sdrm_loadjsonsettings(void *dev, void *jconfig);

int sdrm_loadjsonconfiguration(void *config, void *jconfig);

#define sdrm_loadsettings sdrm_loadjsonsettings
#define sdrm_loadconfiguration sdrm_loadjsonconfiguration
#else
#define sdrm_loadsettings NULL
#define sdrm_loadconfiguration NULL
#endif

extern FastVideoDevice_ops_t sdrm_ops;
#endif
