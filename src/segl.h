#ifndef __SEGL_H__
#define __SEGL_H__

#include <GLES2/gl2.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include "config.h"

#define EGLCONFIG(name, _native) name = { \
	.DEVICECONFIG(parent, name, segl_loadconfiguration), \
	.native = #_native \
	}

#define MAX_SHADERS 4
#define MAX_PROGRANS 5
#define MAX_BUFFERS 4

/**
 * structure shared by segl and segl_glprog
 */
typedef struct GLBuffer_s GLBuffer_t;
struct GLBuffer_s
{
	uint32_t fb_id;
	int egl_fd;
	GLenum textype;
	GLuint dma_texture;
	EGLImageKHR dma_image;
	uint32_t fourcc;
	int dma_fd;
	uint32_t *memory;
	GLuint pitch;
	GLuint offset;
	uint32_t size;
};

typedef struct GLProgram_Uniform_s GLProgram_Uniform_t;
typedef struct EGLConfig_Program_s EGLConfig_Program_t;
struct EGLConfig_Program_s
{
	const char *vertex;
	const char *fragments[MAX_SHADERS];
	const char *tex_name;
	EGLConfig_Program_t *next;
	GLProgram_Uniform_t *controls;
};

typedef struct GLProgram_s GLProgram_t;

typedef struct EGLConfig_s EGLConfig_t;
struct EGLConfig_s
{
	DeviceConf_t parent;
	const char *native;
	const char *device;
	EGLConfig_Program_t *programs;
};

typedef struct EGL_s EGL_t;

EGL_t *segl_create(const char *devicename, EGLConfig_t *config);
int segl_requestbuffer(EGL_t *dev, enum buf_type_e t, ...);
int segl_queue(EGL_t *dev, int id, size_t bytesused);
int segl_dequeue(EGL_t *dev, void **mem, size_t *bytesused);
int segl_start(EGL_t *dev);
int segl_stop(EGL_t *dev);
int segl_fd(EGL_t *dev);
void segl_destroy(EGL_t *dev);

typedef struct EGLNative_s EGLNative_t;
struct EGLNative_s
{
	const char *name;
	EGLNativeDisplayType (*display)(const char *device);
	EGLNativeWindowType (*createwindow)(EGLNativeDisplayType native_display,
							GLuint width, GLuint height, const GLchar *name);
	int (*fd)(EGLNativeWindowType native_win);
	int (*flush)(EGLNativeWindowType native_win);
	int (*sync)(EGLNativeWindowType native_win);
	void (*destroy)(EGLNativeDisplayType native_display);
};

extern const GLchar *defaulttexturename;

GLProgram_t *glprog_create(EGLConfig_Program_t *config);
int glprog_setup(GLProgram_t *program, GLuint width, GLuint height);
GLBuffer_t *glprog_getouttexture(GLProgram_t *program, GLuint nbtex);
int glprog_setintexture(GLProgram_t *program, GLenum type, GLuint nbtex, GLBuffer_t *in_textures);
int glprog_run(GLProgram_t *program, int bufid);
int glprog_setuniform(GLProgram_t *program, GLProgram_Uniform_t *uniform);
void glprog_destroy(GLProgram_t *program);

DeviceConf_t * segl_createconfig();

#ifdef HAVE_JANSSON
int segl_loadjsonsettings(EGL_t *dev, void *jconfig);
int segl_loadjsonconfiguration(void *arg, void *entry);

#define segl_loadsettings segl_loadjsonsettings
#define segl_loadconfiguration segl_loadjsonconfiguration

int glprog_loadjsonsetting(GLProgram_t *program, void *entry);
int glprog_loadjsonconfiguration(void *arg, void *entry);
#else
#define segl_loadsettings NULL
#define segl_loadconfiguration NULL
#endif

#endif
