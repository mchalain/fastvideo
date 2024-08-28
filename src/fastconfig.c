#include <fcntl.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <sys/types.h>

#include <linux/media.h>

#include <jansson.h>

#include "log.h"
#include "sv4l2.h"
#include "smedia.h"

static int all_capabilities_format = 0;

static int _dev_openchar(int major, int minor, char *path, int pathlen)
{
#if 0
	snprintf(path, pathlen,"/dev/char/%d:%d", major, minor);
#else
	snprintf(path, pathlen,"/sys/dev/char/%d:%d", major, minor);
	char target[1024];
	int ret = readlink(path, target, sizeof(target));
	if (ret < 0)
		return -1;
	target[ret] = '\0';
	char *name = strrchr(target, '/');
	if (name == NULL)
		return -1;
	snprintf(path, pathlen, "/dev%s", name);
#endif
	int devfd = open(path, O_RDWR);
	if (devfd < 0)
	{
		err("device %s not found %m", path);
	}
	return devfd;
}

static int sys_opendev(int dirfd, const char *path)
{
	dirfd = openat(dirfd, path, O_DIRECTORY, 0);
	if (dirfd < 0)
	{
		return -1;
	}
	int fd = openat(dirfd, "dev", O_RDONLY, 0);
	if (fd < 0)
	{
		return -1;
	}
	char line[10] = {0};
	int ret = read(fd, line, sizeof(line));
	if (ret > 0)
	{
		char path[32];
		unsigned int major = 0;
		unsigned int minor = 0;
		ret = sscanf(line, "%u:%u", &major, &minor);
		if (ret == 2)
		{
			ret = _dev_openchar(major, minor, path, sizeof(path));
		}
	}
	close(fd);
	return ret;
}

static int sys_device(const char *path, int (*sysdevice)(void *arg, const char *name, int fd), void *cbarg)
{
	int sysfd = open(path, O_DIRECTORY);
	DIR *sys = NULL;
	if (sysfd > 0)
		sys = fdopendir(sysfd);
	if (sys)
	{
		struct dirent *entity;
		do
		{
			entity = readdir(sys);
			if (entity)
			{
				if ((entity->d_type == DT_DIR) || (entity->d_type == DT_LNK))
				{
					int devicefd = sys_opendev(sysfd, entity->d_name);
					if (devicefd > 0 && sysdevice)
					{
						sysdevice(cbarg, entity->d_name, devicefd);
					}
				}
			}
		} while (entity);
		closedir(sys);
	}
	else
		err("enable to open %s: %m", path);
	return 0;
}

static json_t *_device_v4l2(json_t *devices, int major, int minor, const char *name)
{
	char path[32];
	int devfd = _dev_openchar(major, minor, path, sizeof(path));
	size_t index;
	json_t *device = NULL;
	json_array_foreach(devices, index, device)
	{
		json_t *devicepath = json_object_get(device, "device");
		if (devicepath && ! strcmp(path, json_string_value(devicepath)))
		{
			close(devfd);
			return NULL;
		}
	}
	device = NULL;
	V4L2_t *dev = sv4l2_create2(devfd, name, NULL);
	if (dev == NULL)
	{
		close(devfd);
		return device;
	}
	device = json_object();
	json_t *names = json_array();
	json_array_append_new(names, json_string(name));
	json_object_set_new(device, "name", names);
	json_object_set_new(device, "type", json_string("cam"));
	json_object_set_new(device, "device", json_string(path));

	int ret;
	ret = sv4l2_capabilities(dev, device, all_capabilities_format);
	if (ret)
	{
		json_decref(device);
		device = NULL;
	}
	sv4l2_destroy(dev);
	return device;
}

static int _subv4l2_pixformat(void *arg, struct v4l2_subdev_format *ffs)
{
	return 0;
}

static int _subv4l2_fmtbus(void *arg, struct v4l2_subdev_mbus_code_enum *mbuscode)
{
	return 0;
}

