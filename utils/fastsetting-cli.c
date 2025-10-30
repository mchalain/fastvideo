#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdarg.h>

#ifdef HAVE_NCURSES
#include <ncurses.h>
#else
#define WINDOW void
#endif
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

typedef struct win_s win_t;
struct win_s
{
	WINDOW *win;
	int level;
	win_t *parent;
};

typedef struct app_s app_t;
typedef struct env_s env_t;

struct env_s
{
	int (*openwindow)(app_t *app, int level);
	int (*printf)(app_t *app, const char *fmt, ...);
	char (*getc)(app_t *app);
	void (*closewindow)(app_t *app);
};

struct app_s
{
	client_t *client;
	struct
	{
		unsigned int w;
		unsigned int h;
	} screen;
	int level;
	win_t *window;
	env_t *env;
	device_t *devices;
	device_t *device;
};

#define MODE_NCURSES 0x0001
int g_mode = 0;

#ifdef HAVE_NCURSES
/*********************
 * env_t API for ncurses
 */
int _ncurses_openwindow(app_t *app, int level)
{
	unsigned int w = 80, h = 50;
	win_t *window = calloc(1, sizeof(*window));
	window->level = level;
	int x = (app->screen.w - w) / 2;
	int y = (app->screen.h - h) / 2;
	window->win = newwin(w, h, x, y);
	box(window->win, 0,0);
	window->parent = app->window;
	app->window = window;
	return window->level;
}

int _ncurses_printf(app_t *app, const char *fmt, ...)
{
	int ret;
	va_list ap;
	va_start(ap, fmt);
	ret = vwprintw(app->window->win, fmt, ap);
	va_end(ap);
	return ret;
}

char _ncurses_getc(app_t *app)
{
	wrefresh(app->window->win);
	return getch();
}

void _ncurses_closewindow(app_t *app)
{
	wborder(app->window->win, ' ', ' ', ' ',' ',' ',' ',' ',' ');
	wrefresh(app->window->win);
	delwin(app->window->win);
	win_t *old = app->window;
	app->window = old->parent;
	free(old);
}

env_t _ncurses_env =
{
	.openwindow = _ncurses_openwindow,
	.printf = _ncurses_printf,
	.getc = _ncurses_getc,
	.closewindow = _ncurses_closewindow,
};
#endif
/*********************
 * env_t API for simple
 */
int _simple_openwindow(app_t *app, int level)
{
	return level;
}

int _simple_printf(app_t *app, const char *fmt, ...)
{
	int ret;
	va_list ap;
	va_start(ap, fmt);
	ret = vprintf(fmt, ap);
	va_end(ap);
	return ret;
}

char _simple_getc(app_t *app)
{
	char ch = getc(stdin);
#if 0
	char next = getc(stdin);
	while (next != 0x0a)
	{
		next = getc(stdin);
	}
#endif
	return ch;
}

void _simple_closewindow(app_t *app)
{
}

