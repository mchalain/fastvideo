#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/timerfd.h>
#include <fcntl.h>

#ifdef HAVE_JANSSON
#include <jansson.h>
#endif

#include "log.h"
#include "daemonize.h"
#include "sv4l2.h"
#include "sv4l2_subdev.h"
#include "spassthrough.h"
#include "sdrm.h"
#include "segl.h"
#include "sfile.h"
#include "sdvb.h"
#include "config.h"
#include "unixsocket.h"

#define MODE_DAEMONIZE 0x01
#define MODE_INITIALIZE 0x02
//#define DISABLE_TRANSFER

typedef DeviceConf_t * (*FastVideoDevice_createconfig_t)(void);
typedef void *(*FastVideoDevice_create_t)(const char *devicename, device_type_e type, DeviceConf_t *config);
typedef void *(*FastVideoDevice_duplicate_t)(void *dev);
typedef int (*FastVideoDevice_loadsettings_t)(void *dev, void *configentry);
typedef int (*FastVideoDevice_capabilities_t)(void *dev, void *capabilities, int all);
typedef int (*FastVideoDevice_requestbuffer_t)(void *dev, enum buf_type_e t, ...);
typedef int (*FastVideoDevice_eventfd_t)(void *dev);
typedef int (*FastVideoDevice_start_t)(void *dev);
typedef int (*FastVideoDevice_stop_t)(void *dev);
typedef int (*FastVideoDevice_dequeue_t)(void *dev, void **mem, size_t *bytesused);
typedef int (*FastVideoDevice_queue_t)(void *dev, int index, size_t bytesused);
typedef void (*FastVideoDevice_destroy_t)(void *dev);

typedef struct FastVideoDevice_ops_s FastVideoDevice_ops_t;
struct FastVideoDevice_ops_s
{
	const char *name;
	FastVideoDevice_createconfig_t createconfig;
	FastVideoDevice_create_t create;
	FastVideoDevice_duplicate_t duplicate;
	FastVideoDevice_loadsettings_t loadsettings;
	FastVideoDevice_capabilities_t capabilities;
	FastVideoDevice_requestbuffer_t requestbuffer;
	FastVideoDevice_eventfd_t eventfd;
	FastVideoDevice_start_t start;
	FastVideoDevice_stop_t stop;
	FastVideoDevice_dequeue_t dequeue;
	FastVideoDevice_queue_t queue;
	FastVideoDevice_destroy_t destroy;
};

