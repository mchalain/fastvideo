#include <fcntl.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>

#include <linux/media.h>

#include <jansson.h>

#include "log.h"
#include "sv4l2.h"
#include "sv4l2_subdev.h"
#include "sv4l2_meta.h"
#ifdef HAVE_LIBDRM
#include "sdrm.h"
#endif
#include "spassthrough.h"
#include "smedia.h"

static int all_capabilities_format = 0;

static int _devices_append(json_t *devices, json_t *device);

static int _dev_formatcdev_devfs(int major, int minor, char *path, int pathlen)
{
	return (snprintf(path, pathlen,"/dev/char/%d:%d", major, minor) > 0);
}

static int _dev_formatcdev_sysfs(int major, int minor, char *path, int pathlen, const char *dirpath)
{
	snprintf(path, pathlen,"/sys/dev/char/%d:%d", major, minor);
	char target[1024];
	int ret = readlink(path, target, sizeof(target));
	if (ret < 0)
		return -1;
	target[ret] = '\0';
	char *name = strrchr(target, '/');
	if (name == NULL)
		return -1;
	return (snprintf(path, pathlen, "%s%s", dirpath, name) > 0);
}

static int _dev_openchar(int major, int minor, char *path, int pathlen, const char *dirpath)
{
	if (access("/dev/char/", 0) == -1 ||
		_dev_formatcdev_devfs(major, minor, path, pathlen))
		_dev_formatcdev_sysfs(major, minor, path, pathlen, dirpath);
	dbg("try %s", path);
	int devfd = open(path, O_RDWR);
	if (devfd < 0)
	{
		err("device %s not found %m", path);
	}
	return devfd;
}

static int sys_opendev(int dirfd, const char *path, char *devpath, size_t devpathlen, const char *dirpath)
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
		unsigned int major = 0;
		unsigned int minor = 0;
		ret = sscanf(line, "%u:%u", &major, &minor);
		if (ret == 2)
		{
			ret = _dev_openchar(major, minor, devpath, devpathlen, dirpath);
		}
	}
	close(fd);
	return ret;
}

static int sys_device(const char *path, int (*sysdevice)(void *arg, int fd, const char *path, const char *name), void *cbarg, const char *dirpath)
{
	int ret = -1;
	dbg("parse %s tree", path);
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
					char path[256];
					int devicefd = sys_opendev(sysfd, entity->d_name, path, sizeof(path), dirpath);
					if (devicefd > 0 && sysdevice)
					{
						ret = sysdevice(cbarg, devicefd, path, entity->d_name);
					}
				}
			}
		} while (entity);
		closedir(sys);
	}
	else
		err("enable to open %s: %m", path);
	return ret;
}

static json_t *_device_v4l2(json_t *devices, int devfd, const char *path, const char *name)
{
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
	if (index == json_array_size(devices))
		device = NULL;
	/**
	 * TODO
	 * check the devices to find the subdevices corresponding to this entity's id
	 * The id comes from the media like the name.
	 * The subdeivces are injected into the device
	 */
	V4L2_t *dev = NULL;
	device_type_e types[] = {device_input, device_transfer, device_output, device_control};
	for (int i = 0; i < sizeof(types)/sizeof(device_type_e) && dev == NULL; i++)
	{
		dev = sv4l2_ops.create2(devfd, name, types[i], NULL);
	}
	if (dev == NULL)
	{
		close(devfd);
		return device;
	}
	if (device == NULL)
		device = json_object();
	json_t *names = json_array();
	json_array_append_new(names, json_string(name));
	json_object_set_new(device, "name", names);
	json_object_set_new(device, "type", json_string(sv4l2_ops.name));
	json_object_set_new(device, "device", json_string(path));

	int ret;
	ret = sv4l2_ops.capabilities(dev, device, all_capabilities_format);
	if (ret)
	{
		json_decref(device);
		device = NULL;
	}
	sv4l2_ops.destroy(dev);
	return device;
}

