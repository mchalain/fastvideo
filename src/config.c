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
static json_t *g_jconfig = NULL;

int scommon_loaddefinition(DeviceConf_t *config, json_t *definition)
{
	json_t *width = NULL;
	json_t *height = NULL;
	json_t *fourcc = NULL;
	json_t *stride = NULL;
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
					!strcmp(json_string_value(name), "modifier"))
				{
					modifiers = field;
				}
			}
		}
	}
	else if (definition && json_is_object(definition))
	{
		width = json_object_get(definition, "width");
		height = json_object_get(definition, "height");
		fourcc = json_object_get(definition, "fourcc");
		stride = json_object_get(definition, "stride");
		modifiers = json_object_get(definition, "modifier");
	}
	else
		return 0;
	if (width && json_is_object(width))
		width = json_object_get(width, "value");
	if (width && json_is_integer(width))
		config->width = json_integer_value(width);
	if (height && json_is_object(height))
		height = json_object_get(height, "value");
	if (height && json_is_integer(height))
		config->height = json_integer_value(height);
	if (stride && json_is_object(stride))
		stride = json_object_get(stride, "value");
	if (stride && json_is_integer(stride))
		config->stride = json_integer_value(stride);
	if (fourcc && json_is_object(fourcc))
		fourcc = json_object_get(fourcc, "value");
	if (fourcc && json_is_string(fourcc))
	{
		const char *value = json_string_value(fourcc);
		config->fourcc = FOURCC(value[0], value[1], value[2], value[3]);
	}
	if (modifiers && json_is_object(modifiers))
		modifiers = json_object_get(modifiers, "value");
	if (modifiers && json_is_integer(modifiers))
		config->modifiers = json_integer_value(modifiers);
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

int config_loaddevice(json_t *jconfig, int (*cb)(void *data, const char *name, const char *type, void *config), void *data)
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

json_t *config_getdevices(json_t *jconfig)
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

struct _common_getdevice_s
{
	const char *name;
	json_t *entry;
};
static int _common_finddevice(void *data, const char *name, const char *type, void *config)
{
	int ret = -1;
	struct _common_getdevice_s *search = data;
	if (scommon_isnamed(config, search->name))
	{
		search->entry = config;
		ret = 0;
	}
	return ret;
}

json_t *scommon_getdevice(const char *name)
{
	struct _common_getdevice_s search = {0};
	search.name = name;
	if (g_jconfig)
		config_loaddevice(g_jconfig, _common_finddevice, &search);
	return	search.entry;
}

int config_parseconfigfile(const char *configfile, int (*loaddevice)(void *data, const char *name, const char *type, void *config), void *data)
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
	jconfig = config_getdevices(jconfig);
	ret = config_loaddevice(jconfig, loaddevice, data);
	g_jconfig = jconfig;
	fclose(cf);
	return ret;
}

DeviceConf_t *config_create(const char *name, FastVideoDevice_ops_t *ops, void *entry)
{
	DeviceConf_t *devconfig = NULL;
	devconfig = ops->createconfig();
	if (devconfig)
	{
		devconfig->name = name;
		devconfig->type = ops->name;
		devconfig->entry = entry;
	}
	return devconfig;
}

int config_isnamed(DeviceConf_t *devconfig, const char *name)
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

int scommon_parsedevices(const char *name, json_t *jconfig, DeviceConf_t *devconfig)
{
	if (jconfig && json_is_array(jconfig))
	{
		int index = 0;
		json_t *jdevice = NULL;
		json_array_foreach(jconfig, index, jdevice)
		{
			json_t *disable = json_object_get(jdevice, "disable");
			if (disable && json_is_true(disable))
				continue;
			if (!json_is_object(jdevice))
				continue;
			if (scommon_isnamed(jdevice, name))
			{
				jconfig = jdevice;
				break;
			}
		}
	}
	if (jconfig && json_is_object(jconfig))
	{
		json_t *disable = json_object_get(jconfig, "disable");
		if (disable && json_is_true(disable))
			return -1;
		if (!scommon_isnamed(jconfig, name))
			return -1;
		json_t *definition = json_object_get(jconfig, "definition");
		if (definition)
			scommon_loaddefinition(devconfig, definition);
	}
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
			scommon_loaddefinition(devconfig, definition);
	}
	return 0;
}