FastVideoDevice_ops_t sv4l2_ops = {
	.name = "v4l2",
	.createconfig = sv4l2_createconfig,
	.create = (FastVideoDevice_create_t)sv4l2_create,
	.duplicate = (FastVideoDevice_duplicate_t)sv4l2_duplicate,
	.loadsettings = (FastVideoDevice_loadsettings_t)sv4l2_loadsettings,
	.capabilities = (FastVideoDevice_capabilities_t)sv4l2_capabilities,
	.requestbuffer = (FastVideoDevice_requestbuffer_t)sv4l2_requestbuffer,
	.eventfd = (FastVideoDevice_eventfd_t)sv4l2_fd,
	.start = (FastVideoDevice_start_t)sv4l2_start,
	.stop = (FastVideoDevice_stop_t)sv4l2_stop,
	.dequeue = (FastVideoDevice_dequeue_t)sv4l2_dequeue,
	.queue = (FastVideoDevice_queue_t)sv4l2_queue,
	.destroy = (FastVideoDevice_destroy_t)sv4l2_destroy,
};
FastVideoDevice_ops_t subdev_ops = {
	.name = "subv4l",
	.createconfig = sv4l2_subdev_createconfig,
	.create = (FastVideoDevice_create_t)sv4l2_subdev_create,
	.duplicate = NULL,
	.loadsettings = (FastVideoDevice_loadsettings_t)sv4l2_loadsettings,
	.capabilities = (FastVideoDevice_capabilities_t)sv4l2_subdev_capabilities,
	.requestbuffer = NULL,
	.eventfd = (FastVideoDevice_eventfd_t)sv4l2_fd,
	.start = NULL,
	.stop = NULL,
	.dequeue = NULL,
	.queue = NULL,
	.destroy = (FastVideoDevice_destroy_t)sv4l2_subdev_destroy,
};
#ifdef SDVB
FastVideoDevice_ops_t sdvb_ops = {
	.name = "dvb",
	.createconfig = sdvb_createconfig,
	.create = (FastVideoDevice_create_t)sdvb_create,
	.duplicate = (FastVideoDevice_duplicate_t)NULL,
	.loadsettings = (FastVideoDevice_loadsettings_t)sdvb_loadsettings,
	.requestbuffer = (FastVideoDevice_requestbuffer_t)sdvb_requestbuffer,
	.eventfd = (FastVideoDevice_eventfd_t)sdvb_fd,
	.start = (FastVideoDevice_start_t)sdvb_start,
	.stop = (FastVideoDevice_stop_t)sdvb_stop,
	.dequeue = (FastVideoDevice_dequeue_t)sdvb_dequeue,
	.queue = (FastVideoDevice_queue_t)sdvb_queue,
	.destroy = (FastVideoDevice_destroy_t)sdvb_destroy,
};
#endif
#ifdef HAVE_EGL
FastVideoDevice_ops_t segl_ops = {
	.name = "gpu",
	.createconfig = segl_createconfig,
	.create = (FastVideoDevice_create_t)segl_create,
	.duplicate = (FastVideoDevice_duplicate_t)NULL,
	.loadsettings = (FastVideoDevice_loadsettings_t)NULL,
	.requestbuffer = (FastVideoDevice_requestbuffer_t)segl_requestbuffer,
	.eventfd = (FastVideoDevice_eventfd_t)segl_fd,
	.start = (FastVideoDevice_start_t)segl_start,
	.stop = (FastVideoDevice_stop_t)segl_stop,
	.dequeue = (FastVideoDevice_dequeue_t)segl_dequeue,
	.queue = (FastVideoDevice_queue_t)segl_queue,
	.destroy = (FastVideoDevice_destroy_t)segl_destroy,
};
#endif
#ifdef HAVE_LIBDRM
FastVideoDevice_ops_t sdrm_ops = {
	.name = "screen",
	.createconfig = sdrm_createconfig,
	.create = (FastVideoDevice_create_t)sdrm_create,
	.duplicate = (FastVideoDevice_duplicate_t)NULL,
	.loadsettings = (FastVideoDevice_loadsettings_t)sdrm_loadsettings,
	.requestbuffer = (FastVideoDevice_requestbuffer_t)sdrm_requestbuffer,
	.eventfd = (FastVideoDevice_eventfd_t)NULL,
	.start = (FastVideoDevice_start_t)sdrm_start,
	.stop = (FastVideoDevice_stop_t)sdrm_stop,
	.dequeue = (FastVideoDevice_dequeue_t)sdrm_dequeue,
	.queue = (FastVideoDevice_queue_t)sdrm_queue,
	.destroy = (FastVideoDevice_destroy_t)sdrm_destroy,
};
#endif
FastVideoDevice_ops_t sfile_ops = {
	.name = "file",
	.createconfig = sfile_createconfig,
	.create = (FastVideoDevice_create_t)sfile_create,
	.duplicate = (FastVideoDevice_duplicate_t)NULL,
	.loadsettings = (FastVideoDevice_loadsettings_t)NULL,
	.requestbuffer = (FastVideoDevice_requestbuffer_t)sfile_requestbuffer,
	.eventfd = (FastVideoDevice_eventfd_t)NULL,
	.start = (FastVideoDevice_start_t)sfile_start,
	.stop = (FastVideoDevice_stop_t)sfile_stop,
	.dequeue = (FastVideoDevice_dequeue_t)sfile_dequeue,
	.queue = (FastVideoDevice_queue_t)sfile_queue,
	.destroy = (FastVideoDevice_destroy_t)sfile_destroy,
};

