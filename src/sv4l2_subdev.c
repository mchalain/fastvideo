#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <string.h>

#include <linux/v4l2-subdev.h>
#include <linux/v4l2-mediabus.h>
#ifdef HAVE_JANSSON
#include <jansson.h>
#endif

#include "fastvideo.h"
#include "log.h"
#include "sv4l2_subdev.h"

typedef struct sv4l2_subdev_stream_s sv4l2_subdev_stream_t;
struct sv4l2_subdev_stream_s
{
	int pad;
	int stream;
};

typedef struct _V4L2_subdev_format_s _V4L2_subdev_format_t;
struct _V4L2_subdev_format_s
{
	uint32_t fourcc;
	uint32_t fourcc_packed;
	int buscode;
};

#ifndef V4L2_PIX_FMT_SGBRG16P
# define V4L2_PIX_FMT_SGBRG16P 0
# define V4L2_PIX_FMT_SBGGR16P 0
# define V4L2_PIX_FMT_SGRBG16P 0
# define V4L2_PIX_FMT_SRGGB16P 0
#endif

static _V4L2_subdev_format_t _buscode2fourcc[] =
{
	{.fourcc=V4L2_PIX_FMT_SGBRG10, .fourcc_packed=V4L2_PIX_FMT_SGBRG10P, .buscode=MEDIA_BUS_FMT_SGBRG10_1X10},
	{.fourcc=V4L2_PIX_FMT_SBGGR10, .fourcc_packed=V4L2_PIX_FMT_SBGGR10P, .buscode=MEDIA_BUS_FMT_SBGGR10_1X10},
	{.fourcc=V4L2_PIX_FMT_SGRBG10, .fourcc_packed=V4L2_PIX_FMT_SGRBG10P, .buscode=MEDIA_BUS_FMT_SGRBG10_1X10},
	{.fourcc=V4L2_PIX_FMT_SRGGB10, .fourcc_packed=V4L2_PIX_FMT_SRGGB10P, .buscode=MEDIA_BUS_FMT_SRGGB10_1X10},
	{.fourcc=V4L2_PIX_FMT_SGBRG12, .fourcc_packed=V4L2_PIX_FMT_SGBRG12P, .buscode=MEDIA_BUS_FMT_SGBRG12_1X12},
	{.fourcc=V4L2_PIX_FMT_SBGGR12, .fourcc_packed=V4L2_PIX_FMT_SBGGR12P, .buscode=MEDIA_BUS_FMT_SBGGR12_1X12},
	{.fourcc=V4L2_PIX_FMT_SGRBG12, .fourcc_packed=V4L2_PIX_FMT_SGRBG12P, .buscode=MEDIA_BUS_FMT_SGRBG12_1X12},
	{.fourcc=V4L2_PIX_FMT_SRGGB12, .fourcc_packed=V4L2_PIX_FMT_SRGGB12P, .buscode=MEDIA_BUS_FMT_SRGGB12_1X12},
	{.fourcc=V4L2_PIX_FMT_SGBRG16, .fourcc_packed=V4L2_PIX_FMT_SGBRG16P, .buscode=MEDIA_BUS_FMT_SGBRG16_1X16},
	{.fourcc=V4L2_PIX_FMT_SBGGR16, .fourcc_packed=V4L2_PIX_FMT_SBGGR16P, .buscode=MEDIA_BUS_FMT_SBGGR16_1X16},
	{.fourcc=V4L2_PIX_FMT_SGRBG16, .fourcc_packed=V4L2_PIX_FMT_SGRBG16P, .buscode=MEDIA_BUS_FMT_SGRBG16_1X16},
	{.fourcc=V4L2_PIX_FMT_SRGGB16, .fourcc_packed=V4L2_PIX_FMT_SRGGB16P, .buscode=MEDIA_BUS_FMT_SRGGB16_1X16},
};

const char sv4l2_subdev_defaultdevice[20] = "/dev/v4l-subdev0";

uint32_t _v4l2_subdev_buscode2fourcc(int buscode, int packed)
{
	uint32_t fourcc = 0;
	for (int i = 0; i < sizeof(_buscode2fourcc)/sizeof(*_buscode2fourcc); i++)
	{
		if (_buscode2fourcc[i].buscode == buscode)
		{
			fourcc = _buscode2fourcc[i].fourcc;
			if (packed && _buscode2fourcc[i].fourcc_packed)
				fourcc = _buscode2fourcc[i].fourcc_packed;
			break;
		}
	}
	return fourcc;
}