env_t _simple_env =
{
	.openwindow = _simple_openwindow,
	.printf = _simple_printf,
	.getc = _simple_getc,
	.closewindow = _simple_closewindow,
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

int _client_receive_capabilities(app_t *app, client_t *clt, json_t *jdata)
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
		if (app->devices)
		{
			device = device_find_byname(app->devices, json_string_value(jname));
		}
		if (device == NULL)
		{
			device = device_create(json_string_value(jname), json_string_value(jtype), index);
			device->next = app->devices;
			if (app->devices)
				app->devices->previous = device;
			app->devices = device;
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

int _client_receive_loadsetting(app_t *app, client_t *clt, json_t *jdata)
{
	return 0;
}

int _client_receive(void *data, client_t *clt, const char *buffer, size_t length)
{
	app_t *app = data;
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
		return _client_receive_capabilities(app, clt, jdata);
	}
	if (!strcmp(cmd, "loadsetting"))
	{
		return _client_receive_loadsetting(app, clt, jdata);
	}
	return -1;
}

int _app_displaycontrol(app_t *app, control_t *control)
{
	switch (control->type)
	{
	case CTRL_INTEGER:
	case CTRL_BOOLEAN:
		app->env->printf(app, "%.04x % .32s\t\t: %d\n", control->id, control->name, control->value.integer);
	break;
	}
	return 0;
}

int _app_displaydevice(app_t *app, device_t *device)
{
	app->env->printf(app, "device: %s\n", device->name);
	app->env->printf(app, "\t%s\n", device->type);
	app->env->printf(app, "\tdefinition %lux%lu %s\n", device->width, device->height, device->fourcc);
	app->env->printf(app, "\tcontrols:\n");
	for (control_t *control = device->controls; control != NULL; control = control->next)
	{
		_app_displaycontrol(app, control);
	}

	return 0;
}

int _app_managecontrol(app_t *app, device_t *device, control_t *control)
{
	int ret = 2;
	while (ret == 2)
	{
		app->env->printf(app, "select: u (up) | n (down) | [esc (back) | v <value> | + | -\n");
		_app_displaycontrol(app, control);

		char ch = app->env->getc(app);
		if (ch == 0x1b)
		{
			app->env->closewindow(app);
			return ret - 1;
		}
		char string[256] = {0};
		int cmd = 0;
		switch (ch)
		{
			case 'u':
				if (control->previous)
					control = control->previous;
			break;
			case 'n':
				if (control->next)
					control = control->next;
			break;
			case 'r':
				_app_displaydevice(app, device);
			break;
			case 'v':
				for (int i = 0; ch != '\n' && i < sizeof(string); i++ , ch = app->env->getc(app))
				{
					string[i] = ch;
				}
				switch (control->type)
				{
				case CTRL_INTEGER:
					control->value.integer = strtol(string + 1, NULL, 10);
					cmd = 1;
				break;
				}
			break;
			case '-':
			case '+':
				switch (control->type)
				{
				case CTRL_INTEGER:
					if (ch == '+')
						control->value.integer ++;
					else
						control->value.integer --;
					cmd = 1;
				break;
				case CTRL_BOOLEAN:
					if (ch == '+')
						control->value.boolean = 1;
					else
						control->value.integer = 0;
					cmd = 1;
				break;
				}
			break;
		}
		if (cmd)
		{
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
			if (client_request(app->client, request, length) < 0)
				ret = 0;
		}
		while (app->env->getc(app) != '\n');
	}
	return ret;
}

int _app_show_device(app_t *app, device_t *device)
{
	int ret = 2;
	unsigned int w = 80, h = 50;
	if (app->devices == NULL)
		return app->level  - 1;
	if (app->level != ret)
	{
		app->level = app->env->openwindow(app, ret);
	}
	_app_displaydevice(app, device);

	control_t *control = device->controls;
	if (control)
	{
		ret = _app_managecontrol(app, device, control);
	}
	else
		ret -= 1;
	return ret;
}

int _app_show_devices(app_t *app)
{
	if (app->devices == NULL)
		return 0;
	if (app->level != 1)
	{
		app->level = app->env->openwindow(app, 1);
	}
	device_t *last = app->devices;
	for (;last != NULL && last->next != NULL; last = last->next);
	for (device_t *device = last; device != NULL; device = device->previous)
	{
		app->env->printf(app, "%i: %s\n", device->id, device->name);
	}
	app->env->printf(app, "select:");
	char ch = app->env->getc(app);
	if (ch == 0x1b) /// excape key
	{
		app->env->closewindow(app);
		return 0;
	}
	if (ch < 0x30 || ch > 0x39)
		return 1;
	char string[256];
	for (int i = 0; ch != '\n' && i < sizeof(string); i++ , ch = app->env->getc(app))
	{
		string[i] = ch;
	}

	int index = strtol(string, NULL, 10);

	json_t *jrequest = json_object();
	json_object_set_new(jrequest, "cmd", json_string("capabilities"));

	for (device_t *device = app->devices; device; device = device->next)
	{
		app->device = device;
		if (device->id == index)
			break;
	}
	if (app->device == NULL)
		return 1;
	json_t *jdata = json_object();
	json_object_set_new(jdata, "name", json_string(app->device->name));
	json_object_set_new(jrequest, "data", jdata);

	char *request = json_dumps(jrequest, 0);
	size_t length = strnlen(request, UNIXSOCKET_PACKETSIZE);
	dbg("send %s", request);
	int ret = 2;
	if (client_request(app->client, request, length) < 0)
		ret = 0;
	free(request);
	json_decref(jrequest);
	return ret;
}

int _app_show(app_t *app, int level)
{
	if (level == 1)
		level = _app_show_devices(app);
	if (level == 2)
		level = _app_show_device(app, app->device);
	return level;
}

int main(int argc, char * const argv[])
{
	app_t app = {0};
	const char *serverpath = "/tmp/fastsetting_socket";
	unsigned int mode = 0;
	const char *logfile = "-";

	int opt;
	do
	{
		opt = getopt(argc, argv, "L:f:");
		switch (opt)
		{
			case 'L':
				logfile = optarg;
			break;
			case 'f':
				serverpath = optarg;
			break;
		}
	} while(opt != -1);

	app.client = client_create(serverpath);
	app.env = &_simple_env;
#ifdef HAVE_NCURSES
	if (g_mode & MODE_NCURSES)
	{
		initscr();
		raw();
		keypad(stdscr, TRUE);
		noecho();
		app.env = &_ncurses_env;
	}
#endif
	client_attach_receive(app.client, _client_receive, &app);
	const char cmd_capabilities[] = "{\"cmd\":\"capabilities\",\"data\":{\"all\":true}}";
	if (client_request(app.client, (void*)cmd_capabilities, sizeof(cmd_capabilities) - 1) < 0)
		return -1;
	app.level = 1;
	while (app.level > 0)
	{
		app.level = _app_show(&app, app.level);
	}
	client_destroy(app.client);
	return 0;
}
