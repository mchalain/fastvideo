#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <errno.h>
#include <inttypes.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include "segl.h"
#include "log.h"
#include "sdmabuf.h"

#define TEST_TEXTURE_FORMAT 0

#define SEGL_NOPROGRAM 0x01

#define segl_dbg(...)

EXT_API int segl_start(EGL_t *dev);
EXT_API int segl_stop(EGL_t *dev);
EXT_API int segl_queue(EGL_t *dev, int id, void *mem, size_t bytesused, int flags);
EXT_API int segl_dequeue(EGL_t *dev, void **mem, size_t *bytesused, int *flags);

static const EGLNative_t *_segl_get_native(const char *name);

const EGLNative_t * _natives[5] = {0};

void segl_native_append(EGLNative_t *native)
{
	int i = 0;
	for (; _natives[i] && i < sizeof(_natives) / sizeof(*_natives); i++);
	if (i < sizeof(_natives)/sizeof(*_natives))
		_natives[i] = native;
}

const EGLExport_t * _exports[5] = {0};

void segl_export_append(EGLExport_t *export)
{
	int i = 0;
	for (; _exports[i] && i < sizeof(_exports) / sizeof(*_exports); i++);
	if (i < sizeof(_exports)/sizeof(*_exports))
		_exports[i] = export;
}

const EGLProg_ops_t *_prog_ops[5] = {0};

void segl_program_ops_append(EGLProg_ops_t *prog_ops)
{
	int i = 0;
	for (; _prog_ops[i] && i < sizeof(_prog_ops) / sizeof(*_prog_ops); i++);
	if (i < sizeof(_prog_ops)/sizeof(*_prog_ops))
		_prog_ops[i] = prog_ops;
}

typedef struct EGL_s EGL_t;
struct EGL_s
{
	EGLConfig_t *config;
	const EGLNative_t *native;
	device_type_e type;
	EGLDisplay egldisplay;
	EGLConfig eglconfig;
	EGLContext eglcontext;
	EGLSurface eglsurface;
	GLuint fbo;
	EGL_t *dup;
	const EGLExport_t *export;
	void *export_ctx;
	EGLNativeDisplayType native_display;
	EGLNativeWindowType native_window;
	const EGLProg_ops_t *program_ops;
	GLProgram_t *programs;
	GLBuffer_t buffers[MAX_BUFFERS];
	int curbufferid;
	int nbuffers;
};

#ifndef EGL_KHR_image
#error "this version of EGL doesn't support KHR Image"
#endif
#ifndef GL_OES_EGL_image
#error "this version of GLES doesn't support EGL Image"
#endif
#ifndef EGL_EGLEXT_PROTOTYPES
#if defined(EGL_KHR_image)
static PFNEGLCREATEIMAGEKHRPROC eglCreateImageKHR = NULL;
static PFNEGLDESTROYIMAGEKHRPROC eglDestroyImageKHR = NULL;
#endif
#if EGL_EXT_image_dma_buf_import_modifiers
static PFNEGLQUERYDMABUFFORMATSEXTPROC eglQueryDmaBufFormatsEXT = NULL;
static PFNEGLQUERYDMABUFMODIFIERSEXTPROC eglQueryDmaBufModifiersEXT = NULL;
#endif

static int _egl_initprototypes(void)
{
	eglCreateImageKHR = (void *) eglGetProcAddress("eglCreateImageKHR");
	if(eglCreateImageKHR == NULL)
	{
		return -1;
	}
	eglDestroyImageKHR = (void *) eglGetProcAddress("eglDestroyImageKHR");
	if(eglDestroyImageKHR == NULL)
	{
		return -1;
	}
	eglQueryDmaBufFormatsEXT = (void *) eglGetProcAddress("eglQueryDmaBufFormatsEXT");
	if(eglQueryDmaBufFormatsEXT == NULL)
	{
		return -1;
	}
	eglQueryDmaBufModifiersEXT = (void *) eglGetProcAddress("eglQueryDmaBufModifiersEXT");
	if(eglQueryDmaBufModifiersEXT == NULL)
	{
		return -1;
	}
	return 0;
}
#else
#define _egl_initprototypes(...)
#endif


