#include <stdlib.h>
#include <string.h>
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
#include <pthread.h>

#include "fastvideo.h"
#include "sconfig.h"
#include "smpegts.h"
#include "log.h"

#define IP_HEADER_LENGTH 20
#define UDP_HEADER_LENGTH 8

#define Proto_TCP_Http 0x01

#define PROTO_TCP_MAX_BUFFERS 64

typedef struct Proto_TCP_Buffer_s Proto_TCP_Buffer_t;
struct Proto_TCP_Buffer_s
{
	void *data;
	size_t len;
	int flags;
	volatile int state;
	Proto_TCP_Buffer_t *next;
};

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

	pthread_t thread_send;
	pthread_mutex_t mutex;
	pthread_cond_t cond;
	int running;
	int error;
	int dropping;

	Proto_TCP_Buffer_t *buffers;
	Proto_TCP_Buffer_t *out;
	Proto_TCP_Buffer_t *in;
};

static void proto_destroy(void *arg);
static void proto_close(void *arg);

static int _proto_set_buffer(Proto_TCP_t *proto, Proto_TCP_Buffer_t*buffer)
{
	buffer->data = malloc(proto->mtu);
	buffer->state = dequeued;
	return (buffer->data == NULL);
}

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
			if (config->interface && strcmp(ifa_main->ifa_name, config->interface))
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
		config->host = strdup("0.0.0.0");
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

static int _proto_sendall(int fd, Proto_TCP_Buffer_t *buff)
{
	int sflags = 1;
	if (buff->flags & Proto_More)
		sflags = 0;
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (char *) &sflags, sizeof(int));
	sflags = MSG_NOSIGNAL;
	if (buff->flags & Proto_More)
		sflags |= MSG_MORE;
	ssize_t ret = send(fd, buff->data, buff->len, sflags);
	if (ret != buff->len)
		return -1;
	return 0;
}

static void *_proto_send_thread(void *arg)
{
	Proto_TCP_t *proto = (Proto_TCP_t *)arg;
	pthread_mutex_lock(&proto->mutex);
	while (proto->running)
	{
		Proto_TCP_Buffer_t *buff = proto->in;
		if (buff->state != ready)
		{
			pthread_cond_wait(&proto->cond, &proto->mutex);
			continue;
		}
		int fd = proto->clientfd;
		int ret = 0;
		if (fd != -1 && !proto->error)
		{
			pthread_mutex_unlock(&proto->mutex);
			ret = _proto_sendall(fd, buff);
			pthread_mutex_lock(&proto->mutex);
		}
		if (ret < 0 && proto->clientfd == fd)
			proto->error = errno;
		buff->state = dequeued;
		buff->len = 0;
		proto->in = buff->next;
	}
	pthread_mutex_unlock(&proto->mutex);
	return NULL;
}

static void _proto_push(Proto_TCP_t *proto, int flags)
{
	Proto_TCP_Buffer_t *buff = proto->out;
	pthread_mutex_lock(&proto->mutex);
	buff->flags = flags;
	buff->state = ready;
	pthread_cond_signal(&proto->cond);
	pthread_mutex_unlock(&proto->mutex);
	proto->out = buff->next;
}

static int _proto_send(Proto_TCP_t *proto, const void* buf, size_t len, int flags)
{
	if (proto->error)
	{
		errno = proto->error;
		return -1;
	}
	Proto_TCP_Buffer_t *buff = proto->out;
	if (buff->state == queued && buff->len + len > proto->mtu)
	{
		_proto_push(proto, flags | Proto_More);
		buff = proto->out;
	}
	if (buff->state != queued)
	{
		if (proto->dropping && !(flags & Proto_Started))
		{
			errno = EAGAIN;
			return -1;
		}
		pthread_mutex_lock(&proto->mutex);
		int state = buff->state;
		pthread_mutex_unlock(&proto->mutex);
		if (state != dequeued || len > proto->mtu)
		{
			warn("tcp: network too slow, frames are dropped");
			proto->dropping = 1;
			errno = EAGAIN;
			return -1;
		}
		proto->dropping = 0;
		buff->state = queued;
		buff->len = 0;
	}
	memcpy(buff->data + buff->len, buf, len);
	buff->len += len;
	if (!(flags & Proto_More))
		_proto_push(proto, flags);
	return len;
}

static int _proto_recv(Proto_TCP_t *proto, void* buf, size_t len, int flags)
{
	int sflags = MSG_NOSIGNAL | MSG_DONTWAIT;
	return recv(proto->clientfd, buf, len, sflags);
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
	proto->serverfd = -1;
	proto->clientfd = -1;
	proto->dropping = 1;

	proto->buffers = calloc(PROTO_TCP_MAX_BUFFERS, sizeof(*proto->buffers));
	for (int i = 0; i < PROTO_TCP_MAX_BUFFERS; i++)
	{
		_proto_set_buffer(proto, &proto->buffers[i]);
		proto->buffers[i].next = &proto->buffers[i + 1];
	}
	proto->buffers[PROTO_TCP_MAX_BUFFERS - 1].next = &proto->buffers[0];
	proto->out = &proto->buffers[0];
	proto->in = &proto->buffers[0];

	pthread_mutex_init(&proto->mutex, NULL);
	pthread_cond_init(&proto->cond, NULL);
	proto->running = 1;
	if (pthread_create(&proto->thread_send, NULL, _proto_send_thread, proto) != 0)
	{
		err("sproto: thread sender error %m");
		proto_destroy(proto);
		return NULL;
	}
	return proto;
}

