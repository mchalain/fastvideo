#ifndef __SEGL_H__
#define __SEGL_H__

#include <GLES2/gl2.h>
#include <EGL/egl.h>
#include "config.h"

#define EGLCONFIG(name, _native) name = { \
	.DEVICECONFIG(parent, name, segl_loadjsonconfiguration, NULL), \
	.native = #_native \
	}

typedef struct EGLConfig_s EGLConfig_t;
struct EGLConfig_s
{
	DeviceConf_t parent;
	const char *native;
	const char *vertex;
	const char *fragment;
	struct
	{
		char *name;
	} texture;
};

typedef struct EGL_s EGL_t;

EGL_t *segl_create(const char *devicename, EGLConfig_t *config);
int segl_requestbuffer(EGL_t *dev, enum buf_type_e t, ...);
int segl_queue(EGL_t *dev, int id, size_t bytesused);
int segl_dequeue(EGL_t *dev, void **mem, size_t *bytesused);
int segl_start(EGL_t *dev);
int segl_stop(EGL_t *dev);
void segl_destroy(EGL_t *dev);

typedef struct EGLNative_s EGLNative_t;
struct EGLNative_s
{
	const char *name;
	EGLNativeDisplayType (*display)();
	EGLNativeWindowType (*createwindow)(EGLNativeDisplayType native_display,
							GLuint width, GLuint height, const GLchar *name);
	GLboolean (*running)(EGLNativeWindowType native_win);
	void (*destroy)(EGLNativeDisplayType native_display);
};

#ifdef HAVE_JANSSON
int segl_loadjsonconfiguration(void *arg, void *jconfig);
#else
#define segl_loadjsonconfiguration NULL
#endif

#endif
