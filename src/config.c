#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <dlfcn.h>

#include <jansson.h>

#include "log.h"
#include "config.h"
#include "sv4l2.h"
#include "segl.h"
#include "sdrm.h"

static int main_parseconfigdevice(json_t *jconfig, DeviceConf_t *devconfig)
{
	int ret;
	json_t *type = NULL;
	json_t *width = NULL;
	json_t *height = NULL;
	json_t *fourcc = NULL;
	json_t *stride = NULL;

	devconfig->entry = jconfig;

	json_t *definition = json_object_get(jconfig, "definition");
	if (definition && json_is_array(definition))
	{
		json_t *field = NULL;
		int index = 0;
		json_array_foreach(definition, index, field)
		{
			if (json_is_object(field))
			{
				json_t *name = json_object_get(field, "name");
				if (name && json_is_string(name) &&
					!strcmp(json_string_value(name), "type"))
				{
					type = json_object_get(field, "value");
				}
				if (name && json_is_string(name) &&
					!strcmp(json_string_value(name), "width"))
				{
					width = json_object_get(field, "value");
				}
				if (name && json_is_string(name) &&
					!strcmp(json_string_value(name), "height"))
				{
					height = json_object_get(field, "value");
				}
				if (name && json_is_string(name) &&
					!strcmp(json_string_value(name), "fourcc"))
				{
					fourcc = json_object_get(field, "value");
				}
			}
		}
	}
	else if (definition && json_is_object(definition))
	{
		type = json_object_get(jconfig, "type");
		width = json_object_get(definition, "width");
		height = json_object_get(definition, "height");
		fourcc = json_object_get(definition, "fourcc");
		stride = json_object_get(definition, "stride");
	}
	else
	{
		type = json_object_get(jconfig, "type");
		width = json_object_get(jconfig, "width");
		height = json_object_get(jconfig, "height");
		fourcc = json_object_get(jconfig, "fourcc");
		stride = json_object_get(jconfig, "stride");
	}
	if (type && json_is_string(type))
	{
		devconfig->type = json_string_value(type);
	}
	if (width && json_is_integer(width))
	{
		devconfig->width = json_integer_value(width);
	}
	if (height && json_is_integer(height))
	{
		devconfig->height = json_integer_value(height);
	}
	if (fourcc && json_is_string(fourcc))
	{
		const char *value = json_string_value(fourcc);
		devconfig->fourcc = FOURCC(value[0], value[1], value[2], value[3]);
	}
	if (stride && json_is_integer(stride))
	{
		devconfig->stride = json_integer_value(stride);
	}

	if (devconfig->ops.loadconfiguration)
	{
		ret = devconfig->ops.loadconfiguration(devconfig, jconfig);
	}
	return ret;
}

static int main_parseconfigdevices(const char *name, json_t *jconfig, DeviceConf_t *devconfig)
{
	int ret;
	if (name == NULL)
		return -1;
	char tmpname[256] = {0};
	const char *end = strchr(name, ':');
	int length = strlen(name);
	if (end)
		length = end - name;
	if (length > 255)
		return -1;
	strncpy(tmpname, name, length);

	devconfig->name = name;

	if (json_is_array(jconfig))
	{
		int index = 0;
		json_t *jdevice = NULL;
		/**
		 * json format:
		 * [{"name":"cam","device":"/dev/video0","controls":[{"name":"Gain","value":1000},{"name":"Exposure","value":1}]}]
		 */
		json_array_foreach(jconfig, index, jdevice)
		{
			if (!json_is_object(jdevice))
				continue;
			json_t *jname = json_object_get(jdevice, "name");
			if (jname && json_is_string(jname) &&
				!strcmp(json_string_value(jname), tmpname))
			{
				ret = main_parseconfigdevice(jdevice, devconfig);
				break;
			}
		}
	}
	else if (json_is_object(jconfig))
	{
		/**
		 * json format:
		 * { "cam":{"device":"/dev/video0","Gain":1000,"Exposure":1}
		 */
		json_t *jdevice = json_object_get(jconfig, tmpname);
		if (jdevice && json_is_object(jdevice))
		{
			ret = main_parseconfigdevice(jdevice, devconfig);
		}
		else
		{
			ret = main_parseconfigdevice(jconfig, devconfig);
		}
	}
	return ret;
}

int config_parseconfigfile(const char *name, const char *configfile, DeviceConf_t *devconfig)
{
	int ret = -1;
	FILE *cf = fopen(configfile, "r");
	if (cf == NULL)
	{
		err("config %s error %m", configfile);
		return -1;
	}
	json_t *jconfig;
	json_error_t error;
	jconfig = json_loadf(cf, 0, &error);
	if (! jconfig || !(json_is_object(jconfig) || json_is_array(jconfig)))
	{
		err("config %s error %s", configfile, error.text);
		return -1;
	}
	if (json_is_array(jconfig))
	{
		ret = main_parseconfigdevices(name, jconfig, devconfig);
	}
	else if (json_is_object(jconfig))
	{
		json_t *devices = json_object_get(jconfig, "devices");
		if (devices)
			ret = main_parseconfigdevices(name, devices, devconfig);
		else
			ret = main_parseconfigdevices(name, jconfig, devconfig);
	}
	fclose(cf);
	return ret;
}