static FourccFormat_t _FourccFormats[] =
{
	{ .fourcc = FOURCC_RGBA, .internal = GL_RGBA8_OES, .full = GL_RGBA, .data = GL_UNSIGNED_BYTE       , .nplanes = 1, .stride_factor={sizeof(uint32_t),0,0,0}},
	{ .fourcc = FOURCC_AB24, .internal = GL_RGBA8_OES, .full = GL_RGBA, .data = GL_UNSIGNED_BYTE       , .nplanes = 1, .stride_factor={sizeof(uint32_t),0,0,0}},
	{ .fourcc = FOURCC_XB24, .internal = GL_RGBA8_OES, .full = GL_RGBA, .data = GL_UNSIGNED_BYTE       , .nplanes = 1, .stride_factor={sizeof(uint32_t),0,0,0}},
	{ .fourcc = FOURCC_AR24, .internal = GL_RGBA8_OES, .full = GL_RGBA, .data = GL_UNSIGNED_BYTE       , .nplanes = 1, .stride_factor={sizeof(uint32_t),0,0,0}},
	{ .fourcc = FOURCC_XR24, .internal = GL_RGBA8_OES, .full = GL_RGBA, .data = GL_UNSIGNED_BYTE       , .nplanes = 1, .stride_factor={sizeof(uint32_t),0,0,0}},
	{ .fourcc = FOURCC_RGBP, .internal = GL_RGB565   , .full = GL_RGB , .data = GL_UNSIGNED_SHORT_5_6_5, .nplanes = 1, .stride_factor={sizeof(uint16_t),0,0,0}},
	{ .fourcc = FOURCC_RG16, .internal = GL_RGB565   , .full = GL_RGB , .data = GL_UNSIGNED_SHORT_5_6_5, .nplanes = 1, .stride_factor={sizeof(uint16_t),0,0,0}},
	{ .fourcc = FOURCC_R8  , .internal = GL_R8_EXT   , .full = GL_RED_EXT, .data = GL_UNSIGNED_BYTE    , .nplanes = 1, .stride_factor={sizeof(uint8_t) ,0,0,0}},
	{ .fourcc = FOURCC_NV12, .internal = GL_R8_EXT   , .full = GL_RED_EXT, .data = GL_UNSIGNED_BYTE    , .nplanes = 1, .stride_factor={sizeof(uint8_t) ,0,0,0}},
//	{ .fourcc = FOURCC_NV12, .internal = GL_LUMINANCE8_OES, .full = GL_LUMINANCE, .data = GL_UNSIGNED_BYTE , .nplanes = 1, .stride_factor={sizeof(uint8_t),0,0,0}},
	{ .fourcc = FOURCC_YUYV, .internal = GL_RGBA     , .full = GL_RGBA, .data = GL_UNSIGNED_BYTE       , .nplanes = 1, .stride_factor={sizeof(uint32_t),0,0,0}},
};

const FourccFormat_t *fourcc_getformat(uint32_t fourcc)
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

int _egl_hasextension(EGLDisplay eglDisplay, const char *extension)
{
	const char *extensions = eglQueryString(eglDisplay, EGL_EXTENSIONS);
	return (strstr(extensions, extension) != NULL);
}

int segl_hasextension(EGL_t *dev, const char *extension)
{
	return _egl_hasextension(dev->egldisplay, extension);
}

