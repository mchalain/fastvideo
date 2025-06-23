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
#include "config.h"
#include "smpegts.h"
#include "log.h"

#define IP_HEADER_LENGTH 20
#define UDP_HEADER_LENGTH 8

typedef struct Proto_UDP_s Proto_UDP_t;
struct Proto_UDP_s
{
	int serverfd;
	struct sockaddr_storage dest_addr;
	socklen_t dest_size;
	size_t mtu;
};

static void *proto_create(MPEG_TSConf_t *config)
{
	int sock = 0;
	size_t mtu = 1500 - IP_HEADER_LENGTH - UDP_HEADER_LENGTH;
	struct sockaddr* saddr = NULL;
	socklen_t saddrlen = 0;
	int family = 0;
	struct sockaddr_in saddr_in = {0};

	struct addrinfo hints = {0};
	struct addrinfo *result = NULL, *rp = NULL;
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_DGRAM;
	hints.ai_flags = AI_PASSIVE;
	hints.ai_protocol = IPPROTO_UDP;

	if (getaddrinfo(config->host, NULL, &hints, &result))
		err("mpegts: config error %m");
	rp = result;
	if (rp == NULL)
	{
		saddr_in.sin_family = AF_INET;
		inet_aton(config->host, &saddr_in.sin_addr);
		hints.ai_family = AF_INET;
		hints.ai_addr = (struct sockaddr *)&saddr_in;
		hints.ai_addrlen = sizeof(saddr_in);
		rp = &hints;
	}
	unsigned long longaddress = 0;
	if (rp->ai_family == AF_INET)
	{
		longaddress = ((struct sockaddr_in *)rp->ai_addr)->sin_addr.s_addr;
		((struct sockaddr_in *)rp->ai_addr)->sin_port = htons(config->port);
	}
	else
	{
		longaddress = ((struct sockaddr_in6 *)rp->ai_addr)->sin6_addr.s6_addr32[0];
		((struct sockaddr_in6 *)rp->ai_addr)->sin6_port = htons(config->port);
	}

	int status = -1;
	sock = socket(rp->ai_family, SOCK_DGRAM, IPPROTO_UDP);
	if (sock < 0)
		return NULL;

	if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (void *)&(int){ 1 }, sizeof(int))
< 0)
			warn("smpegts: setsockopt(SO_REUSEADDR) failed");
#ifdef SO_REUSEPORT
	if (setsockopt(sock, SOL_SOCKET, SO_REUSEPORT, (void *)&(int){ 1 }, sizeof(int))
< 0)
			warn("smpegts: setsockopt(SO_REUSEPORT) failed");
