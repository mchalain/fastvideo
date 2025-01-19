#ifndef __UNIXSOCKET_H__
#define __UNIXSOCKET_H__

#include <stdint.h>
#include <sys/select.h>

#ifndef UNIXSOCKET_PACKETSIZE
#define UNIXSOCKET_PACKETSIZE 4200
#endif

typedef struct client_s client_t;
typedef struct server_s server_t;

typedef void *(*client_create_t)(void *data, client_t *clt, int fd);
typedef int (*client_receive_t)(void *data, client_t *clt, const char *buffer, size_t length);
typedef int (*client_receivefd_t)(void *data, client_t *clt, int fd);
typedef void (*client_close_t)(void *data, client_t *clt);

#ifdef __UNIXSOCKET_INTERNAL__
struct client_s
{
	int sock;
	client_t *next;
	client_t *prev;
	struct
	{
		client_create_t create;
		client_receive_t receive;
		client_receivefd_t receivefd;
		client_close_t close;
	} ops;
	void *data;
};
#endif

/**
 * server API
 */
server_t *server_create(const char *path, int maxclients);
int server_attach_create(server_t *server, client_create_t callback, void *data);
int server_attach_receive(server_t *server, client_receive_t callback, void *data);
int server_attach_receivefd(server_t *server, client_receivefd_t callback, void *data);
int server_attach_close(server_t *server, client_close_t callback, void *data);
int server_run(server_t *server);
void server_destroy(server_t *server);

/**
 * client API
 */
client_t *client_create(const char *path);
int client_attach_receive(client_t *client, client_receive_t callback, void *data);
int client_attach_receivefd(client_t *client, client_receivefd_t callback, void *data);
int client_attach_close(client_t *client, client_close_t callback, void *data);

ssize_t client_send(client_t *client, void *buffer, size_t length);
ssize_t client_sendfd(client_t *client, void *buffer, size_t length, int fd);

ssize_t client_receive(client_t *client);
int client_wait(client_t *client, int maxfd, fd_set *rfds);
int client_request(client_t *client, void *buffer, size_t length);

void client_destroy(client_t *client);

#endif
