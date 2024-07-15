#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

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

#define USE_VERTEXARRAY

#define MAX_BUFFERS 4

#ifndef FOURCC
#define FOURCC(a,b,c,d)	((a << 0) | (b << 8) | (c << 16) | (d << 24))
#endif

typedef struct GLBuffer_s GLBuffer_t;
struct GLBuffer_s
{
	uint32_t fb_id;
	int dma_fd;
	int egl_fd;
	GLuint dma_texture;
	EGLImageKHR dma_image;
	uint32_t *memory;
	GLuint pitch;
	GLuint offset;
	uint32_t size;
};

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
	GLuint programID;
	GLuint framebufferID;
	GLuint vertexArrayID;
	GLuint vertexBufferObject[3];
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
PFNGLBINDVERTEXARRAYOESPROC glBindVertexArrayOES = NULL;
PFNGLGENVERTEXARRAYSOESPROC glGenVertexArraysOES = NULL;

#ifdef GLESV3
static GLchar defaultvertex[] = "#version 300 es \n\
layout(location = 0) in vec3 vPosition;\n\
layout(location = 1) in vec2 vUV;\n\
varying vec2 texUV;\n\
\n\
void main (void)\n\
{\n\
	gl_Position = vec4(vPosition, 1);\n\
	texUV = vUV;\n\
}\n\
";
static GLchar defaultfragment[] = "#version 300 es\n\
precision mediump float;\n\
uniform sampler2D vTexture;\n\
varying vec2 texUV;\n\
void main() {\n\
	gl_FragColor = texture((vTexture, texUV);\n\
}\n\
";
#else
static GLchar defaultvertex[] = "\
attribute vec3 vPosition;\n\
attribute vec2 vUV;\n\
varying vec2 texUV;\n\
\n\
void main (void)\n\
{\n\
	gl_Position = vec4(vPosition, 1);\n\
	texUV = vUV;\n\
}\n\
";
static GLchar defaultfragment[] = ""
"#extension GL_OES_EGL_image_external : require\n"
"precision mediump float;\n"
"uniform samplerExternalOES vTexture;\n"
"varying vec2 texUV;\n"
"void main() {\n"
"	gl_FragColor = texture2D(vTexture, texUV);\n"
"}\n";
#endif

static GLchar defaulttexturename[] = "vTexture";

static void display_log(GLuint instance)
{
	GLint logSize = 0;
	GLchar* log = NULL;

	glGetProgramiv(instance, GL_INFO_LOG_LENGTH, &logSize);
	log = (GLchar*)malloc(logSize);
	if ( log == NULL )
	{
		err("segl: Log memory allocation error %m");
		return;
	}
	glGetProgramInfoLog(instance, logSize, &logSize, log);
	err("%s",log);
	free(log);
}

static GLint getFileSize(FILE* const pFile)
{
	GLint length = 0;

	fseek(pFile,0,SEEK_END);
	length = ftell(pFile);
	fseek(pFile,0,SEEK_SET);

	return length;
}


static GLint readFile(const char* fileName, char** fileContent)
{
	FILE* pFile = NULL;
	GLint fileSize = 0;

	pFile = fopen(fileName, "r");
	if ( pFile == NULL )
	{
		err("segl: shader file '%s' opening error %m",fileName);
		return 0;
	}

	fileSize = getFileSize(pFile);

	*fileContent = (char*)malloc(fileSize + 1);
	if ( *fileContent == NULL )
	{
		err("segl: shader file loading memory allocation error %m");
		return 0;
	}

	if (fread(*fileContent, fileSize, 1, pFile) < 0)
	{
		fclose(pFile);
		err("glmotor: File loading error %m");
	}
	(*fileContent)[fileSize] = '\0';

	fclose(pFile);

	return fileSize;
}

static void deleteShader(GLuint programID, GLuint fragmentID, GLuint vertexID)
{
	if (programID)
	{
		glUseProgram(0);

		glDetachShader(programID, fragmentID);
		glDetachShader(programID, vertexID);

		glDeleteProgram(programID);
	}
	if (fragmentID)
		glDeleteShader(fragmentID);
	if (vertexID)
		glDeleteShader(vertexID);
}

static GLuint loadShader(EGL_t *dev, GLenum shadertype, const char *shaderfile, const char *defaultshader)
{
	GLchar* shaderSource = NULL;
	GLuint shaderID = glCreateShader(shadertype);

	GLuint shaderSize = 0;
	if (shaderfile)
	{
		shaderSize = readFile(shaderfile, &shaderSource);
		warn("load dynamic shader:\n%s<=", shaderSource);
	}
	else
	{
		shaderSource = defaultshader;
		shaderSize = strlen(shaderSource);
		warn("load default shader:\n%s", shaderSource);
	}
	if (shaderSource == NULL)
		return -1;
	glShaderSource(shaderID, 1, (const GLchar**)(&shaderSource), &shaderSize);
	glCompileShader(shaderID);
	GLint compilationStatus = 0;

	glGetShaderiv(shaderID, GL_COMPILE_STATUS, &compilationStatus);
	if ( compilationStatus != GL_TRUE )
	{
		display_log(shaderID);
		return -1;
	}

	return shaderID;
}

