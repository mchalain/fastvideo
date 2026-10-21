#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "log.h"
#include "rectshm.h"

/*
 * Server example to use the "data/rectangle.glsl".
 */

#define DEFAULT_WORKDIR "/tmp/fastvideo.segl/"
#define DEFAULT_CONFIG "/etc/fastvideo/raspicam.json"
#define DEFAULT_DEVICE "gpu"
#define DEFAULT_PROGRAM "rectangles"

static void help(const char *name)
{
	fprintf(stderr, "Usage: %s [-j <json>] [-d <device>] [-p <program>] [-W <workdir>] -a <x,y,w,h> | -r | -c | -l ...\n", name);
	fprintf(stderr, "\n");
	fprintf(stderr, "  -h            display this message\n");
	fprintf(stderr, "  -j <json>     fastvideo configuration file (default %s)\n", DEFAULT_CONFIG);
	fprintf(stderr, "  -d <device>   name of the gpu device into the configuration (default %s)\n", DEFAULT_DEVICE);
	fprintf(stderr, "  -p <program>  name of the GLSL program of this device (default %s)\n", DEFAULT_PROGRAM);
	fprintf(stderr, "  -W <path>     segl working directory (default %s)\n", DEFAULT_WORKDIR);
	fprintf(stderr, "  -a <x,y,w,h>  set the first empty rectangle (normalised image coordinates)\n");
	fprintf(stderr, "  -r            clear the last rectangle in use\n");
	fprintf(stderr, "  -c            clear all the rectangles\n");
	fprintf(stderr, "  -l            list the rectangles\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "The actions are run in the command line order.\n");
}

int main(int argc, char *const argv[])
{
	const char *workdir = DEFAULT_WORKDIR;
	const char *configfile = DEFAULT_CONFIG;
	const char *device = DEFAULT_DEVICE;
	const char *program = DEFAULT_PROGRAM;
	int nactions = 0;

	static const char options[] = "hj:d:p:W:a:rcl";
	int opt;
	while ((opt = getopt(argc, argv, options)) != -1)
	{
		switch (opt)
		{
			case 'j':
				configfile = optarg;
			break;
			case 'd':
				device = optarg;
			break;
			case 'p':
				program = optarg;
			break;
			case 'W':
				workdir = optarg;
			break;
			case 'a':
			case 'r':
			case 'c':
			case 'l':
				nactions++;
			break;
			case 'h':
			default:
				help(argv[0]);
				return 1;
		}
	}
	if (nactions == 0)
	{
		help(argv[0]);
		return 1;
	}

	RectShm_t *shm = rectshm_open(configfile, device, program, workdir);
	if (shm == NULL)
		return 1;
	int nrects = rectshm_count(shm);

	int ret = 0;
	optind = 1;
	while ((opt = getopt(argc, argv, options)) != -1)
	{
		switch (opt)
		{
			case 'a':
			{
				float x, y, w, h;
				if (sscanf(optarg, "%f,%f,%f,%f", &x, &y, &w, &h) != 4 || w <= 0.0 || h <= 0.0)
				{
					err("rectdraw: %s: expected x,y,w,h with w > 0 and h > 0", optarg);
					ret = 1;
					break;
				}
				int i;
				for (i = 0; i < nrects; i++)
				{
					if (rectshm_isempty(shm, i))
						break;
				}
				if (i == nrects)
				{
					err("rectdraw: no empty rectangle left (%d max)", nrects);
					ret = 1;
					break;
				}
				rectshm_set(shm, i, x, y, x + w, y + h);
				printf("vRects[%d] = %g,%g,%g,%g\n", rectshm_index(shm, i), x, y, w, h);
			}
			break;
			case 'r':
			{
				int i;
				for (i = nrects - 1; i >= 0; i--)
				{
					if (!rectshm_isempty(shm, i))
						break;
				}
				if (i < 0)
				{
					warn("rectdraw: no rectangle to clear");
					break;
				}
				rectshm_clear(shm, i);
				printf("vRects[%d] cleared\n", rectshm_index(shm, i));
			}
			break;
			case 'c':
				for (int i = nrects - 1; i >= 0; i--)
					rectshm_clear(shm, i);
			break;
			case 'l':
				for (int i = 0; i < nrects; i++)
				{
					if (rectshm_isempty(shm, i))
					{
						printf("vRects[%d] empty\n", rectshm_index(shm, i));
						continue;
					}
					float rect[4];
					rectshm_get(shm, i, rect);
					printf("vRects[%d] = %g,%g,%g,%g\n", rectshm_index(shm, i),
						rect[0], rect[1], rect[2] - rect[0], rect[3] - rect[1]);
				}
			break;
		}
	}
	rectshm_close(shm);
	return ret;
}
