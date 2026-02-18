#ifndef __SMPEGTS_H__
#define __SMPEGTS_H__

#ifndef MSG_MORE
#define MSG_MORE 0x8000
#endif

typedef struct Proto_s Proto_t;

typedef struct MPEG_TSConf_s MPEG_TSConf_t;
struct MPEG_TSConf_s
{
	union {
		struct {
			DeviceConf_t parent;
			char *host;
			int port;
			int maxclients;
			const char *mode;
		};
		Proto_Config_t protoconf;
	};
	int pid;
	int periodic;
	uint32_t maxframes;
	const Proto_t *proto;
};

extern const Proto_t proto_udp;
#endif
