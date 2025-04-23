#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <linux/v4l2-subdev.h>
#ifdef HAVE_JANSSON
#include <jansson.h>
#endif

#include "log.h"
#include "sv4l2_subdev.h"

const char sv4l2_subdev_defaultdevice[20] = "/dev/v4l_subdev0";

static int _v4l2_subdev_fmtbus(void *arg, struct v4l2_subdev_mbus_code_enum *mbus_code)
{
	uint32_t code = *(uint32_t *)arg;
	if (code == mbus_code->code)
		return 0;
	return -1;
}

uint32_t _v4l2_subdev_getfmtbus(int ctrlfd, int(*fmtbus)(void *arg, struct v4l2_subdev_mbus_code_enum *mbuscode), void *cbarg)
{
	uint32_t ret = 0;
	for (int i = 0; ; i++)
	{
		struct v4l2_subdev_mbus_code_enum mbusEnum = {0};
		mbusEnum.pad = 0;
		mbusEnum.index = i;
		mbusEnum.which = V4L2_SUBDEV_FORMAT_ACTIVE;

		if (ioctl(ctrlfd, VIDIOC_SUBDEV_ENUM_MBUS_CODE, &mbusEnum) != 0)
		{
			dbg("sv4l2: %d supported formats", i);
			break;
		}
		dbg("sv4l2: format supported %#x", mbusEnum.code);
		if (fmtbus)
		{
			if (!fmtbus(cbarg, &mbusEnum))
				ret = mbusEnum.code;
		}
	}
	return ret;
}

uint32_t sv4l2_subdev_getfmtbus(V4L2_t *subdev, int(*fmtbus)(void *arg, struct v4l2_subdev_mbus_code_enum *mbuscode), void *cbarg)
{
	return _v4l2_subdev_getfmtbus(subdev->fd, fmtbus, cbarg);
}

static uint32_t sv4l2_subdev_translate_fmtbus(int ctrlfd, uint32_t fourcc)
{
	uint32_t ret = -1;
	uint32_t code = -1;
	switch (fourcc)
	{
	case V4L2_PIX_FMT_SGBRG10:
	case V4L2_PIX_FMT_SGBRG10P:
		code = V4L2_MBUS_FMT_SGBRG10_1X10;
	break;
	case V4L2_PIX_FMT_SBGGR10:
	case V4L2_PIX_FMT_SBGGR10P:
		code = V4L2_MBUS_FMT_SBGGR10_1X10;
	break;
	case V4L2_PIX_FMT_SGRBG10:
#ifdef V4L2_PIX_FMT_SGRBG10P
	case V4L2_PIX_FMT_SGRBG10P:
#endif
		code = MEDIA_BUS_FMT_SGRBG10_1X10;
	break;
	case V4L2_PIX_FMT_SRGGB12:
#ifdef V4L2_PIX_FMT_SRGGB12P
	case V4L2_PIX_FMT_SRGGB12P:
#endif
		code = V4L2_MBUS_FMT_SRGGB12_1X12;
	break;
	case V4L2_PIX_FMT_SRGGB10:
	case V4L2_PIX_FMT_SRGGB10P:
		code = MEDIA_BUS_FMT_SRGGB10_1X10;
	break;
	case V4L2_PIX_FMT_SBGGR16:
		code = MEDIA_BUS_FMT_SBGGR16_1X16;
	break;
	};
	ret = _v4l2_subdev_getfmtbus(ctrlfd, _v4l2_subdev_fmtbus, &code);
	return ret;
}

int sv4l2_subdev_setpixformat(V4L2_t *subdev, uint32_t fourcc, uint32_t width, uint32_t height)
{
	struct v4l2_subdev_format ffs = {0};
	ffs.pad = 0;
	ffs.which = V4L2_SUBDEV_FORMAT_ACTIVE;
	ffs.format.width = width;
	ffs.format.height = height;
	ffs.format.code = sv4l2_subdev_translate_fmtbus(subdev->fd, fourcc);
	dbg("sv4l2: subdev format request %ux%u %#x for %.4s", width, height, ffs.format.code, &fourcc);
	if (ffs.format.code != (uint32_t)-1 && ioctl(subdev->fd, VIDIOC_SUBDEV_S_FMT, &ffs) != 0)
	{
		err("sv4l2: subdev set format error %m");
		return -1;
	}
	return 0;
}

