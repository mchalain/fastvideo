#ifndef __SDVB_H__
#define __SDVB_H__

#include "fastvideo.h"
#include "sconfig.h"

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

extern FastVideoDevice_ops_t sdvb_ops;

#endif
