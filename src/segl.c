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
	{ .fourcc = FOURCC_RGBA, .internal = GL_RGBA8_OES, .full = GL_RGBA, .data = GL_UNSIGNED_BYTE       , .nplanes = 1, .stride_factor={sizeof(uint32_t),0,0,0}},
	{ .fourcc = FOURCC_AB24, .internal = GL_RGBA8_OES, .full = GL_RGBA, .data = GL_UNSIGNED_BYTE       , .nplanes = 1, .stride_factor={sizeof(uint32_t),0,0,0}},
	{ .fourcc = FOURCC_XB24, .internal = GL_RGBA8_OES, .full = GL_RGBA, .data = GL_UNSIGNED_BYTE       , .nplanes = 1, .stride_factor={sizeof(uint32_t),0,0,0}},
	{ .fourcc = FOURCC_AR24, .internal = GL_RGBA8_OES, .full = GL_RGBA, .data = GL_UNSIGNED_BYTE       , .nplanes = 1, .stride_factor={sizeof(uint32_t),0,0,0}},
	{ .fourcc = FOURCC_XR24, .internal = GL_RGBA8_OES, .full = GL_RGBA, .data = GL_UNSIGNED_BYTE       , .nplanes = 1, .stride_factor={sizeof(uint32_t),0,0,0}},
	{ .fourcc = FOURCC_RGBP, .internal = GL_RGB , .full = GL_RGB , .data = GL_UNSIGNED_SHORT_5_6_5, .nplanes = 1, .stride_factor={sizeof(uint16_t),0,0,0}},
	{ .fourcc = FOURCC_RG16, .internal = GL_RGB , .full = GL_RGB , .data = GL_UNSIGNED_SHORT_5_6_5, .nplanes = 1, .stride_factor={sizeof(uint16_t),0,0,0}},
	{ .fourcc = FOURCC_R8  , .internal = GL_RED_EXT, .full = GL_RED_EXT, .data = GL_UNSIGNED_BYTE , .nplanes = 1, .stride_factor={sizeof(uint8_t),0,0,0}},
	{ .fourcc = FOURCC_NV12, .internal = GL_RED_EXT, .full = GL_RED_EXT, .data = GL_UNSIGNED_BYTE , .nplanes = 1, .stride_factor={sizeof(uint8_t),0,0,0}},
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

EXT_API EGL_t *segl_create(const char *devicename, device_type_e type, EGLConfig_t *config)
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
				warn("segl: native %s", native->name);
				ndisplay = native->display(config);
				if (EGL_CAST(EGLint,ndisplay) != EGL_UNKNOWN)
					break;
			}
		}
	}

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

#ifdef DEBUG
	EGLConfig eglConfigs[20];
	eglGetConfigs(eglDisplay, eglConfigs, 20, &num_config);
	for (int i = 0; i < num_config; i++)
	{
		EGLint redsize;
		eglGetConfigAttrib(eglDisplay, eglConfigs[i], EGL_RED_SIZE, &redsize);
		EGLint greensize;
		eglGetConfigAttrib(eglDisplay, eglConfigs[i], EGL_GREEN_SIZE, &greensize);
		EGLint bluesize;
		eglGetConfigAttrib(eglDisplay, eglConfigs[i], EGL_BLUE_SIZE, &bluesize);
		EGLint alphasize;
		eglGetConfigAttrib(eglDisplay, eglConfigs[i], EGL_ALPHA_SIZE, &alphasize);
		EGLint texturetype;
		eglGetConfigAttrib(eglDisplay, eglConfigs[i], EGL_BIND_TO_TEXTURE_RGB, &texturetype);
		EGLint buffertype;
		eglGetConfigAttrib(eglDisplay, eglConfigs[i], EGL_COLOR_BUFFER_TYPE, &buffertype);
		EGLint surfacetype;
		eglGetConfigAttrib(eglDisplay, eglConfigs[i], EGL_SURFACE_TYPE, &surfacetype);
		dbg("segl: config[%.2d]\t%s %d/%d/%d/%d %s", i, surfacetype == EGL_PBUFFER_BIT?"pbuffer":"window", redsize, greensize, bluesize, alphasize, (buffertype== EGL_LUMINANCE_BUFFER)?"LUMINANCE":(texturetype)?"RGB":"RGBA");
	}