static void *proto_create_server(Proto_Config_t *config, ImageDefinition_t *definition)
{
	Proto_TCP_t *proto = _proto_create(config, _proto_bindserver);
	if (proto == NULL)
		return NULL;
	if (config->mode && strstr(config->mode, "http"))
		proto->mode |= Proto_TCP_Http;
	proto->serverfd = proto->sock;
	proto->clientfd = -1;
	warn("tcp: server on %s:%d", config->host, config->port);
	return proto;
}

static void *proto_create_client(Proto_Config_t *config, ImageDefinition_t *definition)
{
	Proto_TCP_t *proto = _proto_create(config, _proto_bindclient);
	if (proto == NULL)
		return NULL;
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
	fcntl(proto->clientfd, F_SETFL, flags & ~O_NONBLOCK);
	return 0;
}

static int proto_connect_server(void *arg)
{
	int ret = -1;
	Proto_TCP_t *proto = (Proto_TCP_t *)arg;
	if (proto->serverfd == -1)
		return -1;

	/// the listening socket is non-blocking (see _proto_create()) - this
	/// polls for a pending client, it never waits for one.
	int clientfd = accept(proto->serverfd, NULL, 0);
	if (clientfd < 0)
	{
		if (errno == EAGAIN || errno == EWOULDBLOCK)
			return 0;
		err("tcp: error on client connection %m");
		return -1;
	}
	int flags = fcntl(clientfd, F_GETFL, 0);
	ret = fcntl(clientfd, F_SETFL, flags & ~O_NONBLOCK);
	pthread_mutex_lock(&proto->mutex);
	proto->clientfd = clientfd;
	proto->error = 0;
	pthread_mutex_unlock(&proto->mutex);
	warn("tcp: new connection");
	if (proto->mode & Proto_TCP_Http)
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
Connection: Close\r\n\r\n\
";
		size_t len = sizeof(buf);
		ret = _proto_send(proto, buf, len, Proto_Started);
		if (ret > 0)
		{
			warn("tcp: send HTTP response");
			ret = 0;
		}
	}
	return ret;
}

static ssize_t proto_send(void *arg, const void *buf, size_t len, Proto_Flags_t flags)
{
	Proto_TCP_t *proto = (Proto_TCP_t *)arg;
	ssize_t ret = -1;
	errno = EAGAIN;
	if (proto->clientfd == -1 && proto->serverfd > 0)
		return proto_connect_server(arg);
	else if (proto->clientfd == -1)
	{
		warn("tcp: no client connected");
		return -1;
	}
	if (len == 0)
		warn("tcp: send empty packet");

	ret = _proto_send(proto, buf, len, flags);
	if (ret < 0 && errno != EAGAIN)
	{
		if (errno == ECONNRESET || errno == EPIPE)
			warn("tcp: connection closed");
		else
			err("tcp: sending error %m");
		proto_close(proto);
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
	ret = _proto_recv(proto, buf, len, flags);
	if (ret < 0 && errno != EAGAIN)
	{
		err("tcp: receiving error %m");
		proto_close(proto);
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
	pthread_mutex_lock(&proto->mutex);
	int fd = proto->clientfd;
	proto->clientfd = -1;
	proto->error = 0;
	pthread_mutex_unlock(&proto->mutex);
	if (fd != -1)
	{
		shutdown(fd, SHUT_RDWR);
		close(fd);
	}
	if (proto->out->state == queued)
	{
		proto->out->state = dequeued;
		proto->out->len = 0;
	}
	proto->dropping = 1;
}

static void proto_destroy(void *arg)
{
	Proto_TCP_t *proto = (Proto_TCP_t *)arg;
	if (proto->serverfd > -1)
	{
		shutdown(proto->serverfd, SHUT_RDWR);
		close(proto->serverfd);
	}
	pthread_mutex_lock(&proto->mutex);
	proto->running = 0;
	pthread_cond_signal(&proto->cond);
	pthread_mutex_unlock(&proto->mutex);
	/// wake up the thread blocked into send()
	if (proto->clientfd != -1)
		shutdown(proto->clientfd, SHUT_RDWR);
	pthread_join(proto->thread_send, NULL);
	if (proto->clientfd != -1 && proto->clientfd != proto->serverfd)
		close(proto->clientfd);
	pthread_mutex_destroy(&proto->mutex);
	pthread_cond_destroy(&proto->cond);
	for (int i = 0; i < PROTO_TCP_MAX_BUFFERS; i++)
		free(proto->buffers[i].data);
	free(proto->buffers);
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
