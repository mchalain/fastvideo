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

#ifdef HAVE_GBM
extern EGLNative_t *eglnative_drm;
#endif
#ifdef HAVE_X11
extern EGLNative_t *eglnative_x11;
#endif

typedef struct EGL_s EGL_t;
struct EGL_s
{
	EGLConfig_t *config;
	EGLNative_t *native;
	EGLDisplay egldisplay;
	EGLConfig eglconfig;
	EGLContext eglcontext;
	EGLSurface eglsurface;
	EGLNativeDisplayType native_display;
	EGLNativeWindowType native_window;
	GLProgram_t *programs[MAX_PROGRANS];
	GLBuffer_t buffers[MAX_BUFFERS];
	int curbufferid;
	int nbuffers;
};

PFNEGLCREATEIMAGEKHRPROC eglCreateImageKHR = NULL;
PFNEGLDESTROYIMAGEKHRPROC eglDestroyImageKHR = NULL;
PFNEGLEXPORTDMABUFIMAGEQUERYMESAPROC eglExportDMABUFImageQueryMESA = NULL;
PFNEGLEXPORTDMABUFIMAGEMESAPROC eglExportDMABUFImageMESA = NULL;
PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES = NULL;
PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC glEGLImageTargetRenderbufferStorageOES = NULL;

EGL_t *segl_create(const char *devicename, EGLConfig_t *config)
{
	EGLNativeDisplayType ndisplay = EGL_DEFAULT_DISPLAY;
	EGLNative_t *natives[] = 
	{
#ifdef HAVE_GBM
		eglnative_drm,
#endif
#ifdef HAVE_X11
		eglnative_x11,
#endif
		NULL,
	};
	EGLNative_t *native = natives[0];

	if (config->native)
	{
		for (int i = 0; i < sizeof(natives) / sizeof(*natives); i++)
		{
			if (!strcmp(natives[i]->name, config->native))
			{
				native = natives[i];
				break;
			}
		}
	}
	ndisplay = native->display();
	EGLNativeWindowType nwindow = native->createwindow(ndisplay, config->parent.width, config->parent.height, "segl");

	EGLDisplay eglDisplay = eglGetDisplay(ndisplay);

	EGLint major, minor;
	if (!eglInitialize(eglDisplay, &major, &minor))
	{
		err("segl: failed to initialize");
		return NULL;
	}

	if (!eglBindAPI(EGL_OPENGL_ES_API))
	{
		err("segl: failed to bind api EGL_OPENGL_ES_API");
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
		err("segl: failed to choose config: %d", num_config);
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
		err("segl: failed to create context");
		return NULL;
	}

	EGLSurface eglSurface = eglCreateWindowSurface(eglDisplay, eglConfig, nwindow, NULL);
	if (eglSurface == EGL_NO_SURFACE)
	{
		err("segl: failed to create egl surface");
		return NULL;
	}
	eglMakeCurrent(eglDisplay, eglSurface, eglSurface, eglContext);

	GLint minswapinterval = 1;
	eglGetConfigAttrib(eglDisplay, eglConfig, EGL_MIN_SWAP_INTERVAL, &minswapinterval);
	dbg("segl: swap interval %d", minswapinterval);
	eglSwapInterval(eglDisplay, minswapinterval);

	EGL_t *dev = calloc(1, sizeof(*dev));
	dev->config = config;
	dev->native = native;
	dev->egldisplay = eglDisplay;
	dev->eglconfig = eglConfig;
	dev->eglcontext = eglContext;
	dev->eglsurface = eglSurface;

	int nbprg = 1;
	// The first program MUST use the default shaders
	// The texture is EXTERNAL_OES which is incompatible with FRAMEBUFFERS
	dev->programs[0] = glprog_create(NULL);
	for (int i = 0; i < MAX_PROGRANS - 1; i++)
	{
		EGLConfig_Program_t *prconfig = &config->programs[i];
		if (prconfig->vertex && prconfig->fragments[0])
		{
			dev->programs[nbprg] = glprog_create(prconfig);
			if (dev->programs[nbprg] == NULL)
				err("segl: program %d loading error", i);
			else
				nbprg++;
		}
	}
	eglCreateImageKHR = (void *) eglGetProcAddress("eglCreateImageKHR");
	if(eglCreateImageKHR == NULL)
	{
		return NULL;
	}
	eglDestroyImageKHR = (void *) eglGetProcAddress("eglDestroyImageKHR");
	if(eglDestroyImageKHR == NULL)
	{
		return NULL;
	}
	eglExportDMABUFImageQueryMESA = (void *) eglGetProcAddress("eglExportDMABUFImageQueryMESA");
	if(eglExportDMABUFImageQueryMESA == NULL)
	{
		return NULL;
	}
	eglExportDMABUFImageMESA = (void *) eglGetProcAddress("eglExportDMABUFImageMESA");
	if(eglExportDMABUFImageMESA == NULL)
	{
		return NULL;
	}
	glEGLImageTargetTexture2DOES = (void *) eglGetProcAddress("glEGLImageTargetTexture2DOES");
	if(glEGLImageTargetTexture2DOES == NULL)
	{
		return NULL;
	}

	for (int i = 0; dev->programs[i]; i++)
	{
		GLProgram_t *program = dev->programs[i];
		glprog_setup(program, config->parent.width, config->parent.height);
	}

	dev->native_window = nwindow;
	dev->native_display = ndisplay;
	dev->curbufferid = -1;
	return dev;
}