static GLuint loadShaders(EGL_t *dev, const char *vertex, const char *fragment)
{
	GLchar* vertexSource = NULL;
	GLchar* fragmentSource = NULL;
	GLint programState = 0;

	GLuint vertexID = loadShader(dev, GL_VERTEX_SHADER, vertex, defaultvertex);
	if ( vertexID == -1)
	{
		err("segl: vertex shader compilation error");
		return GL_FALSE;
	}
	GLuint fragmentID = loadShader(dev, GL_FRAGMENT_SHADER, fragment, defaultfragment);
	if ( fragmentID == -1)
	{
		err("segl: fragment shader compilation error");
		deleteShader(0, vertexID, 0);
		return GL_FALSE;
	}

	GLuint programID = glCreateProgram();

	glAttachShader(programID, vertexID);
	glAttachShader(programID, fragmentID);

	glLinkProgram(programID);

	glGetProgramiv(programID , GL_LINK_STATUS  , &programState);
	if ( programState != GL_TRUE)
	{
		display_log(programID);
		deleteShader(programID, fragmentID, vertexID);
		return -1;
	}

    glDetachShader(programID, vertexID);
    glDetachShader(programID, fragmentID);

	glDeleteShader(vertexID);
	glDeleteShader(fragmentID);

	glUseProgram(programID);

	return programID;
}

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
	EGLNativeWindowType nwindow = native->createwindow(ndisplay, config->texture.width, config->texture.height, "segl");

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
	glEnable(GL_TEXTURE_EXTERNAL_OES);
	EGLint num_config;
	eglGetConfigs(eglDisplay, NULL, 0, &num_config);

	static const EGLint config_attribs[] = {
		EGL_RED_SIZE, 8,
		EGL_GREEN_SIZE, 8,
		EGL_BLUE_SIZE, 8,
		EGL_ALPHA_SIZE, 8,
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

	EGL_t *dev = calloc(1, sizeof(*dev));
	dev->config = config;
	dev->native = native;
	dev->egldisplay = eglDisplay;
	dev->eglconfig = eglConfig;
	dev->eglcontext = eglContext;
	dev->eglsurface = eglSurface;

	dev->programID = loadShaders(dev, config->vertex, config->fragment);
	if (dev->programID == 0)
	{
		err("segl: shader loading error");
		free(dev);
		return NULL;
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
	glGenVertexArraysOES = (void *) eglGetProcAddress("glGenVertexArraysOES");
	if(glGenVertexArraysOES == NULL)
	{
		return NULL;
	}
#ifdef USE_VERTEXARRAY
	glBindVertexArrayOES = (void *) eglGetProcAddress("glBindVertexArrayOES");
	if(glBindVertexArrayOES == NULL)
	{
		return NULL;
	}
#endif

	glClearColor(0.5, 0.5, 0.5, 1.0);
	glViewport(0, 0, config->texture.width,config->texture.height);
	glClearDepthf(1.0);
	glEnable(GL_DEPTH_TEST);
	glDepthFunc(GL_LESS);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

#ifdef USE_VERTEXARRAY
	glGenVertexArraysOES(1, &dev->vertexArrayID);
	glBindVertexArrayOES(dev->vertexArrayID);
#endif
	glGenBuffers(3, dev->vertexBufferObject);

	GLfloat vertices[] = {
		-1.0f, -1.0f,  0.0f, // bottom left
		-1.0f,  1.0f,  0.0f, // top left
		 1.0f,  1.0f,  0.0f, // top right
		 1.0f, -1.0f,  0.0f, // bottom right
	};
	glBindBuffer(GL_ARRAY_BUFFER, dev->vertexBufferObject[0]);
	glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
	GLint pos = glGetAttribLocation(dev->programID, "vPosition");
	glEnableVertexAttribArray(pos);
	glVertexAttribPointer(pos, 3, GL_FLOAT, GL_FALSE, 0, 0);

	GLfloat uves[] = {
		 0.0f,  1.0f, // bottom left
		 0.0f,  0.0f, // top left
		 1.0f,  0.0f, // top right
		 1.0f,  1.0f, // bottom right
	};
	glBindBuffer(GL_ARRAY_BUFFER, dev->vertexBufferObject[1]);
	glBufferData(GL_ARRAY_BUFFER, sizeof(uves), uves, GL_STATIC_DRAW);
	GLint UV = glGetAttribLocation(dev->programID, "vUV");
	glEnableVertexAttribArray(UV);
	glVertexAttribPointer(UV, 2, GL_FLOAT, GL_FALSE, 0, 0);

	GLushort indexes[] = {
		0, 1, 3,
		1, 2, 3
	};
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, dev->vertexBufferObject[2]);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indexes), indexes, GL_STATIC_DRAW);

	if (config->texture.name == NULL)
		config->texture.name = defaulttexturename;
	GLuint texid;
	GLint texMap = glGetUniformLocation(dev->programID, config->texture.name);
	glUniform1i(texMap, 0); // GL_TEXTURE0
	glActiveTexture(GL_TEXTURE0);

	dev->native_window = nwindow;
	dev->native_display = ndisplay;
	dev->curbufferid = -1;
