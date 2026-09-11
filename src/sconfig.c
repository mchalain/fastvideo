#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>

#include <jansson.h>

#include "log.h"
#include "sconfig.h"
#include "sv4l2.h"
#include "segl.h"
#include "sdrm.h"
static json_t *g_jconfig = NULL;

int sconfig_loaddefinition(DeviceConf_t *config, json_t *definition)
{
	json_t *x = NULL;
	json_t *y = NULL;
	json_t *width = NULL;
	json_t *height = NULL;
	json_t *fourcc = NULL;
	json_t *stride = NULL;
	json_t *fps = NULL;
	json_t *modifiers = NULL;

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
					!strcmp(json_string_value(name), "x"))
				{
					x = field;
				}
				if (name && json_is_string(name) &&
					!strcmp(json_string_value(name), "y"))
				{
					y = field;
				}
				if (name && json_is_string(name) &&
					!strcmp(json_string_value(name), "width"))
				{
					width = field;
				}
				if (name && json_is_string(name) &&
					!strcmp(json_string_value(name), "height"))
				{
					height = field;
				}
				if (name && json_is_string(name) &&
					!strcmp(json_string_value(name), "stride"))
				{
					stride = field;
				}
				if (name && json_is_string(name) &&
					!strcmp(json_string_value(name), "fourcc"))
				{
					fourcc = field;
				}
				if (name && json_is_string(name) &&
					!strcmp(json_string_value(name), "fps"))
				{
					fps = field;
				}
				if (name && json_is_string(name) &&
					!strcmp(json_string_value(name), "modifier"))
				{
					modifiers = field;
				}
			}
		}
	}
	else if (definition && json_is_object(definition))
	{
		x = json_object_get(definition, "x");
		y = json_object_get(definition, "y");
		width = json_object_get(definition, "width");
		height = json_object_get(definition, "height");
		fourcc = json_object_get(definition, "fourcc");
		stride = json_object_get(definition, "stride");
		fps = json_object_get(definition, "fps");
		modifiers = json_object_get(definition, "modifier");
	}
	else
		return 0;
	if (width && json_is_object(width))
		width = json_object_get(width, "value");
	if (width && !config->width && json_is_integer(width))
		config->width = json_integer_value(width);
	if (x && json_is_object(x))
		x = json_object_get(x, "value");
	if (x && !config->x && json_is_integer(x))
		config->x = json_integer_value(x);
	if (y && json_is_object(y))
		y = json_object_get(y, "value");
	if (y && !config->y && json_is_integer(y))
		config->y = json_integer_value(y);
	if (height && json_is_object(height))
		height = json_object_get(height, "value");
	if (height && !config->height && json_is_integer(height))
		config->height = json_integer_value(height);
	if (stride && json_is_object(stride))
		stride = json_object_get(stride, "value");
	if (stride && !config->stride && json_is_integer(stride))
		config->stride = json_integer_value(stride);
	if (fps && json_is_object(fps))
		fps = json_object_get(fourcc, "fps");
	if (fps && config->fps == -1 && json_is_integer(fps))
		config->fps = json_integer_value(fps);
	if (fourcc && json_is_object(fourcc))
		fourcc = json_object_get(fourcc, "value");
	if (fourcc && !config->fourcc && json_is_string(fourcc))
	{
		const char *value = json_string_value(fourcc);
		config->fourcc = FOURCC(value[0], value[1], value[2], value[3]);
	}
	if (modifiers && json_is_object(modifiers))
		modifiers = json_object_get(modifiers, "value");
	if (modifiers && !config->modifiers && json_is_integer(modifiers))
		config->modifiers = json_integer_value(modifiers);
	return 0;
}

int sconfig_mergedefinition(DeviceConf_t *dest, DeviceConf_t *src)
{
	if (!dest->x)
		dest->x = src->x;
	if (!dest->y)
		dest->y = src->y;
	if (!dest->width)
		dest->width = src->width;
	if (!dest->height)
		dest->height = src->height;
	if (!dest->stride)
		dest->stride = src->stride;
	if (!dest->modifiers)
		dest->modifiers = src->modifiers;
	if (!dest->fourcc)
		dest->fourcc = src->fourcc;
	if (!dest->fps)
		dest->fps = src->fps;
	return 0;
}

static const char unknown_str[] = "unknown";
static int main_parseconfigdevice(json_t *jconfig, int (*cb)(void *data, const char *name, const char *type, void *config), void *data)
{
	int ret = -1;
	json_t *jtype = NULL;
	const char *type = unknown_str;

	jtype = json_object_get(jconfig, "type");
	if (jtype && json_is_string(jtype))
		type = json_string_value(jtype);

	json_t *jname = json_object_get(jconfig, "name");
	json_t *disable = json_object_get(jconfig, "disable");
	if (disable && json_is_true(disable))
	{
		if (jname && json_is_array(jname))
			jname = json_array_get(jname, 0);
		warn("config: device %s is disabled", json_string_value(jname));
		return -1;
	}
	if (jname && json_is_array(jname))
	{
		int index;
		json_t *jit;
		json_array_foreach(jname, index, jit)
		{
			if (jit && json_is_string(jit))
			{
				ret = cb(data, json_string_value(jit), type, jconfig);
				if (ret >= 0)
					break;
			}
		}
	}
	if (jname && json_is_string(jname))
	{
		ret = cb(data, json_string_value(jname), type, jconfig);
	}
	return ret;
}

