#ifndef __SPASSTROUGHT_H__
#define __SPASSTROUGHT_H__

#include "fastvideo.h"
#include "sconfig.h"

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

#define STATE_SHOOT 0x01
#define STATE_TEE 0x02
#define STATE_DRYRUN 0x04
struct Passthrough_Control_s
{
	int state;
	uint32_t periodic;
};
typedef struct Passthrough_Control_s Passthrough_Control_t;

typedef struct Passthrough_s Passthrough_t;

struct Convert_s
{
	const char *name;
	union {
		struct {
			int reserved:15;
			int copy:1;
		};
		short int mode;
	};
	uint32_t fourcc_in;
	uint32_t fourcc_out;
	/*
	 * bytes per input pixel - nonzero marks this converter as
	 * stride-aware, i.e. safe for a caller like stile.c to invoke once
	 * per tile row (stride == size, a single-row degenerate case) instead
	 * of once for a whole frame. A converter that still derives its own
	 * row geometry from ctx (e.g. GRAYtoYUV pulling width/height back out
	 * of its own config) must leave this 0 - that's the only signal
	 * stile.c has to tell candidates apart, there is no separate bit.
	 */
	uint32_t bpp;
	struct{
		unsigned int numerator;
		unsigned int denominator;
	} resize;
	struct {
		void *(*create)(Passthrough_config_t *);
		/*
		 * size = total bytes to process, stride = bytes per row
		 * (nrows = size/stride, must divide evenly). A whole-frame call
		 * (spassthrough's own usage) passes the real per-row stride so a
		 * stride-aware converter (.bpp != 0, e.g. BG10toR16) can still
		 * alternate per-row state (Bayer coefficients) correctly; a
		 * single-row call (stile.c, one tile row at a time) passes
		 * stride == size, which every stride-aware converter must treat
		 * as "exactly one row, do it and return" with no other state.
		 */
		size_t (*convert)(void *, const char *const , char *, size_t size, size_t stride);
		void (*destroy)(void *);
	} ops;
};
typedef void (*spassthrough_convert_append_t)(Convert_t *convert);
void spassthrough_convert_append(Convert_t *convert);
Convert_t *spassthrough_convert_next(Convert_t *convert);

extern FastVideoDevice_ops_t spassthrough_ops;
#endif
