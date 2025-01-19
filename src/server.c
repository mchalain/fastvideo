#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/un.h>
#include <errno.h>
#include <sys/stat.h>

#include "log.h"
#define __UNIXSOCKET_INTERNAL__
#include "unixsocket.h"

#define SERVER_READY 0x0001
#define SERVER_RUNNING 0x0002
#define SERVER_STOPPING 0x0004

typedef struct server_s server_t;
struct server_s
{
	int sock;
	client_t *clients;
	int state;
	struct sockaddr_un addr;
	struct
	{
		client_create_t create;
		client_receive_t receive;
		client_receivefd_t receivefd;
		client_close_t close;
	} ops;
	void *data;
};

server_t *server_create(const char *path, int maxclients)
{
	server_t *server = NULL;
	int sock = socket(AF_UNIX, SOCK_STREAM, 0);
	if (sock > 0)
	{
		struct sockaddr_un addr;
		memset(&addr, 0, sizeof(struct sockaddr_un));
		addr.sun_family = AF_UNIX;
		snprintf(addr.sun_path, sizeof(addr.sun_path) - 1, "%s", path);
		unlink(addr.sun_path);

		int ret;

		ret = bind(sock, (struct sockaddr *) &addr, sizeof(addr));
		if (ret == 0)
		{
			chmod(addr.sun_path, 0777);
			ret = listen(sock, maxclients);
		}
		else
			err("server: bind error %m");
		if (ret == 0)
		{
			server = calloc(1, sizeof(*server));
			server->sock = sock;
			server->state = SERVER_READY;
			memcpy(&server->addr, &addr, sizeof(addr));
		}
		else
			err("server: listen error %m");
	}
	return server;
}

int server_attach_create_t(server_t *server, client_create_t callback, void *data)
{
	if (server->state != SERVER_READY)
		return -1;
	server->ops.create = callback;
	server->data = data;
	return 0;
}

int server_attach_receive(server_t *server, client_receive_t callback, void *data)
{
	if (server->state != SERVER_READY)
		return -1;
	server->ops.receive = callback;
	server->data = data;
	return 0;
}

int server_attach_receivefd(server_t *server, client_receivefd_t callback, void *data)
{
	if (server->state != SERVER_READY)
		return -1;
	server->ops.receivefd = callback;
	server->data = data;
	return 0;
}

int server_attach_close(server_t *server, client_close_t callback, void *data)
{
	if (server->state != SERVER_READY)
		return -1;
	server->ops.close = callback;
	server->data = data;
	return 0;
}

static int _server_connect(server_t *server)
{
	int ret = -1;
	int newsock = -1;
	struct sockaddr_storage addr;
	int addrsize = sizeof(addr);
	newsock = accept(server->sock, (struct sockaddr *)&addr, &addrsize);
	if (newsock > 0)
	{
		client_t *client = calloc(1, sizeof(*client));
		client->sock = newsock;
		client->next = server->clients;
		client->ops.receive = server->ops.receive;
		client->ops.receivefd = server->ops.receivefd;
		client->ops.close = server->ops.close;
		client->data = server->data;
		if (server->ops.create)
			client->data = server->ops.create(server->data, client, newsock);
		if (server->clients)
			server->clients->prev = client;
		server->clients = client;
		warn("new connection");
		ret = 0;
	}
	else
		server->state = SERVER_STOPPING;
	return ret;
}

static int _server_message(server_t *server, client_t *client)
{
	int ret = -1;
	ret = client_receive(client);
	if (ret <= 0)
	{
		if (server->clients = client)
			server->clients = client->next;
		client_destroy(client);
	}
	return ret;
}

int server_run(server_t *server)
{
	server->state = SERVER_RUNNING;
	while (server->state == SERVER_RUNNING)
	{
		fd_set rfds;
		int maxfd = server->sock;
		FD_ZERO(&rfds);
		FD_SET(server->sock, &rfds);
		for (client_t *client = server->clients; client != NULL; client = client->next)
		{
			FD_SET(client->sock, &rfds);
			maxfd = (maxfd < client->sock)?client->sock:maxfd;
		}

		int ret;
		ret = select(maxfd + 1, &rfds, NULL, NULL, NULL);
		if (ret < 0)
		{
			server->state = SERVER_STOPPING;
			continue;
		}
		if (ret == 0)
			continue;

		if (FD_ISSET(server->sock, &rfds))
		{
			ret = _server_connect(server);
		}
		client_t *next = NULL;
		for (client_t *client = server->clients; client != NULL; client = next)
		{
			next = client->next;
			if (FD_ISSET(client->sock, &rfds))
			{
				warn("new message");
				ret = _server_message(server, client);
			}
		}
	}
	return 0;
}

void server_destroy(server_t *server)
{
	unlink(server->addr.sun_path);
	client_t *next = NULL;
	for (client_t *client = server->clients; client != NULL; client = next)
	{
		if (client->ops.close)
			client->ops.close(client->data, client);
		close(client->sock);
		next = client->next;
		free(client);
	}
	close(server->sock);
	free(server);
}