uint32_t sv4l2_subdev_getpixformat(V4L2_t *subdev, int (*busformat)(void *arg, struct v4l2_subdev_format *ffs), void *cbarg)
{
	struct v4l2_subdev_format ffs = {0};
	ffs.pad = 0;
	ffs.which = V4L2_SUBDEV_FORMAT_ACTIVE;
	if (ioctl(subdev->fd, VIDIOC_SUBDEV_G_FMT, &ffs) != 0)
	{
		err("sv4l2: subdev get format error %m");
		return -1;
	}
	dbg("sv4l2: current subdev %lu x %lu %#X", ffs.format.width, ffs.format.height, ffs.format.code);
	if (busformat)
		return busformat(cbarg, &ffs);
	return 0;
}

static int _v4l2_subdev_set_config(void *arg, struct v4l2_subdev_format *ffs)
{
	DeviceConf_t *config = arg;
	uint32_t fourcc = 0xFFFFFFFF;
	switch (ffs->format.code)
	{
	case MEDIA_BUS_FMT_SGBRG10_1X10:
		fourcc = V4L2_PIX_FMT_SGBRG10; // GB10
	break;
	case MEDIA_BUS_FMT_SBGGR10_1X10:
		fourcc = V4L2_PIX_FMT_SBGGR10; // BG10
	break;
	case MEDIA_BUS_FMT_SGRBG10_1X10:
		fourcc = V4L2_PIX_FMT_SGRBG10; // BA10
	break;
	case MEDIA_BUS_FMT_SRGGB10_1X10:
		fourcc = V4L2_PIX_FMT_SRGGB10; // RG10
	break;
	case MEDIA_BUS_FMT_SRGGB12_1X12:
		fourcc = V4L2_PIX_FMT_SRGGB12; // RG12
	break;
	case MEDIA_BUS_FMT_SBGGR16_1X16:
		fourcc = V4L2_PIX_FMT_SRGGB16; // RG16
	break;
	default:
		warn("sv4l2: subdev format %#x not supported", ffs->format.code);
		fourcc = 0;
	break;
	};
	config->fourcc = fourcc;
	config->width = ffs->format.width;
	config->height = ffs->format.height;
	dbg("sv4l2: subdev format %dx%d %.4s", config->width, config->height, &config->fourcc);
	return 0;
}

V4L2_t *sv4l2_subdev_create2(int ctrlfd, V4l2Config_t *config)
{
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
	if (config)
	{
		subdev->name = config->parent.name;
		subdev->width = config->parent.width;
		subdev->height = config->parent.height;
		subdev->stride = config->parent.stride;
		subdev->fourcc = config->parent.fourcc;
		dbg("sv4l2: subdev %s created", subdev->name);
	}
	return subdev;
}

