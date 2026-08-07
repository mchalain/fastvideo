#include <stdlib.h>
#include <unistd.h>
#include <errno.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/un.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include <ifaddrs.h>

#include "fastvideo.h"
#include "sconfig.h"
#include "smpegts.h"
#include "log.h"

#define IP_HEADER_LENGTH 20
#define UDP_HEADER_LENGTH 8

#define Proto_TCP_Http 0x01

#define PROTO_TCP_SEND_RETRY_MAX 5
#define PROTO_TCP_SEND_RETRY_DELAY_US 200

typedef struct Proto_TCP_s Proto_TCP_t;
struct Proto_TCP_s
{
	int sock;
	int serverfd;
	int clientfd;
	struct sockaddr_storage dest_addr;
	socklen_t dest_size;
	size_t mtu;
	int mode;
};

static socklen_t _proto_interface(Proto_Config_t *config, struct sockaddr_storage *address)
{
	struct sockaddr* saddr = NULL;
	socklen_t saddrlen = 0;
	struct ifaddrs *ifa_list;
	struct ifaddrs *ifa_main;
	int ret = -1;
	int family = address->ss_family;
	ret = getifaddrs(&ifa_list);
	if (ret == 0)
	{
		ret = -1;
		for (ifa_main = ifa_list; ifa_main != NULL; ifa_main = ifa_main->ifa_next)
		{
			if (ifa_main->ifa_addr == NULL)
				continue;
			if (ifa_main->ifa_addr->sa_family != family)
				continue;
			if ((ifa_main->ifa_flags & IFF_UP) == 0)
				continue;
			if (ifa_main->ifa_flags & IFF_LOOPBACK)
				continue;
			family = ifa_main->ifa_addr->sa_family;
			saddr = ifa_main->ifa_addr;
			if (family == AF_INET)
			{
				((struct sockaddr_in *)saddr)->sin_port=htons(config->port);
				saddrlen = sizeof(struct sockaddr_in);
			}
			else
			{
				((struct sockaddr_in6 *)saddr)->sin6_port=htons(config->port);
				saddrlen = sizeof(struct sockaddr_in6);
			}

			char host[NI_MAXHOST];
			getnameinfo(ifa_main->ifa_addr,
			   (family == AF_INET) ? sizeof(struct sockaddr_in) :
									 sizeof(struct sockaddr_in6),
			   host, NI_MAXHOST,
			   NULL, 0, NI_NUMERICHOST);
			dbg("tcp: interface %s %s %s %d", ifa_main->ifa_name, family == AF_INET?"IPv4": family == AF_INET6?"IPv6":"???", host, ntohs(((struct sockaddr_in *)saddr)->sin_port));
			ret = 0;
			break;
		}
		if (!ret)
		{
			memmove(address, saddr, saddrlen);
		}
	}
	return saddrlen;
}

static socklen_t _proto_address(Proto_Config_t *config, struct sockaddr_storage *address)
{
	struct addrinfo hints = {0};
	struct addrinfo *result = NULL, *rp = NULL;

	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE;
	hints.ai_protocol = IPPROTO_TCP;

	if (config->host == NULL)
		return -1;
	if (getaddrinfo(config->host, NULL, &hints, &result))
		err("tcp: config error %m");
	rp = result;
	if (rp == NULL)
	{
		return 0;
	}
	if (rp->ai_family == AF_INET)
	{
		((struct sockaddr_in *)rp->ai_addr)->sin_port = htons(config->port);
	}
	else if (rp->ai_family == AF_INET6)
	{
		((struct sockaddr_in6 *)rp->ai_addr)->sin6_port = htons(config->port);
	}
	else
		return 0;
	memmove(address, rp->ai_addr, rp->ai_addrlen);
	socklen_t len = rp->ai_addrlen;
	if (result)
		freeaddrinfo(result);

	return len;
}

static int _proto_bindclient(int sock, struct sockaddr *saddr, socklen_t saddrlen)
{
	int status = -1;
	status = connect(sock, saddr, saddrlen);
	return status;
}

static int _proto_bindserver(int sock, struct sockaddr *saddr, socklen_t saddrlen)
{
	int status = -1;
	status = bind(sock, saddr, saddrlen);
	if (!status)
	{
		status = listen(sock, 1);
	}
	return status;
}

