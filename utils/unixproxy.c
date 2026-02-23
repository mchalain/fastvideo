#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdarg.h>
#include <fcntl.h>

#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#include "log.h"
#include "unixsocket.h"

typedef struct app_s app_t;
struct app_s
{
	client_t *client;
	int fd;
};

int tcpsocket(int port)
{
	int sock;
	struct sockaddr_storage address = {0};
	socklen_t addresslen = 0;
	int family = AF_INET;

	sock = socket(family, SOCK_STREAM, IPPROTO_TCP);
	setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (void *)&(int){ 1 }, sizeof(int));
	setsockopt(sock, SOL_SOCKET, SO_REUSEPORT, (void *)&(int){ 1 }, sizeof(int));

	if (family == AF_INET)
	{
		struct sockaddr_in *saddr = (struct sockaddr_in *)&address;
		saddr->sin_family = family;
		saddr->sin_addr.s_addr = INADDR_ANY;
		saddr->sin_port = htons(port);
		addresslen = sizeof(*saddr);
	}
	bind(sock, (struct sockaddr*)&address, addresslen);
	listen(sock, 1);

	return sock;
}

ssize_t _client_receive(void *data, client_t *clt, const char *buffer, size_t length)
{
	int ret = length;
	app_t *app = (app_t *)data;
	//dbg("recieve %lu bytes", length);
	if (app->fd > 0)
		ret = write(app->fd, buffer, length);
	return ret;
}

int main_run(app_t *app)
{
	int ret = 0;
	while (ret >= 0)
	{
		ret = client_wait(app->client, 0, NULL);
	}
	return 0;
}

int main(int argc, char * const argv[])
{
	app_t app = {0};
	const char *serverpath = "/tmp/fastsetting_socket";
	const char *logfile = NULL;
	int port= 1024;

	int opt;
	do
	{
		opt = getopt(argc, argv, "L:f:");
		switch (opt)
		{
			case 'L':
				logfile = optarg;
			break;
			case 'f':
				serverpath = optarg;
			break;
		}
	} while(opt != -1);

	if (strcmp(logfile,"-"))
	{
		int logfd = open(logfile, O_WRONLY | O_CREAT | O_TRUNC, 00644);
		if (logfd > 0)
		{
			dup2(logfd, 1);
			dup2(logfd, 2);
			close(logfd);
		}
		else
			err("log file error %m");
	}

	int sock = tcpsocket(port);
	if (sock > 0)
	{
		app.fd = accept(sock, NULL, 0);
		warn("dump to socket(%d %m)", app.fd);
	}
	app.client = client_create(serverpath);
	client_attach_receive(app.client, _client_receive, &app);
	main_run(&app);
	client_destroy(app.client);
	if (app.fd > 0)
		close(app.fd);
	close(sock);
	return 0;
}