#endif

	struct ifaddrs *ifa_list;
	struct ifaddrs *ifa_main;
	int ret = -1;
	ret = getifaddrs(&ifa_list);
	if (ret == 0)
	{
		for (ifa_main = ifa_list; ifa_main != NULL; ifa_main = ifa_main->ifa_next)
		{
			if (ifa_main->ifa_addr == NULL)
				continue;
			if (ifa_main->ifa_addr->sa_family != rp->ai_family)
				continue;
			if ((ifa_main->ifa_flags & IFF_UP) == 0)
				continue;
			if (ifa_main->ifa_flags & IFF_LOOPBACK)
				continue;
			family = ifa_main->ifa_addr->sa_family;
			saddr = ifa_main->ifa_addr;
			saddrlen = ifa_main->ifa_addr->sa_family == AF_INET?
				sizeof(struct sockaddr_in) :
				sizeof(struct sockaddr_in6);

			char host[NI_MAXHOST];
			getnameinfo(ifa_main->ifa_addr,
			   (family == AF_INET) ? sizeof(struct sockaddr_in) :
									 sizeof(struct sockaddr_in6),
			   host, NI_MAXHOST,
			   NULL, 0, NI_NUMERICHOST);
			dbg("mpegts: interface %s %s %s %d", ifa_main->ifa_name, family == AF_INET?"IPv4": family == AF_INET6?"IPv6":"???", host, sock);
			break;
		}
	}

	if (saddr != NULL)
		status = bind(sock, saddr, saddrlen);

	if (status == 0)
	{
		struct ifreq ifr;
		memset(&ifr, 0, sizeof(ifr));
		ifr.ifr_addr.sa_family = family;
		if (ioctl(sock, SIOCGIFMTU, &ifr) != -1)
			mtu = ifr.ifr_mtu - IP_HEADER_LENGTH - UDP_HEADER_LENGTH; /// size of udp/ip header

		// check if the address is for multicast diffusion
		if (IN_MULTICAST(htonl(longaddress)) ||
			(family == AF_INET6 && htonl(longaddress) == 0xff020000))
		{
			if (! ifr.ifr_flags & IFF_MULTICAST)
				err("smpegts: udp multicast interface not supported %s", config->host);
			else
				warn("smpegts: udp multicast to %s %d", config->host, config->port);

			// Set the outgoing interface to DEFAULT
			status = setsockopt(sock, IPPROTO_IP, IP_MULTICAST_IF, saddr, saddrlen);
			if (status != 0)
				warn("smpegts: not allowed to change interface");

			unsigned char ttl = 3;
			// Set multicast packet TTL to 3; default TTL is 1
			status = setsockopt(sock, IPPROTO_IP, IP_MULTICAST_TTL, &ttl,
							sizeof(unsigned char));
			if (status != 0)
				warn("smpegts: not allowed to set TTL");

			unsigned char one = 1;
			// send multicast traffic to myself too
			status = setsockopt(sock, IPPROTO_IP, IP_MULTICAST_LOOP, &one,
							sizeof(unsigned char));
			if (status != 0)
				warn("smpegts: not allowed to make a loop on data sending");
		}
		else if (htonl(longaddress) > 0xff000000)
		{
			status = setsockopt(sock, SOL_SOCKET, SO_BROADCAST, (void *)&(int){ 1 }, sizeof(int));
			if (status == -1)
				err("smpegts: udp broadcast error %m");
			if (! ifr.ifr_flags & IFF_BROADCAST)
				err("smpegts: udp broadcast interface not supported %s", config->host);
			else
				warn("smpegts: udp broadcast to %s %d", config->host, config->port);
		}
		else
			warn("smpegts: udp unicast to %s %d", config->host, config->port);
	}

	if (status)
	{
		err("smpegts: network error %m");
		close(sock);
		return NULL;
	}
	int flags;
	flags = fcntl(sock, F_GETFL, 0);
	fcntl(sock, F_SETFL, flags | O_NONBLOCK);

	Proto_UDP_t *proto = calloc(1, sizeof(*proto));
	proto->mtu = mtu;
	proto->serverfd = sock;
	proto->dest_size = rp->ai_addrlen;
	memcpy(&proto->dest_addr, rp->ai_addr, rp->ai_addrlen);
	if (result)
		freeaddrinfo(result);

	return proto;
}

static int proto_connect(void *arg)
{
	Proto_UDP_t *proto = (Proto_UDP_t *)arg;
	if (proto->serverfd == -1)
		return -1;
	return 0;
}

static ssize_t proto_send(void *arg, const void *buf, size_t len, int flags)
{
	Proto_UDP_t *proto = (Proto_UDP_t *)arg;
	ssize_t ret = -1;
	errno = EAGAIN;
	while (ret == -1 && errno == EAGAIN)
		ret = sendto(proto->serverfd, buf, len, flags,
					(struct sockaddr *)&proto->dest_addr, proto->dest_size);
	if (ret < 0)
	{
		char host[NI_MAXHOST];
		getnameinfo((struct sockaddr *)&proto->dest_addr, proto->dest_size,
			host, NI_MAXHOST,
			NULL, 0, NI_NUMERICHOST);
		err("mpegts: sending on %s error %m", host);
	}

	errno = 0;
	return ret;
}

static void proto_flush(void *arg)
{
	Proto_UDP_t *proto = (Proto_UDP_t *)arg;
	proto_send(proto, NULL, 0, 0);
#ifdef UDP_CORK
	int value = 0;
	setsockopt(proto->serverfd, IPPROTO_UDP, UDP_CORK, &value, sizeof(value));
#endif
}

static int proto_fd(void *arg)
{
	Proto_UDP_t *proto = (Proto_UDP_t *)arg;
	return proto->serverfd;
}

static size_t proto_mtu(void *arg)
{
	Proto_UDP_t *proto = (Proto_UDP_t *)arg;
	return proto->mtu;
}

static void proto_close(void *arg)
{
	Proto_UDP_t *proto = (Proto_UDP_t *)arg;
	shutdown(proto->serverfd, SHUT_RDWR);
	close(proto->serverfd);
}

static void proto_destroy(void *arg)
{
	Proto_UDP_t *proto = (Proto_UDP_t *)arg;
	free(proto);
}

Proto_t proto_udp =
{
	.create = proto_create,
	.connect = proto_connect,
	.close = proto_close,
	.mtu = proto_mtu,
	.fd = proto_fd,
	.send = proto_send,
	.flush = proto_flush,
	.destroy = proto_destroy,
};
