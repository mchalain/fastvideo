#ifndef __SPASSTROUGHT_H__
#define __SPASSTROUGHT_H__

#include "fastvideo.h"
#include "config.h"

typedef struct Passthrough_config_s Passthrough_config_t;
typedef struct Convert_s Convert_t;
struct Passthrough_config_s
{
	DeviceConf_t parent;
	DeviceConf_t transfer;
	int mode;
	DeviceConf_t branch;
	Convert_t *convert;
	void *libraryhdl;
};

typedef struct Passthrough_s Passthrough_t;

struct Convert_s
{
	const char *name;
	union {
		struct {
			int reserved:3;
			int copy:1;
		};
		int mode;
	};
	uint32_t fourcc_in;
	uint32_t fourcc_out;
	struct{
		uint numerator;
		uint denominator;
	} resize;
	struct {
		void *(*create)(Passthrough_config_t *);
		size_t (*convert)(void *, const char *const , char *, size_t);
		void (*destroy)(void *);
	} ops;
};
typedef void (*spassthrough_convert_append_t)(Convert_t *convert);
void spassthrough_convert_append(Convert_t *convert);

extern FastVideoDevice_ops_t spassthrough_ops;
#endif
