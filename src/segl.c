#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <errno.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include "segl.h"
#include "log.h"

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

extern EGLNative_t *eglnative_offscreen;
#ifdef HAVE_GBM
extern EGLNative_t *eglnative_drm;
#endif
#ifdef HAVE_X11
extern EGLNative_t *eglnative_x11;
#endif
#ifdef HAVE_WAYLAND_EGL
extern EGLNative_t *eglnative_wayland;
#endif

typedef struct EGL_s EGL_t;
struct EGL_s
{
	EGLConfig_t *config;
	EGLNative_t *native;
	device_type_e type;
	EGLDisplay egldisplay;
	EGLConfig eglconfig;
	EGLContext eglcontext;
	EGLSurface eglsurface;
	GLuint fbo;
	EGL_t *dup;
	EGLNativeDisplayType native_display;
	EGLNativeWindowType native_window;
	GLProgram_t *programs;
	GLBuffer_t buffers[MAX_BUFFERS];
	int curbufferid;
	int nbuffers;
};

#ifndef GL_TEXTURE_EXTERNAL_OES
#define GL_TEXTURE_EXTERNAL_OES GL_TEXTURE_2D;
#endif

#ifndef EGL_KHR_image
#error "this version of EGL doesn't support KHR Image"
#endif
#ifndef GL_OES_EGL_image
#error "this version of GLES doesn't support EGL Image"
#endif
#if defined(EGL_KHR_image) && !defined(EGL_EGLEXT_PROTOTYPES)
PFNEGLCREATEIMAGEKHRPROC eglCreateImageKHR = NULL;
PFNEGLDESTROYIMAGEKHRPROC eglDestroyImageKHR = NULL;
#endif
#if defined(EGL_MESA_image_dma_buf_export) && !defined(EGL_EGLEXT_PROTOTYPES)
PFNEGLEXPORTDMABUFIMAGEQUERYMESAPROC eglExportDMABUFImageQueryMESA = NULL;
PFNEGLEXPORTDMABUFIMAGEMESAPROC eglExportDMABUFImageMESA = NULL;
#endif
#if defined(GL_OES_EGL_image) && !defined(EGL_EGLEXT_PROTOTYPES)
PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES = NULL;
PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC glEGLImageTargetRenderbufferStorageOES = NULL;
#endif

static FourccFormat_t _FourccFormats[] =
{
	{ .fourcc = FOURCC_RGBA, .internal = GL_RGBA, .full = GL_RGBA, .data = GL_UNSIGNED_BYTE       , .nplanes = 1, .stride_factor={sizeof(uint32_t),0,0,0}},
	{ .fourcc = FOURCC_AB24, .internal = GL_RGBA, .full = GL_RGBA, .data = GL_UNSIGNED_BYTE       , .nplanes = 1, .stride_factor={sizeof(uint32_t),0,0,0}},
	{ .fourcc = FOURCC_XB24, .internal = GL_RGBA, .full = GL_RGBA, .data = GL_UNSIGNED_BYTE       , .nplanes = 1, .stride_factor={sizeof(uint32_t),0,0,0}},
	{ .fourcc = FOURCC_AR24, .internal = GL_RGBA, .full = GL_RGBA, .data = GL_UNSIGNED_BYTE       , .nplanes = 1, .stride_factor={sizeof(uint32_t),0,0,0}},
	{ .fourcc = FOURCC_XR24, .internal = GL_RGBA, .full = GL_RGBA, .data = GL_UNSIGNED_BYTE       , .nplanes = 1, .stride_factor={sizeof(uint32_t),0,0,0}},
	{ .fourcc = FOURCC_RGBP, .internal = GL_RGB , .full = GL_RGB , .data = GL_UNSIGNED_SHORT_5_6_5, .nplanes = 1, .stride_factor={sizeof(uint16_t),0,0,0}},
	{ .fourcc = FOURCC_R8  , .internal = GL_RED_EXT, .full = GL_RED_EXT, .data = GL_UNSIGNED_BYTE , .nplanes = 1, .stride_factor={sizeof(uint8_t),0,0,0}},
	{ .fourcc = FOURCC_YUYV, .internal = GL_RGBA, .full = GL_RGBA, .data = GL_UNSIGNED_BYTE       , .nplanes = 1, .stride_factor={sizeof(uint32_t),0,0,0}},
};

