#ifndef __SV4L2_SUBDEV_H__
#define __SV4L2_SUBDEV_H__

#include <stdint.h>
#include <linux/v4l2-subdev.h>

#include "config.h"
#include "sv4l2.h"

typedef struct sv4l2_subdev_stream_s sv4l2_subdev_stream_t;

DeviceConf_t * sv4l2_subdev_createconfig(const char *name);
/**
 * @brief create a video subdevice and check capabilities
 *
 * @param config the configuration object
 *
 * @return the new obejct or NULL
 */
V4L2_t *sv4l2_subdev_create(const char *devicename, device_type_e type, V4l2Config_t *config);

/**
 * @brief returns information about subdevice definition
 * The function calls the callback with the subdevice format
 *
 * @param subdev the subdevice
 * @param pad the media pad to request or set
 * @param buformat the callback
 * @param cbarg the first argument of the callback
 *
 * @return the pixmap code (not the fourcc)
 */
struct v4l2_subdev_format;
uint32_t sv4l2_subdev_getpixformat(V4L2_t *subdev, sv4l2_subdev_stream_t *stream, int (*pixformat)(void *arg, struct v4l2_subdev_format *ffs), void *cbarg);
int sv4l2_subdev_setpixformat(V4L2_t *subdev, sv4l2_subdev_stream_t *stream, uint32_t fmtbus, uint32_t width, uint32_t height);

/**
 * @brief returns information about fmtbus values available
 * The function calls the callback with the subdevice format
 *
 * @param subdev the subdevice
 * @param pad the pad number inside the media
 * @param fmtbus the callback, returns 0 to select a fmtbus otherwise -1
 * @param cbarg the first argument of the callback
 *
 * @return the selected pixmap code (not the fourcc)
 */
struct v4l2_subdev_mbus_code_enum;
uint32_t sv4l2_subdev_getfmtbus(V4L2_t *subdev, sv4l2_subdev_stream_t *stream, int(*fmtbus)(void *arg, struct v4l2_subdev_mbus_code_enum *mbuscode), void *cbarg);

/**
 * @brief get/set frame rate
 *
 * @param subdev the subdevice
 * @param pad the pad number inside the media
 * @param fps the frame rate (>0 is frames / seconde, <0 if secondes / frame, -1 to read only)
 *
 * @return the fps
 */
int sv4l2_subdev_fps(V4L2_t *subdev, sv4l2_subdev_stream_t *stream, int fps);

/**
 * @brief release memory of the instance
 *
 * @param subdev the instance of the object
 */
void sv4l2_subdev_destroy(V4L2_t *subdev);

#ifdef HAVE_JANSSON
int sv4l2_subdev_loadjsonconfiguration(void *arg, void *entry);
int sv4l2_subdev_capabilities(V4L2_t *subdev, json_t *capabilities, int all);
#endif

extern const FastVideoDevice_ops_t subdev_ops;
#endif
