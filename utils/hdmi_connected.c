#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

#include <xf86drm.h>
#include <xf86drmMode.h>

/*
 * /sys/class/drm/card0-HDMI-A-1/status always reports "connected" on this
 * board, even with nothing plugged in, because hdmi_force_hotplug=1 is
 * required in config.txt for HDMI to work at all (hardware issue) - that
 * setting also forces the hotplug/connected state regardless of reality.
 * The reliable signal instead is whether the connector's mode list
 * contains a real display's mode: with nothing actually connected, the
 * kernel falls back to a fixed default mode list instead of a real
 * EDID-derived one, so looking for a specific expected mode (e.g. the one
 * this project actually wants to drive) tells the two cases apart.
 */

#define DEFAULT_DEVICE "/dev/dri/card0"
#define DEFAULT_WIDTH 1280
#define DEFAULT_HEIGHT 720

static void help(const char *name)
{
	fprintf(stderr, "Usage: %s [OPTION]...\n", name);
	fprintf(stderr, "\n");
	fprintf(stderr, "  -h            display this message\n");
	fprintf(stderr, "  -d <device>   DRM device to open (default %s)\n", DEFAULT_DEVICE);
	fprintf(stderr, "  -m <WxH>      mode to look for on the HDMI connector (default %dx%d)\n",
		DEFAULT_WIDTH, DEFAULT_HEIGHT);
}

int main(int argc, char *const argv[])
{
	const char *device = DEFAULT_DEVICE;
	int width = DEFAULT_WIDTH;
	int height = DEFAULT_HEIGHT;

	int opt;
	while ((opt = getopt(argc, argv, "hd:m:")) != -1)
	{
		switch (opt)
		{
			case 'd':
				device = optarg;
			break;
			case 'm':
			{
				int w = 0, h = 0;
				if (sscanf(optarg, "%dx%d", &w, &h) != 2 || w <= 0 || h <= 0)
				{
					fprintf(stderr, "hdmi_connected: invalid mode '%s', expected WxH\n", optarg);
					return 1;
				}
				width = w;
				height = h;
			}
			break;
			case 'h':
			default:
				help(argv[0]);
				return 1;
		}
	}

	int fd = open(device, O_RDWR | O_CLOEXEC);
	if (fd < 0)
	{
		fprintf(stderr, "hdmi_connected: opening %s error: %m\n", device);
		return 1;
	}

	drmModeRes *resources = drmModeGetResources(fd);
	if (!resources)
	{
		fprintf(stderr, "hdmi_connected: %s resources error: %m\n", device);
		close(fd);
		return 1;
	}

	int found = 0;
	for (int i = 0; i < resources->count_connectors && !found; i++)
	{
		drmModeConnector *connector = drmModeGetConnector(fd, resources->connectors[i]);
		if (!connector)
			continue;
		if (connector->connector_type == DRM_MODE_CONNECTOR_HDMIA ||
			connector->connector_type == DRM_MODE_CONNECTOR_HDMIB)
		{
			for (int m = 0; m < connector->count_modes; m++)
			{
				if (connector->modes[m].hdisplay == width &&
					connector->modes[m].vdisplay == height)
				{
					found = 1;
					break;
				}
			}
		}
		drmModeFreeConnector(connector);
	}

	drmModeFreeResources(resources);
	close(fd);

	return found ? 0 : 1;
}
