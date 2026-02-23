#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/un.h>
#include <net/if.h>
#include <fcntl.h>

#include <pthread.h>

#include "fastvideo.h"
#include "config.h"
#include "smpegts.h"
#include "log.h"

#define IP_HEADER_LENGTH 20
#define TCP_HEADER_LENGTH 60

#define UNIX_PACKETIZER 1

typedef struct Client_s Client_t;
struct Client_s
{
	int fd;
	enum {
		e_connected,
		e_started,
		e_slow,
	} state;
};

typedef struct Proto_UNIX_s Proto_UNIX_t;
struct Proto_UNIX_s
{
	Proto_Config_t *config;
	pthread_t thread;
	int serverfd;
	size_t mtu;
	FastVideoList_t *clients;
	FastVideoList_t *clientspool;
	unsigned char *packet;
	size_t offset;
};

static void *proto_create(Proto_Config_t *config)
{
	int sock = 0;
	size_t mtu = 1500;
	int family = AF_UNIX;

	sock = socket(family, SOCK_STREAM, 0);
	if (sock < 0)
		return NULL;

	if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (void *)&(int){ 1 }, sizeof(int)) < 0)
			warn("unix: setsockopt(SO_REUSEADDR) failed");
#ifdef SO_REUSEPORT
	if (setsockopt(sock, SOL_SOCKET, SO_REUSEPORT, (void *)&(int){ 1 }, sizeof(int)) < 0)
			warn("unix: setsockopt(SO_REUSEPORT) failed");
#endif

	struct sockaddr_un addr;
	memset(&addr, 0, sizeof(struct sockaddr_un));
	addr.sun_family = AF_UNIX;
	snprintf(addr.sun_path, sizeof(addr.sun_path) - 1, "%s", config->host);
	unlink(addr.sun_path);

	int status = bind(sock, (struct sockaddr *) &addr, sizeof(addr));
	chmod(addr.sun_path, 0777);

	if (status)
	{
		err("unix: network error %m");
		close(sock);
		return NULL;
	}

	struct ifreq ifr;
	memset(&ifr, 0, sizeof(ifr));
	ifr.ifr_addr.sa_family = family;
	if (ioctl(sock, SIOCGIFMTU, &ifr) != -1)
		mtu = ifr.ifr_mtu;

	Proto_UNIX_t *proto = calloc(1, sizeof(*proto));
	proto->config = config;
	proto->mtu = mtu - IP_HEADER_LENGTH - TCP_HEADER_LENGTH; /// maxsize of tcp/ip header
	proto->mtu = 7 * 188 + 1;
	warn("unix: unix to %s (mtu %zu)", config->host, proto->mtu);
	proto->serverfd = sock;
	for (int i = 0 ; i < config->maxclients; i++)
	{
		Client_t *clt = calloc(1, sizeof(*clt));
		proto->clientspool = fastvideolist_append(proto->clientspool, clt);
	}
#if UNIX_PACKETIZER
	proto->packet = malloc(proto->mtu);
#endif
	return proto;
}

static void *proto_thread(void *arg)
{
	Proto_UNIX_t *proto = (Proto_UNIX_t *)arg;
	int sock = 0;
	dbg("unix: server running %d", proto->serverfd);
	while ((sock = accept(proto->serverfd, NULL, NULL)) > 0)
	{
		if (sock > 0)
		{
			dbg("unix: new client");
			int flags;
			flags = fcntl(sock, F_GETFL, 0);
			fcntl(sock, F_SETFL, flags | O_NONBLOCK);

			FastVideoList_t *client = NULL;
			proto->clientspool = fastvideolist_poplast(proto->clientspool, &client);
			if (client)
			{
				Client_t *clt = fastvideolist_get(client);
				if (clt)
				{
					clt->fd = sock;
					proto->clients = fastvideolist_push(proto->clients, client);
					dbg("unix: client registered");
				}
			}
		}
		else if (!proto->serverfd)
			break;
	}
	dbg("unix: accept end %m");
	return NULL;
}

static int proto_connect(void *arg)
{
	Proto_UNIX_t *proto = (Proto_UNIX_t *)arg;
	if (proto->serverfd == -1)
		return -1;
	dbg("unix: max %d clients", proto->config->maxclients);
	if (listen(proto->serverfd, proto->config->maxclients))
		return -1;
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
	int ret = pthread_create(&(proto->thread), &attr, proto_thread, proto);
	if (ret)
	{
		err("unix: server internal error (%d)", ret);
		return -1;
	}
	return 0;
}