#ifdef DEBUG
static int _egl_configinfo(EGLDisplay eglDisplay, EGLConfig eglConfig)
{
	if (eglConfig == EGL_NO_CONFIG_KHR)
		return -1;
	EGLint id;
	eglGetConfigAttrib(eglDisplay, eglConfig, EGL_CONFIG_ID, &id);
	EGLint redsize;
	eglGetConfigAttrib(eglDisplay, eglConfig, EGL_RED_SIZE, &redsize);
	EGLint greensize;
	eglGetConfigAttrib(eglDisplay, eglConfig, EGL_GREEN_SIZE, &greensize);
	EGLint bluesize;
	eglGetConfigAttrib(eglDisplay, eglConfig, EGL_BLUE_SIZE, &bluesize);
	EGLint alphasize;
	eglGetConfigAttrib(eglDisplay, eglConfig, EGL_ALPHA_SIZE, &alphasize);
	EGLint texturetype;
	eglGetConfigAttrib(eglDisplay, eglConfig, EGL_BIND_TO_TEXTURE_RGB, &texturetype);
	EGLint buffertype;
	eglGetConfigAttrib(eglDisplay, eglConfig, EGL_COLOR_BUFFER_TYPE, &buffertype);
	EGLint surfacetype;
	eglGetConfigAttrib(eglDisplay, eglConfig, EGL_SURFACE_TYPE, &surfacetype);
	segl_dbg("segl: config[%.2d]\t%s %d/%d/%d/%d %s", id, (surfacetype & EGL_PBUFFER_BIT)?"pbuffer":"window", redsize, greensize, bluesize, alphasize, (buffertype == EGL_LUMINANCE_BUFFER)?"LUMINANCE":(texturetype)?"RGB":"RGBA");
	segl_dbg("\t EGL_SURFACE_TYPE %#x", surfacetype);
	EGLint value;
	eglGetConfigAttrib(eglDisplay, eglConfig, EGL_DEPTH_SIZE, &value);
	segl_dbg("\t EGL_DEPTH_SIZE %#x", value);
	eglGetConfigAttrib(eglDisplay, eglConfig, EGL_CONFIG_CAVEAT, &value);
	segl_dbg("\t EGL_CONFIG_CAVEAT %#x", value);
	eglGetConfigAttrib(eglDisplay, eglConfig, EGL_NATIVE_RENDERABLE, &value);
	segl_dbg("\t EGL_NATIVE_RENDERABLE %#x", value);
	eglGetConfigAttrib(eglDisplay, eglConfig, EGL_NATIVE_VISUAL_ID, &value);
	segl_dbg("\t EGL_NATIVE_VISUAL_ID %.4s", (value)?(char*)&value:"none");
	eglGetConfigAttrib(eglDisplay, eglConfig, EGL_NATIVE_VISUAL_TYPE, &value);
	segl_dbg("\t EGL_NATIVE_VISUAL_TYPE %#x", value);
	eglGetConfigAttrib(eglDisplay, eglConfig, EGL_SAMPLE_BUFFERS, &value);
	segl_dbg("\t EGL_SAMPLE_BUFFERS %#x", value);
	eglGetConfigAttrib(eglDisplay, eglConfig, EGL_SAMPLES, &value);
	segl_dbg("\t EGL_SAMPLES %#x", value);
	eglGetConfigAttrib(eglDisplay, eglConfig, EGL_CONFORMANT, &value);
	segl_dbg("\t EGL_CONFORMANT %#x", value);
	eglGetConfigAttrib(eglDisplay, eglConfig, EGL_LEVEL, &value);
	segl_dbg("\t EGL_LEVEL %#x", value);
	eglGetConfigAttrib(eglDisplay, eglConfig, EGL_MATCH_NATIVE_PIXMAP, &value);
	segl_dbg("\t EGL_MATCH_NATIVE_PIXMAP %#x", value);
	return 0;
}
#endif

static EGL_t *_egl_create(const char *devicename, device_type_e type, EGLConfig_t *config)
{
	uint32_t width = config->parent.width;
	uint32_t height = config->parent.height;

	const EGLProg_ops_t *prog_ops = _prog_ops[0];
	for (int i = 0; config->programs && i < sizeof(_prog_ops)/sizeof(*_prog_ops); i++)
	{
		if (_prog_ops[i] && strcmp(_prog_ops[i]->name, config->programs->type))
		{
			prog_ops = _prog_ops[i];
			break;
		}
	}
	GLProgram_t *programs = NULL;
	if (type == device_control)
		programs = prog_ops->create_controler(config->programs, width, height);
	else
		programs = prog_ops->create(config->programs, width, height);
	if (programs == NULL)
		return NULL;

	EGL_t *dev = calloc(1, sizeof(*dev));
	dev->config = config;
	dev->program_ops = prog_ops;
	dev->programs = programs;

	dev->curbufferid = -1;
	dev->type = type;
	warn("segl: create device %ux%u %.4s", width, height, (char *)&dev->config->parent.fourcc);
	return dev;
}

