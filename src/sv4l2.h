#ifndef __SV4L2_H__
#define __SV4L2_H__

#include <stdint.h>
#include <linux/videodev2.h>

#include "config.h"

#define MODE_VERBOSE 0x01
#define MODE_INTERACTIVE 0x04
#define MODE_SHOT 0x08

#define CAMERACONFIG(config, defaultdevice) config = { \
	.DEVICECONFIG(parent, config, sv4l2_loadjsonconfiguration, sv4l2_loadjsonsettings), \
	.device = defaultdevice, \
	}

/**
 * @param device the device path as "/dev/video0".
 * @param transfer the callback may be used with sv4l2_loop function.
 * @param fd the file descriptor from another V4L2_t object.
 * @param fourcc the graphic code formated on 32 bits as "XRGB" or "YUYV".
 * @param mode a bits field build with MODE_VERBOSE, MODE_INTERACTIVE...
 * @param width the width of the image.
 * @param height the height of the image.
 * @param fps the number of frames per second, positive value for more than 1 fps,
 * negative value if one frame in more than 1 second
 */
typedef struct CameraConfig_s CameraConfig_t;
struct CameraConfig_s
{
	DeviceConf_t parent;
	const char *device;
	const char *subdevice;
	int (*transfer)(void *, int id, const char *mem, size_t size);
	int fd;
	int mode;
	int fps;
};

typedef struct V4L2_s V4L2_t;

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
V4L2_t *sv4l2_create(const char *devicename, CameraConfig_t *config);
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
 * @brief simple function to transfer data using a callback.
 *
 * @param dev the V4L2_t object.
 * @param transfer the callback.
 * @param transferarg the first argument of the callbak.
 *
 * @return -1 on error, 0 otherwise.
 */
int sv4l2_loop(V4L2_t *dev, int (*transfer)(void *, int id, const char *mem, size_t size), void *transferarg);
/**
 * @brief simple function to transfer data betwwen a slave and a master objects.
 *
 * @param dev the V4L2_t object.
 * @param link the slave object.
 *
 * @return -1 on error, 0 otherwise.
 */
int sv4l2_transfer(V4L2_t *dev, V4L2_t *link);

/**
 * @brief get the file descriptor of the device
 * The file descriptor with select or inside the CameraConfig structure
 *
 * @param dev the V4L2_t object.
 *
 * @return fd.
 */
int sv4l2_fd(V4L2_t *dev);
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
 *
 * @return the buffer index on success, otherwise -1.
 */
int sv4l2_dequeue(V4L2_t *dev, void **mem, size_t *bytesused);
/**
 * @brief request to push a buffer into device.
 *
 * @param dev the V4L2_t object.
 * @param id the index of the buffer to push.
 * @param bytesused the size of data.
 *
 * @return -1 on error, 0 otherwise.
 */
int sv4l2_queue(V4L2_t *dev, int index, size_t bytesused);
/**
 * @brief set a rectaongle inseide the image to treat.
 *
 * @param dev the V4L2_t object.
 * @param r the rectangle strucutre cf standard v4l2 documentation.
 * @param dev the V4L2_t object.
 *
 * @return -1 on error, 0 otherwise.
 */
int sv4l2_crop(V4L2_t *dev, struct v4l2_rect *r);
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
int sv4l2_treecontrols(V4L2_t *dev, int (*cb)(void *arg, struct v4l2_queryctrl *ctrl, V4L2_t *dev), void * arg);
/**
 * @brief parse a control menu.
 * it calls the cb function for each entry of the menu.
 *
 * @param dev the V4L2_t object.
 * @param id the menu control id.
 * @param cb the called function.
 * @param arg the first argument of cb.
 *
 * @return -1 on error, 0 otherwise.
 */
int sv4l2_treecontrolmenu(V4L2_t *dev, int id, int (*cb)(void *arg, struct v4l2_querymenu *ctrl, V4L2_t *dev), void * arg);
#ifdef HAVE_JANSSON
/**
 * @brief callback for sv4l2_treecontrols.
 * it fills a json_t object with controls information
 *
 * @param arg a pointer on json_object of jansson library.
 * @param ctrl the control cf the standard v4l2 dpcumentation.
 * @param dev the V4L2_t object.
 *
 * @return -1 on error, 0 otherwise.
 */
int sv4l2_jsoncontrol_cb(void *arg, struct v4l2_queryctrl *ctrl, V4L2_t *dev);
#endif

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
DeviceConf_t * sv4l2_createconfig();

#ifdef HAVE_JANSSON
int sv4l2_loadjsonsettings(void *dev, void *jconfig);
int sv4l2_loadjsonconfiguration(void *config, void *jconfig);
#else
#define sv4l2_loadjsonsettings NULL
#define sv4l2_loadjsonconfiguration NULL
#endif

#endif
