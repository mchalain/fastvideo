#ifndef __SFILE_H__
#define __SFILE_H__

#include <stdint.h>
#include "fastvideo.h"
#include "config.h"

#define FILECONFIG(config, ...) config = { \
	.DEVICECONFIG(parent, config, sfile_loadconfiguration), \
	}

typedef struct File_ops_s File_ops_t;
typedef struct FileConfig_s FileConfig_t;
struct FileConfig_s
{
	DeviceConf_t parent;
	const char *rootpath;
	const char *filename;
	enum {
		File_Regular_e = 0,
		File_Fifo_e,
		File_Socket_e,
	} type;
	enum {
		File_None_e = 0,
		File_TIFF_e,
	} header;
	enum
	{
		File_Input_e = 0x01,
		File_Output_e = 0x02,
	} direction;
};

typedef struct File_s File_t;
struct File_s
{
	FileConfig_t *config;
	const char *path;
	void *ctx;
	File_ops_t *ops;
	device_type_e type;
	uint32_t fourcc;
	size_t nbuffers;
	FrameBuffer_t *buffers;
	int lastbufferid;
	char header[128];
	size_t headerlen;
};

#ifdef HAVE_JANSSON
int sfile_loadjsonconfiguration(void *arg, void *entry);

# define sfile_loadconfiguration sfile_loadjsonconfiguration
#else
# define sfile_loadconfiguration NULL
#endif

typedef void *(*File_ops_open_t)(int atfd, const char *name, device_type_e mode);
typedef int (*File_ops_fd_t)(void *arg);
typedef ssize_t (*File_ops_read_t)(void *arg, void *mem, size_t size);
typedef ssize_t (*File_ops_write_t)(void *arg, void *mem, size_t size);
typedef void (*File_ops_close_t)(void *arg);

struct File_ops_s
{
	const char *name;
	File_ops_open_t open;
	File_ops_fd_t fd;
	File_ops_read_t read;
	File_ops_write_t write;
	File_ops_close_t close;
};
extern File_ops_t _regular_ops;
extern File_ops_t _fifo_ops;

extern FastVideoDevice_ops_t sfile_ops;
#endif