typedef struct FastVideoDevice_s FastVideoDevice_t;
struct FastVideoDevice_s
{
	DeviceConf_t *config;
	void *dev;
	int id;
	FastVideoDevice_ops_t *ops;
	FastVideoDevice_t *next;
};

int _createdevices(void *data, const char *name, const char *type, void *config)
{
	FastVideoDevice_t **devices = data;
	FastVideoDevice_ops_t *fastVideoDevice_ops[] =
	{
		&sv4l2_ops,
		&subdev_ops,
#ifdef SDVB
		&sdvb_ops,
#endif
#ifdef HAVE_EGL
		&segl_ops,
#endif
#ifdef HAVE_LIBDRM
		&sdrm_ops,
#endif
		&sfile_ops,
		NULL
	};
	for (int i = 0; fastVideoDevice_ops[i] != NULL; i++)
	{
		if (! strcmp(fastVideoDevice_ops[i]->name, type))
		{
			DeviceConf_t *devconfig = NULL;
			devconfig = fastVideoDevice_ops[i]->createconfig();
			if (devconfig)
			{
				devconfig->name = name;
				devconfig->type = type;
				devconfig->entry = config;
				devconfig->ops.loadconfiguration(devconfig, config);
				FastVideoDevice_t *device = NULL;
				device = calloc(1, sizeof(*device));
				device->config = devconfig;
				device->ops = fastVideoDevice_ops[i];
				device->next = *devices;
				*devices = device;
			}
		}
	}
	/// return always -1 because 0 stop the parsing
	return -1;
}

#ifdef HAVE_JANSSON
int _loadjsonsetting(FastVideoDevice_t *devices, const char *name, json_t *jentry)
{
	int ret = -1;
	for (FastVideoDevice_t *device = devices; device != NULL; device = device->next)
	{
		if (!strcmp(name, device->config->name) &&
			device->ops->loadsettings && device->dev)
		{
			dbg("loadsettings");
			ret = device->ops->loadsettings(device->dev, jentry);
			break;
		}
	}
	return ret;
}
int _loadsetting(FastVideoDevice_t *devices, client_t *clt, json_t *jentry)
{
	int ret = 0;
	if (jentry && json_is_object(jentry))
	{
		json_t *jname = json_object_get(jentry, "name");

		if (jname && json_is_string(jname))
			ret = _loadjsonsetting(devices, json_string_value(jname), jentry);
#define RESPONSE_LOADSETTING_OK "{\"cmd\":\"loadsetting\", \"status\":0}"
#define RESPONSE_LOADSETTING_KO "{\"cmd\":\"loadsetting\", \"status\":-1}"
		if (ret)
			client_send(clt, RESPONSE_LOADSETTING_OK, sizeof(RESPONSE_LOADSETTING_OK) - 1);
		else
			client_send(clt, RESPONSE_LOADSETTING_OK, sizeof(RESPONSE_LOADSETTING_KO) - 1);
	}
	return ret;
}