static void *_proto_create(Proto_Config_t *config, int (*_bind)(int sock, struct sockaddr *, socklen_t))
{
	int sock = -1;
	struct sockaddr_storage address = {0};
	socklen_t addresslen = 0;
	int mtu = 1500;

	if ((addresslen = _proto_address(config, &address)) == 0)
	{
		if (address.ss_family == 0)
			address.ss_family = AF_INET;
		if ((addresslen = _proto_interface(config, &address)) == 0)
		{
			struct sockaddr_in *saddr = (struct sockaddr_in *)&address;
			saddr->sin_family = AF_INET;
			saddr->sin_addr.s_addr = INADDR_ANY;
			saddr->sin_port = htons(config->port);
			addresslen = sizeof(*saddr);
		}
	}
	if (addresslen != 0)
		sock = socket(address.ss_family, SOCK_STREAM, IPPROTO_TCP);
	if (sock < 0)
		return NULL;

	if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (void *)&(int){ 1 }, sizeof(int)) < 0)
			warn("tcp: setsockopt(SO_REUSEADDR) failed");
#ifdef SO_REUSEPORT
	if (setsockopt(sock, SOL_SOCKET, SO_REUSEPORT, (void *)&(int){ 1 }, sizeof(int)) < 0)
			warn("tcp: setsockopt(SO_REUSEPORT) failed");
#endif
	if (_bind  && _bind(sock, (struct sockaddr*)&address, addresslen))
	{
		err("tcp: connection %s:%d error %m", config->host, config->port);
		close(sock);
		return NULL;
	}

	struct ifreq ifr;
	memset(&ifr, 0, sizeof(ifr));
	ifr.ifr_addr.sa_family = address.ss_family;
	if (ioctl(sock, SIOCGIFMTU, &ifr) != -1)
		mtu = ifr.ifr_mtu;

	int flags;
	flags = fcntl(sock, F_GETFL, 0);
	fcntl(sock, F_SETFL, flags | O_NONBLOCK);

	Proto_TCP_t *proto = calloc(1, sizeof(*proto));
	proto->mtu = mtu - IP_HEADER_LENGTH - UDP_HEADER_LENGTH; /// size of udp/ip header
	proto->sock = sock;

	return proto;
}

static void *proto_create_server(Proto_Config_t *config)
{
	Proto_TCP_t *proto = _proto_create(config, _proto_bindserver);
	if (config->mode && strstr(config->mode, "http"))
		proto->mode |= Proto_TCP_Http;
	proto->serverfd = proto->sock;
	proto->clientfd = -1;
	warn("tcp: server on %s:%d", config->host, config->port);
	return proto;
}

static void *proto_create_client(Proto_Config_t *config)
{
	Proto_TCP_t *proto = _proto_create(config, _proto_bindclient);
	proto->clientfd = proto->sock;
	proto->serverfd = -1;
	warn("tcp: client to %s:%d", config->host, config->port);
	return proto;
}

static int proto_connect_client(void *arg)
{
	Proto_TCP_t *proto = (Proto_TCP_t *)arg;
	int flags;
	flags = fcntl(proto->clientfd, F_GETFL, 0);
	fcntl(proto->clientfd, F_SETFL, flags | O_NONBLOCK);
	return 0;
}

static int proto_connect_server(void *arg)
{
	Proto_TCP_t *proto = (Proto_TCP_t *)arg;
	if (proto->serverfd == -1)
		return -1;

	/// this is currently a blocked socket
	proto->clientfd = accept(proto->serverfd, NULL, 0);
	if (proto->clientfd > 0)
	{
		int flags = fcntl(proto->clientfd, F_GETFL, 0);
		fcntl(proto->clientfd, F_SETFL, flags | O_NONBLOCK);
		warn("tcp: new connection");
	}
	if (proto->clientfd > 0 && proto->mode & Proto_TCP_Http)
	{
		const char buf[] = "HTTP/1.1 200 OK\r\n\
Server: "PACKAGE_NAME"/"PACKAGE_VERSION"\r\n\
Cache-Control: no-cache,no-store,max-age=0,must-revalidate\r\n\
Pragma: no-cache\r\n\
Expires: 0\r\n\
X-Content-Type-Options: nosniff\r\n\
X-Frame-Options: SAMEORIGIN\r\n\
Referrer-Policy: origin-when-cross-origin\r\n\
Access-Control-Allow-Origin: *\r\n\
Content-Type: video/mp2t\r\n\
Connection: Close\r\n\
";
		int sflag = 1;
		setsockopt(proto->clientfd, IPPROTO_TCP, TCP_NODELAY, (char *) &sflag, sizeof(int));
		size_t len = sizeof(buf);
		int flags = MSG_NOSIGNAL;
		int ret = send(proto->clientfd, buf, len, flags);
		if (ret > 0)
			warn("tcp: send HTTP response");
	}
	return 0;
}

