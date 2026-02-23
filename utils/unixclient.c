#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdarg.h>
#include <fcntl.h>

#include "log.h"
#include "unixsocket.h"

typedef struct app_s app_t;
struct app_s
{
	client_t *client;
	int fd;
};

ssize_t _client_receive(void *data, client_t *clt, const char *buffer, size_t length)
{
	ssize_t ret = 0;
	app_t *app = (app_t *)data;
	//dbg("recieve %lu bytes", length);
	if (app->fd > 0)
		ret = write(app->fd, buffer, length);
	return ret;
}

int main_run(app_t *app)
{
	while (1)
	{
		client_wait(app->client, 0, NULL);
	}
	return 0;
}

int main(int argc, char * const argv[])
{
	app_t app = {0};
	const char *serverpath = "/tmp/fastsetting_socket";
	const char *logfile = NULL;

	int opt;
	do
	{
		opt = getopt(argc, argv, "L:f:");
		switch (opt)
		{
			case 'L':
				dbg("coucou");
				logfile = optarg;
			break;
			case 'f':
				serverpath = optarg;
			break;
		}
	} while(opt != -1);

	if (logfile)
	{
		app.fd = open(logfile, O_WRONLY | O_CREAT);
		warn("dump to file %s (%d %m)", logfile, app.fd);
	}
	app.client = client_create(serverpath);
	client_attach_receive(app.client, _client_receive, &app);
	main_run(&app);
	client_destroy(app.client);
	if (app.fd > 0)
		close(app.fd);
	return 0;
}
