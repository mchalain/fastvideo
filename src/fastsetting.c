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

int _createdevices(void *data, const char *name, const char *type, void *config)
{
	FastVideoList_t **devices = data;
	for (FastVideoDevice_ops_t *ops = fastvideodevice_ops_next(NULL);
		ops != NULL; ops = fastvideodevice_ops_next(ops))
	{
		if (! strcmp(ops->name, type))
		{
			DeviceConf_t *devconfig = NULL;
			devconfig = config_create(name, ops, config);
			if (devconfig)
			{
				devconfig->ops.loadconfiguration(devconfig, config);
				FastVideoDevice_t *device = NULL;
				device = calloc(1, sizeof(*device));
				device->config = devconfig;
				device->ops = ops;
				*devices = fastvideolist_insert(*devices, device);
				err("new config for %s %p", name, devconfig);
				/// 1 will stop the loop about the names list but continue the loop about the devices list
				return 1;
			}
		}
	}
	/// return always -1 because 0 stop the parsing
	return -1;
}

#ifdef HAVE_JANSSON
int _loadjsonsetting(FastVideoList_t *devices, const char *name, json_t *jentry)
{
	int ret = -1;
	fastvideolist_first(devices);
	dbg("loadsettings look for %s", name);
	for (FastVideoDevice_t *device = fastvideolist_next(devices);
			device != NULL; device = fastvideolist_next(devices))
	{
		if (device->config && config_isnamed(device->config, name) &&
			device->ops->loadsettings && device->dev)
		{
			warn("loadsttings for %s", name);
			ret = device->ops->loadsettings(device->dev, jentry);
			break;
		}
	}
	return ret;
}
int _loadsetting(FastVideoList_t *devices, client_t *clt, json_t *jsetting)
{
	int ret = 0;
	if (jsetting && json_is_array(jsetting))
	{
		json_t *jentry;
		int index;
		json_array_foreach(jsetting, index, jentry)
		{
			ret = _loadsetting(devices, clt, jentry);
		}
	}
	if (jsetting && json_is_object(jsetting))
	{
		json_t *jname = json_object_get(jsetting, "name");
		if (jname && json_is_array(jname))
			jname = json_array_get(jname, 0);
		if (jname && json_is_string(jname))
			ret = _loadjsonsetting(devices, json_string_value(jname), jsetting);
#define RESPONSE_LOADSETTING_OK "{\"cmd\":\"loadsetting\", \"status\":0}"
#define RESPONSE_LOADSETTING_KO "{\"cmd\":\"loadsetting\", \"status\":-1}"
		if (clt && ret > 0)
			client_send(clt, RESPONSE_LOADSETTING_OK, sizeof(RESPONSE_LOADSETTING_OK) - 1);
		else if (clt)
			client_send(clt, RESPONSE_LOADSETTING_KO, sizeof(RESPONSE_LOADSETTING_KO) - 1);
	}
	return ret;
}

int _capabilities(FastVideoList_t *devices, client_t *clt, json_t *jentry)
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
	fastvideolist_first(devices);
	for (FastVideoDevice_t *device = fastvideolist_next(devices);
			device != NULL; device = fastvideolist_next(devices))
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
	dbg("send %lu %.*s", length, length, status);
	client_send(clt, status, length);
	free(status);
	json_decref(jstatus);
	return ret;
}
int _runcmd(FastVideoList_t *devices, client_t *clt, json_t *jentry)
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

ssize_t _server_control(void *data, client_t *clt, const char *buffer, size_t length)
{
	int ret = -1;
	dbg("receive: %.*s", length, buffer);
	FastVideoList_t *devices = data;
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
ssize_t _server_control(void *data, client_t *clt, const char *buffer, size_t length)
{
	return 0;
}
#endif

void _device_destroy(void *arg)
{
	FastVideoDevice_t *device = arg;
	if (device->dev)
		device->ops->destroy(device->dev);
	free(device);
}

int main(int argc, char * const argv[])
{
	const char *owner = NULL;
	const char *pidfile= NULL;
	const char *configfile = NULL;
	const char *serverpath = FASTSETTING_DEFAULT_SERVER;
	unsigned int mode = 0;
	const char *logfile = "-";
	const char *cwd = NULL;
	FastVideoList_t *settings = NULL;

#ifdef V4L2_SUBDEV
	fastvideodevice_ops_append(&subdev_ops);
#endif

	int opt;
	do
	{
		opt = getopt(argc, argv, "j:J:s:L:W:IDP:");
		switch (opt)
		{
			case 'j':
				configfile = optarg;
			break;
			case 'J':
				settings = fastvideolist_insert(settings, optarg);
			break;
			case 's':
				serverpath = optarg;
			break;
			case 'L':
				logfile = optarg;
			break;
			case 'W':
				cwd = optarg;
			break;
			case 'I':
				mode |= MODE_INITIALIZE;
			break;
			case 'D':
				mode |= MODE_DAEMONIZE;
			break;
			case 'P':
				pidfile = optarg;
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

	daemonize((mode & MODE_DAEMONIZE) == MODE_DAEMONIZE, pidfile, owner, cwd);

	FastVideoList_t *devices = NULL;
	config_parseconfigfile(configfile, _createdevices, &devices);
	if (devices == NULL)
		return -1;

	for (FastVideoDevice_t *device = fastvideolist_next(devices);
			device != NULL; device = fastvideolist_next(devices))
	{
		device->dev = device->ops->create("any" , device_control, device->config);
		if (device->dev == NULL)
			continue;
		if (device->ops->loadsettings && device->config->entry)
		{
			dbg("loadsettings");
			device->ops->loadsettings(device->dev, device->config->entry);
			json_t *subdevices = config_getdevices(device->config->entry);
			if (subdevices != device->config->entry)
			{
				err("subdevices are presents");
				config_loaddevice(subdevices, _createdevices, &devices);
			}
		}
	}
	for (const char *configfile = fastvideolist_next(settings); configfile != NULL; configfile = fastvideolist_next(settings))
	{
		FILE *cf = fopen(configfile, "r");
		if (cf == NULL)
		{
			err("config %s error %m", configfile);
			return -1;
		}
		json_t *jsettings;
		json_error_t error;
		jsettings = json_loadf(cf, 0, &error);
		if (! jsettings || !(json_is_object(jsettings) || json_is_array(jsettings)))
		{
			err("config %s:%d error %s", configfile, error.line, error.text);
			return -1;
		}
		warn("load json file %s", configfile);
		_loadsetting(devices, NULL, jsettings);

	}
	if ((mode & MODE_INITIALIZE) == 0)
	{
		server_t *server = server_create(serverpath, 2);
		if (server == NULL)
			return -1;
		warn("fastsetting server runs on %s", serverpath);
		server_attach_receive(server, _server_control, devices);

		server_run(server);
		killdaemon(pidfile);
		server_destroy(server);
	}
	fastvideolist_destroy(devices, _device_destroy);

	return 0;
}