static ssize_t proto_send(void *arg, const void *buf, size_t len, Proto_Flags_t flags)
{
	Proto_TCP_t *proto = (Proto_TCP_t *)arg;
	ssize_t ret = -1;
	errno = EAGAIN;
	if (proto->clientfd == -1 && proto->serverfd > 0)
		proto_connect_server(arg);
	else if (proto->clientfd == -1)
	{
		warn("tcp: no client connected");
		return -1;
	}
	/// server mode and no client are connected
	if (proto->clientfd == -1)
		return len;
	int sflags = 1;
	if (flags & Proto_More)
		sflags = 0;
	setsockopt(proto->clientfd, IPPROTO_TCP, TCP_NODELAY, (char *) &sflags, sizeof(int));
	if (len == 0)
		warn("tcp: send empty packet");
	sflags = MSG_NOSIGNAL;
	if (flags & Proto_More)
		sflags |= MSG_MORE;
	int retries = 0;
	ret = send(proto->clientfd, buf, len, sflags);
	while (ret < 0 && errno == EAGAIN && ++retries < PROTO_TCP_SEND_RETRY_MAX)
	{
		usleep(PROTO_TCP_SEND_RETRY_DELAY_US);
		ret = send(proto->clientfd, buf, len, sflags);
	}
	if (ret < 0 && errno != EAGAIN)
	{
		if (errno == ECONNRESET || errno == EPIPE)
			warn("tcp: connection closed");
		else
			err("tcp: sending error %m");
		close(proto->clientfd);
		proto->clientfd = -1;
		if (proto->serverfd > 0)
			ret = 0;
	}

	return ret;
}

static ssize_t proto_recv(void *arg, void *buf, size_t len, Proto_Flags_t flags)
{
	Proto_TCP_t *proto = (Proto_TCP_t *)arg;
	ssize_t ret = -1;
	if (proto->clientfd == -1)
	{
		warn("tcp: no client connected");
		return -1;
	}
	int sflags = MSG_NOSIGNAL;
	ret = recv(proto->clientfd, buf, len, sflags);
	if (ret < 0 && errno != EAGAIN)
	{
		err("tcp: receiving error %m");
		close(proto->clientfd);
		proto->clientfd = -1;
		if (proto->serverfd > 0)
			ret = 0;
	}

	return ret;
}

static void proto_flush(void *arg)
{
}

static int proto_fd(void *arg)
{
	Proto_TCP_t *proto = (Proto_TCP_t *)arg;
	return proto->clientfd;
}

static size_t proto_mtu(void *arg)
{
	Proto_TCP_t *proto = (Proto_TCP_t *)arg;
	return proto->mtu;
}

static void proto_close(void *arg)
{
	Proto_TCP_t *proto = (Proto_TCP_t *)arg;
	shutdown(proto->clientfd, SHUT_RDWR);
	close(proto->clientfd);
	proto->clientfd = -1;
}

static void proto_destroy(void *arg)
{
	Proto_TCP_t *proto = (Proto_TCP_t *)arg;
	shutdown(proto->serverfd, SHUT_RDWR);
	close(proto->serverfd);
	free(proto);
}

Proto_t proto_tcpserver =
{
	.name = "tcpserver",
	.create = proto_create_server,
	.connect = proto_connect_server,
	.close = proto_close,
	.mtu = proto_mtu,
	.fd = proto_fd,
	.send = proto_send,
	.flush = proto_flush,
	.destroy = proto_destroy,
};

const Proto_t proto_tcpclient =
{
	.name = "tcpclient",
	.create = proto_create_client,
	.connect = proto_connect_client,
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
		_fastvideo_proto_append(&proto_tcpserver);
		_fastvideo_proto_append(&proto_tcpclient);
	}
}