#ifdef USE_VERTEXARRAY
	glBindVertexArrayOES(0);
#endif
	return dev;
}

static int link_texturedma(EGL_t *dev, int dma_fd, size_t size)
{
	uint32_t stride = size / dev->config->texture.height;
	EGLImageKHR dma_image;
	GLint attrib_list[] = {
		EGL_WIDTH, dev->config->texture.width,
		EGL_HEIGHT, dev->config->texture.height,
		EGL_LINUX_DRM_FOURCC_EXT, dev->config->texture.fourcc,
		EGL_DMA_BUF_PLANE0_FD_EXT, dma_fd,
		EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
		EGL_DMA_BUF_PLANE0_PITCH_EXT, stride,
		EGL_NONE
	};
	dbg("segl: create image for dma %d : %dx%d %u %.4s", dma_fd, dev->config->texture.width, dev->config->texture.height, stride, (char*)&dev->config->texture.fourcc);
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

	glBindTexture(GL_TEXTURE_EXTERNAL_OES, dma_texture);
	glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, dma_image);
	eglDestroyImageKHR(dev->egldisplay, dma_image);

	dev->buffers[dev->nbuffers].size = size;
	dev->buffers[dev->nbuffers].pitch = stride;
	dev->buffers[dev->nbuffers].dma_fd = dma_fd;
	dev->buffers[dev->nbuffers].dma_texture = dma_texture;
	dev->buffers[dev->nbuffers].dma_image = NULL;
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
	eglMakeCurrent(dev->egldisplay, dev->eglsurface, dev->eglsurface, dev->eglcontext);
	return 0;
}

int segl_stop(EGL_t *dev)
{
	eglMakeCurrent(dev->egldisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	return 0;
};

int segl_queue(EGL_t *dev, int id, size_t bytesused)
{
	eglSwapBuffers(dev->egldisplay, dev->eglsurface);
	if ((int)id > dev->nbuffers)
		return -1;
	if (dev->curbufferid != -1)
		return -1;
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	glUseProgram(dev->programID);
#ifdef USE_VERTEXARRAY
	glBindVertexArrayOES(dev->vertexArrayID);
#else
	glBindBuffer(GL_ARRAY_BUFFER, dev->vertexBufferObject[0]);
	glBindBuffer(GL_ARRAY_BUFFER, dev->vertexBufferObject[1]);
	glBindTexture(GL_TEXTURE_EXTERNAL_OES, dev->buffers[(int)id].dma_texture);
#endif

	glDrawElements(GL_TRIANGLE_STRIP, 3 * 2, GL_UNSIGNED_SHORT, 0);

	dev->curbufferid = (int)id;
	dev->native->running(dev->native_window);
	return 0;
}

int segl_dequeue(EGL_t *dev, void **mem, size_t *bytesused)
{
	int id = dev->curbufferid;
	dev->curbufferid = -1;
#ifdef USE_VERTEXARRAY
	glBindVertexArrayOES(0);
#endif
	return id;
}

void segl_destroy(EGL_t *dev)
{
	eglDestroySurface(dev->egldisplay, dev->eglsurface);
	eglDestroyContext(dev->egldisplay, dev->eglcontext);
	dev->native->destroy(dev->native_display);
	free(dev);
}

#ifdef HAVE_JANSSON
#include <jansson.h>

int segl_loadjsonconfiguration(void *arg, void *entry)
{
	json_t *jconfig = entry;

	EGLConfig_t *config = (EGLConfig_t *)arg;
	json_t *vertex = json_object_get(jconfig, "vertex");
	if (vertex && json_is_string(vertex))
	{
		const char *value = json_string_value(vertex);
		config->vertex = value;
	}
	json_t *fragment = json_object_get(jconfig, "fragment");
	if (fragment && json_is_string(fragment))
	{
		const char *value = json_string_value(fragment);
		config->fragment = value;
	}
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