static const FourccFormat_t *fourcc_getformat(uint32_t fourcc)
{
	FourccFormat_t *format = NULL;
	for (int i = 0; i < sizeof(_FourccFormats)/sizeof(*_FourccFormats); i++)
	{
		format = &_FourccFormats[i];
		if (format->fourcc == fourcc)
			break;
	}
	return format;
}

EGL_t *segl_create(const char *devicename, device_type_e type, EGLConfig_t *config)
{
	if (type != device_output && type != device_transfer)
	{
		err("segl: %s bad device type", config->parent.name);
		return NULL;
	}
	EGLNativeDisplayType ndisplay = EGL_DEFAULT_DISPLAY;
	EGLNative_t *natives[] =
	{
#ifdef HAVE_GBM
		eglnative_drm,
#endif
#ifdef HAVE_X11
		eglnative_x11,
#endif
#ifdef HAVE_WAYLAND_EGL
		eglnative_wayland,
#endif
		eglnative_offscreen,
		NULL,
	};
	EGLNative_t *native = natives[0];

	if (config->native)
	{
		for (int i = 0; i < sizeof(natives) / sizeof(*natives) && natives[i]; i++)
		{
			if (!strcmp(natives[i]->name, config->native))
			{
				native = natives[i];
				break;
			}
		}
	}
	warn("segl: native %s", native->name);
	ndisplay = native->display(config);

	EGLNativeWindowType nwindow = native->createwindow(ndisplay, config->parent.width, config->parent.height, "segl");

	EGLDisplay eglDisplay = eglGetDisplay(ndisplay);

	EGLint major, minor;
	if (!eglInitialize(eglDisplay, &major, &minor))
	{
		err("segl: failed to initialize");
		native->destroy(ndisplay);
		return NULL;
	}

	if (!eglBindAPI(EGL_OPENGL_ES_API))
	{
		err("segl: failed to bind api EGL_OPENGL_ES_API");
		native->destroy(ndisplay);
		return NULL;
	}
#ifndef GLSLV300
	glEnable(GL_TEXTURE_EXTERNAL_OES);
#endif
	EGLint num_config;
	eglGetConfigs(eglDisplay, NULL, 0, &num_config);

	static const EGLint config_attribs[] = {
		EGL_RED_SIZE, 8,
		EGL_GREEN_SIZE, 8,
		EGL_BLUE_SIZE, 8,
		EGL_ALPHA_SIZE, 8,
		//EGL_DEPTH_SIZE, 16, // DEPTH management in useless for this application
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
		EGL_NONE
	};
	EGLConfig eglConfig;
	if (!eglChooseConfig(eglDisplay, config_attribs, &eglConfig, 1, &num_config))
	{
		err("segl: failed to choose config: %d (%#x)", num_config, eglGetError());
		native->destroy(ndisplay);
		return NULL;
	}

	static const EGLint context_attribs[] = {
		EGL_CONTEXT_CLIENT_VERSION, 2,
		EGL_NONE
	};
	EGLContext eglContext;
	eglContext = eglCreateContext(eglDisplay, eglConfig, EGL_NO_CONTEXT, context_attribs);
	if (eglContext == NULL)
	{
		err("segl: failed to create context (%#x)", eglGetError());
		native->destroy(ndisplay);
		return NULL;
	}

	EGLSurface eglSurface = NULL;
	if (nwindow != (EGLNativeWindowType)NULL)
	{
		eglSurface = eglCreateWindowSurface(eglDisplay, eglConfig, nwindow, NULL);
	}
	else
	{
		warn("segl: surface on pbuffer");
		EGLint pbufferAttribs[] = {
			EGL_WIDTH, config->parent.width,
			EGL_HEIGHT, config->parent.height,
			EGL_TEXTURE_FORMAT, EGL_TEXTURE_RGBA,
			EGL_TEXTURE_TARGET, EGL_TEXTURE_2D,
			EGL_NONE,
		};

		eglSurface = eglCreatePbufferSurface(eglDisplay, eglConfig, pbufferAttribs);
	}
	if (eglSurface == EGL_NO_SURFACE)
	{
		err("segl: failed to create egl surface (%#x)", eglGetError());
		native->destroy(ndisplay);
		return NULL;
	}
	eglMakeCurrent(eglDisplay, eglSurface, eglSurface, eglContext);

	GLint minswapinterval = 1;
	eglGetConfigAttrib(eglDisplay, eglConfig, EGL_MIN_SWAP_INTERVAL, &minswapinterval);
	dbg("segl: swap interval %d", minswapinterval);
	eglSwapInterval(eglDisplay, minswapinterval);

	GLProgram_t *programs = glprog_create(config->programs);
	if (programs == NULL)
		return NULL;

	EGL_t *dev = calloc(1, sizeof(*dev));
	dev->config = config;
	dev->native = native;
	dev->egldisplay = eglDisplay;
	dev->eglconfig = eglConfig;
	dev->eglcontext = eglContext;
	dev->eglsurface = eglSurface;
	dev->programs = programs;

#ifndef EGL_EGLEXT_PROTOTYPES
	eglCreateImageKHR = (void *) eglGetProcAddress("eglCreateImageKHR");
	if(eglCreateImageKHR == NULL)
	{
		native->destroy(ndisplay);
		return NULL;
	}
	eglDestroyImageKHR = (void *) eglGetProcAddress("eglDestroyImageKHR");
	if(eglDestroyImageKHR == NULL)
	{
		native->destroy(ndisplay);
		return NULL;
	}
#if defined(EGL_MESA_image_dma_buf_export)
	eglExportDMABUFImageQueryMESA = (void *) eglGetProcAddress("eglExportDMABUFImageQueryMESA");
	if(eglExportDMABUFImageQueryMESA == NULL)
	{
		native->destroy(ndisplay);
		return NULL;
	}
	eglExportDMABUFImageMESA = (void *) eglGetProcAddress("eglExportDMABUFImageMESA");
	if(eglExportDMABUFImageMESA == NULL)
	{
		native->destroy(ndisplay);
		return NULL;
	}
#endif
	glEGLImageTargetTexture2DOES = (void *) eglGetProcAddress("glEGLImageTargetTexture2DOES");
	if(glEGLImageTargetTexture2DOES == NULL)
	{
		native->destroy(ndisplay);
		return NULL;
	}
#endif

	glprog_setup(dev->programs, config->parent.width, config->parent.height);

	dev->native_window = nwindow;
	dev->native_display = ndisplay;
	dev->curbufferid = -1;
	dev->type = type;
	return dev;
}