#endif
	EGLConfig eglConfig = EGL_NO_CONFIG_KHR;
	if (eglChooseConfig(eglDisplay, native->attributes(ndisplay), &eglConfig, 1, &num_config) == EGL_FALSE || num_config == 0)
	{
		err("segl: failed to choose config: %d (%#x)", num_config, eglGetError());
		native->destroy(ndisplay);
		return NULL;
	}

	static const EGLint context_attribs[] = {
		EGL_CONTEXT_MAJOR_VERSION, 2,
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

	EGLNativeWindowType nwindow = native->createwindow(ndisplay, config->parent.width, config->parent.height, "segl");

	EGLSurface eglSurface = NULL;
	if (nwindow != (EGLNativeWindowType)NULL)
	{
		eglSurface = eglCreateWindowSurface(eglDisplay, eglConfig, nwindow, NULL);
	}
	else
	{
		EGLint texture_format = EGL_TEXTURE_RGBA;
		EGLint texturergb = 0;
		eglGetConfigAttrib(eglDisplay, eglConfig, EGL_BIND_TO_TEXTURE_RGB, &texturergb);
		if (texturergb)
			texture_format = EGL_TEXTURE_RGB;
		warn("segl: surface on pbuffer");
		EGLint pbufferAttribs[] = {
			EGL_WIDTH, config->parent.width,
			EGL_HEIGHT, config->parent.height,
			EGL_TEXTURE_FORMAT, texture_format,
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
#if 0
	uint32_t width = dev->config->parent.width;
	uint32_t height = dev->config->parent.height;
	const FourccFormat_t *format = fourcc_getformat(dev->config->parent.fourcc);
	glTexImage2D(textype, 0, format->internal, width, height, 0, format->full, GL_UNSIGNED_BYTE, NULL);
#endif
	glTexParameteri(textype, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(textype, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri(textype, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(textype, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	return dma_texture;
}

static int texturedma_link(EGL_t *dev, GLuint dma_texture, int dma_fd, size_t size)
{
	uint32_t stride = size / dev->config->parent.height;
	uint32_t fourcc;
	switch (dev->config->parent.fourcc)
	{
		/**
		 * change multi-planar format to mono-planar grey format
		 */
		case FOURCC('I','4','2','0'):
		case FOURCC('N','V','2','1'):
		case FOURCC('N','V','1','2'):
		case FOURCC('Y','V','1','2'):
		case FOURCC('Y','V','1','6'):
			fourcc = FOURCC('G','R','E','Y');
			fourcc = FOURCC('R','8',' ',' ');
			stride = dev->config->parent.width;
			size = dev->config->parent.width * dev->config->parent.height;
		break;
		default:
			fourcc = dev->config->parent.fourcc;
	}
	EGLImageKHR dma_image;
	GLint attrib_list[] = {
		EGL_WIDTH, dev->config->parent.width,
		EGL_HEIGHT, dev->config->parent.height,
		EGL_LINUX_DRM_FOURCC_EXT, fourcc,
		EGL_DMA_BUF_PLANE0_FD_EXT, dma_fd,
		EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
		EGL_DMA_BUF_PLANE0_PITCH_EXT, stride,
		EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, (uint32_t)(dev->config->parent.modifiers & ((((uint64_t)1) << 33) - 1)),
		EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, (uint32_t)((dev->config->parent.modifiers>>32) & ((((uint64_t)1) << 33) - 1)),
		EGL_NONE
	};
	dbg("segl: create image for dma %d : %dx%d %u %.4s", dma_fd, dev->config->parent.width, dev->config->parent.height, stride, (char*)&fourcc);
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

static int texturemem_link(EGL_t *dev, GLuint texture, void *mem, size_t size)
{
	uint32_t stride = size / dev->config->parent.height;
	EGLImageKHR image;
	dbg("segl: create image for mmap %p : %dx%d %u %.4s", mem, dev->config->parent.width, dev->config->parent.height, stride, (char*)&dev->config->parent.fourcc);
	image = eglCreateImageKHR(
					dev->egldisplay,
					dev->eglcontext,
					EGL_GL_TEXTURE_2D_KHR,
					(EGLClientBuffer)(long)texture,
					mem);

	if(image == EGL_NO_IMAGE_KHR)
	{
		err("segl: Image creation error %#X", eglGetError());
		return -1;
	}

	dev->buffers[dev->nbuffers].size = size;
	dev->buffers[dev->nbuffers].pitch = stride;
	dev->buffers[dev->nbuffers].memory = mem;
	dev->buffers[dev->nbuffers].dma_texture = texture;
	dev->buffers[dev->nbuffers].dma_image = image;
	dev->buffers[dev->nbuffers].textype = GL_TEXTURE_EXTERNAL_OES;
	dev->nbuffers++;

	return 0;
}

static int segl_requestbuffer_output(EGL_t *dev, enum buf_type_e t, va_list ap)
{
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
		case buf_type_memory:
		{
			int ntargets = va_arg(ap, int);
			void **targets = va_arg(ap, void **);
			size_t size = va_arg(ap, size_t);
			for (int i = 0; i < ntargets; i++)
			{
				GLuint texture = -1;
				//GLuint textype = GL_TEXTURE_EXTERNAL_OES;
				GLuint textype = GL_TEXTURE_2D;
				texture = texture_create(dev, textype);
				ret = texturemem_link(dev, texture, targets[i], size);
//				glEGLImageTargetTexture2DOES(textype, dev->buffers[i].dma_image);
//				eglDestroyImageKHR(dev->egldisplay, dev->buffers[i].dma_image);
				dev->buffers[i].dma_image = 0;
				if (ret)
					break;
			}
		}
		break;
	}
	return ret;
}

static int texturedma_get(EGL_t *dev, int id)
{
/// the both have the same result
#if 0
	GLint *attributes = NULL;

	EGLImage image = eglCreateImageKHR(dev->egldisplay, dev->eglcontext, EGL_GL_TEXTURE_2D,
		(void *)(long)dev->buffers[id].dma_texture, attributes);
#else
	const EGLAttrib tattributes[] = {
		EGL_IMAGE_PRESERVED, EGL_TRUE,
		EGL_NONE,
	};
	const EGLAttrib *attributes = tattributes;

	EGLImage image = eglCreateImage(dev->egldisplay, dev->eglcontext, EGL_GL_TEXTURE_2D,
		(void *)(long)dev->buffers[id].dma_texture, attributes);
#endif

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
	uint64_t modifiers = 0;

	eglExportDMABUFImageQueryMESA(dev->egldisplay, image,
								&fourcc, &numplanes, &modifiers);
	if (numplanes < 5)
	{
		eglExportDMABUFImageMESA(dev->egldisplay, image, &dma_buf[0], &stride[0], &offset[0]);
	}
	if (dev->config->parent.fourcc && dev->config->parent.fourcc != fourcc)
		err("segl: requests %.4s, obtains %.4s", &dev->config->parent.fourcc, &fourcc);
	dev->config->parent.fourcc = fourcc;

	dev->buffers[id].dma_image = image;
	dev->buffers[id].size = stride[0] * dev->config->parent.height;
	dev->buffers[id].pitch = stride[0];
	dev->buffers[id].modifiers = modifiers;
	if (modifiers != dev->config->parent.modifiers)
		err("segl: format modifier present but not set (%d/%d)", modifiers, dev->config->parent.modifiers);

	return dma_buf[0];
}

static void *texturemem_get(EGL_t *dev, int id)
{
	void *mem = calloc(1, dev->buffers[id].size);

	return mem;
}

static int segl_requestbuffer_input(EGL_t *dev, enum buf_type_e t, va_list ap)
{
	int ret = -1;
	switch (t)
	{
		case buf_type_dmabuf | buf_type_master:
		{
			int *ntargets = va_arg(ap, int *);
			int **targets = va_arg(ap, int **);
			size_t *size = va_arg(ap, size_t *);
			if (targets != NULL)
			{
				*targets = calloc(dev->nbuffers, sizeof(int));
				for (int i = 0; i < dev->nbuffers; i++)
				{
					int dma_fd = dev->buffers[i].dma_fd;
					if (dma_fd == 0)
					{
						dma_fd = texturedma_get(dev, i);
						if (dma_fd <= 0)
						{
							err("segl: export dma_buf error %d", dma_fd);
							dev->nbuffers = i;
							break;
						}
						dev->buffers[i].dma_fd = dma_fd;
						eglDestroyImageKHR(dev->egldisplay, dev->buffers[i].dma_image);
						dev->buffers[i].dma_image = 0;
						dev->buffers[i].modifiers = dev->config->parent.modifiers;
					}
					(*targets)[i] = dma_fd;
					dbg("segl: export dmabuffer[%d]: %d %lu", i, dma_fd, dev->buffers[i].size);
				}
			}
			if (ntargets != NULL)
				*ntargets = dev->nbuffers;
			if (size != NULL)
				*size = dev->buffers[0].size;
			ret = (dev->nbuffers == 0);
		}
		break;
		case buf_type_memory:
		{
			int ntargets = va_arg(ap, int);
			void **targets = va_arg(ap, void **);
			size_t size = va_arg(ap, size_t);
			for (int i = 0; i < ntargets && i < dev->nbuffers; i++)
			{
				if (dev->buffers[i].memory)
				{
					free(dev->buffers[i].memory);
					dev->buffers[i].memory = NULL;
				}
				if (dev->buffers[i].size > size)
					err("segl: output buffer is too small for the image");
				dev->buffers[i].memory = targets[i];
				if (size > 0)
					dev->buffers[i].size = size;
				dbg("segl: push data into buffer %p (%lu)", dev->buffers[i].memory, dev->buffers[i].size);
			}
			ret = 0;
		}
		break;
		case buf_type_memory | buf_type_master:
		{
			int *ntargets = va_arg(ap, int *);
			void ***targets = va_arg(ap, void ***);
			size_t *size = va_arg(ap, size_t *);
			if (targets != NULL)
			{
				*targets = calloc(dev->nbuffers, sizeof(int));
				for (int i = 0; i < dev->nbuffers; i++)
				{
					void *mem = dev->buffers[i].memory;
					if (mem == NULL)
					{
						mem = texturemem_get(dev, i);
						if (mem == NULL)
						{
							err("segl: export dma_buf error %p", mem);
							dev->nbuffers = i;
							break;
						}
						dev->buffers[i].memory = mem;
						eglDestroyImageKHR(dev->egldisplay, dev->buffers[i].dma_image);
						dev->buffers[i].dma_image = 0;
					}
					(*targets)[i] = mem;
					dbg("segl: export dmabuffer[%d]: %p %lu", i, mem, dev->buffers[i].size);
				}
			}
			if (ntargets != NULL)
				*ntargets = dev->nbuffers;
			if (size != NULL)
				*size = dev->buffers[0].size;
			ret = (dev->nbuffers == 0);
		}
		break;
	}
	return ret;
}

EXT_API int segl_requestbuffer(EGL_t *dev, enum buf_type_e t, ...)
{
	int ret = -1;
	va_list ap;
	va_start(ap, t);
	if (dev->type != device_input)
		ret = segl_requestbuffer_output(dev, t, ap);
	else
		ret = segl_requestbuffer_input(dev, t, ap);
	va_end(ap);
	return ret;
}

EXT_API EGL_t *segl_duplicate(EGL_t *dev, EGLConfig_t **pconfig)
{
	EGL_t *dup = NULL;
	if (dev->type != device_transfer)
	{
		err("segl: device may not support duplication");
		return NULL;
	}
	dev->type = device_output;
	dup = malloc(sizeof(*dup));
	if (!dup)
		return NULL;
	memcpy(dup, dev, sizeof(*dup));
	*pconfig = malloc(sizeof(*(dup->config)));
	memcpy(*pconfig, dev->config, sizeof(*(dup->config)));
	dup->config = *pconfig;
	dup->config->parent.fourcc = dup->config->transfer;
	dup->config->parent.modifiers = dup->config->transfer_modifiers;
	dup->type = device_input;
	dev->dup = dup;

	GLuint glget = 0;
	glGetIntegerv(GL_MAX_RENDERBUFFER_SIZE, &glget);
	if (glget <= dup->config->parent.width)
		warn("segl: width to large max %d", glget);
	if (glget <= dup->config->parent.height)
		warn("segl: width to height max %d", glget);

	const FourccFormat_t *fformat = fourcc_getformat(dup->config->parent.fourcc);

	/*  Framebuffer */
	glGenFramebuffers(1, &dup->fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, dup->fbo);
	for (int i = 0; i < MAX_BUFFERS; i++, dup->nbuffers++)
	{
		GLuint dma_texture = 0;
		/// glFramebufferTexture2D support only GL_TEXTURE_2D
		dup->buffers[i].textype = GL_TEXTURE_2D;
		dma_texture = texture_create(dup, dup->buffers[i].textype);
		if (dma_texture == 0)
			break;
		dup->buffers[i].dma_texture = dma_texture;
		dup->buffers[i].size = dup->config->parent.width;
		dup->buffers[i].size *= dup->config->parent.height;
		dup->buffers[i].size *= fformat->stride_factor[0];

		glTexImage2D(dup->buffers[i].textype, 0, fformat->internal,
				dup->config->parent.width, dup->config->parent.height, 0,
				fformat->full, fformat->data, NULL);

		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
			dup->buffers[i].textype, dup->buffers[i].dma_texture, 0);
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

EXT_API int segl_start(EGL_t *dev)
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

EXT_API int segl_stop(EGL_t *dev)
{
	if (dev->type == device_input)
		return 0;
	eglMakeCurrent(dev->egldisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	return 0;
};

static void segl_queue_output(EGL_t *dev, int id, size_t bytesused, GLuint fbo, int flags)
{
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);

#ifdef GLESV300
	uint32_t width = dev->config->parent.width;
	uint32_t height = dev->config->parent.height;

	glPixelStorei(GL_PACK_ROW_LENGTH, width);
	glPixelStorei(GL_PACK_IMAGE_HEIGHT, height);
#endif
	glPixelStorei(GL_PACK_ALIGNMENT, 4);

	glClearColor(0.5, 0.5, 0.5, 1.0);

	glprog_run(dev->programs, id);
}

EXT_API int segl_queue(EGL_t *dev, int id, void *mem, size_t bytesused, int flags)
{
	errno = 0;
	uint32_t width = dev->config->parent.width;
	uint32_t height = dev->config->parent.height;

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

#if 0
	if (dev->buffers[id].dma_fd == 0)
	{
		glTexSubImage2D(GL_TEXTURE_2D, 0, GL_RGB, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, mem);
	}
#endif
	dev->buffers[id].modifiers = 0;
	if (flags & FB_FLAGS_MODIFIER)
		dev->buffers[id].modifiers = dev->config->parent.modifiers;
	segl_queue_output(dev, id, bytesused, 0, flags);
	dev->curbufferid = id;
	int ret = dev->native->flush(dev->native_window);
	if (dev->dup)
	{
		dev->dup->curbufferid = dev->curbufferid;
		segl_queue_output(dev->dup, id, dev->dup->buffers[id].size, dev->dup->fbo, 0);
#if 1
		glBindTexture(dev->dup->buffers[id].textype, dev->dup->buffers[id].dma_texture);
		glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 0, 0, width, height, 0);
#endif
	}
	return ret;
}

EXT_API int segl_dequeue(EGL_t *dev, void **mem, size_t *bytesused, int *flags)
{
	errno = 0;
	int id = dev->curbufferid;
	dev->curbufferid = -1;
	if (dev->type == device_input)
	{
		if (id == -1)
		{
			errno = EAGAIN;
			return id;
		}
		if (flags && dev->buffers[id].modifiers)
			*flags |= FB_FLAGS_MODIFIER;
		return id;
	}
	glUseProgram(0);
	glBindTexture(dev->buffers[0].textype, 0);
	glBindTexture(GL_TEXTURE_2D, 0);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	if (dev->native->sync(dev->native_window) < 0)
		return -1;

	return id;
}

EXT_API int segl_fd(EGL_t *dev, int writer)
{
	if (writer && dev->curbufferid == -1)
		return 0;
	return dev->native->fd(dev->native_window);
}

EXT_API void segl_destroy(EGL_t *dev)
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
		json_t *modifiers = json_object_get(definition, "transfer_modifiers");
		if (modifiers && json_is_integer(modifiers))
		{
			uint64_t value = json_integer_value(modifiers);
			config->transfer_modifiers = value;
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
