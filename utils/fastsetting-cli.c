#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdarg.h>

#include <jansson.h>

#include "log.h"
#include "unixsocket.h"

typedef struct control_s control_t;
struct control_s
{
	char name[32];
	long unsigned id;
	enum {
		CTRL_UNKONWN,
		CTRL_INTEGER,
		CTRL_BOOLEAN,
	} type;
	union {
		int integer;
		int boolean;
	} value;
	control_t *next;
	control_t *previous;
};

typedef struct device_s device_t;
struct device_s
{
	char name[36];
	char type[36];
	int id;
	unsigned long width;
	unsigned long height;
	char fourcc[5];
	control_t *controls;
	device_t *next;
	device_t *previous;
};

typedef struct fastsetting_s fastsetting_t;

struct fastsetting_s
{
	client_t *client;
	device_t *devices;
	device_t *device;
};

/*********************
 * control_t API
 */
control_t *control_create(const char *name)
{
	control_t *control = calloc(1, sizeof(*control));
	snprintf(control->name, sizeof(control->name) -1, "%s", name);
	return control;
}

void control_destroy(control_t *control)
{
	free(control);
}

/*********************
 * device_t API
 */
device_t *device_create(const char *name, const char *type, int id)
{
	device_t *device = calloc(1, sizeof(*device));
	snprintf(device->name, sizeof(device->name) -1, "%s", name);
	snprintf(device->type, sizeof(device->type) -1, "%s", type);
	device->id = id;
	return device;
}

void device_destroy(device_t *device)
{
	control_t *next = NULL;
	for (control_t *control = device->controls; control != NULL; control = next)
	{
		next = control->next;
		control_destroy(control);
	}
	free(device);
}

int _device_destroyall(device_t *devices)
{
	device_t *next = NULL;
	for (device_t *device = devices; device != NULL; device = next)
	{
		next = device->next;
		device_destroy(device);
	}
}

device_t *device_find_byname(device_t *devices, const char *name)
{
	for (device_t *device = devices; device != NULL; device = device->next)
	{
		if (!strncmp(name, device->name, sizeof(device->name) - 2))
			return device;
	}
	return NULL;
}

device_t *device_find_byid(device_t *devices, int id)
{
	device_t *device = devices;
	for (; device != NULL; device = device->next)
	{
		if (id == 0)
			break;
		id--;
	}
	return device;
}

int _client_receive_capabilities(fastsetting_t *data, client_t *clt, json_t *jdata)
{
	json_t *jdevices = json_object_get(jdata, "devices");
	if (jdevices != NULL && !json_is_array(jdevices))
		return -1;

	int index = -1;
	json_t *jdevice = NULL;
	json_array_foreach(jdevices, index, jdevice)
	{
		json_t *jname = json_object_get(jdevice, "name");
		if (jname == NULL || !json_is_string(jname))
			continue;
		json_t *jtype = json_object_get(jdevice, "type");
		if (jtype == NULL || !json_is_string(jtype))
			continue;
		json_t *jdefinition = json_object_get(jdevice, "definition");
		if (jdefinition && !(json_is_object(jdefinition) || json_is_array(jdefinition)))
			continue;
		json_t *jcontrols = json_object_get(jdevice, "controls");
		if (jcontrols && !json_is_array(jcontrols))
			continue;
		device_t *device = NULL;
		if (data->devices)
		{
			device = device_find_byname(data->devices, json_string_value(jname));
		}
		if (device == NULL)
		{
			device = device_create(json_string_value(jname), json_string_value(jtype), index);
			device->next = data->devices;
			if (data->devices)
				data->devices->previous = device;
			data->devices = device;
		}

		if (jdefinition && json_is_object(jdefinition))
		{
			json_t *jvalue;
			jvalue = json_object_get(jdefinition, "width");
			if (jvalue && json_is_integer(jvalue))
				device->width = json_integer_value(jvalue);
			jvalue = json_object_get(jdefinition, "height");
			if (jvalue && json_is_integer(jvalue))
				device->height = json_integer_value(jvalue);
			jvalue = json_object_get(jdefinition, "fourcc");
			if (jvalue && json_is_string(jvalue))
				snprintf(device->fourcc, sizeof(device->fourcc) -1, "%s", json_string_value(jvalue));
		}
		if (jdefinition && json_is_array(jdefinition))
		{
			int index = -1;
			json_t *jfield = NULL;
			json_array_foreach(jdefinition, index, jfield)
			{
				long unsigned *integerfield = NULL;
				char *stringfield = NULL;
				json_t *jvalue;
				jvalue = json_object_get(jfield, "name");
				if (jvalue && json_is_string(jvalue))
				{
					const char *name = json_string_value(jvalue);
					if (!strcmp(name, "width"))
						integerfield = &device->width;
					if (!strcmp(name, "height"))
						integerfield = &device->height;
					if (!strcmp(name, "fourcc"))
						stringfield = device->fourcc;
				}
				jvalue = json_object_get(jfield, "value");
				if (jvalue && json_is_integer(jvalue) && integerfield != NULL)
				{
					*integerfield = json_integer_value(jvalue);
				}
				if (jvalue && json_is_string(jvalue) && stringfield != NULL)
				{
					snprintf(stringfield, 4, "%s", json_string_value(jvalue));
				}
			}
		}
		if (jcontrols && json_is_array(jcontrols))
		{
			int index = -1;
			json_t *jfield = NULL;
			json_array_foreach(jcontrols, index, jfield)
			{
				json_t *jvalue;
				jvalue = json_object_get(jfield, "id");
				if (jvalue == NULL || !json_is_integer(jvalue))
				{
					continue;
				}
				unsigned long id = json_integer_value(jvalue);
				jvalue = json_object_get(jfield, "name");
				if (jvalue == NULL || !json_is_string(jvalue))
				{
					continue;
				}

				control_t *ctrl = NULL;
				for (control_t *control = device->controls; control != NULL; control = control->next)
				{
					if (control->id == id)
					{
						ctrl = control;
						break;
					}
				}
				if (ctrl == NULL)
				{
					ctrl = control_create(json_string_value(jvalue));
					ctrl->id = id;
					ctrl->next = device->controls;
					if (device->controls)
						device->controls->previous = ctrl;
					device->controls = ctrl;
				}

				jvalue = json_object_get(jfield, "value");
				if (jvalue && json_is_boolean(jvalue))
				{
					ctrl->type = CTRL_BOOLEAN;
					ctrl->value.boolean = json_integer_value(jvalue);
				}
				else if (jvalue && json_is_integer(jvalue))
				{
					ctrl->type = CTRL_INTEGER;
					ctrl->value.integer = json_integer_value(jvalue);
				}
				else
				{
					err("control type unkown");
				}
			}
		}
	}
	return 0;
}