int _capabilities(FastVideoDevice_t *devices, client_t *clt, json_t *jentry)
{
	int ret = 0;
	int all = 0;
	int id = -2;
	const char *name = NULL;
	if (jentry && json_is_object(jentry))
	{
		json_t *jall = json_object_get(jentry, "all");
		all = json_boolean_value(jall);
		json_t *jname = json_object_get(jentry, "name");
		if (jname && json_is_string(jname))
			name = json_string_value(jname);
	}
	json_t *jdevices = json_array();
	for (FastVideoDevice_t *device = devices; device != NULL; device = device->next)
	{
		if (device->dev == NULL)
			continue;
		if (name && strcmp(name, device->config->name))
			continue;
		if (device->ops->capabilities == NULL)
			continue;
		json_t *jdevice = json_object();
		json_object_set_new(jdevice, "name", json_string(device->config->name));
		json_object_set_new(jdevice, "type", json_string(device->ops->name));

		if (name)
		{
			ret = device->ops->capabilities(device->dev, jdevice, all);
			json_array_append_new(jdevices, jdevice);
			break;
		}
		json_array_append_new(jdevices, jdevice);
	}
	json_t *jstatus = json_object();
	json_object_set_new(jstatus, "cmd", json_string("capabilities"));
	json_object_set_new(jstatus, "status", json_integer(ret));
	json_t *jdata = json_object();
	json_object_set_new(jdata, "devices", jdevices);
	json_object_set_new(jstatus, "data", jdata);
	char *status = json_dumps(jstatus, 0);
	size_t length = strnlen(status, UNIXSOCKET_PACKETSIZE);
	dbg("send %s", status);
	client_send(clt, status, length);
	free(status);
	json_decref(jstatus);
	return ret;
}
int _runcmd(FastVideoDevice_t *devices, client_t *clt, json_t *jentry)
{
	int ret = -1;
	json_t *jcmd = json_object_get(jentry, "cmd");
	json_t *jdata = json_object_get(jentry, "data");
	if (jcmd && json_is_string(jcmd))
	{
		if (!strcmp(json_string_value(jcmd), "loadsetting"))
			ret = _loadsetting(devices, clt, jdata);
		else if (!strcmp(json_string_value(jcmd), "capabilities"))
			ret = _capabilities(devices, clt, jdata);
		else
		{
			char cmd_unkonwn[64] = {0};
			size_t length = snprintf(cmd_unkonwn, sizeof(cmd_unkonwn) - 1, "{\"cmd\":\"%.*s\", \"status\":-1}", 64 - 27 - 1, json_string_value(jcmd));
			client_send(clt, cmd_unkonwn, length);
		}
	}
	else
	{
		char cmd_unkonwn[] = "{\"cmd\":\"unknown\", \"status\":-1}";
		client_send(clt, cmd_unkonwn, sizeof(cmd_unkonwn) - 1);
	}
	return ret;
}

int _server_control(void *data, client_t *clt, const char *buffer, size_t length)
{
	int ret = -1;
	dbg("receive: %.*s", length, buffer);
	FastVideoDevice_t *devices = data;
	json_error_t error;
	json_t *jentry = json_loadb(buffer, length, JSON_DECODE_ANY, &error);
	if (jentry && json_is_object(jentry))
	{
		ret = _runcmd(devices, clt, jentry);
	}
	else
	{
		char cmd_unkonwn[] = "{\"cmd\":\"unknown\", \"status\":-1}";
		client_send(clt, cmd_unkonwn, sizeof(cmd_unkonwn) - 1);
	}
	return ret;
}
#else
int _server_control(void *data, client_t *clt, const char *buffer, size_t length)
{
	return 0;
}
#endif
int main(int argc, char * const argv[])
{
	const char *owner = NULL;
	const char *pidfile= NULL;
	const char *configfile = NULL;
	const char *serverpath = "/tmp/fastsetting_socket";
	unsigned int mode = 0;
	const char *logfile = "-";
	const char *cwd = NULL;

	int opt;
	do
	{
		opt = getopt(argc, argv, "j:L:W:");
		switch (opt)
		{
			case 'j':
				configfile = optarg;
			break;
			case 'L':
				logfile = optarg;
			break;
			case 'W':
				cwd = optarg;
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

	if (cwd != NULL && chdir(cwd) != 0)
		err("main: working directory %m");

	FastVideoDevice_t *devices = NULL;
	config_parseconfigfile(configfile, _createdevices, &devices);
	if (devices == NULL)
		return -1;

	for (FastVideoDevice_t *device = devices; device != NULL; device = device->next)
	{
		device->dev = device->ops->create("any" , device_control, device->config);
		if (device->dev == NULL)
			continue;
		if (device->ops->loadsettings && device->config->entry)
		{
			dbg("loadsettings");
			device->ops->loadsettings(device->dev, device->config->entry);
		}
	}
	server_t *server = server_create(serverpath, 2);
	server_attach_receive(server, _server_control, devices);

	server_run(server);
	FastVideoDevice_t *next = NULL;
	for (FastVideoDevice_t *device = devices; device != NULL; device = next)
	{
		next = device->next;
		if (device->dev)
			device->ops->destroy(device->dev);
	}
	server_destroy(server);

	return 0;
}
