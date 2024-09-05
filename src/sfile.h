#ifndef __SFILE_H__
#define __SFILE_H__

#include <stdint.h>
#include "config.h"

#define FILECONFIG(config, ...) config = { \
	.DEVICECONFIG(parent, config, sfile_loadconfiguration), \
	}

typedef struct FileConfig_s FileConfig_t;
struct FileConfig_s
{
	DeviceConf_t parent;
	const char *rootpath;
	const char *filename;
	enum
	{
		File_Input_e = 0x01,
		File_Output_e = 0x02,
	} direction;
};

typedef struct File_s File_t;

DeviceConf_t * sfile_createconfig();

File_t * sfile_create(const char *name, device_type_e type, FileConfig_t *config);
int sfile_requestbuffer(File_t *dev, enum buf_type_e t, ...);
int sfile_fd(File_t *dev);
int sfile_start(File_t *dev);
int sfile_stop(File_t *dev);
int sfile_dequeue(File_t *dev, void **mem, size_t *bytesused);
int sfile_queue(File_t *dev, int index, size_t bytesused);
void sfile_destroy(File_t *dev);

#ifdef HAVE_JANSSON
int sfile_loadjsonconfiguration(void *arg, void *entry);

# define sfile_loadconfiguration sfile_loadjsonconfiguration
#else
# define sfile_loadconfiguration NULL
#endif
#endif