static json_t * _device_subv4l2(json_t *devices, int major, int minor, const char *name, uint32_t type)
{
	char path[32];
	int devfd = _dev_openchar(major, minor, path, sizeof(path));
	json_t *device = json_object();
	json_object_set_new(device, "name", json_string(name));
#if 0
	switch (type)
	{
	case MEDIA_ENT_T_V4L2_SUBDEV_SENSOR:
		json_object_set_new(device, "type", json_string("sensor"));
	break;
	case MEDIA_ENT_T_V4L2_SUBDEV_FLASH:
		json_object_set_new(device, "type", json_string("flash"));
	break;
	case MEDIA_ENT_T_V4L2_SUBDEV_LENS:
		json_object_set_new(device, "type", json_string("lens"));
	break;
	}
#else
	json_object_set_new(device, "type", json_string("subv4l"));
#endif
	json_object_set_new(device, "device", json_string(path));
	sv4l2_subdev_getpixformat(devfd, _subv4l2_pixformat, device);
	sv4l2_subdev_getfmtbus(devfd, _subv4l2_fmtbus, device);
	return device;
}

int _device_pads(void *arg, struct media_pad_desc *pad)
{
	json_t *device = (json_t *)arg;
	if (pad == NULL)
		return -1;
	if (pad->flags & MEDIA_PAD_FL_SINK)
	{
		json_object_set_new(device, "sink", json_integer(pad->entity));
	}
	if (pad->flags & MEDIA_PAD_FL_SOURCE)
	{
		json_object_set_new(device, "source", json_integer(pad->entity));
	}
	return 0;
}

int _device_links(void *arg, struct media_link_desc *link)
{
	json_t *device = (json_t *)arg;
	if (link == NULL)
		return -1;
	json_object_set_new(device, "sink", json_integer(link->sink.entity));
	return 0;
}

static int _device_video(void *arg, Media_t *media, struct media_entity_desc *entity)
{
	json_t *devices = (json_t *)arg;
	if (!json_is_array(devices))
		return -1;
	if (entity->dev.major == 0)
		return -1;
	warn("entity %s type %x", entity->name, entity->type);
	json_t *device = NULL;
	if (entity->type == MEDIA_ENT_F_IO_V4L)
	{
		device = _device_v4l2(devices, entity->dev.major, entity->dev.minor, entity->name);
	}
	if ((entity->type & MEDIA_ENT_TYPE_MASK) == MEDIA_ENT_T_V4L2_SUBDEV)
	{
		device = _device_subv4l2(devices, entity->dev.major, entity->dev.minor, entity->name, entity->type);
		smedia_enumlinks(media, entity, _device_links, device);
	}
	if (device != NULL)
	{
		json_object_set_new(device, "id", json_integer(entity->id));
		json_object_set_new(device, "media", json_string(smedia_name(media)));
		json_array_append_new(devices, device);
		return 0;
	}
	return -1;
}

static int _media_device(void *arg, const char *name, int fd)
{
	json_t *devices = (json_t *)arg;
	json_t *mediadevices = json_array();
	Media_t *media = smedia_create2(fd, name);
	smedia_enumentities(media, _device_video, mediadevices);
	smedia_destroy(media);
	/**
	 * This part is uncomplete and needs to be refactored.
	 * The goal is to move the subdevices inside their sink device
	 */
	json_t *subdevices = NULL;
	int sink = -1;
	int index;
	json_t *device;
	json_array_foreach(mediadevices, index, device)
	{
		if (!strcmp("subv4l", json_string_value(json_object_get(device, "type"))))
		{
			int tmpsink = json_integer_value(json_object_get(device, "sink"));
			if (sink > 0 && sink != tmpsink)
				continue;

			if (subdevices == NULL)
			{
				subdevices = json_array();
			}
			json_array_append_new(subdevices, device);
			sink = tmpsink;
			continue;
		}
		int id = json_integer_value(json_object_get(device, "id"));
		if (id == sink)
		{
			json_object_set_new(device,"subdevice", subdevices);
			subdevices = NULL;
		}
		json_array_append_new(devices, device);
	}
	return 0;
}

int main(int argc, char *const argv[])
{
	const char *media = NULL;
	const char *output = "fastconfig.json";
	const char sysmedia[] = "/sys/bus/media/devices";

	int opt;
	do
	{
		opt = getopt(argc, argv, "ao:");
		switch (opt)
		{
			case 'a':
				all_capabilities_format = 1;
			break;
			case 'o':
				output = optarg;
			break;
		}
	} while(opt != -1);

	json_t *devices = json_array();

	sys_device(sysmedia, _media_device, devices);
	json_dump_file(devices, output, JSON_INDENT(2));
	json_decref(devices);
	return 0;
}