static json_t * _device_subv4l2(json_t *devices, int devfd, const char *path, const char *name, uint32_t type)
{
	json_t *device = NULL;
#ifdef V4L2_SUBDEV
	device = json_object();
	json_t *jname = json_array();
	json_array_insert_new(jname, 0, json_string(name));
	json_object_set_new(device, "name", jname);
#if 0
	switch (type)
	{
	case MEDIA_ENT_T_V4L2_SUBDEV_SENSOR:
		json_object_set_new(device, "type", json_string("subv4l_sensor"));
	break;
	case MEDIA_ENT_T_V4L2_SUBDEV_FLASH:
		json_object_set_new(device, "type", json_string("subv4l_flash"));
	break;
	case MEDIA_ENT_T_V4L2_SUBDEV_LENS:
		json_object_set_new(device, "type", json_string("subv4l_lens"));
	break;
	}
#else
	json_object_set_new(device, "type", json_string(subdev_ops.name));
#endif
	json_object_set_new(device, "device", json_string(path));
	V4L2_t *subdev = subdev_ops.create2(devfd, name, device_control, NULL);
	if (subdev)
	{
		subdev_ops.capabilities(subdev, device, all_capabilities_format);
		subdev_ops.destroy(subdev);
	}
	else
#endif
		close(devfd);
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
	json_t *sink = json_object_get(device, "sink");
	if (sink == NULL)
	{
		sink = json_array();
		json_object_set(device,"sink", sink);
	}
	json_array_append_new(sink, json_integer(link->sink.entity));
	return 0;
}

static int _video_device(void *arg, int fd, const char *path, const char *name)
{
	json_t *devices = (json_t *)arg;
	json_t *device = NULL;
	device = _device_v4l2(devices, fd, path, name);
	if (device != NULL)
	{
		json_array_append_new(devices, device);
		return 0;
	}
	return -1;
}