static int link_texturedma(EGL_t *dev, int dma_fd, size_t size)
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
		err("segl: Image creation error");
		return -1;
	}
	GLuint dma_texture;
	glGenTextures(1, &dma_texture);

#ifdef GLSLV300
	GLenum textype = GL_TEXTURE_2D;
#else
	GLenum textype = GL_TEXTURE_EXTERNAL_OES;
#endif
	glBindTexture(textype, dma_texture);
	glTexParameteri(textype, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(textype, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glEGLImageTargetTexture2DOES(textype, dma_image);
	eglDestroyImageKHR(dev->egldisplay, dma_image);

	dev->buffers[dev->nbuffers].size = size;
	dev->buffers[dev->nbuffers].pitch = stride;
	dev->buffers[dev->nbuffers].dma_fd = dma_fd;
	dev->buffers[dev->nbuffers].dma_texture = dma_texture;
	dev->buffers[dev->nbuffers].dma_image = NULL;
	dev->buffers[dev->nbuffers].textype = textype;
	dev->nbuffers++;

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
				ret = link_texturedma(dev, targets[i], size);
				if (ret)
					break;
			}
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

int segl_start(EGL_t *dev)
{
	glViewport(0, 0, dev->config->parent.width, dev->config->parent.height);

	// initialize the first program with the input stream
	glprog_setintexture(dev->programs[0], dev->buffers[0].textype, dev->nbuffers, dev->buffers);
	for (int i = 1; dev->programs[i]; i++)
	{
		GLBuffer_t *textures;
		textures = glprog_getouttexture(dev->programs[i - 1], dev->nbuffers);
		glprog_setintexture(dev->programs[i], GL_TEXTURE_2D, dev->nbuffers, textures);
	}

	eglMakeCurrent(dev->egldisplay, dev->eglsurface, dev->eglsurface, dev->eglcontext);
	dev->curbufferid = -1;
	return 0;
}

int segl_stop(EGL_t *dev)
{
	eglMakeCurrent(dev->egldisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	return 0;
};

int segl_queue(EGL_t *dev, int id, size_t bytesused)
{
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
		err("segl: device not ready %d", dev->curbufferid);
		return -1;
	}

	glClearColor(0.5, 0.5, 0.5, 1.0);
	glClear(GL_COLOR_BUFFER_BIT);

	for (int i = 0; dev->programs[i]; i++)
	{
		glprog_run(dev->programs[i], (int)id);
	}

	dev->curbufferid = (int)id;
	
	return dev->native->flush(dev->native_window);
}

int segl_dequeue(EGL_t *dev, void **mem, size_t *bytesused)
{
	int id = dev->curbufferid;
	dev->curbufferid = -1;
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
	for (int i = 0; dev->programs[i]; i++)
	{
		glprog_destroy(dev->programs[i]);
	}
	eglDestroySurface(dev->egldisplay, dev->eglsurface);
	eglDestroyContext(dev->egldisplay, dev->eglcontext);
	dev->native->destroy(dev->native_display);
	free(dev);
}

DeviceConf_t * segl_createconfig()
{
	EGLConfig_t *devconfig = NULL;
	devconfig = calloc(1, sizeof(EGLConfig_t));
	devconfig->parent.ops.loadconfiguration = segl_loadjsonconfiguration;
	return (DeviceConf_t *)devconfig;
}

#ifdef HAVE_JANSSON
#include <jansson.h>

int segl_loadjsonconfiguration(void *arg, void *entry)
{
	json_t *jconfig = entry;
	EGLConfig_t *config = (EGLConfig_t *)arg;

	void *prgconfig = &config->programs[0];
	json_t *programs = json_object_get(jconfig, "programs");
	if (programs && json_is_array(programs))
	{
		json_t *jfield = NULL;
		int i = 0;
		json_array_foreach(programs, i, jfield)
		{
			prgconfig = &config->programs[i];
			glprog_loadjsonconfiguration(prgconfig, jfield);
		}
	}
	else
		glprog_loadjsonconfiguration(prgconfig, jconfig);
	json_t *native = json_object_get(jconfig, "native");
	if (native && json_is_string(native))
	{
		const char *value = json_string_value(native);
		config->native = value;
	}
library_end:
	return 0;
}
#endif //EGL