int _v4l2_subdev_fourcc2buscode(int fourcc)
{
	int buscode = 0;
	for (int i = 0; i < sizeof(_buscode2fourcc)/sizeof(*_buscode2fourcc); i++)
	{
		if (_buscode2fourcc[i].fourcc == fourcc)
		{
			buscode = _buscode2fourcc[i].buscode;
			break;
		}
		if (_buscode2fourcc[i].fourcc_packed == fourcc)
		{
			buscode = _buscode2fourcc[i].buscode;
			break;
		}
	}
	return buscode;
}

static int _v4l2_subdev_fmtbus(void *arg, struct v4l2_subdev_mbus_code_enum *mbus_code)
{
	uint32_t code = *(uint32_t *)arg;
	if (code == mbus_code->code)
		return 0;
	return -1;
}

static uint32_t _v4l2_subdev_getfmtbus(int ctrlfd, sv4l2_subdev_stream_t *stream, int(*fmtbus)(void *arg, struct v4l2_subdev_mbus_code_enum *mbuscode), void *cbarg)
{
	uint32_t ret = 0;
	dbg("sv4l2: subdev format :");
	for (int i = 0; ; i++)
	{
		struct v4l2_subdev_mbus_code_enum mbusEnum = {0};
		mbusEnum.pad = stream->pad;
		mbusEnum.index = i;
		mbusEnum.which = V4L2_SUBDEV_FORMAT_ACTIVE;

		if (ioctl(ctrlfd, VIDIOC_SUBDEV_ENUM_MBUS_CODE, &mbusEnum) != 0)
		{
			dbg("sv4l2: %d supported formats", i);
			break;
		}
		dbg("\t%#x", mbusEnum.code);
		if (fmtbus)
		{
			if (!fmtbus(cbarg, &mbusEnum))
				ret = mbusEnum.code;
		}
	}
	return ret;
}

uint32_t sv4l2_subdev_getfmtbus(V4L2_t *subdev, sv4l2_subdev_stream_t *stream, int(*fmtbus)(void *arg, struct v4l2_subdev_mbus_code_enum *mbuscode), void *cbarg)
{
	return _v4l2_subdev_getfmtbus(subdev->fd, stream, fmtbus, cbarg);
}

static uint32_t sv4l2_subdev_translate_fmtbus(int ctrlfd, uint32_t fourcc)
{
	uint32_t ret = -1;
	uint32_t code = _v4l2_subdev_fourcc2buscode(fourcc);
	ret = code;
	return ret;
}

int sv4l2_subdev_setpixformat(V4L2_t *subdev, sv4l2_subdev_stream_t *stream, uint32_t fourcc, uint32_t width, uint32_t height)
{
	uint32_t fmtbus = sv4l2_subdev_translate_fmtbus(subdev->fd, fourcc);

	struct v4l2_subdev_format ffs = {0};
	ffs.pad = stream->pad;
	ffs.which = V4L2_SUBDEV_FORMAT_ACTIVE;
	ffs.format.width = width;
	ffs.format.height = height;
	ffs.format.code = 0;
	ffs.format.field = V4L2_FIELD_NONE;
	dbg("sv4l2: subdev format request %lux%lu for %.4s(%#x)", width, height, &fourcc, fmtbus);
	/**
	 * The sensor has a Bayer colour filter which is arranged depending a colours' grid
	 * he only way you can change the colour format would be either:
	 * - cropping an odd number of pixels off the left side
	 * - cropping an odd number of lines off the top of the image
	 * - horizontal flip to start reading from the right hand side
	 * - vertical flip to start reading from the last line
	 * The first two aren't supported by the sensor, but the last two are.
	 * Thx 6by9
	 */
	struct control_s
	{
		int id;
		int value;
	};
	struct control_s controls[] = {
		{0, 0},
		{V4L2_CID_VFLIP, 1},
		{V4L2_CID_HFLIP, 1},
		{V4L2_CID_VFLIP, 0},
	};
	for (int i = 0; i < (sizeof(controls)/sizeof(*controls)) &&
			ffs.format.code != fmtbus; i++)
	{
		ffs.format.code = fmtbus;
		int ret = -1;
		if (controls[i].id)
		{
			struct v4l2_control control = {0};
			control.id = controls[i].id;
			control.value = controls[i].value;
			ret = ioctl(subdev->fd, VIDIOC_S_CTRL, &control);
			if (ret)
				err("sv4l2: subdev control error %m");
		}
		if (ffs.format.code != (uint32_t)-1)
			ret = ioctl(subdev->fd, VIDIOC_SUBDEV_S_FMT, &ffs);
		if (ret != 0)
		{
			err("sv4l2: subdev set format error %m");
			return -1;
		}
	}
	if (fmtbus != ffs.format.code)
		err("v4l2: subdev bus format not set! %#x", ffs.format.code);
	return 0;
}

