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
	uint64_t modifiers;
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
	uint32_t transfer;
	uint64_t transfer_modifiers;
	EGLConfig_Program_t *programs;
};

typedef struct EGL_s EGL_t;

typedef struct EGLNative_s EGLNative_t;
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

extern const GLchar *defaulttexturename;

GLProgram_t *glprog_create(EGLConfig_Program_t *config);
int glprog_setup(GLProgram_t *program, GLuint width, GLuint height);
GLBuffer_t *glprog_getouttexture(GLProgram_t *program, GLuint nbtex);
int glprog_setintexture(GLProgram_t *program, GLenum type, GLuint nbtex, GLBuffer_t *in_textures);
int glprog_run(GLProgram_t *program, int bufid);
int glprog_setuniform(GLProgram_t *program, GLProgram_Uniform_t *uniform);
void glprog_destroy(GLProgram_t *program);

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