static ssize_t proto_send(void *arg, const void *buf, size_t len, Proto_Flags_t pflags)
{
	Proto_UNIX_t *proto = (Proto_UNIX_t *)arg;
	ssize_t ret = 0;

#if UNIX_PACKETIZER
	errno = 0;
	if (proto->offset + len >= proto->mtu)
	{
		len = proto->mtu - proto->offset;
		pflags &= ~Proto_More;
	}
	memcpy(proto->packet + proto->offset, buf, len);
	proto->offset += len;
	ret = len;
	if ((pflags & Proto_More) == 0)
#endif
	{
		int flags = MSG_NOSIGNAL;
#if UNIX_PACKETIZER
		flags |= MSG_DONTWAIT;
#else
		if (pflags & Proto_More)
			flags |= MSG_MORE;
#endif

		fastvideolist_first(proto->clients);
		for (Client_t *clt = fastvideolist_next(proto->clients); clt != NULL && proto->offset > 0; clt = fastvideolist_next(proto->clients))
		{
			if (clt->state == e_connected && pflags & Proto_Started)
				continue;
			clt->state = e_started;
#if UNIX_PACKETIZER
			ret = send(clt->fd, proto->packet, proto->offset, flags);
#else
			ret = send(clt->fd, buf, len, flags);
#endif
			//dbg("send %d of %d", ret, len);
			if (ret <= 0)
			{
				if (errno == EAGAIN)
				{
					err("EAGAIN");
					continue;
				}
				clt->fd = -1;
				FastVideoList_t *client = NULL;
				proto->clients = fastvideolist_pop(proto->clients, &client);
				proto->clientspool = fastvideolist_push(proto->clientspool, client);
				dbg("unix: client disconnected %p %m %zd", client, ret);
				errno = 0;
				ret = 0;
			}
		}
		if (ret >= 0)
			ret = len;
#if UNIX_PACKETIZER
		proto->offset = 0;
#endif
	}
	if (errno)
		ret = -1;
	return ret;
}

static ssize_t proto_recv(void *arg, void *buf, size_t len, Proto_Flags_t flags)
{
	Proto_UNIX_t *proto = (Proto_UNIX_t *)arg;
	ssize_t ret = 0;
	/// Only one client may receive data
	fastvideolist_first(proto->clients);
	Client_t *clt = fastvideolist_get(proto->clients);
	ret = recv(clt->fd, buf, len, 0);
	return ret;
}

static void proto_flush(void *arg)
{
#if UNIX_PACKETIZER
	proto_send(arg, NULL, 0, 0);
#endif
}

static int proto_fd(void *arg)
{
	return -1;
}

static size_t proto_mtu(void *arg)
{
	Proto_UNIX_t *proto = (Proto_UNIX_t *)arg;
	return proto->mtu;
}

static void proto_close(void *arg)
{
	Proto_UNIX_t *proto = (Proto_UNIX_t *)arg;
	FastVideoList_t *client = NULL;
	do {
		proto->clients = fastvideolist_poplast(proto->clients, &client);
		if (client)
		{
			Client_t *clt = fastvideolist_get(client);
			if (clt == NULL)
				break;
			close(clt->fd);
			clt->fd = -1;
			proto->clientspool = fastvideolist_push(proto->clientspool, client);
		}
	} while (client);
}

static void proto_destroy(void *arg)
{
	Proto_UNIX_t *proto = (Proto_UNIX_t *)arg;
	FastVideoList_t *client = NULL;
	do {
		client = NULL;
		proto->clientspool = fastvideolist_poplast(proto->clientspool, &client);
		if (client)
		{
			free(fastvideolist_next(client));
		}
	} while (client);
	shutdown(proto->serverfd, SHUT_RDWR);
	close(proto->serverfd);
#if UNIX_PACKETIZER
	free(proto->packet);
#endif
	free(proto);
}

const Proto_t proto_unix =
{
	.name = "unix",
	.create = proto_create,
	.connect = proto_connect,
	.close = proto_close,
	.mtu = proto_mtu,
	.fd = proto_fd,
	.send = proto_send,
	.recv = proto_recv,
	.flush = proto_flush,
	.destroy = proto_destroy,
};

#include <dlfcn.h>

static void __attribute__ ((constructor)) smpegts_init()
{
	fastvideo_proto_append_t _fastvideo_proto_append;
	void *hdl = dlopen(NULL, RTLD_NOW);
	_fastvideo_proto_append = dlsym(hdl, "fastvideo_proto_append");
	if (_fastvideo_proto_append)
	{
		_fastvideo_proto_append(&proto_unix);
	}
}