static int _v4l2_subdev_loadformat(void *arg, struct v4l2_subdev_format *ffs)
{
	V4L2_t *subdev = (V4L2_t *)arg;
	subdev->width = ffs->format.width;
	subdev->height = ffs->format.height;
	subdev->fourcc = _v4l2_subdev_buscode2fourcc(ffs->format.code, 0);
	return 0;
}

uint32_t sv4l2_subdev_getpixformat(V4L2_t *subdev, sv4l2_subdev_stream_t *stream, int (*busformat)(void *arg, struct v4l2_subdev_format *ffs), void *cbarg)
{
	struct v4l2_subdev_format ffs = {0};
	ffs.pad = stream->pad;
	ffs.which = V4L2_SUBDEV_FORMAT_ACTIVE;
	if (ioctl(subdev->fd, VIDIOC_SUBDEV_G_FMT, &ffs) != 0)
	{
		err("sv4l2: subdev get format error %m");
		return -1;
	}
	dbg("sv4l2: current subdev %lu x %lu %#X", ffs.format.width, ffs.format.height, ffs.format.code);
	if (busformat)
		return busformat(cbarg, &ffs);
	return ffs.format.code;
}

int sv4l2_subdev_fps(V4L2_t *subdev, sv4l2_subdev_stream_t *stream, int fps)
{
	int ret = 0;
	struct v4l2_subdev_frame_interval interval = {0};
	interval.pad = stream->pad;
	ret = ioctl(subdev->fd, VIDIOC_SUBDEV_G_FRAME_INTERVAL, &interval);
	if (ret)
	{
		err("sv4l2: subdev getting frame interval error %m");
		fps = -1;
	}
	else if (fps != -1)
	{
		if (fps > 0)
		{
			interval.interval.numerator = 1;
			interval.interval.denominator = fps;
		}
		else
		{
			interval.interval.numerator = fps;
			interval.interval.denominator = 1;
		}
		if (ioctl(subdev->fd, VIDIOC_SUBDEV_S_FRAME_INTERVAL, &interval))
			err("sv4l2: subdev setting frame interval error %m");
		else if (interval.interval.numerator < interval.interval.denominator)
		{
			fps = interval.interval.denominator / interval.interval.numerator;
		}
		else if (interval.interval.numerator > interval.interval.denominator)
		{
			fps = - interval.interval.numerator / interval.interval.denominator;
		}
		warn("sv4l2: subdev Frame rate: %d/%d fps",
			(fps > 0)?fps:1, (fps > 0)?1:-fps);
	}
	return fps;
}

V4L2_t *sv4l2_subdev_create2(int ctrlfd, const char *name, device_type_e dtype, V4l2Config_t *config)
{
	struct v4l2_capability cap = {0};
	if (ioctl(ctrlfd, VIDIOC_QUERYCAP, &cap) != 0)
		err("sv4l2: subdev is not video %m");
	else
		warn("sv4l2: subdev %.32s", cap.card);
#ifdef VIDIOC_SUBDEV_QUERYCAP
	struct v4l2_subdev_capability caps = {0};
	if (ioctl(ctrlfd, VIDIOC_SUBDEV_QUERYCAP, &caps) != 0)
	{
		warn("sv4l2: subdev control error %m");
	}
#ifdef V4L2_SUBDEV_CAP_STREAMS
	if (caps.capabilities & V4L2_SUBDEV_CAP_STREAMS)
	{
		struct v4l2_subdev_client_capability clientCaps;
		clientCaps.capabilities = V4L2_SUBDEV_CLIENT_CAP_STREAMS;

		if (ioctl(ctrlfd, VIDIOC_SUBDEV_S_CLIENT_CAP, &clientCaps) != 0)
		{
			err("sv4l2: subdev control error %m");
			return NULL;
		}
		warn("sv4l2: client streams capabilities");
	}
#endif
	if (caps.capabilities & V4L2_SUBDEV_CAP_RO_SUBDEV)
	{
		warn("sv4l2: subdev read-only");
		return NULL;
	}
#endif
	V4L2_t *subdev = calloc(1, sizeof(*subdev));
	subdev->fd = ctrlfd;
	subdev->config = config;
	subdev->type = dtype;
	subdev->name = subdev->devicename;
	strncpy(subdev->devicename, name, sizeof(subdev->devicename) - 1);
	sv4l2_subdev_stream_t stream = {0};
	sv4l2_subdev_getpixformat(subdev, &stream, _v4l2_subdev_loadformat, subdev);
	warn("sv4l2: subdev %s created", subdev->name);
	return subdev;
}

