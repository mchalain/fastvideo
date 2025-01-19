#include <stdlib.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/un.h>
#include <unistd.h>

#include "log.h"
#define __UNIXSOCKET_INTERNAL__
#include "unixsocket.h"

client_t *client_create(const char *path)
{
	client_t *client = NULL;
	int sock = socket(AF_UNIX, SOCK_STREAM, 0);

	if (sock == -1)
	{
		return NULL;
	}

	struct sockaddr_un addr;
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	snprintf(addr.sun_path, sizeof(addr.sun_path) - 1, "%s", path);

	int ret;
	ret = connect(sock, (struct sockaddr *)&addr, sizeof(addr));
	if (ret == 0)
	{
		client = calloc(1, sizeof(*client));
		client->sock = sock;
	}
	return client;
}

int client_attach_receive(client_t *client, client_receive_t callback, void *data)
{
	client->ops.receive = callback;
	client->data = data;
	return 0;
}

int client_attach_receivefd(client_t *client, client_receivefd_t callback, void *data)
{
	client->ops.receivefd = callback;
	client->data = data;
	return 0;
}

ssize_t client_receive(client_t *client)
{
	ssize_t ret = -1;
	char buffer[UNIXSOCKET_PACKETSIZE];

	struct msghdr msg = {0};
	struct iovec iov[1] = {{.iov_base = buffer, .iov_len = UNIXSOCKET_PACKETSIZE}};
	char ctrl_buf[CMSG_SPACE(sizeof(int))];

	msg.msg_control = ctrl_buf;
	msg.msg_controllen = CMSG_SPACE(sizeof(int));
	msg.msg_iov = iov;
	msg.msg_iovlen = 1;

	ret = recvmsg(client->sock, &msg, MSG_CMSG_CLOEXEC);
	if (ret < 1)
	{
		warn("client: goodbye %p", client);
		return -1;
	}
	buffer[ret] = 0;
	if (client->ops.receive)
		client->ops.receive(client->data, client, buffer, ret);

	struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
	if (cmsg != NULL)
	{
		unsigned char *mdata = CMSG_DATA(cmsg);
		int fd = *((int*) mdata);
		if (client->ops.receivefd)
			client->ops.receivefd(client->data, client, fd);
	}
	return ret;
}

int client_wait(client_t *client, int maxfd, fd_set *rfds)
{
	fd_set srfds;
	if (rfds == NULL)
	{
		FD_ZERO(&srfds);
		rfds = &srfds;
		maxfd = 0;
	}
	FD_SET(client->sock, rfds);
	maxfd = (maxfd < client->sock)?client->sock:maxfd;

	int ret = 0;
	while (ret == 0)
	{
		ret = select(maxfd + 1, rfds, NULL, NULL, NULL);
		if (ret < 0)
			return ret;

		if (FD_ISSET(client->sock, rfds))
		{
			ret = client_receive(client);
		}
	}
	return ret;
}

ssize_t client_send(client_t *client, void *buffer, size_t length)
{
	ssize_t ret = -1;
	ret = send(client->sock, buffer, length, MSG_DONTWAIT);
	return ret;
}

int client_request(client_t *client, void *buffer, size_t length)
{
	ssize_t sended = client_send(client, buffer, length);
	if (length == sended)
		return client_wait(client, 0, NULL);
	return -1;
}

ssize_t client_sendfd(client_t *client, void *buffer, size_t length, int fd)
{
	struct msghdr msg = {0};
	struct cmsghdr *cmsg;
	char buf[CMSG_SPACE(sizeof(fd))];
	int *fdptr;

	struct iovec io = { .iov_base = buffer, .iov_len = length };
	msg.msg_iov = &io;
	msg.msg_iovlen = 1;

	memset(buf, '\0', sizeof(buf));
	msg.msg_control = buf;
	msg.msg_controllen = sizeof(buf);
	cmsg = CMSG_FIRSTHDR(&msg);
	cmsg->cmsg_level = SOL_SOCKET;
	cmsg->cmsg_type = SCM_RIGHTS;
	cmsg->cmsg_len = CMSG_LEN(sizeof(fd));

	fdptr = (int *)CMSG_DATA(cmsg);
	*fdptr = fd;

	return sendmsg(client->sock, &msg, MSG_DONTWAIT);
}

void client_destroy(client_t *client)
{
	if (client->next)
		client->next->prev = client->prev;
	if (client->prev)
		client->prev->next = client->next;
	close(client->sock);
	free(client);
}