static GLuint texture_create(EGL_t *dev, GLenum textype)
{
	GLuint dma_texture;
	glGenTextures(1, &dma_texture);

	glBindTexture(textype, dma_texture);
	glTexParameteri(textype, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(textype, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri(textype, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(textype, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	return dma_texture;
}

static int texturedma_link(EGL_t *dev, GLuint dma_texture, int dma_fd, size_t size)
{
	uint32_t stride = size / dev->config->parent.height;
	EGLImageKHR dma_image;
	GLint attrib_list[] = {
		EGL_WIDTH, dev->config->parent.width,
		EGL_HEIGHT, dev->config->parent.height,
		EGL_LINUX_DRM_FOURCC_EXT, dev->config->parent.fourcc,
		EGL_DMA_BUF_PLANE0_FD_EXT, dma_fd,
		EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
		EGL_DMA_BUF_PLANE0_PITCH_EXT, stride,
		EGL_NONE
	};
	dbg("segl: create image for dma %d : %dx%d %u %.4s", dma_fd, dev->config->parent.width, dev->config->parent.height, stride, (char*)&dev->config->parent.fourcc);
	dma_image = eglCreateImageKHR(
					dev->egldisplay,
					EGL_NO_CONTEXT,
					EGL_LINUX_DMA_BUF_EXT,
					NULL,
					attrib_list);

	if(dma_image == EGL_NO_IMAGE_KHR)
	{
		err("segl: Image creation error %#x", eglGetError());
		return -1;
	}

	dev->buffers[dev->nbuffers].size = size;
	dev->buffers[dev->nbuffers].pitch = stride;
	dev->buffers[dev->nbuffers].dma_fd = dma_fd;
	dev->buffers[dev->nbuffers].dma_texture = dma_texture;
	dev->buffers[dev->nbuffers].dma_image = dma_image;
	dev->buffers[dev->nbuffers].textype = GL_TEXTURE_EXTERNAL_OES;
	dev->nbuffers++;

	return 0;
}

static int texturedma_get(EGL_t *dev, GLuint dma_texture)
{
	EGLImage image = eglCreateImage(dev->egldisplay, dev->eglcontext, EGL_GL_TEXTURE_2D, (void *)(long)dma_texture, NULL);

	if (image == EGL_NO_IMAGE)
		return -1;

#if 1
	PFNEGLEXPORTDMABUFIMAGEQUERYMESAPROC eglExportDMABUFImageQueryMESA =
		(PFNEGLEXPORTDMABUFIMAGEQUERYMESAPROC)eglGetProcAddress("eglExportDMABUFImageQueryMESA");
	PFNEGLEXPORTDMABUFIMAGEMESAPROC eglExportDMABUFImageMESA =
		(PFNEGLEXPORTDMABUFIMAGEMESAPROC)eglGetProcAddress("eglExportDMABUFImageMESA");
#endif
	int numplanes = 0;
	EGLint stride[5] = {0};
	EGLint offset[5] = {0};
	int fourcc = dev->config->parent.fourcc;
	int dma_buf[5] = {0};

	eglExportDMABUFImageQueryMESA(dev->egldisplay, image,
								&fourcc, &numplanes, NULL);
	if (numplanes < 5)
	{
		eglExportDMABUFImageMESA(dev->egldisplay, image, &dma_buf[0], &stride[0], &offset[0]);
	}
	if (dev->config->parent.fourcc && dev->config->parent.fourcc != fourcc)
		err("segl: requests %.4s, obtains %.4s", &dev->config->parent.fourcc, &fourcc);
	dev->config->parent.fourcc = fourcc;

	dev->buffers[dev->nbuffers].size = stride[0] * dev->config->parent.height;
	dev->buffers[dev->nbuffers].pitch = stride[0];
	dev->buffers[dev->nbuffers].dma_fd = dma_buf[0];
	dev->buffers[dev->nbuffers].dma_texture = dma_texture;
	dev->buffers[dev->nbuffers].dma_image = 0;
	dev->buffers[dev->nbuffers].textype = GL_TEXTURE_2D;
	return 0;
}

int segl_requestbuffer(EGL_t *dev, enum buf_type_e t, ...)
{
	va_list ap;
	va_start(ap, t);
	int ret = -1;
	switch (t)
	{
		case buf_type_dmabuf:
		{
			int ntargets = va_arg(ap, int);
			int *targets = va_arg(ap, int *);
			size_t size = va_arg(ap, size_t);
			for (int i = 0; i < ntargets; i++)
			{
				GLuint dma_texture = -1;
				dma_texture = texture_create(dev, GL_TEXTURE_EXTERNAL_OES);
				ret = texturedma_link(dev, dma_texture, targets[i], size);
				glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, dev->buffers[i].dma_image);
				eglDestroyImageKHR(dev->egldisplay, dev->buffers[i].dma_image);
				dev->buffers[i].dma_image = 0;
				if (ret)
					break;
			}
		}
		break;
		case buf_type_dmabuf | buf_type_master:
		{
			int *ntargets = va_arg(ap, int *);
			int **targets = va_arg(ap, int **);
			size_t *size = va_arg(ap, size_t *);
			if (ntargets != NULL)
				*ntargets = dev->nbuffers;
			if (targets != NULL)
			{
				*targets = calloc(dev->nbuffers, sizeof(int));
				for (int i = 0; i < dev->nbuffers; i++)
					(*targets)[i] = dev->buffers[i].dma_fd;
			}
			if (size != NULL)
				*size = dev->buffers[0].size;
			ret = (dev->nbuffers == 0);
		}
		break;
		default:
			err("segl: support only dmabuf");
			va_end(ap);
			return -1;
	}
	va_end(ap);
	return ret;
}

EGL_t *segl_duplicate(EGL_t *dev, EGLConfig_t **pconfig)
{
	EGL_t *dup = NULL;
	if (dev->type != device_transfer)
	{
		err("segl: device may not support duplication");
		return NULL;
	}
	dup = malloc(sizeof(*dup));
	if (!dup)
		return NULL;
	memcpy(dup, dev, sizeof(*dup));
	*pconfig = malloc(sizeof(*(dup->config)));
	memcpy(*pconfig, dev->config, sizeof(*(dup->config)));
	dup->config = *pconfig;
	dup->config->parent.fourcc = dup->config->transfer;
	dup->type = device_input;
	dev->dup = dup;

	GLuint glget = 0;
	glGetIntegerv(GL_MAX_RENDERBUFFER_SIZE, &glget);
	if (glget <= dup->config->parent.width)
		warn("segl: width to large max %d", glget);
	if (glget <= dup->config->parent.height)
		warn("segl: width to height max %d", glget);

#if 0
	int index = 4;
	GLint formats[] =
	{
		GL_ALPHA,
		GL_LUMINANCE,
		GL_LUMINANCE_ALPHA,
		GL_RGB,
		GL_RGBA,
	};
	GLenum type = GL_UNSIGNED_BYTE;
	switch (dup->config->parent.fourcc)
	{
		case FOURCC('R','G','B', 'P'):
			index = 3;
			type = GL_UNSIGNED_SHORT_5_6_5;
		break;
		default:
		break;
	}
	GLint intformat = formats[index];
	GLint format = formats[index];
#else
	const FourccFormat_t *fformat = fourcc_getformat(dup->config->parent.fourcc);
	GLint intformat = fformat->internal;
	GLint format = fformat->full;
	GLenum type = fformat->data;
#endif
dbg("%.4s %#x %#x %#x", &dup->config->parent.fourcc, intformat, format, type);
	/*  Framebuffer */
	glGenFramebuffers(1, &dup->fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, dup->fbo);
	for (int i = 0; i < MAX_BUFFERS; i++)
	{
		GLuint dma_texture = -1;
		dma_texture = texture_create(dev, GL_TEXTURE_2D);
#if 0
		glTexImage2D(GL_TEXTURE_2D, 0, formats[index],
				dup->config->parent.width, dup->config->parent.height, 0,
				formats[index], type, NULL);
#else
		glTexImage2D(GL_TEXTURE_2D, 0, fformat->internal,
				dup->config->parent.width, dup->config->parent.height, 0,
				fformat->full, fformat->data, NULL);
#endif
		if (texturedma_get(dup, dma_texture))
			break;
		dup->nbuffers++;
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
			GL_TEXTURE_2D, dup->buffers[i].dma_texture, 0);
	}
	/* Sanity check. */
	GLint ret = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	if (ret != GL_FRAMEBUFFER_COMPLETE)
	{
		err("segl: offscreen generator failed for %.4s", &dup->config->parent.fourcc);
		return NULL;
	}
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	return dup;
}

int segl_start(EGL_t *dev)
{
	if (dev->type == device_input)
		return 0;
	glViewport(0, 0, dev->config->parent.width, dev->config->parent.height);

	// initialize the first program with the input stream
	glprog_setintexture(dev->programs, dev->buffers[0].textype, dev->nbuffers, dev->buffers);

	eglMakeCurrent(dev->egldisplay, dev->eglsurface, dev->eglsurface, dev->eglcontext);
	dev->curbufferid = -1;
	return 0;
}

int segl_stop(EGL_t *dev)
{
	if (dev->type == device_input)
		return 0;
	eglMakeCurrent(dev->egldisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	return 0;
};

void segl_queue_output(EGL_t *dev, int id, size_t bytesused, GLuint fbo)
{
	glClearColor(0.5, 0.5, 0.5, 1.0);

	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glprog_run(dev->programs, id);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

int segl_queue(EGL_t *dev, int id, void *mem, size_t bytesused)
{
	if (dev->type == device_input)
	{
		dev->curbufferid = -1;
		return 0;
	}
	if (eglSwapBuffers(dev->egldisplay, dev->eglsurface) == EGL_FALSE)
		err("EGL swapbuffers error %m");
	// errno is set to EAGAIN after eglSwapBuffers
	errno = 0;
	if ((int)id > dev->nbuffers)
	{
		err("segl: unknown buffer id %d", id);
		return -1;
	}
	if (dev->curbufferid != -1)
	{
		err("segl: device %s not ready %d", dev->config->parent.name, dev->curbufferid);
		return -1;
	}

	segl_queue_output(dev, id, bytesused, 0);
	dev->curbufferid = id;
	if (dev->dup)
		dev->dup->curbufferid = dev->curbufferid;
	return dev->native->flush(dev->native_window);
}

int segl_dequeue(EGL_t *dev, void **mem, size_t *bytesused)
{
	int id = dev->curbufferid;
	dev->curbufferid = -1;
	if (dev->type == device_input)
	{
		*bytesused = dev->buffers[0].size;
		segl_queue_output(dev, id, *bytesused, dev->fbo);
		if (id == -1)
			errno = EAGAIN;
		return id;
	}
	glUseProgram(0);
	glBindTexture(dev->buffers[0].textype, 0);
	glBindTexture(GL_TEXTURE_2D, 0);
	if (dev->native->sync(dev->native_window) < 0)
		return -1;

	return id;
}

int segl_fd(EGL_t *dev)
{
	return dev->native->fd(dev->native_window);
}

void segl_destroy(EGL_t *dev)
{
	if (dev->type != device_input)
	{
		glprog_destroy(dev->programs);
		eglDestroySurface(dev->egldisplay, dev->eglsurface);
		eglDestroyContext(dev->egldisplay, dev->eglcontext);
		dev->native->destroy(dev->native_display);
	}
	free(dev);
}

DeviceConf_t * segl_createconfig()
{
	EGLConfig_t *devconfig = NULL;
	devconfig = calloc(1, sizeof(EGLConfig_t));
#ifdef HAVE_JANSSON
	devconfig->parent.ops.loadconfiguration = segl_loadjsonconfiguration;
#endif
	return (DeviceConf_t *)devconfig;
}

#ifdef HAVE_JANSSON
#include <jansson.h>

int segl_loadjsonsettings(EGL_t *dev, void *jconfig)
{
	return glprog_loadjsonsetting(dev->programs, jconfig);
}

int segl_loadjsonconfiguration(void *arg, void *entry)
{
	json_t *jconfig = entry;
	EGLConfig_t *config = (EGLConfig_t *)arg;

	json_t *jprograms = json_object_get(jconfig, "programs");
	glprog_loadjsonconfiguration(&config->programs, jprograms);
	json_t *native = json_object_get(jconfig, "native");
	if (native && json_is_string(native))
	{
		const char *value = json_string_value(native);
		config->native = value;
	}
	json_t *device = json_object_get(jconfig, "device");
	if (device && json_is_string(device))
	{
		const char *value = json_string_value(device);
		config->device = value;
	}
	json_t *definition = json_object_get(jconfig, "definition");
	scommon_loaddefinition(&config->parent, definition);
	if (definition && json_is_object(definition))
	{
		json_t *transfer = json_object_get(definition, "transfer");
		if (transfer && json_is_string(transfer))
		{
			const char *value = json_string_value(transfer);
			config->transfer = FOURCC(value[0], value[1], value[2], value[3]);
		}
	}
library_end:
	return 0;
}

typedef int (*definition_cb)(void *arg, ImageDefinition_t *image);
static int _egl_parsedefinition(EGL_t *dev, definition_cb cb, void *arg)
{
	return -1;
}

typedef struct _JSON_Cb_Arg_s _JSON_Cb_Arg_t;
struct _JSON_Cb_Arg_s
{
	json_t *array;
	void *dev;
	int all;
};

static int _egl_setjsondefinition(void *arg, ImageDefinition_t *image)
{
	_JSON_Cb_Arg_t *definitions = (_JSON_Cb_Arg_t *)arg;
	json_t *definition = json_object();
	json_object_set_new(definition, "width", json_integer(image->width));
	json_object_set_new(definition, "height", json_integer(image->height));
	if (image->stride)
		json_object_set_new(definition, "stride", json_integer(image->stride));
	json_object_set_new(definition, "fourcc", json_sprintf("%.4s", &image->fourcc));
	json_array_append_new(definitions->array, definition);
	return 0;
}

int segl_capabilities(EGL_t *dev, json_t *capabilities, int all)
{
	json_object_set_new(capabilities, "native", json_string(dev->native->name));
	_JSON_Cb_Arg_t definitions;
	definitions.array = json_array();
	definitions.all = all;
	definitions.dev = dev;
	_egl_parsedefinition(dev, _egl_setjsondefinition, &definitions);
	json_object_set_new(capabilities, "definition", definitions.array);
	return 0;
}

#endif //HAVE_JANSSON

FastVideoDevice_ops_t segl_ops = {
	.name = "gpu",
	.createconfig = segl_createconfig,
	.create = (FastVideoDevice_create_t)segl_create,
	.duplicate = (FastVideoDevice_duplicate_t)segl_duplicate,
	.loadsettings = (FastVideoDevice_loadsettings_t)NULL,
	.requestbuffer = (FastVideoDevice_requestbuffer_t)segl_requestbuffer,
	.eventfd = (FastVideoDevice_eventfd_t)segl_fd,
	.start = (FastVideoDevice_start_t)segl_start,
	.stop = (FastVideoDevice_stop_t)segl_stop,
	.dequeue = (FastVideoDevice_dequeue_t)segl_dequeue,
	.queue = (FastVideoDevice_queue_t)segl_queue,
	.destroy = (FastVideoDevice_destroy_t)segl_destroy,
};

#include <dlfcn.h>

static void __attribute__ ((constructor)) segl_init()
{
	fastvideodevice_ops_append_t _fastvideodevice_ops_append;
	void *hdl = dlopen(NULL, RTLD_NOW);
	_fastvideodevice_ops_append = dlsym(hdl, "fastvideodevice_ops_append");
	if (_fastvideodevice_ops_append)
	{
		_fastvideodevice_ops_append(&segl_ops);
	}
}