V4L2_t *sv4l2_subdev_create(const char *devicename, device_type_e type, V4l2Config_t *config)
{
	int pad = 0;
	int ctrlfd = -1;
	if (config->device)
		ctrlfd = open(config->device, O_RDWR, 0);
	if (ctrlfd < 0)
	{
		ctrlfd = open(devicename, O_RDWR, 0);
	}
	if (ctrlfd < 0)
	{
		err("sv4l2: subdevice %s not exist", config->device);
		return NULL;
	}
	V4L2_t *subdev = sv4l2_subdev_create2(ctrlfd, devicename, type, config);
	if (subdev == NULL)
	{
		close(ctrlfd);
		return NULL;
	}
	if (config->parent.width) subdev->width = config->parent.width;
	if (config->parent.height) subdev->height = config->parent.height;
	if (config->parent.fourcc) subdev->fourcc = config->parent.fourcc;

	sv4l2_subdev_stream_t stream = {0};
	sv4l2_subdev_setpixformat(subdev, &stream, subdev->fourcc, subdev->width, subdev->height);
	if (sv4l2_subdev_fps(subdev, &stream, config->fps) == -1)
		sv4l2_fps(subdev, config->fps);
	sv4l2_subdev_fps(subdev, &stream, -1);
	return subdev;
}

void sv4l2_subdev_destroy(V4L2_t *subdev)
{
	dbg("sv4l2: subdev %s destroying", subdev->name);
	close(subdev->fd);
	free(subdev);
}

DeviceConf_t * sv4l2_subdev_createconfig()
{
	V4l2Config_t *devconfig = NULL;
	devconfig = calloc(1, sizeof(V4l2Config_t));
	devconfig->device = sv4l2_subdev_defaultdevice;
#ifdef HAVE_JANSSON
	devconfig->parent.ops.loadconfiguration = sv4l2_subdev_loadjsonconfiguration;
#endif
	return (DeviceConf_t *)devconfig;
}

#ifdef HAVE_JANSSON
int sv4l2_subdev_loadjsonconfiguration(void *arg, void *entry)
{
	int ret = -1;
	json_t *subdevice = entry;
	V4l2Config_t *config = (V4l2Config_t *)arg;

	if (subdevice && json_is_object(subdevice))
	{
		sv4l2_loadjsonconfiguration(config, subdevice);
		json_t *definition = json_object_get(subdevice, "definition");
		json_t *fmtbus = NULL;
		if (definition && json_is_array(definition))
		{
			int index;
			json_t *item;
			json_array_foreach(definition, index, item)
			{
				if (json_is_object(item))
				{
					json_t *name = json_object_get(item, "name");
					if (name && !strcmp(json_string_value(name), "fmtbus"))
					{
						fmtbus = json_object_get(item, "value");
						break;
					}
				}
			}
		}
		if (definition && json_is_object(definition))
		{
				fmtbus = json_object_get(definition, "fmtbus");
		}
		if (fmtbus && json_is_string(fmtbus))
		{
			config->fmtbus = strtol(json_string_value(fmtbus), NULL, 16);
		}
		if (fmtbus && json_is_integer(fmtbus))
		{
			config->fmtbus = json_integer_value(fmtbus);
		}
	}
	if (subdevice && json_is_string(subdevice))
	{
		const char *value = json_string_value(subdevice);
		config->device = value;
		ret = 0;
	}
	return ret;
}