static int _media_video(void *arg, Media_t *media, struct media_entity_desc *entity)
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
		char path[32];
		int devfd = _dev_openchar(entity->dev.major, entity->dev.minor, path, sizeof(path), "/dev");
		device = _device_v4l2(devices, devfd, path, entity->name);
	}
	if ((entity->type & MEDIA_ENT_TYPE_MASK) == MEDIA_ENT_T_V4L2_SUBDEV)
	{
		char path[32];
		int devfd = _dev_openchar(entity->dev.major, entity->dev.minor, path, sizeof(path), "/dev" );
		device = _device_subv4l2(devices, devfd, path, entity->name, entity->type);
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

static int _devices_append(json_t *devices, json_t *device)
{
	/**
	 * check if the device is already inside the array
	 */
	json_t *jname = json_object_get(device, "name");
	if (jname && json_is_array(jname))
	{
		int last = json_array_size(jname) - 1;
		jname = json_array_get(jname, last);
	}
	const char *name = json_string_value(jname);
	int j;
	json_t *olddevice = NULL;
	json_array_foreach(devices, j, olddevice)
	{
		json_t *oldname = json_object_get(olddevice, "name");
		if (oldname && json_is_array(oldname))
		{
			int last = json_array_size(oldname) - 1;
			oldname = json_array_get(oldname, last);
		}
		if (oldname && json_is_string(oldname))
		{
			if (name && !strcmp(json_string_value(oldname), name))
				break;
		}
	}
	/** free if the device existing or append **/
	if (olddevice && j < json_array_size(olddevice))
		json_decref(device);
	else
	{
		json_array_append_new(devices, device);
	}
	return 0;
}

#ifdef HAVE_LIBDRM
static int _drm_device(void *arg, int fd, const char *path, const char *name)
{
	static int numdisplay = 0;
	if (numdisplay > 9)
		return -1;
	json_t *devices = (json_t *)arg;
	Display_t *disp = sdrm_ops.create2(fd, name, device_output, NULL);
	if (disp)
	{
		json_t *device = json_object();
		char staticname[] = "screenX";
		if (name == NULL)
		{
			name = staticname;
			staticname[6] = (char)(0x30 + numdisplay);
		}
		json_object_set_new(device, "name", json_string(name));
		json_object_set_new(device, "device", json_string(path));
		json_object_set_new(device, "type", json_string(sdrm_ops.name));
		int ret = sdrm_ops.capabilities(disp, device, all_capabilities_format);
		if (ret == 0)
		{
			_devices_append(devices, device);
		}
		sdrm_ops.destroy(disp);
		return ret;
	}
	else
		err("drm card %s not supported", path);
	return -1;
}
#endif

static int _media_device(void *arg, int fd, const char *path, const char *name)
{
	json_t *devices = (json_t *)arg;
	json_t *mediadevices = json_array();
	Media_t *media = smedia_create2(fd, name);
	smedia_enumentities(media, _media_video, mediadevices);
	smedia_destroy(media);
	if (json_array_size(mediadevices) == 0)
		return -1;
	/**
	 * This part is uncomplete and needs to be refactored.
	 * The goal is to move the subdevices inside their sink device
	 */
	json_t *allsubdevices = NULL;
	allsubdevices = json_array();
	int sink = -1;
	int i;
	json_t *device;
	json_t *definition = NULL;
	json_array_foreach(mediadevices, i, device)
	{
		json_t * jname = json_object_get(device, "name");
		if (json_is_array(jname))
			jname = json_array_get(jname, 0);
		dbg("device found %s", json_string_value(jname));
		const char *type = json_string_value(json_object_get(device, "type"));
		if (type && !strcmp("subv4l", type))
		{
			json_array_append_new(allsubdevices, device);
		}
	}
	json_array_foreach(mediadevices, i, device)
	{
		json_t *jdevicename = json_object_get(device, "name");
		if (json_is_array(jdevicename))
			jdevicename = json_array_get(jdevicename, 0);
		json_t *subdevices = NULL;
		int id = json_integer_value(json_object_get(device, "id"));
		json_t *subdevice;
		int j;
		json_array_foreach(allsubdevices, j, subdevice)
		{
			json_t *jsinks = json_object_get(subdevice, "sink");
			json_t *jsink;
			int k;
			json_array_foreach(jsinks, k, jsink)
			{
				int sink = json_integer_value(jsink);
				if (sink == id)
				{
					if (subdevices == NULL)
						subdevices = json_array();
					json_array_append_new(subdevices, subdevice);
				}
			}
		}
		if (subdevices != NULL)
		{
			json_t *subdevice;
			int index;
			json_array_foreach(subdevices, index, subdevice)
			{
				json_t *jname = json_object_get(subdevice, "name");
				if (json_is_array(jname))
					json_array_insert_new(jname, 0, jdevicename);
			}
			json_object_set_new(device,"subdevices", subdevices);
		}
		const char *type = json_string_value(json_object_get(device, "type"));

		if (type && !strcmp("v4l2", type))
		{
			_devices_append(devices, device);
		}
		if (type && !strcmp("subv4l", type))
		{
			_devices_append(devices, device);
		}
	}
	return 0;
}

int _passthrough_device(void *arg, int fd, const char *path, const char *name)
{
	json_t *devices = (json_t *)arg;
	json_t *passthrough = json_object();
	void *dev = spassthrough_ops.create(path, device_output, NULL);
	spassthrough_ops.capabilities(dev, passthrough, all_capabilities_format);
	spassthrough_ops.destroy(dev);
	_devices_append(devices, passthrough);
	return 0;
}

int _v4l2_meta_device(void *arg, int fd, const char *path, const char *name)
{
	json_t *devices = (json_t *)arg;
	json_t *metadevice = json_object();
	void *dev = sv4l2_meta_ops.create(path, device_output, NULL);
	sv4l2_meta_ops.capabilities(dev, metadevice, all_capabilities_format);
	sv4l2_meta_ops.destroy(dev);
	_devices_append(devices, metadevice);
	return 0;
}

int main(int argc, char *const argv[])
{
	const char *media = NULL;
	const char *video = NULL;
	const char *drm = NULL;
	const char *output = "fastconfig.json";
	const char sysmedia[] = "/sys/bus/media/devices";
	const char sysvideo[] = "/sys/class/video4linux";
	const char sysdrm[] = "/sys/class/drm";

	int opt;
	do
	{
		opt = getopt(argc, argv, "ao:m:d:v:");
		switch (opt)
		{
			case 'a':
				all_capabilities_format = 1;
			break;
			case 'o':
				output = optarg;
			break;
			case 'm':
				media = optarg;
			break;
			case 'v':
				video = optarg;
			break;
			case 'd':
				drm = optarg;
			break;
		}
	} while(opt != -1);

	json_t *devices = NULL;

	struct stat statd = {0};
	if (stat(output, &statd))
	{
		json_error_t jerror;
		devices = json_load_file(output, 0, &jerror);
	}
	if (devices == NULL)
		devices = json_array();

	if (video)
	{
		int fd = open(video, O_RDWR);
		if (fd < 0)
		{
			err("video %s not found %m", video);
		}
		else
			_video_device(devices, fd, video, video);
	}
	else if (media == NULL)
	{
		if (sys_device(sysmedia, _media_device, devices, "/dev"))
			sys_device(sysvideo, _video_device, devices, "/dev");
	}
	else
	{
		int fd = open(media, O_RDWR);
		if (fd < 0)
		{
			err("media %s not found %m", media);
		}
		else
			_media_device(devices, fd, media, media);
	}
#ifdef HAVE_LIBDRM
	if (drm == NULL)
		sys_device(sysdrm, _drm_device, devices, "/dev/dri");
	else
	{
		int fd = open(drm, O_RDWR);
		if (fd < 0)
		{
			err("drm %s not found %m", drm);
		}
		else
			_drm_device(devices, fd, drm, drm);
	}
#endif
	_passthrough_device(devices, 0, "passthrough", "passthrough");
	_v4l2_meta_device(devices, 0, "metadevice", "metadevice");
	json_dump_file(devices, output, JSON_INDENT(2));
	json_decref(devices);
	return 0;
}