int scommon_loaddevice(json_t *jconfig, int (*cb)(void *data, const char *name, const char *type, void *config), void *data)
{
	int ret = -1;
	if (json_is_array(jconfig))
	{
		int index = 0;
		json_t *jdevice = NULL;
		json_array_foreach(jconfig, index, jdevice)
		{
			if (!json_is_object(jdevice))
				continue;
			ret = main_parseconfigdevice(jdevice, cb, data);
			if (ret == 0)
				break;
		}
	}
	else if (json_is_object(jconfig))
	{
		ret = main_parseconfigdevice(jconfig, cb, data);
	}
	return ret;
}

json_t *scommon_getdevice(json_t *jconfig)
{
	if (json_is_object(jconfig))
	{
		json_t *devices = json_object_get(jconfig, "devices");
		if (devices)
			jconfig = devices;
		else
			devices = json_object_get(jconfig, "subdevices");
		if (devices)
			jconfig = devices;
	}
	return jconfig;
}

json_t *sconfig_getdevice(DeviceConf_t *config)
{
	json_t *jconfig = config->entry;
	return scommon_getdevice(jconfig);
}

int scommon_parseconfigfile(const char *configfile, int (*loaddevice)(void *data, const char *name, const char *type, void *config), void *data)
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
		err("config %s:%d error %s", configfile, error.line, error.text);
		return -1;
	}
	jconfig = scommon_getdevice(jconfig);
	ret = scommon_loaddevice(jconfig, loaddevice, data);
	g_jconfig = jconfig;
	fclose(cf);
	return ret;
}

DeviceConf_t *config_create(const char *name, FastVideoDevice_ops_t *ops, void *entry)
{
	DeviceConf_t *devconfig = NULL;
	if (ops->createconfig)
		devconfig = ops->createconfig(name);
	if (devconfig)
	{
		devconfig->name = name;
		devconfig->type = ops->name;
		devconfig->entry = entry;
		char *opts = strchr(name, ':');
		if (opts && opts[1] == '/' && opts[2] == '/')
		{
			opts = strchr(name, '?');
			if (opts)
			{
				opts++;
			}
		}
		const char *x = NULL;
		const char *y = NULL;
		const char *width = NULL;
		const char *height = NULL;
		const char *stride = NULL;
		const char *fourcc = NULL;
		const char *fps = NULL;
		if (opts)
		{
			x = strstr(opts, "x=");
			y = strstr(opts, "y=");
			width = strstr(opts, "width=");
			height = strstr(opts, "height=");
			stride = strstr(opts, "stride=");
			fourcc = strstr(opts, "fourcc=");
			fps = strstr(opts, "fps=");
		}
		if (x)
		{
			devconfig->x = strtol(x + 2, NULL, 10);
		}
		if (y)
		{
			devconfig->y = strtol(y + 2, NULL, 10);
		}
		if (width)
		{
			devconfig->width = strtol(width + 6, NULL, 10);
		}
		if (height)
		{
			devconfig->height = strtol(height + 7, NULL, 10);
		}
		if (stride)
		{
			devconfig->stride = strtol(stride + 7, NULL, 10);
		}
		if (fourcc)
		{
			unsigned char _fourcc[4] = {0x20, 0x20, 0x20, 0x20};
			const char *value = fourcc + 7; /* on saute "fourcc=" */

			for (int i = 0; i < 4 && value[i] != '\0' && value[i] != ','; i++)
				_fourcc[i] = value[i];

			devconfig->fourcc = FOURCC(_fourcc[0], _fourcc[1], _fourcc[2], _fourcc[3]);
		}
		if (fps)
		{
			devconfig->fps = strtol(fps + 4, NULL, 10);
		}
	}
	return devconfig;
}

int sconfig_isnamed(DeviceConf_t *devconfig, const char *name)
{
	return scommon_isnamed(devconfig->entry, name);
}

int scommon_isnamed(json_t *jdevice, const char *name)
{
	json_t *jname = json_object_get(jdevice, "name");
	if (jname && json_is_array(jname))
	{
		int index = 0;
		json_t *jit = NULL;
		json_array_foreach(jname, index, jit)
		{
			if (jit && json_is_string(jit) && !strcmp(json_string_value(jit),name))
			{
				return 1;
			}
		}
	}
	if (jname && json_is_string(jname) && !strcmp(json_string_value(jname),name))
		return 1;
	return 0;
}

int scommon_loadconfiguration(void *arg, void *entry)
{
	DeviceConf_t *devconfig = (DeviceConf_t *)arg;
	json_t *jconfig = (json_t *)entry;

	if (jconfig && json_is_object(jconfig))
	{
		json_t *definition = json_object_get(jconfig, "definition");
		if (definition)
			sconfig_loaddefinition(devconfig, definition);
	}
	return 0;
}