static int _client_receive_loadsetting(fastsetting_t *data, client_t *clt, json_t *jdata)
{
	return 0;
}

static int _client_receive(void *arg, client_t *clt, const char *buffer, size_t length)
{
	fastsetting_t *data = arg;
	dbg("receive %d: %.*s", length, length, buffer);

	json_error_t error;
	json_t *jentry = json_loadb(buffer, length, JSON_DECODE_ANY, &error);
	if (jentry == NULL || !json_is_object(jentry))
	{
		err("json error (%d - %d) %s", error.line, error.column, error.text);
		return -1;
	}
	json_t *jcmd = json_object_get(jentry, "cmd");
	if (jcmd == NULL || !json_is_string(jcmd))
		return -1;
	json_t *jstatus = json_object_get(jentry, "status");
	if (jstatus == NULL || !json_is_integer(jstatus))
		return -1;
	json_t *jdata = json_object_get(jentry, "data");
	if (jdata != NULL && !json_is_object(jdata))
		return -1;

	if (json_integer_value(jstatus) == -1)
		return -1;
	const char *cmd = json_string_value(jcmd);
	if (!strcmp(cmd, "capabilities"))
	{
		return _client_receive_capabilities(data, clt, jdata);
	}
	if (!strcmp(cmd, "loadsetting"))
	{
		return _client_receive_loadsetting(data, clt, jdata);
	}
	return -1;
}

int fastsetting_change(fastsetting_t * data, device_t *device, control_t *control)
{
	int ret = -1;
	json_t *jrequest = json_object();
	json_object_set_new(jrequest, "cmd", json_string("loadsetting"));

	json_t *jdata = json_object();
	json_object_set_new(jdata, "name", json_string(device->name));
	json_t *jcontrol = json_object();
	json_object_set_new(jcontrol, "id", json_integer(control->id));
	json_object_set_new(jcontrol, "value", json_integer(control->value.integer));
	json_t *jcontrols = json_array();
	json_array_append_new(jcontrols, jcontrol);
	json_object_set_new(jdata, "controls", jcontrols);
	json_object_set_new(jrequest, "data", jdata);

	char *request = json_dumps(jrequest, 0);
	size_t length = strnlen(request, UNIXSOCKET_PACKETSIZE);
	dbg("send %s", request);
	if (client_request(data->client, request, length) < 0)
		ret = 0;
	return ret;
}

device_t *fastsetting_device(fastsetting_t *data, int index)
{
	device_t *device = NULL;
	for (device = data->devices; device != NULL && device->id != index; device = device->next);
	return device;
}

int fastsetting_capabilities(fastsetting_t *data, device_t *device)
{
	int ret = 0;
	json_t *jrequest = json_object();
	json_object_set_new(jrequest, "cmd", json_string("capabilities"));

	json_t *jdata = json_object();
	json_object_set_new(jdata, "name", json_string(device->name));
	json_object_set_new(jrequest, "data", jdata);

	char *request = json_dumps(jrequest, 0);
	size_t length = strnlen(request, UNIXSOCKET_PACKETSIZE);
	dbg("send %s", request);
	if (client_request(data->client, request, length) < 0)
		ret = -1;
	free(request);
	json_decref(jrequest);
	return ret;
}

fastsetting_t *fastsetting_create(const char *serverpath)
{
	fastsetting_t *data = calloc(1, sizeof(*data));
	data->client = client_create(serverpath);
	client_attach_receive(data->client, _client_receive, &data);
	const char cmd_capabilities[] = "{\"cmd\":\"capabilities\",\"data\":{\"all\":true}}";
	if (client_request(data->client, (void*)cmd_capabilities, sizeof(cmd_capabilities) - 1) < 0)
	{
		client_destroy(data->client);
		free(data);
		return NULL;
	}
	return data;
}

void fastsetting_destroy(fastsetting_t *data)
{
	client_destroy(data->client);
	free(data);
}