EXT_API EGL_t *segl_create(const char *devicename, device_type_e type, EGLConfig_t *config)
{
	if (type == device_control)
		return _egl_create(devicename, type, config);
	if (type != device_output && type != device_transfer)
	{
		err("segl: %s bad device type", config->parent.name);
		return NULL;
	}
	config->type = type;
	EGLNativeDisplayType ndisplay = EGL_DEFAULT_DISPLAY;

	uint32_t width = config->parent.width;
	uint32_t height = config->parent.height;

	const EGLNative_t * native = config->native;
	if (type == device_transfer && config->export && config->export->native)
	{
		warn("segl: native changed from %s to %s", native->name, config->export->native);
		native = _segl_get_native(config->export->native);
	}

	if (native == NULL)
	{
		err("segl: native is not available");
		return NULL;
	}
	ndisplay = native->display(config);
	if (EGL_CAST(EGLint,ndisplay) == EGL_UNKNOWN)
		return NULL;

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

	EGLint num_configs;
	eglGetConfigs(eglDisplay, NULL, 0, &num_configs);

#if 0
	// the function eglQueryDmaBufModifiersEXT looks bugged
	uint32_t fourccs[4] = {0};
	int numfourccs = 0;
	eglQueryDmaBufFormatsEXT(eglDisplay, 4, &fourccs[0], &numfourccs);
	for (int i = 0; i < 4 && i < numfourccs; i++)
	{
		dbg("segl: %.4s supported with modifiers:", &fourccs[i]);
		uint64_t modifiers[4] = {0};
		EGLint nummodifiers = 0;
		EGLBoolean external = 0;
		eglQueryDmaBufModifiersEXT(eglDisplay, fourccs[i], 0, NULL, &external, &nummodifiers);
		if (nummodifiers > (sizeof(modifiers)/sizeof(*modifiers)))
			nummodifiers = (sizeof(modifiers)/sizeof(*modifiers));
		eglQueryDmaBufModifiersEXT(eglDisplay, fourccs[i], nummodifiers, modifiers, &external, &nummodifiers);
		for (int j = 0; j < 4 && j < nummodifiers;j++)
		{
			dbg("\t%#llx %s", modifiers[j], external?"ext":"");
		}
	}
#endif
	EGLConfig eglConfigs[20];
	if (num_configs > 20)
	{
		dbg("segl: choose %d/%d configs", 20, num_configs);
		num_configs = 20;
	}
#ifdef DEBUG
	eglGetConfigs(eglDisplay, eglConfigs, sizeof(eglConfigs)/ sizeof(*eglConfigs), &num_configs);
	for (int i = 0; i < num_configs; i++)
	{
		_egl_configinfo(eglDisplay, eglConfigs[i]);
	}
#endif
	if (eglChooseConfig(eglDisplay, native->attributes(ndisplay), eglConfigs, num_configs, &num_configs) == EGL_FALSE || num_configs == 0)
	{
		err("segl: failed to choose config: %d (%#x)", num_configs, eglGetError());
		native->destroy(ndisplay);
		return NULL;
	}
	dbg("segl: found %d configs", num_configs);
	int configid = 0;
#ifdef DEBUG
	for (int i = 0; i < num_configs; i++)
	{
		_egl_configinfo(eglDisplay, eglConfigs[i]);
	}
#endif

	static const EGLint context_attribs[] = {
		EGL_CONTEXT_MAJOR_VERSION, 2,
		EGL_NONE
	};
	EGLContext eglContext;
	eglContext = eglCreateContext(eglDisplay, eglConfigs[configid], EGL_NO_CONTEXT, context_attribs);
	if (eglContext == NULL)
	{
		err("segl: failed to create context (%#x)", eglGetError());
		native->destroy(ndisplay);
		return NULL;
	}

	EGLNativeWindowType nwindow = native->createwindow(ndisplay, width, height, "segl");

	EGLSurface eglSurface = NULL;
	if (nwindow != (EGLNativeWindowType)NULL)
	{
		EGLint attribs[] = {
			//EGL_GL_COLORSPACE,  EGL_GL_COLORSPACE_LINEAR,
			EGL_NONE,
		};
		eglSurface = eglCreateWindowSurface(eglDisplay, eglConfigs[configid], nwindow, attribs);
	}
	else
	{
		EGLint texture_format = EGL_TEXTURE_RGBA;
		EGLint texturergb = 0;
		eglGetConfigAttrib(eglDisplay, eglConfigs[configid], EGL_BIND_TO_TEXTURE_RGB, &texturergb);
		if (texturergb)
			texture_format = EGL_TEXTURE_RGB;
		warn("segl: surface on pbuffer");
		EGLint attribs[] = {
			EGL_WIDTH, width,
			EGL_HEIGHT, height,
			EGL_TEXTURE_FORMAT, texture_format,
			EGL_TEXTURE_TARGET, EGL_TEXTURE_2D,
			//EGL_LARGEST_PBUFFER, EGL_TRUE, // no visible change
			EGL_NONE,
		};

		eglSurface = eglCreatePbufferSurface(eglDisplay, eglConfigs[configid], attribs);
	}
	if (eglSurface == EGL_NO_SURFACE)
	{
		err("segl: failed to create egl surface (%#x)", eglGetError());
		native->destroy(ndisplay);
		return NULL;
	}
	eglMakeCurrent(eglDisplay, eglSurface, eglSurface, eglContext);

	GLint minswapinterval = 1;
	eglGetConfigAttrib(eglDisplay, eglConfigs[configid], EGL_MIN_SWAP_INTERVAL, &minswapinterval);
	dbg("segl: swap interval %d", minswapinterval);
	eglSwapInterval(eglDisplay, minswapinterval);

	EGL_t *dev = _egl_create(devicename, type, config);
	if (dev)
	{
		dev->egldisplay = eglDisplay;
		dev->eglconfig = eglConfigs[configid];
		dev->eglcontext = eglContext;
		dev->eglsurface = eglSurface;

		dev->native = native;
		dev->native_window = nwindow;
		dev->native_display = ndisplay;
		dev->curbufferid = -1;
	}
	return dev;
}

