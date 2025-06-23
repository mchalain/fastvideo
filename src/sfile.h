#ifndef __SFILE_H__
#define __SFILE_H__

#include <stdint.h>
#include "fastvideo.h"
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

#ifdef HAVE_JANSSON
int sfile_loadjsonconfiguration(void *arg, void *entry);

# define sfile_loadconfiguration sfile_loadjsonconfiguration
#else
# define sfile_loadconfiguration NULL
#endif

typedef void *(*File_ops_open_t)(int atfd, const char *name, int mode);
typedef int (*File_ops_fd_t)(File_t *dev);
typedef ssize_t (*File_ops_read_t)(File_t *dev, void *mem, size_t size);
typedef ssize_t (*File_ops_write_t)(File_t *dev, void *mem, size_t size);
typedef void (*File_ops_close_t)(File_t *dev);

typedef struct File_ops_s File_ops_t;
struct File_ops_s
{
	const char *name;
	File_ops_open_t open;
	File_ops_fd_t fd;
	File_ops_read_t read;
	File_ops_write_t write;
	File_ops_close_t close;
};
extern File_ops_t _passthrough_ops;

extern FastVideoDevice_ops_t sfile_ops;
#endif