V4L2_t *sv4l2_subdev_create(const char *devicename, device_type_e type, V4l2Config_t *config)
{
	int ctrlfd = open(config->device, O_RDWR, 0);
	if (ctrlfd < 0)
	{
		err("sv4l2: subdevice %s not exist", config->device);
		return NULL;
	}
	V4L2_t *subdev = sv4l2_subdev_create2(ctrlfd, config);
	if (subdev == NULL)
		close(ctrlfd);
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
		int disable = json_is_true(json_object_get(subdevice, "disable"));
		if (!disable)
		{
			sv4l2_loadjsonconfiguration(config, subdevice);
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

static int _sv4l2_subdev_capabilities_pixformat(void *arg, struct v4l2_subdev_format *ffs)
{
	_JSONControl_Arg_t *jsoncontrol_arg = arg;
	json_t *definition = jsoncontrol_arg->controls;
	DeviceConf_t config = {0};

	json_t *pixelformat = json_object();
	json_object_set_new(pixelformat, "name", json_string("fourcc"));

	json_t *width = json_object();
	json_object_set_new(width, "name", json_string("width"));

	json_t *height = json_object();
	json_object_set_new(height, "name", json_string("height"));

	json_object_set_new(pixelformat, "value", json_sprintf("%.4s",&config.fourcc));
	json_object_set_new(width, "value", json_integer(config.width));
	json_object_set_new(height, "value", json_integer(config.height));

	if (jsoncontrol_arg->all)
	{
		json_object_set_new(pixelformat, "type", json_string(sv4l2_CTRLTYPE(V4L2_CTRL_TYPE_STRING)));
		json_t *items = json_array();
		struct v4l2_subdev_mbus_code_enum mbusEnum = {0};
		mbusEnum.pad = ffs->pad;
		mbusEnum.which = ffs->which;
		for (mbusEnum.index = 0; ioctl(jsoncontrol_arg->ctrlfd, VIDIOC_SUBDEV_ENUM_MBUS_CODE, &mbusEnum) == 0; mbusEnum.index++)
		{
			json_array_append_new(items, json_sprintf("%.4s",&config.fourcc));
		}
		if (mbusEnum.index > 0)
		{
			json_object_set(pixelformat, "items", items);
		}
		json_decref(items);

		json_object_set_new(width, "type", json_string(sv4l2_CTRLTYPE(V4L2_CTRL_TYPE_INTEGER)));
		json_object_set_new(height, "type", json_string(sv4l2_CTRLTYPE(V4L2_CTRL_TYPE_INTEGER)));
		json_t *items1 = json_array();
		json_t *items2 = json_array();
		struct v4l2_subdev_frame_size_enum framesizes = {0};
		framesizes.pad = ffs->pad;
		framesizes.code = ffs->format.code;
		framesizes.which = ffs->which;
		for (framesizes.index = 0; ioctl(jsoncontrol_arg->ctrlfd, VIDIOC_SUBDEV_ENUM_FRAME_SIZE, &framesizes) == 0; framesizes.index++)
		{
			if (framesizes.min_width == framesizes.max_width)
				json_array_append_new(items1, json_integer(framesizes.min_width));
			else
			{
				json_object_set_new(width, "minimum", json_integer(framesizes.min_width));
				json_object_set_new(width, "maximum", json_integer(framesizes.max_width));
			}
			if (framesizes.min_height == framesizes.max_height)
				json_array_append_new(items2, json_integer(framesizes.min_height));
			else
			{
				json_object_set_new(width, "minimum", json_integer(framesizes.min_height));
				json_object_set_new(width, "maximum", json_integer(framesizes.max_height));
			}
		}
		if (framesizes.index > 0)
		{
			json_object_set(width, "items", items1);
			json_object_set(height, "items", items2);
		}
		json_decref(items1);
		json_decref(items2);
	}
	json_array_append_new(definition, pixelformat);
	json_array_append_new(definition, width);
	json_array_append_new(definition, height);
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
	_JSONControl_Arg_t arg = {0};
	arg.controls = json_array();
	arg.all = all;
	arg.ctrlfd = subdev->fd;
	if (!sv4l2_subdev_getpixformat(subdev, _sv4l2_subdev_capabilities_pixformat, &arg))
		json_object_set(capabilities, "definition", arg.controls);
	json_decref(arg.controls);

	arg.controls = json_array();
	int ret;
	ret = sv4l2_treecontrols(subdev, sv4l2_jsoncontrol_cb, &arg);
	if (ret > 0)
		json_object_set(capabilities, "controls", arg.controls);
	json_decref(arg.controls);
	return 0;
}

#endif

FastVideoDevice_ops_t subdev_ops = {
	.name = "subv4l",
	.createconfig = sv4l2_subdev_createconfig,
	.create = (FastVideoDevice_create_t)sv4l2_subdev_create,
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