typedef struct _JSONControl_Arg_s _JSONControl_Arg_t;
struct _JSONControl_Arg_s
{
	json_t *controls;
	int ctrlfd;
	int all;
};

static int _sv4l2_subdev_capabilities_fmtbus_items(void *arg, struct v4l2_subdev_mbus_code_enum *mbuscode)
{
	_JSONControl_Arg_t *jsoncontrol_arg = arg;
	json_t *items = jsoncontrol_arg->controls;
	json_array_append_new(items, json_sprintf("%#x", mbuscode->code));
}

static int _v4l2_subdev_capabilities_fmtbus(V4L2_t *subdev, json_t *definition, int all)
{
	sv4l2_subdev_stream_t stream = {0};
	json_t *fmtbus = json_object();
	json_object_set_new(fmtbus, "name", json_string("fmtbus"));

	int ret = 0;
	struct v4l2_subdev_format mbus = {0};
	mbus.pad = stream.pad;
	mbus.which = V4L2_SUBDEV_FORMAT_ACTIVE;
	ret = ioctl(subdev->fd, VIDIOC_SUBDEV_G_FMT, &mbus);
	if (!ret)
	{
		json_object_set_new(fmtbus, "value", json_sprintf("%#x", mbus.format.code));
	}
	if (all)
	{
		json_object_set_new(fmtbus, "type", json_string(sv4l2_CTRLTYPE(V4L2_CTRL_TYPE_INTEGER)));

		json_t *items = json_array();
		_JSONControl_Arg_t arg = {0};
		arg.controls = items;
		arg.all = all;
		arg.ctrlfd = subdev->fd;
		sv4l2_subdev_getfmtbus(subdev, &stream, _sv4l2_subdev_capabilities_fmtbus_items, &arg);
		if (json_array_size > 0)
			json_object_set_new(fmtbus, "items", items);
	}
	json_array_append_new(definition, fmtbus);
	return 0;
}

int sv4l2_subdev_capabilities(V4L2_t *subdev, json_t *capabilities, int all)
{
#ifdef VIDIOC_SUBDEV_QUERYCAP
	struct v4l2_subdev_capability caps;
	if (ioctl(subdev->fd, VIDIOC_SUBDEV_QUERYCAP, &caps) != 0)
	{
		err("smedia: subdev control error %m");
		return -1;
	}
#ifdef V4L2_SUBDEV_CAP_STREAMS
	if (caps.capabilities & V4L2_SUBDEV_CAP_STREAMS)
	{
		dbg("sv4l2: subdevice streaming");
		json_object_set_new(capabilities, "stream", json_true());
	}
#endif
	if (caps.capabilities & V4L2_SUBDEV_CAP_RO_SUBDEV)
	{
		warn("smedia: subdev read-only");
		return -1;
	}
#endif
	int ret = 0;
	json_t *definition = json_array();
	ret = sv4l2_capabilities_definition(subdev, definition, all);
	ret = _v4l2_subdev_capabilities_fmtbus(subdev, definition, all);
	if (!ret)
	{
		json_object_set_new(capabilities, "definition", definition);
	}

	_JSONControl_Arg_t arg = {0};
	arg.controls = json_array();
	arg.all = all;
	arg.ctrlfd = sv4l2_fd(subdev, 0);
	ret = sv4l2_treecontrols(subdev, sv4l2_jsoncontrol_cb, &arg);
	if (ret > 0)
		json_object_set_new(capabilities, "controls", arg.controls);

	return 0;
}

#endif

FastVideoDevice_ops_t subdev_ops = {
	.name = "subv4l",
	.createconfig = sv4l2_subdev_createconfig,
	.create = (FastVideoDevice_create_t)sv4l2_subdev_create,
	.create2 = (FastVideoDevice_create2_t)sv4l2_subdev_create2,
	.duplicate = NULL,
	.loadsettings = (FastVideoDevice_loadsettings_t)sv4l2_loadsettings,
	.capabilities = (FastVideoDevice_capabilities_t)sv4l2_subdev_capabilities,
	.requestbuffer = NULL,
	.eventfd = (FastVideoDevice_eventfd_t)sv4l2_fd,
	.start = NULL,
	.stop = NULL,
	.dequeue = NULL,
	.queue = NULL,
	.destroy = (FastVideoDevice_destroy_t)sv4l2_subdev_destroy,
};