static int texture_fromdma(EGL_t *dev, GLBuffer_t *buffer, int dma_fd, size_t size)
{
	GL_Buffer_t *glbuffer = dev->program_ops->buffer.create(dev->programs, dev->config->parent.fourcc);

	uint32_t stride = dev->config->parent.stride;
	if (stride == 0)
		stride = size / dev->config->parent.height;
	uint32_t fourcc;
	fourcc = dev->config->parent.fourcc;
	EGLImageKHR image;
	GLint attrib_list[] = {
		EGL_IMAGE_PRESERVED_KHR, EGL_TRUE,
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
#if TEST_TEXTURE_FORMAT
const uint32_t formats[] =
{
	FOURCC_AB24,
	FOURCC_XB24,
	FOURCC_AR24,
	FOURCC_XR24,
	FOURCC_BGR4,
	FOURCC_BG24,
	FOURCC_RG24,
	FOURCC_RGBA,
	FOURCC_RGBP,
	FOURCC_RG16,
	FOURCC_R8  ,
	FOURCC_R10 ,
	FOURCC_R12 ,
	FOURCC_R16 ,
	FOURCC_GR88,
	FOURCC_GREY,
	FOURCC_YUYV,
	FOURCC_YUY2,
	FOURCC_NV12,
	FOURCC_U008,
	FOURCC_BA81,
	FOURCC_RGGB,
	FOURCC_GRBG,
	FOURCC_GBRG,
	FOURCC_BG10,
	FOURCC_GB10,
	FOURCC_BA10,
	FOURCC_RG10,
	FOURCC_BG12,
	FOURCC_GB12,
	FOURCC_BA12,
	FOURCC_RG12,
	0
};
for (int i = 0; i < sizeof(formats) / sizeof(*formats); i++)
{
	if (formats[i] == 0)
		attrib_list[7] = fourcc;
	else
		attrib_list[7] = formats[i];
#endif
	image = eglCreateImageKHR(
					dev->egldisplay,
					EGL_NO_CONTEXT,
					EGL_LINUX_DMA_BUF_EXT,
					NULL,
					attrib_list);

#if TEST_TEXTURE_FORMAT
	if(image != EGL_NO_IMAGE_KHR)
		warn("seg: %.4s supported", &attrib_list[7]);
}
#endif
	if(image == EGL_NO_IMAGE_KHR)
	{
		err("segl: Image creation error %#x", eglGetError());
		return -1;
	}

	buffer->size = size;
	buffer->pitch = stride;
	buffer->dma_fd = dma_fd;

	dev->program_ops->buffer.attach(glbuffer, image);
	eglDestroyImageKHR(dev->egldisplay, image);
	buffer->private = glbuffer;

	return 0;
}

static int texture_frommem(EGL_t *dev, GLBuffer_t *buffer, void *mem, size_t size)
{
	GL_Buffer_t *glbuffer = dev->program_ops->buffer.create(dev->programs, dev->config->parent.fourcc);

	uint32_t stride = size / dev->config->parent.height;
	EGLImageKHR image;
	dbg("segl: create image for mmap %p : %dx%d %u %.4s", mem, dev->config->parent.width, dev->config->parent.height, stride, (char*)&dev->config->parent.fourcc);
	image = eglCreateImageKHR(
					dev->egldisplay,
					dev->eglcontext,
					EGL_GL_TEXTURE_2D_KHR,
					(EGLClientBuffer)(long)dev->program_ops->buffer.id(glbuffer),
					mem);

	if(image == EGL_NO_IMAGE_KHR)
	{
		err("segl: Image creation error %#X", eglGetError());
		return -1;
	}

	buffer->size = size;
	buffer->pitch = stride;
	buffer->memory = mem;

	dev->program_ops->buffer.attach(glbuffer, image);
	eglDestroyImageKHR(dev->egldisplay, image);
	buffer->private = glbuffer;

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
				ret = texture_fromdma(dev, &dev->buffers[i], targets[i], size);
				if (ret)
					break;
				dev->nbuffers++;
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
				ret = texture_frommem(dev, &dev->buffers[i], targets[i], size);
				if (ret)
					break;
				dev->nbuffers++;
			}
		}
		break;
		default:
		break;
	}
	if (ret == 0)
		warn("segl: input image %ux%u  %.4s %"PRIu64,
			dev->config->parent.width, dev->config->parent.height,
			(char *)&dev->config->parent.fourcc, dev->config->parent.modifiers);
	return ret;
}

