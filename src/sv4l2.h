#ifndef __SV4L2_H__
#define __SV4L2_H__

#include <stdint.h>
#include <linux/videodev2.h>

#include "fastvideo.h"
#include "config.h"

#define MAX_SUBDEVS 4
#define MAX_SUBDEVPADS 10
#define CAMERACONFIG(config, defaultdevice) config = { \
	.DEVICECONFIG(parent, config, sv4l2_loadconfiguration), \
	.device = defaultdevice, \
	}

/**
 * @param device the device path as "/dev/video0".
 * @param transfer the callback may be used with sv4l2_loop function.
 * @param fd the file descriptor from another V4L2_t object.
 * @param mode a bits field build with MODE_CAPTURE, MODE_OUTPUT, MODE_META...
 * @param fps the number of frames per second, positive value for more than 1 fps,
 * negative value if one frame in more than 1 second
 */
typedef struct V4l2Config_s V4l2Config_t;
struct V4l2Config_s
{
	DeviceConf_t parent;
	DeviceConf_t transfer;
	const char *device;
	int mode;
	int fps;
	int periodic;
	int periodiccontrol;
	V4l2Config_t *subdev_entries[MAX_SUBDEVS];
	uint32_t fmtbus[MAX_SUBDEVPADS];
};

typedef struct V4L2Buffer_s V4L2Buffer_t;

typedef struct V4L2_s V4L2_t;
struct V4L2_s
{
	const char *name;
	char devicename[32];
	V4l2Config_t *config;
	uint32_t width;
	uint32_t height;
	uint32_t stride;
	uint32_t fourcc;
	int fd;
	enum v4l2_buf_type type;
	int nbuffers;
	int nplanes;
	V4L2Buffer_t *buffers;
	int mode;
	struct {
		V4L2Buffer_t *(*createbuffers)(V4L2_t *dev, int number, enum v4l2_memory memory);
	} ops;
	int (*periodicfunc)(V4L2_t *dev, int bufferid);
	int periodic;
	V4L2_t *subdevs[MAX_SUBDEVS];
};

/**
 * @brief create v4l2 device
 * The pointer must be passed to each other functions of the API.
 *
 * @param devicename it must be a name to different of other v4l2 device,
 * it may be the device path if this one is not defined into config.
 * @param type read v4l2 litterature, only CAPTURE,
 * OUTPUT and M2M are managed. The *_MPLANE is automaticly added if necessary
 * @param config a pointer to the configuration cf struct CameraConfig_s.
 *
 * @return V4L2_t object.
 */
V4L2_t *sv4l2_create(const char *devicename, device_type_e type, V4l2Config_t *config);
V4L2_t *sv4l2_create2(int fd, const char *devicename, device_type_e type, V4l2Config_t *config);

/**
 * @brief create a new object with a previous one
 * The new object may be used to M2M device
 *
 * @param dev the object to duplicate;
 *
 * @return V4L2_t object.
 */
V4L2_t *sv4l2_duplicate(V4L2_t *dev, V4l2Config_t **pconfig);

/**
 * @brief select and create a type of buffers.
 *
 * @param dev the V4L2_t object.
 * @param t a bits fiels defining the type of buffers.
 * @param ... other params depending on t cf enum buf_type_e.
 *
 * @return -1 on error, 0 otherwise.
 */
int sv4l2_requestbuffer(V4L2_t *dev, enum buf_type_e t, ...);

/**
 * @brief get the file descriptor of the device
 * The file descriptor with select or inside the CameraConfig structure
 *
 * @param dev		the V4L2_t object.
 * @param writer	0 if used with rdfs 1 if used with wfds
 *
 * @return fd.
 */
int sv4l2_fd(V4L2_t *dev, int writer);
/**
 * @brief get the true type of the buffers.
 *
 * @param dev the V4L2_t object.
 *
 * @return type;
 */
int sv4l2_type(V4L2_t *dev);
/**
 * @brief start the stream.
 *
 * @param dev the V4L2_t object.
 *
 * @return -1 on error, 0 otherwise.
 */
int sv4l2_start(V4L2_t *dev);
/**
 * @brief stop the stream.
 *
 * @param dev the V4L2_t object.
 *
 * @return -1 on error, 0 otherwise.
 */
