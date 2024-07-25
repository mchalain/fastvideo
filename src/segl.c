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
	GLenum textype;
	GLuint dma_texture;
	EGLImageKHR dma_image;
	uint32_t *memory;
	GLuint pitch;
	GLuint offset;
	uint32_t size;
};

typedef struct GLProgram_s GLProgram_t;
struct GLProgram_s
{
	GLuint ID;
	GLuint vertexArrayID;
	GLuint vertexBufferObject[3];
	char *in_texturename;
	GLenum in_textype;
	GLBuffer_t *in_textures;
	GLBuffer_t out_textures[MAX_BUFFERS];
	GLuint fbID;
	GLfloat width;
	GLfloat height;
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
	GLProgram_t programs[MAX_PROGRANS];
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

//#define GLSLV300

#ifdef GLSLV300
static GLchar defaultvertex[] = "#version 300 es \n\
layout(location = 0) in vec3 vPosition;\n\
out vec2 texUV;\n\
\n\
void main (void)\n\
{\n\
	gl_Position = vec4(vPosition, 1);\n\
	texUV = (vec2(0.5, 0.5) - vPosition.xy / 2.0);\n\
}\n\
";
static GLchar defaultfragment[] = "#version 300 es\n\
precision mediump float;\n\
uniform sampler2D vTexture;\n\
in vec2 texUV;\n\
out vec4 fragColor;\n\
\n\
void main() {\n\
	fragColor = texture(vTexture, texUV);\n\
}\n\
";
#else
static GLchar defaultvertex[] = ""
"\n""attribute vec3 vPosition;"
"\n""varying vec2 texUV;"
"\n"
"\n""void main (void)"
"\n""{"
"\n""	texUV = vec2(0.5 + vPosition.x / 2.0, 0.5 - vPosition.y / 2.0);"
"\n""	gl_Position = vec4(vPosition,1.);"
"\n""}"
"\n";
static GLchar defaultfragment[] = ""
"\n""#extension GL_OES_EGL_image_external : require"
"\n""precision mediump float;"
"\n""uniform samplerExternalOES vTexture;"
"\n""varying vec2 texUV;"
"\n"
"\n""void main() {"
"\n""	gl_FragColor = texture2D(vTexture, texUV);"
"\n""}"
"\n";
#endif

static GLchar defaulttexturename[] = "vTexture";

static void display_log(GLuint instance)
{
	GLint logSize = 0;
	GLchar* log = NULL;

	glGetProgramiv(instance, GL_INFO_LOG_LENGTH, &logSize);
	if (!logSize)
	{
		err("no log");
		return;
	}
	log = (GLchar*)malloc(logSize);
	if ( log == NULL )
	{
		err("segl: Log memory allocation error %m");
		return;
	}
	if (glIsShader(instance))
		glGetShaderInfoLog(instance, logSize, NULL, log);
	else
		glGetProgramInfoLog(instance, logSize, NULL, log);
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
	if (shaderID == 0)
		return 0;

	GLuint shaderSize = 0;
	if (shaderfile)
	{
		shaderSize = readFile(shaderfile, &shaderSource);
		if (shaderSource == NULL)
			return 0;
		warn("load dynamic shader:\n%s<=", shaderSource);
	}
	else
	{
		shaderSource = defaultshader;
		shaderSize = strlen(shaderSource);
		if (shaderSource == NULL)
			return 0;
		warn("load default shader:\n%s", shaderSource);
	}
	glShaderSource(shaderID, 1, (const GLchar**)(&shaderSource), &shaderSize);
	glCompileShader(shaderID);
	GLint compilationStatus = 0;

	glGetShaderiv(shaderID, GL_COMPILE_STATUS, &compilationStatus);
	if ( compilationStatus != GL_TRUE )
	{
		display_log(shaderID);
		return 0;
	}

	return shaderID;
}

static GLuint loadShaders(EGL_t *dev, GLenum shadertype, const char *shaderfiles[MAX_SHADERS])
{
	GLuint shaderID = glCreateShader(shadertype);
	if (shaderID == 0)
		return 0;

	GLint nbShaderSources = 0;
	GLchar* shaderSources[4] = {0};
	GLuint shaderSizes[4] = {0};

	for (int i = 0; i < MAX_SHADERS; i++)
	{
		if (shaderfiles[i])
		{
			shaderSizes[i] = readFile(shaderfiles[i], &shaderSources[i]);
			if (shaderSources[i] == NULL)
				return 0;
			warn("load dynamic shader:\n%s<=", shaderSources[i]);
			nbShaderSources++;
		}
	}
	glShaderSource(shaderID, nbShaderSources, (const char *const*)shaderSources, shaderSizes);
	glCompileShader(shaderID);
	GLint compilationStatus = 0;

	glGetShaderiv(shaderID, GL_COMPILE_STATUS, &compilationStatus);
	if ( compilationStatus != GL_TRUE )
	{
		display_log(shaderID);
		return 0;
	}
	return shaderID;
}

static GLuint buildProgramm(EGL_t *dev, const char *vertex, const char *fragments[MAX_SHADERS])
{
	GLchar* vertexSource = NULL;
	GLchar* fragmentSource = NULL;
	GLint programState = 0;

	GLuint vertexID = loadShader(dev, GL_VERTEX_SHADER, vertex, defaultvertex);
	if ( vertexID == 0)
	{
		err("segl: vertex shader compilation error");
		return 0;
	}

	GLuint fragmentID = 0;
	if (fragments == NULL)
		fragmentID = loadShader(dev, GL_FRAGMENT_SHADER, NULL, defaultfragment);
	else if (fragments[1] == NULL)
		fragmentID = loadShader(dev, GL_FRAGMENT_SHADER, fragments[0], defaultfragment);
	else
		fragmentID = loadShaders(dev, GL_FRAGMENT_SHADER, fragments);
	if (fragmentID == 0)
	{
		err("segl: fragment shader compilation error");
		deleteShader(0, vertexID, 0);
		return 0;
	}

	GLuint programID = glCreateProgram();
	if (programID == 0)
	{
		err("");
		return 0;
	}

	glAttachShader(programID, vertexID);
	glAttachShader(programID, fragmentID);

	glLinkProgram(programID);

	glGetProgramiv(programID , GL_LINK_STATUS  , &programState);
	if ( programState != GL_TRUE)
	{
		display_log(programID);
		deleteShader(programID, fragmentID, vertexID);
		return 0;
	}

    glDetachShader(programID, vertexID);
    glDetachShader(programID, fragmentID);

	glDeleteShader(vertexID);
	glDeleteShader(fragmentID);

	return programID;
}

static int glprog_setup(GLProgram_t *program, EGLConfig_t *config)
{
	program->width = config->parent.width;
	program->height = config->parent.height;

	glUseProgram(program->ID);

	glGenVertexArraysOES(1, &program->vertexArrayID);
	glBindVertexArrayOES(program->vertexArrayID);

	glGenBuffers(1, program->vertexBufferObject);

	GLfloat vertices[] = {
		-1.0f,  1.0f,  0.0f, // top left
		-1.0f, -1.0f,  0.0f, // bottom left
		 1.0f, -1.0f,  0.0f, // bottom right
		-1.0f,  1.0f,  0.0f, // top left
		 1.0f, -1.0f,  0.0f, // bottom right
		 1.0f,  1.0f,  0.0f, // top right
	};
	glBindBuffer(GL_ARRAY_BUFFER, program->vertexBufferObject[0]);
	glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
	GLint pos = glGetAttribLocation(program->ID, "vPosition");
	glEnableVertexAttribArray(pos);
	glVertexAttribPointer(pos, 3, GL_FLOAT, GL_FALSE, 0, 0);

	if (config->texture.name == NULL)
		config->texture.name = defaulttexturename;
	GLuint texid;
	GLint texMap = glGetUniformLocation(program->ID, config->texture.name);
	glUniform1i(texMap, 0); // GL_TEXTURE0
	glActiveTexture(GL_TEXTURE0);

	GLuint resolutionID = glGetUniformLocation(program->ID, "vResolution");
	glUniform2f(resolutionID, program->width, program->height);

	glBindVertexArrayOES(0);
	return 0;
}

static GLBuffer_t *glprog_getouttexture(GLProgram_t *program, GLuint nbtex)
{
	if (program->out_textures[0].dma_texture)
	{
		return program->out_textures;
	}
	glGenFramebuffers(1, &program->fbID);
	glBindFramebuffer(GL_FRAMEBUFFER, program->fbID);
	glEnable(GL_TEXTURE_2D);
	GLuint texture = 0;
	for (int i = 0; i < nbtex; i++)
	{
		glGenTextures(1, &texture);
		glBindTexture(GL_TEXTURE_2D, texture);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, program->width, program->height, 0, GL_RGBA,  GL_UNSIGNED_BYTE, NULL);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		program->out_textures[i].dma_texture = texture;
	}
	return program->out_textures;
}

