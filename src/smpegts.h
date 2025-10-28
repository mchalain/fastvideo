#ifndef __SMPEGTS_H__
#define __SMPEGTS_H__

#ifndef MSG_MORE
#define MSG_MORE 0x8000
#endif

typedef struct Proto_s Proto_t;

typedef struct MPEG_TSConf_s MPEG_TSConf_t;
struct MPEG_TSConf_s
{
	DeviceConf_t parent;
	const char *host;
	int port;
	int pid;
	int periodic;
	int maxclients;
	uint32_t maxframes;
	Proto_t *proto;
};

struct Proto_s
{
	void *(*create)(MPEG_TSConf_t *config);
	int (*connect)(void *arg);
	void (*close)(void *arg);
	size_t (*mtu)(void *arg);
	int (*fd)(void *arg);
	ssize_t (*send)(void *arg, const void *buf, size_t len, int flags);
	void (*flush)(void *arg);
	void (*destroy)(void *arg);
};

extern Proto_t proto_udp;
extern Proto_t proto_unix;
extern Proto_t proto_file;
#endif
