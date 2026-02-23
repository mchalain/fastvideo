#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdarg.h>
#include <fcntl.h>

#ifdef HAVE_NCURSES
#include <ncurses.h>
#else
#define WINDOW void
#endif
#include <jansson.h>

#include "log.h"
#include "fastsetting-cli.h"

typedef struct win_s win_t;
struct win_s
{
	WINDOW *win;
	int level;
	win_t *parent;
};

typedef struct env_s env_t;
typedef struct app_s app_t;

struct env_s
{
	int (*openwindow)(app_t *app, int level);
	int (*printf)(app_t *app, const char *fmt, ...);
	char (*getc)(app_t *app);
	void (*closewindow)(app_t *app);
};

struct app_s
{
	fastsetting_t *setting;
	struct
	{
		unsigned int w;
		unsigned int h;
	} screen;
	env_t *env;
	win_t *window;
	int level;
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
	ret = vw_printw(app->window->win, fmt, ap);
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
 * main app functions
 */
int _app_displaycontrol(app_t *app, control_t *control)
{
	switch (control->type)
	{
	case CTRL_INTEGER:
	case CTRL_BOOLEAN:
		app->env->printf(app, "%.04x % .32s\t\t: %d\n", control->id, control->name, control->value.integer);
	break;
	default:
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
				default:
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
				default:
				break;
				}
			break;
		}
		if (cmd)
		{
			if (fastsetting_change(app->setting, device, control) < 0)
				ret = 0;
		}
		while (app->env->getc(app) != '\n');
	}
	return ret;
}

int _app_show_device(app_t *app, device_t *device)
{
	int ret = 2;
	if (device == NULL)
		return app->level - 1;
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
	device_t *last = fastsetting_device(app->setting, 0);
	if (last == NULL)
		return 0;
	if (app->level != 1)
	{
		app->level = app->env->openwindow(app, 1);
	}
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

	device_t *device = fastsetting_device(app->setting, index);
	int ret = 0;
	if (device && fastsetting_capabilities(app->setting, device) == 0)
	{
		app->device = device;
		ret = 2;
	}
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

	app.setting = fastsetting_create(serverpath);
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
	app.level = 1;
	while (app.level > 0)
	{
		app.level = _app_show(&app, app.level);
	}
	fastsetting_destroy(app.setting);
	return 0;
}