int sv4l2_stop(V4L2_t *dev);
/**
 * @brief get the last ready buffer.
 *
 * @param dev the V4L2_t object.
 * @param mem the pointer to the memory containing the data.
 * @param bytesused the size of data.
 * @param flags few flags about the buffer (keyframe)
 *
 * @return the buffer index on success, otherwise -1.
 */
int sv4l2_dequeue(V4L2_t *dev, void **mem, size_t *bytesused, int *flags);
/**
 * @brief request to push a buffer into device.
 *
 * @param dev the V4L2_t object.
 * @param id the index of the buffer to push.
 * @param mem userptr buffer.
 * @param bytesused the size of data.
 * @param flags few flags about the buffer (keyframe)
 *
 * @return -1 on error, 0 otherwise.
 */
int sv4l2_queue(V4L2_t *dev, int index, void *mem, size_t bytesused, int flags);
/**
 * @brief set a rectaongle inseide the image to treat.
 *
 * @param dev the V4L2_t object.
 * @param r the rectangle strucutre cf standard v4l2 documentation.
 *
 * @return -1 on error, 0 otherwise.
 */
int sv4l2_crop(V4L2_t *dev, struct v4l2_rect *r);
int sv4l2_compose(V4L2_t *dev, struct v4l2_rect *r);

/**
 * @brief get/set device control
 *
 * @param dev the V4L2_t object.
 * @param id the CID_ cf the standard v4l2 documentation.
 * @param value the value to set.
 *
 * @return -1 on error, 0 otherwise.
 */
void *sv4l2_control(V4L2_t *dev, int id, void *value);

/**
 * @brief parse all controls.
 * it calls the cb function for each control available on the device.
 *
 * @param dev the V4L2_t object.
 * @param cb the called function.
 * @param arg the first argument of cb.
 *
 * @return -1 on error, 0 otherwise.
 */
int sv4l2_treecontrols(V4L2_t *dev, int (*cb)(void *arg, struct v4l2_query_ext_ctrl *ctrl), void * arg);
/**
 * @brief parse a control menu.
 * it calls the cb function for each entry of the menu.
 *
 * @param dev the V4L2_t object.
 * @param ctrl the control
 * @param cb the called function.
 * @param arg the first argument of cb.
 *
 * @return -1 on error, 0 otherwise.
 */
int sv4l2_treecontrolmenu(V4L2_t *dev, struct v4l2_query_ext_ctrl *ctrl, int (*cb)(void *arg, struct v4l2_querymenu *ctrl), void * arg);

/**
 * @brief get/set the frame per second.
 *
 * @param dev the V4L2_t object.
 * @param fps the value must be -1 for getting otherwise fps is set,if fps<0, the value is set 1/fps.
 *
 * @return -1 on error, fps otherwise.
 */
int sv4l2_fps(V4L2_t *dev, int fps);

/**
 * @brief send dynamic configuration to an interactive loop
 *
 * @param dev the V4L2_t object.
 * @param json a string containing json controls.
 * @param length the size of the string.
 *
 * @return -1 on error, 0 otherwise.
 */
int sv4l2_interactive(V4L2_t *dev, const char *json, size_t length);

/**
 * @brief free and delete the object.
 *
 * @param dev the V4L2_t object.
 */
void sv4l2_destroy(V4L2_t *dev);

/**
 * @brief create a default config object.
 *
 * @return the object or NULL
 */
DeviceConf_t * sv4l2_createconfig(const char *name);

const char *sv4l2_CTRLTYPE(enum v4l2_ctrl_type type);
const char *sv4l2_CTRLNAME(uint32_t id);

#ifdef HAVE_JANSSON
int sv4l2_loadjsonsettings(V4L2_t *dev, void *jconfig);
int sv4l2_loadjsonconfiguration(void *config, void *jconfig);

#define sv4l2_loadsettings sv4l2_loadjsonsettings
#define sv4l2_loadconfiguration sv4l2_loadjsonconfiguration

int sv4l2_capabilities(V4L2_t *dev, json_t *capabilities, int all);
int sv4l2_capabilities_definition(V4L2_t *dev, json_t *definition, int all);
int sv4l2_jsoncontrol_cb(void *arg, struct v4l2_query_ext_ctrl *ctrl);

#else
#define sv4l2_loadsettings NULL
#define sv4l2_loadconfiguration NULL
#define sv4l2_capabilities NULL
#endif

extern const FastVideoDevice_ops_t sv4l2_ops;
#endif
