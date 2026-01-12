#ifndef __SEGL_H__
#define __SEGL_H__

#include <GLES2/gl2.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include "fastvideo.h"
#include "config.h"
#include "log.h"

#define EGLCONFIG(name, _native) name = { \
	.DEVICECONFIG(parent, name, segl_loadconfiguration), \
	.native = #_native \
	}

#define MAX_SHADERS 4
#define MAX_PROGRANS 5
#define MAX_BUFFERS 4

#define segl_checkerror(...) \
{ \
	GLenum err = glGetError(); \
	if (err != GL_NO_ERROR) \
	{ \
		err("segl: %s %d %#X", __FILE__, __LINE__, err); \
	} \
} \

/**
 * structure shared by segl and segl_glprog
 */
typedef struct FourccFormat_s FourccFormat_t;
struct FourccFormat_s
{
	uint32_t fourcc;
	GLuint internal;
	GLuint full;
	GLuint data;
	int nplanes;
	int stride_factor[4];
};
const FourccFormat_t *fourcc_getformat(uint32_t fourcc);

typedef struct GL_Buffer_s GL_Buffer_t;
struct GL_Buffer_s
{
	EGLImageKHR image;
	GLuint texture;
	GLenum textype;
	EGLint egltarget;
};

typedef struct GLBuffer_s GLBuffer_t;
struct GLBuffer_s
{
	int id;
	uint32_t fourcc;
	int dma_fd;
	uint32_t *memory;
	GLuint pitch;
	GLuint offset;
	uint32_t size;
	uint64_t modifiers;
	GL_Buffer_t gl;
	FrameBuffer_state_e state;
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
typedef struct EGLExport_s EGLExport_t;
typedef struct EGLNative_s EGLNative_t;

typedef struct EGLConfig_s EGLConfig_t;
struct EGLConfig_s
{
	DeviceConf_t parent;
	DeviceConf_t transfer;
	const EGLNative_t *native;
	const char *device;
	EGLConfig_Program_t *programs;
	const EGLExport_t *export;
	int type;
};

typedef struct EGL_s EGL_t;

struct EGLNative_s
{
	const char *name;
	EGLNativeDisplayType (*display)(EGLConfig_t *config);
	const EGLint *(*attributes)(EGLNativeDisplayType native_display);
	EGLNativeWindowType (*createwindow)(EGLNativeDisplayType native_display,
							GLuint width, GLuint height, const GLchar *name);
	int (*fd)(EGLNativeWindowType native_win);
	int (*flush)(EGLNativeWindowType native_win);
	int (*sync)(EGLNativeWindowType native_win);
	void (*destroy)(EGLNativeDisplayType native_display);
};
typedef void (*segl_native_append_t)(EGLNative_t *native);

struct EGLExport_s
{
	const char *name;
	const char *native;
	void *(*create)(EGLConfig_t *config, EGLDisplay eglDisplay, EGLContext eglContext);
	GLuint (*fbo)(void *arg);
	GL_Buffer_t * (*out)(void *arg);
	int (*fd)(void *arg);
	int (*setbuffer)(void *arg, GLBuffer_t *buffer);
	int (*releasebuffer)(void *arg, GLBuffer_t *buffer);
	int (*flush)(void *arg, GLBuffer_t *buffer);
	void (*destroy)(void*arg);
};

typedef void (*segl_export_append_t)(EGLExport_t *export);

extern const GLchar *defaulttexturename;

GLProgram_t *glprog_create(EGLConfig_Program_t *config, GLuint width, GLuint height);
int glprog_setup(GLProgram_t *program, GLuint fbo, GL_Buffer_t *out);
int glprog_run(GLProgram_t *program, GL_Buffer_t *buffer);
int glprog_setuniform(GLProgram_t *program, GLProgram_Uniform_t *uniform);
void glprog_destroy(GLProgram_t *program);

int _egl_hasextension(EGLDisplay eglDisplay, const char *extension);
int segl_hasextension(EGL_t *dev, const char *extension);

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

extern FastVideoDevice_ops_t segl_ops;
#endif