static int glprog_setintexture(GLProgram_t *program, GLenum type, GLuint nbtex, GLBuffer_t *in_textures)
{
	program->in_textures = in_textures;
	program->in_textype = type;
}

static int glprog_run(GLProgram_t *program, int bufid)
{
	if (program->fbID)
	{
		glBindFramebuffer(GL_FRAMEBUFFER, program->fbID);
		glBindTexture(GL_TEXTURE_2D, program->out_textures[bufid].dma_texture);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
					program->out_textures[bufid].dma_texture, 0);
	}

	glBindVertexArrayOES(program->vertexArrayID);
	glUseProgram(program->ID);

	glActiveTexture(GL_TEXTURE0);
	glBindTexture(program->in_textype, program->in_textures[bufid].dma_texture);

	glDrawArrays(GL_TRIANGLE_STRIP, 0, 6);

	if (program->fbID != -1)
	{
		GLint status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
		if (status != GL_FRAMEBUFFER_COMPLETE)
		{
			err("framebuffer incomplet");
			//return -1;
		}
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, 0, 0, 0);
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
	}

	return 0;
}

static void glprog_destroy(GLProgram_t *program)
{
	if (program->fbID)
	{
		glDeleteFramebuffers(1, &program->fbID);
		for (int i = 0; i < MAX_BUFFERS; i++)
		{
			glDeleteTextures(1, &program->out_textures[i].dma_texture);
		}
	}
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
	dev->programs[0].ID = buildProgramm(dev, NULL, NULL);
	for (int i = 0; i < MAX_PROGRANS; i++)
	{
		EGLConfig_Program_t *prconfig = &config->programs[i];
		if (prconfig->vertex && prconfig->fragments[0])
		{
			dev->programs[nbprg].ID = buildProgramm(dev, prconfig->vertex, prconfig->fragments);
			if (dev->programs[nbprg].ID == 0)
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
#ifdef USE_VERTEXARRAY
	glGenVertexArraysOES = (void *) eglGetProcAddress("glGenVertexArraysOES");
	if(glGenVertexArraysOES == NULL)
	{
		return NULL;
	}
	glBindVertexArrayOES = (void *) eglGetProcAddress("glBindVertexArrayOES");
	if(glBindVertexArrayOES == NULL)
	{
		return NULL;
	}
#endif

	for (int i = 0; i < nbprg; i++)
	{
		GLProgram_t *program = &dev->programs[i];
		glprog_setup(program, config);
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
	glprog_setintexture(&dev->programs[0], dev->buffers[0].textype, dev->nbuffers, dev->buffers);
	for (int i = 1; dev->programs[i].ID; i++)
	{
		GLBuffer_t *textures;
		textures = glprog_getouttexture(&dev->programs[i - 1], dev->nbuffers);
		glprog_setintexture(&dev->programs[i], GL_TEXTURE_2D, dev->nbuffers, textures);
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

	for (int i = 0; i < MAX_PROGRANS && dev->programs[i].ID; i++)
	{
		glprog_run(&dev->programs[i], (int)id);
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

static int _segl_loadjsonprogram(json_t *jconfig, EGLConfig_Program_t *config)
{
	json_t *disable = json_object_get(jconfig, "disable");
	if (disable && json_is_boolean(disable) && json_is_true(disable))
	{
		return -1;
	}
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
		config->fragments[0] = value;
	}
	if (fragment && json_is_array(fragment))
	{
		int index = 0;
		json_t *string;
		json_array_foreach(fragment, index, string)
		{
			if (json_is_string(string))
			{
				const char *value = json_string_value(string);
				config->fragments[index] = value;
			}
		}
	}
	return 0;
}
int segl_loadjsonconfiguration(void *arg, void *entry)
{
	json_t *jconfig = entry;

	EGLConfig_t *config = (EGLConfig_t *)arg;
	EGLConfig_Program_t *prgconfig = &config->programs[0];
	json_t *programs = json_object_get(jconfig, "programs");
	if (programs && json_is_array(programs))
	{
		json_t *jfield = NULL;
		int i = 0;
		json_array_foreach(programs, i, jfield)
		{
			prgconfig = &config->programs[i];
			_segl_loadjsonprogram(jfield, prgconfig);
		}
	}
	else
		_segl_loadjsonprogram(jconfig, prgconfig);
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
