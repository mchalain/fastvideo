#ifndef __SV4L2_H__
#define __SV4L2_H__

#include <stdint.h>
#include <linux/videodev2.h>

#include "config.h"

#define MODE_VERBOSE 0x01
#define MODE_INTERACTIVE 0x04
#define MODE_SHOT 0x08

#define CAMERACONFIG(config, defaultdevice) config = { \
	.DEVICECONFIG(parent, config, sv4l2_loadconfiguration), \
	.device = defaultdevice, \
	}
typedef struct CameraDefinition_s CameraDefinition_t;
struct CameraDefinition_s
{
	int mode;
	int fps;
};

typedef struct SubDevConfig_s  SubDevConfig_t;
struct SubDevConfig_s
{
	DeviceConf_t parent;
	const char *device;
	CameraDefinition_t definition;
};

/**
 * @param device the device path as "/dev/video0".
 * @param transfer the callback may be used with sv4l2_loop function.
 * @param fd the file descriptor from another V4L2_t object.
 * @param mode a bits field build with MODE_VERBOSE, MODE_INTERACTIVE...
 * @param fps the number of frames per second, positive value for more than 1 fps,
 * negative value if one frame in more than 1 second
 */
typedef struct CameraConfig_s CameraConfig_t;
struct CameraConfig_s
{
	DeviceConf_t parent;
	const char *device;
	SubDevConfig_t *subdevices;
	int nsubdevices;
	int (*transfer)(void *, int id, const char *mem, size_t size);
	int fd;
	CameraDefinition_t definition;
};

typedef struct V4L2_s V4L2_t;
typedef struct V4L2Subdev_s V4L2Subdev_t;

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
V4L2_t *sv4l2_create(const char *devicename, device_type_e type, CameraConfig_t *config);
V4L2_t *sv4l2_create2(int fd, const char *devicename, device_type_e type, CameraConfig_t *config);

/**
 * @brief create a new object with a previous one
 * The new object may be used to M2M device
 *
 * @param dev the object to duplicate;
 *
 * @return V4L2_t object.
 */
V4L2_t *sv4l2_duplicate(V4L2_t *dev);

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

/**
 * @brief create a video subdevice and check capabilities
 *
 * @param config the configuration object
 *
 * @return the new obejct or NULL
 */
V4L2Subdev_t *sv4l2_subdev_create(SubDevConfig_t *config);
V4L2Subdev_t *sv4l2_subdev_create2(int ctrlfd, SubDevConfig_t *config);

/**
 * @brief returns information about subdevice definition
 * The function calls the callback with the subdevice format
 *
 * @param ctrlfd the file descriptor of the subdevice
 * @param buformat the callback
 * @param cbarg the first argument of the callback
 *
 * @return the pixmap code (not the fourcc)
 */
struct v4l2_subdev_format;
uint32_t sv4l2_subdev_getpixformat(V4L2Subdev_t *subdev, int (*pixformat)(void *arg, struct v4l2_subdev_format *ffs), void *cbarg);
int sv4l2_subdev_setpixformat(V4L2Subdev_t *subdev, uint32_t fourcc, uint32_t width, uint32_t height);

/**
 * @brief returns information about fmtbus values available
 * The function calls the callback with the subdevice format
 *
 * @param ctrlfd the file descriptor of the subdevice
 * @param fmtbus the callback, returns 0 to select a fmtbus otherwise -1
 * @param cbarg the first argument of the callback
 *
 * @return the selected pixmap code (not the fourcc)
 */
struct v4l2_subdev_mbus_code_enum;
uint32_t sv4l2_subdev_getfmtbus(V4L2Subdev_t *subdev, int(*fmtbus)(void *arg, struct v4l2_subdev_mbus_code_enum *mbuscode), void *cbarg);

/**
 * @brief release memory of the instance
 *
 * @param subdev the instance of the object
 */
void sv4L2_subdev_destroy(V4L2Subdev_t *subdev);

#ifdef HAVE_JANSSON
int sv4l2_loadjsonsettings(V4L2_t *dev, void *jconfig);
int sv4l2_loadjsonconfiguration(void *config, void *jconfig);
int sv4l2_subdev_loadjsonconfiguration(void *arg, void *entry);

#define sv4l2_loadsettings sv4l2_loadjsonsettings
#define sv4l2_loadconfiguration sv4l2_loadjsonconfiguration

int sv4l2_capabilities(V4L2_t *dev, json_t *capabilities, int all);
int sv4l2_subdev_capabilities(V4L2Subdev_t *subdev, json_t *capabilities, int all);

#else
#define sv4l2_loadsettings NULL
#define sv4l2_loadconfiguration NULL
#endif

#endif