static void _egl_releasebuffer(EGL_t *dev, int id)
{
	if (dev->export_ctx && dev->export->releasebuffer)
		dev->export->releasebuffer(dev->export_ctx, &dev->buffers[id]);
	dev->program_ops->buffer.destroy(dev->buffers[id].private);
	dev->buffers[id].memory = NULL;
}

static int segl_requestbuffer_input(EGL_t *dev, enum buf_type_e t, va_list ap)
{
	int ret = -1;
	switch (t)
	{
		case buf_type_dmabuf_master:
		{
			int *ntargets = va_arg(ap, int *);
			int **targets = va_arg(ap, int **);
			size_t *size = va_arg(ap, size_t *);
			if (targets != NULL)
			{
				*targets = calloc(dev->nbuffers, sizeof(int));
				for (int i = 0; i < dev->nbuffers; i++)
				{
					GLBuffer_t *buffer = &dev->buffers[i];
					(*targets)[i] = buffer->dma_fd;
					dbg("segl: export dmabuffer[%d]: %d %u", i, buffer->dma_fd, buffer->size);
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
			/// here we can use several buffers
			dev->nbuffers = ntargets;
			for (int i = 0; i < ntargets && i < dev->nbuffers; i++)
			{
				if (dev->buffers[i].size > size)
					err("segl: output buffer is too small for the image");
				dev->buffers[i].memory = targets[i];
				if (size > 0)
					dev->buffers[i].size = size;
				dbg("segl: push data into buffer %p (%u)", dev->buffers[i].memory, dev->buffers[i].size);
			}
			ret = 0;
		}
		break;
		case buf_type_memory_master:
		{
			int *ntargets = va_arg(ap, int *);
			void ***targets = va_arg(ap, void ***);
			size_t *size = va_arg(ap, size_t *);
			if (targets != NULL)
			{
				*targets = calloc(dev->nbuffers, sizeof(int));
				for (int i = 0; i < dev->nbuffers; i++)
				{
					(*targets)[i] = dev->buffers[i].memory;
					dbg("segl: export memory[%d]: %p %u", i, dev->buffers[i].memory, dev->buffers[i].size);
				}
			}
			if (ntargets != NULL)
				*ntargets = dev->nbuffers;
			if (size != NULL)
				*size = dev->buffers[0].size;
			ret = (dev->nbuffers == 0);
		}
		break;
		default:
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
	memmove(&(*pconfig)->parent, &dev->config->transfer, sizeof((*pconfig)->parent));
	if ((*pconfig)->parent.width == 0)
		(*pconfig)->parent.width = dev->config->parent.width;
	if ((*pconfig)->parent.height == 0)
		(*pconfig)->parent.height = dev->config->parent.height;
	dup->config = *pconfig;
	uint32_t width = dup->config->parent.width;
	uint32_t height = dup->config->parent.height;
	uint32_t fourcc = dup->config->parent.fourcc;
	dup->type = device_input;
	dev->dup = dup;
	dup->export = _exports[0];
	if (dup->config->export)
		dup->export = dup->config->export;
	dup->export_ctx = dup->export->create(dup->config, dev->egldisplay, dev->eglcontext);
	if (!dup->export_ctx)
	{
		err("segl: impossible to export data");
		free(dup);
		return NULL;
	}
	warn("segl: create device export %ux%u %.4s with %s", width, height, (char*)&fourcc, dup->export->name);

	dup->nbuffers = 0;
	const FourccFormat_t *fformat = fourcc_getformat(fourcc);
	size_t size = width;
	size *= height;
	size *= fformat->stride_factor[0];
	for (int i = 0; i < MAX_BUFFERS; i++, dup->nbuffers++)
	{
		dup->buffers[i].id = i;
		dup->buffers[i].size = size;
		dup->buffers[i].pitch = fformat->stride_factor[0];
		dup->export->setbuffer(dup->export_ctx, &dup->buffers[i]);
	}
	/// set the parent fbo
	dev->fbo = dup->export->fbo(dup->export_ctx);
	return dup;
}

EXT_API int segl_start(EGL_t *dev)
{
	if (dev->type == device_input)
	{
		dbg("segl: %s start buffers enqueuing", dev->config->parent.name);
		for (int i = 0; i < dev->nbuffers; i++)
		{
			if (segl_queue(dev, i, NULL, 0, 0))
				return -1;
		}
		dev->curbufferid = 0;
		return 0;
	}

	// initialize the first program with the output framebuffer
	GL_Buffer_t *out = NULL;
	if (dev->dup)
		out = dev->dup->export->out(dev->dup->export_ctx);
	dev->program_ops->setup(dev->programs, dev->fbo, out);

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

EXT_API int segl_queue(EGL_t *dev, int id, void *mem, size_t bytesused, int flags)
{
	errno = 0;

	if ((int)id > dev->nbuffers)
	{
		err("segl: unknown buffer id %d", id);
		return -1;
	}
	GLBuffer_t *buffer = &dev->buffers[id];

	if (dev->type == device_input)
	{
		buffer->state = queued;
		return 0;
	}
	if (dev->curbufferid != -1)
	{
		err("segl: device %s not ready %d", dev->config->parent.name, dev->curbufferid);
		return -1;
	}

	buffer->modifiers = 0;
	if (flags & FB_FLAGS_MODIFIER)
		buffer->modifiers = dev->config->parent.modifiers;

	dev->program_ops->run(dev->programs, buffer->private);
	if (eglSwapBuffers(dev->egldisplay, dev->eglsurface) == EGL_FALSE)
		err("EGL swapbuffers error %m");
	// errno is set to EAGAIN after eglSwapBuffers
	errno = 0;
	int ret = dev->native->flush(dev->native_window);
	if (!ret)
	{
		dev->curbufferid = id;
		if (dev->dup)
		{
			buffer = &dev->dup->buffers[id];
			if (buffer->state == queued)
			{
				buffer->state = ready;
			}
		}
	}
	return ret;
}

EXT_API int segl_dequeue(EGL_t *dev, void **mem, size_t *bytesused, int *flags)
{
	errno = 0;
	int id = dev->curbufferid;
	if (dev->type == device_input)
	{
		if (id == -1)
		{
			errno = EAGAIN;
			return id;
		}
		GLBuffer_t *buffer = &dev->buffers[id];
		if (!buffer)
			return -1;
		if (buffer->state != ready)
		{
			errno = EAGAIN;
			return -1;
		}
		dev->export->flush(dev->export_ctx, buffer);
		if (mem)
			*mem = buffer->memory;

		if (flags && buffer->modifiers)
			*flags |= FB_FLAGS_MODIFIER;
		if (bytesused)
			*bytesused = buffer->size;
		buffer->state = dequeued;
		dev->curbufferid++;
		dev->curbufferid %= dev->nbuffers;
		return id;
	}
	dev->curbufferid = -1;
	GLBuffer_t *buffer = &dev->buffers[id];
	dev->program_ops->stop(dev->programs, buffer->private);
	if (dev->native->sync(dev->native_window) < 0)
		return -1;

	return id;
}

EXT_API int segl_fd(EGL_t *dev, int writer)
{
	if (writer && dev->curbufferid == -1)
		return 0;
	if (writer)
		return -1;
	if (dev->type == device_input)
		return dev->export->fd(dev->export_ctx);
	return dev->native->fd(dev->native_window);
}

EXT_API void segl_destroy(EGL_t *dev)
{
	if (dev->type != device_input)
	{
		dev->program_ops->destroy(dev->programs);
		eglDestroySurface(dev->egldisplay, dev->eglsurface);
		eglDestroyContext(dev->egldisplay, dev->eglcontext);
		dev->native->destroy(dev->native_display);
	}
	for (int i = 0; i < dev->nbuffers; i++)
	{
		_egl_releasebuffer(dev,i);
	}
	if (dev->export_ctx)
		dev->export->destroy(dev->export_ctx);
	free(dev);
}

DeviceConf_t * segl_createconfig(const char *name)
{
	EGLConfig_t *devconfig = NULL;
	devconfig = calloc(1, sizeof(EGLConfig_t));
#ifdef HAVE_JANSSON
	devconfig->parent.ops.loadconfiguration = segl_loadjsonconfiguration;
#endif
	devconfig->export = NULL;
	devconfig->native = _natives[0];
	if (strstr(name, "noprogram") != NULL)
		devconfig->mode |= SEGL_NOPROGRAM;
	return (DeviceConf_t *)devconfig;
}

static const EGLNative_t *_segl_get_native(const char *name)
{
	const EGLNative_t *native = NULL;
	for (int i = 0; i < sizeof(_natives) / sizeof(*_natives) && _natives[i]; i++)
	{
		if (!strcmp(_natives[i]->name, name))
		{
			native = _natives[i];
			break;
		}
	}
	return native;
}

#ifdef HAVE_JANSSON
#include <jansson.h>

int segl_loadjsonsettings(EGL_t *dev, void *entry)
{
	json_t *jconfig = entry;
	json_t *jprograms = json_object_get(jconfig, "programs");
	return dev->program_ops->loadjsonsetting(dev->programs, jprograms);
}

int segl_loadjsonconfiguration(void *arg, void *entry)
{
	json_t *jconfig = entry;
	EGLConfig_t *config = (EGLConfig_t *)arg;

	json_t *jprograms = json_object_get(jconfig, "programs");
	for (int i = 0; i < sizeof(_prog_ops)/sizeof(*_prog_ops); i++)
	{
		const EGLProg_ops_t *prog_ops = _prog_ops[i];
		if (prog_ops && !(config->mode & SEGL_NOPROGRAM))
		{
			prog_ops->loadjsonconfiguration(&config->programs, jprograms);
		}
	}

	json_t *native = json_object_get(jconfig, "native");
	if (native && json_is_array(native))
	{
		native = json_array_get(native, 0);
	}
	if (native && json_is_string(native))
	{
		const char *value = json_string_value(native);
		const EGLNative_t *native = _segl_get_native(value);
		if (native != NULL)
		{
			config->native = native;
		}
	}
	json_t *device = json_object_get(jconfig, "device");
	if (device && json_is_string(device))
	{
		const char *value = json_string_value(device);
		config->device = value;
	}
	json_t *definition = json_object_get(jconfig, "definition");
	scommon_loaddefinition(&config->parent, definition);
	json_t *transfer = json_object_get(jconfig, "transfer");
	scommon_loaddefinition(&config->transfer, transfer);
	if (config->transfer.width == 0)
		config->transfer.width = config->parent.width;
	if (config->transfer.height == 0)
		config->transfer.height = config->parent.height;
	if (config->transfer.fourcc == 0)
		config->transfer.fourcc = config->parent.fourcc;
	if (config->transfer.modifiers == 0)
		config->transfer.modifiers = config->parent.modifiers;
	json_t *export = json_object_get(jconfig, "export");
	if (!export)
		export = json_object_get(transfer, "export");
	if (export && json_is_string(export))
	{
		const char *value = json_string_value(export);
		for (int i = 0; i < sizeof(_exports) / sizeof(*_exports) && _exports[i]; i++)
		{
			if (!strcmp(_exports[i]->name, value))
			{
				config->export = _exports[i];
				break;
			}
		}
	}

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
	json_object_set_new(definition, "fourcc", json_sprintf("%.4s", (char*)&image->fourcc));
	json_array_append_new(definitions->array, definition);
	return 0;
}

int segl_capabilities(EGL_t *dev, json_t *capabilities, int all)
{
	if (all)
	{
		json_t *native = json_array();
		for (int i = 0; i < sizeof(_natives) / sizeof(*_natives) && _natives[i]; i++)
			json_array_append_new(native, json_string(_natives[i]->name));
		json_object_set_new(capabilities, "native", native);
	}
	else
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

const FastVideoDevice_ops_t segl_ops = {
	.name = "gpu",
	.createconfig = segl_createconfig,
	.create = (FastVideoDevice_create_t)segl_create,
	.duplicate = (FastVideoDevice_duplicate_t)segl_duplicate,
	.loadsettings = (FastVideoDevice_loadsettings_t)segl_loadjsonsettings,
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
	_egl_initprototypes();

	fastvideodevice_ops_append_t _fastvideodevice_ops_append;
	void *hdl = dlopen(NULL, RTLD_NOW);
	_fastvideodevice_ops_append = dlsym(hdl, "fastvideodevice_ops_append");
	if (_fastvideodevice_ops_append)
	{
		_fastvideodevice_ops_append(&segl_ops);
	}
}
