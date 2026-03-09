#include <string.h>
#include <sys/shm.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <errno.h>
#include <sys/stat.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include "segl.h"
#include "log.h"

#define segl_dbg(...)

struct GL_Buffer_s
{
	const char *name;
	GLint unit;
	GLint loc;
	EGLImageKHR image;
	GLuint fbo;
	GLuint texture;
	GLenum textype;
	EGLint egltarget;
};

typedef enum{
	Uniform_UNKNOWN_e = 0,
	Uniform_INT_e,
	Uniform_FLOAT_e,
	Uniform_FVEC2_e,
	Uniform_FVEC3_e,
	Uniform_FVEC4_e,
	Uniform_IVEC2_e,
	Uniform_IVEC3_e,
	Uniform_IVEC4_e,
	Uniform_MAT2_e,
	Uniform_MAT3_e,
	Uniform_MAT4_e,
	Uniform_FUNC_e,
} Uniform_Type_e;

#define	Uniform_SHARED_e 0x1000

typedef struct GLProgram_Uniform_s GLProgram_Uniform_t;
struct GLProgram_Uniform_s
{
	const char *name;
	GLint loc;
	Uniform_Type_e type;
	void *value;
	void *data;
	GLProgram_Uniform_t *next;
};

static GLProgram_Uniform_t * _glprog_uniform_create(void *setting);
static int _glprog_uniform_size(GLProgram_Uniform_t *uniform);
static void _glprog_uniform_destroy(GLProgram_Uniform_t *uniform);
int glprog_setuniform(GLProgram_t *program, GLProgram_Uniform_t *uniform);
static void _glprog_uniform_destroy(GLProgram_Uniform_t *uniform);

static EGLProg_ops_t _gles2_ops;

typedef struct GLProgram_s GLProgram_t;
struct GLProgram_s
{
	int index;
	GLProgram_t *next;
	EGLConfig_Program_t *config;
	GLuint ID;
	GLuint vertexArrayID;
	GLuint vertexBufferObject[3];
	GLfloat *movectx;
	GLfloat *(*move)(GLfloat *);
	GL_Buffer_t *out;
	uint32_t width;
	uint32_t height;
	uint32_t fourcc;
	void *controls_data;
	GLProgram_Uniform_t *controls;
};

const GLchar _defaultname[] = "display";
const GLchar _defaulttexturename[] = "vTexture";
static const char _segldir[] = "/tmp/fastvideo.segl";

//#define GLSLV300

#ifdef GLSLV300
static const GLchar _defaultvertex[] = "#version 300 es \n\
layout(location = 0) in vec3 vPosition;\n\
out vec2 texUV;\n\
\n\
void main (void)\n\
{\n\
	gl_Position = vec4(vPosition, 1);\n\
	texUV = (vec2(0.5, 0.5) - vPosition.xy / 2.0);\n\
}\n\
";
static const GLchar _defaultfragment[] = "#version 300 es\n\
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
static const GLchar _defaultvertex[] = ""
"\n""attribute vec3 vPosition;"
"\n""varying vec2 texUV;"
"\n"
"\n""void main (void)"
"\n""{"
"\n""	texUV = vec2(0.5 - vPosition.x / 2.0, 0.5 - vPosition.y / 2.0);"
"\n""	gl_Position = vec4(vPosition,1.);"
"\n""}"
"\n";
static const GLchar _defaultfragment[] = ""
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

#ifndef GL_TEXTURE_EXTERNAL_OES
#define GL_TEXTURE_EXTERNAL_OES GL_TEXTURE_2D;
#endif

#ifndef EGL_EGLEXT_PROTOTYPES
static PFNGLBINDVERTEXARRAYOESPROC glBindVertexArrayOES = NULL;
static PFNGLGENVERTEXARRAYSOESPROC glGenVertexArraysOES = NULL;
#ifdef EGL_KHR_image
static PFNEGLCREATEIMAGEKHRPROC eglCreateImageKHR = NULL;
#endif
#if defined(GL_OES_EGL_image)
static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES = NULL;
static PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC glEGLImageTargetRenderbufferStorageOES = NULL;
#endif
static int _egl_initprototypes(void)
{
	glGenVertexArraysOES = (void *) eglGetProcAddress("glGenVertexArraysOES");
	if(glGenVertexArraysOES == NULL)
	{
		return -1;
	}
	glBindVertexArrayOES = (void *) eglGetProcAddress("glBindVertexArrayOES");
	if(glBindVertexArrayOES == NULL)
	{
		return -1;
	}
#if defined(GL_OES_EGL_image)
	glEGLImageTargetTexture2DOES = (void *) eglGetProcAddress("glEGLImageTargetTexture2DOES");
	if(glEGLImageTargetTexture2DOES == NULL)
	{
		return -1;
	}
	glEGLImageTargetRenderbufferStorageOES = (void *) eglGetProcAddress("glEGLImageTargetRenderbufferStorageOES");
	if(glEGLImageTargetRenderbufferStorageOES == NULL)
	{
		return -1;
	}
#endif
#ifdef EGL_KHR_image
	eglCreateImageKHR = (void *) eglGetProcAddress("eglCreateImageKHR");
	if(eglCreateImageKHR == NULL)
	{
		return -1;
	}
#endif
	return 0;
}
#else
#define _egl_initprototypes(...)
#endif

GL_Buffer_t *gltexture_create(GLProgram_t *program, const char *name, const char *src, uint32_t fourcc);
void gltexture_attach(GL_Buffer_t *glbuffer, EGLImageKHR image);
GLuint gltexture_id(GL_Buffer_t *glbuffer);
void gltexture_destroy(GL_Buffer_t *glbuffer);

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

static void display_log(GLuint instance)
{
	GLint logSize = 0;
	GLchar* log = NULL;

	if (glIsShader(instance))
		glGetShaderiv(instance, GL_INFO_LOG_LENGTH, &logSize);
	else
		glGetProgramiv(instance, GL_INFO_LOG_LENGTH, &logSize);
	if (!logSize)
	{
		err("segl: no log");
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
		err("File loading error %m");
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

static GLuint loadShader(GLenum shadertype, const char *shaderfile, const char *defaultshader)
{
	const GLchar* shaderSource = NULL;
	GLchar* shaderSourceDyn = NULL;
	GLuint shaderID = glCreateShader(shadertype);
	if (shaderID == 0)
		return 0;

	GLint shaderSize = 0;
	if (shaderfile)
	{
		shaderSize = readFile(shaderfile, &shaderSourceDyn);
		if (shaderSourceDyn == NULL)
			return 0;
		shaderSource = shaderSourceDyn;
		warn("segl: load dynamic shader %s", shaderfile);
		segl_dbg("segl: load dynamic shader:\n%s<=", shaderSource);
	}
	else
	{
		shaderSource = defaultshader;
		shaderSize = strlen(shaderSource);
		if (shaderSource == NULL)
			return 0;
		segl_dbg("load default shader:\n%s", shaderSource);
	}
	glShaderSource(shaderID, 1, (const GLchar**)(&shaderSource), &shaderSize);
	glCompileShader(shaderID);
	GLint compilationStatus = 0;
	if (shaderSourceDyn)
		free(shaderSourceDyn);

	glGetShaderiv(shaderID, GL_COMPILE_STATUS, &compilationStatus);
	if ( compilationStatus != GL_TRUE )
	{
		display_log(shaderID);
		return 0;
	}

	return shaderID;
}

static GLuint loadShaders(GLenum shadertype, const char *shaderfiles[MAX_SHADERS])
{
	GLuint shaderID = glCreateShader(shadertype);
	if (shaderID == 0)
		return 0;

	GLint nbShaderSources = 0;
	GLchar* shaderSources[MAX_SHADERS] = {0};
	GLint shaderSizes[MAX_SHADERS] = {0};

	for (int i = 0; i < MAX_SHADERS && shaderfiles[i]; i++)
	{
		shaderSizes[i] = readFile(shaderfiles[i], &shaderSources[i]);
		if (shaderSources[i] == NULL)
		{
			err("segl: shader %s not loaded", shaderfiles[i]);
			break;
		}
		warn("segl: load dynamic shader %s", shaderfiles[i]);
		segl_dbg("segl: load dynamic shader:\n%s<=", shaderSources[i]);
		nbShaderSources++;
	}
	glShaderSource(shaderID, nbShaderSources, (const char *const*)shaderSources, shaderSizes);
	glCompileShader(shaderID);
	GLint compilationStatus = 0;
	for (int i = 0; i < MAX_SHADERS && shaderSources[i]; i++)
	{
		free(shaderSources[i]);
	}
	glGetShaderiv(shaderID, GL_COMPILE_STATUS, &compilationStatus);
	if ( compilationStatus != GL_TRUE )
	{
		display_log(shaderID);
		return 0;
	}
	return shaderID;
}

static GLuint buildProgramm(const char *vertex, const char *fragments[MAX_SHADERS])
{
	GLint programState = 0;

	GLuint vertexID = loadShader(GL_VERTEX_SHADER, vertex, _defaultvertex);
	if ( vertexID == 0)
	{
		err("segl: vertex shader compilation error");
		return 0;
	}

	GLuint fragmentID = 0;
	if (fragments == NULL)
		fragmentID = loadShader(GL_FRAGMENT_SHADER, NULL, _defaultfragment);
	else if (fragments[1] == NULL)
		fragmentID = loadShader(GL_FRAGMENT_SHADER, fragments[0], _defaultfragment);
	else
		fragmentID = loadShaders(GL_FRAGMENT_SHADER, fragments);
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

static GLfloat *_movestatic(GLfloat * ctx)
{
	if (ctx == NULL)
	{
		ctx = calloc(16, sizeof(GLfloat));
		ctx[0] = ctx[5] = ctx[10] = ctx[15] = 1.0;
	}
	return ctx;
}
static GLfloat *(*_move)(GLfloat * ctx) = NULL;

static GLProgram_t *_glprog_create_controler(EGLConfig_Program_t *config, GLuint width, GLuint height)
{
	void *uniform_data = NULL;
	if (config)
	{
		size_t size = 0;
		for (GLProgram_Uniform_t *uniform = config->controls; uniform; uniform = uniform->next)
		{
			int usize = _glprog_uniform_size(uniform);
			if (usize > 0)
				size += usize;
		}
		if (size > 0)
		{
			int curdir = open(".", O_DIRECTORY);
			const char *keyname = "program.shm";
			if (config->name)
				keyname = config->name;
			if (mkdir(_segldir, 0) && errno != EEXIST)
				err("segl: programs directory creation error %m");
			int rootfd = open(_segldir, O_DIRECTORY);
			if (rootfd == -1)
				rootfd = AT_FDCWD;
			int ret = faccessat(rootfd, keyname, F_OK, AT_EACCESS);
			if (!ret)
			{
				if (unlinkat(rootfd, keyname, 0))
					err("segl: shm file access error %m");
			}
			int fd = openat(rootfd, keyname, O_CREAT|O_WRONLY|O_TRUNC, 0644);
			if (fd < 0)
				err("segl: shm file error %m");
			close(fd);
			int shmid = 0;
			key_t key;
			fchdir(rootfd);
			key = ftok(keyname, 'R');
			fchdir(curdir);
			close(curdir);
			close(rootfd);
			if (key == -1)
				err("segl: shm token error %m");
			uniform_data = (void *)-1;
			if (key != -1)
				shmid = shmget(key, size, IPC_CREAT|SHM_R|SHM_W);
			if (shmid > 0)
			{
				uniform_data = shmat(shmid, NULL, 0);
			}
			if (uniform_data == (void *)-1)
			{
				err("segl: memory allocation error %m");
				size = 0;
				uniform_data = NULL;
			}
		}
		off_t offset = 0;
		for (GLProgram_Uniform_t *uniform = config->controls; uniform_data && uniform && offset < size; uniform = uniform->next)
		{
			int size = _glprog_uniform_size(uniform);
			if (size > 0)
			{
				uniform->value = uniform_data + offset;
				uniform->type |= Uniform_SHARED_e;
				offset += size;
			}
		}
	}
	GLProgram_t *program = calloc(1, sizeof(*program));
	program->move = _move;
	program->config = config;
	program->controls_data = uniform_data;
	if (config)
		program->controls = config->controls;

	program->width = width;
	program->height = height;
	return program;
}

GLProgram_t *glprog_create_controler(EGLConfig_Program_t *config, GLuint width, GLuint height)
{
	GLProgram_t *program = _glprog_create_controler(config, width, height);
	if (config && config->next)
	{
		program->next = glprog_create_controler(config->next, width, height);
	}
	return program;
}

GLProgram_t *glprog_create(EGLConfig_Program_t *config, GLuint width, GLuint height)
{
	static int index = 1;
	GLuint programID = 0;
	warn("segl: GPU %s %s", glGetString(GL_VENDOR), glGetString(GL_RENDERER));
	warn("segl: %s", glGetString(GL_VERSION));
	warn("segl: %s", glGetString(GL_SHADING_LANGUAGE_VERSION));
	if (config)
		programID = buildProgramm(config->vertex, config->fragments);
	else
		programID = buildProgramm(NULL, NULL);
	if (!programID)
	{
		return NULL;
	}

	GLProgram_t *program = _glprog_create_controler(config, width, height);
	if (! program)
		return NULL;
	program->ID = programID;

	glEnable(GL_TEXTURE_EXTERNAL_OES);

	glViewport(0, 0, width, height);
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

	GLuint moveID = glGetUniformLocation(program->ID, "vMove");
	void *movectx = _movestatic(NULL);
	glUniformMatrix4fv(moveID, 1, GL_FALSE, movectx);
	free(movectx);
	if (program->move)
	{
		program->movectx = program->move(program->movectx);
		glUniformMatrix4fv(moveID, 1, GL_FALSE, program->movectx);
	}

	GLuint resolutionID = glGetUniformLocation(program->ID, "vResolution");
	glUniform4f(resolutionID, (GLfloat)program->width, (GLfloat)program->height, 1 / (GLfloat)program->width, 1 / (GLfloat)program->height);

	glBindVertexArrayOES(0);
	program->index = index++;
	if (config && config->next)
	{
		program->next = glprog_create(config->next, width, height);
	}
	return program;
}

static int _glbuffer_setframetexture(GLuint texture, GLenum textype, uint32_t width, uint32_t height)
{
	GLuint glerror = glGetError();
	glBindTexture(textype, texture);
	const FourccFormat_t *format = fourcc_getformat(FOURCC_XB24);
	glTexImage2D(textype, 0, format->internal, width, height, 0, format->full, format->data, NULL);
	glTexParameteri(textype, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(textype, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameterf(textype, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameterf(textype, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glerror = glGetError();
	if (glerror)
	{
		err ("segl: Texturebuffer error %#x", glerror);
		return -1;
	}

	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, textype, texture, 0);
	/* Sanity check. */
	GLint ret = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	if (ret != GL_FRAMEBUFFER_COMPLETE)
	{
		return -1;
	}
	return 0;
}

GL_Buffer_t *glbuffer_outtexture(uint32_t width, uint32_t height, const char *name)
{
	GLenum textype = GL_TEXTURE_2D;
	GLuint fbo;
	glGenFramebuffers(1, &fbo);
	if (fbo == 0)
	{
		err("segl: framebuffer unsupported");
		return NULL;
	}
	glEnable(textype);
	GLuint texture = 0;
	glGenTextures(1, &texture);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	if (_glbuffer_setframetexture(texture, textype, width, height))
	{
		err("segl: buffer %s out buffer error", name);
		return NULL;
	}
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	GL_Buffer_t *out = calloc(1, sizeof(*out));
	out->fbo = fbo;
	out->texture = texture;
	out->textype = textype;
	out->egltarget = EGL_GL_TEXTURE_2D;
	out->name = _defaultname;
	if (name)
		out->name = name;
	glBindFramebuffer(GL_FRAMEBUFFER, 0);

	return out;
}

EGLImage glbuffer_getimage(GL_Buffer_t *buffer, EGLDisplay egldisplay, EGLContext eglcontext)
{
	const EGLint tattributes[] = {
		EGL_IMAGE_PRESERVED, EGL_TRUE,
		EGL_NONE,
	};
	const EGLint *attributes = tattributes;

	/// eglCreateImage and eglCreateImageKHR have the same result
	EGLImage image = eglCreateImageKHR(egldisplay, eglcontext,
		buffer->egltarget, (void *)(long)buffer->texture, attributes);
	return image;
}

void glbuffer_destroy(GL_Buffer_t *buffer)
{
	glDeleteFramebuffers(1, &buffer->fbo);
	glDeleteTextures(1, &buffer->texture);
	free(buffer);
}

static int glprog_outtexture(GLProgram_t *program, GLenum textype)
{
	program->out = glbuffer_outtexture(program->width, program->height, program->config->name);
	if (program->out == NULL)
		return -1;
	return 0;
}

int glprog_setup(GLProgram_t *program, GL_Buffer_t *out)
{
	if (program->next)
	{
		if (glprog_outtexture(program, GL_TEXTURE_2D))
			return -1;
		return glprog_setup(program->next, out);
	}
	if (out)
	{
		program->out = out;
	}
	return 0;
}

GL_Buffer_t *gltexture_create(GLProgram_t *program, const char *name, const char *src, uint32_t fourcc)
{
	GLenum textype = GL_TEXTURE_2D;
	if (! strcmp("camera", src))
		textype = GL_TEXTURE_EXTERNAL_OES;
	GLuint dma_texture;
	glBindVertexArrayOES(program->vertexArrayID);
	glActiveTexture(GL_TEXTURE0);
	glGenTextures(1, &dma_texture);

	glBindTexture(textype, dma_texture);

	for (GLProgram_t *it = program; it != NULL; it = it->next)
	{
		it->fourcc = fourcc;
	}
	uint32_t width = program->width;
	uint32_t height = program->height;
	const FourccFormat_t *format = fourcc_getformat(fourcc);
	glTexImage2D(textype, 0, format->internal, width, height, 0, format->full, format->data, NULL);
	glTexParameteri(textype, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(textype, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri(textype, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(textype, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(textype, GL_TEXTURE_MAX_LEVEL_APPLE, 0);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	GL_Buffer_t *glbuffer = calloc(1, sizeof(*glbuffer));
	glbuffer->texture = dma_texture;
	glbuffer->textype = textype;

	glbuffer->name = name;
	if (program->config && program->config->input.name)
		glbuffer->name = program->config->input.name;
	glbuffer->loc = glGetUniformLocation(program->ID, glbuffer->name);
	glbuffer->unit = 0;
	glUniform1i(glbuffer->loc, glbuffer->unit);

	glBindVertexArrayOES(0);
	return glbuffer;
}

void gltexture_attach(GL_Buffer_t *glbuffer, EGLImageKHR image)
{
	glEGLImageTargetTexture2DOES(glbuffer->textype, image);
}

GLuint gltexture_id(GL_Buffer_t *glbuffer)
{
	return glbuffer->texture;
}

void gltexture_destroy(GL_Buffer_t *glbuffer)
{
	free(glbuffer);
}

static int _glprog_run(GLProgram_t *program, GL_Buffer_t *buffer, GLProgram_t *prevprog)
{
	GLenum error = 0;
	glUseProgram(program->ID);
	error = glGetError();

	if (program->out)
	{
		glBindFramebuffer(GL_FRAMEBUFFER, program->out->fbo);
        	GLenum error = glCheckFramebufferStatus(GL_FRAMEBUFFER);
		if (error != GL_FRAMEBUFFER_COMPLETE)
			err("segl: framebuffer incomplet: %#xn", error);
	}
	glClear(GL_COLOR_BUFFER_BIT);

	glBindVertexArrayOES(program->vertexArrayID);

	glActiveTexture(GL_TEXTURE0 + buffer->unit);
	glBindTexture(buffer->textype, buffer->texture);
	glUniform1i(buffer->loc, buffer->unit);

	if (prevprog)
	{
		glActiveTexture(GL_TEXTURE0 + prevprog->index);
		glBindTexture(prevprog->out->textype, prevprog->out->texture);
		if (prevprog->out->loc)
			glUniform1i(prevprog->out->loc, prevprog->index);
	}

	if (program->move)
	{
		GLuint moveID = glGetUniformLocation(program->ID, "vMove");
		glUniformMatrix4fv(moveID, 1, GL_FALSE, program->move(program->movectx));
	}
	for (GLProgram_Uniform_t *uniform = program->controls; uniform; uniform = uniform->next)
	{
		glprog_setuniform(program, uniform);
	}

	glDrawArrays(GL_TRIANGLES, 0, 6);
	error = glGetError();
	if (error != GL_NO_ERROR)
		err("segl: %s running error %#x", program->config->name, error);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glBindTexture(buffer->textype, 0);
	if (program->next)
	{
		return _glprog_run(program->next, buffer, program);
	}
	return 0;
}

int glprog_run(GLProgram_t *program, GL_Buffer_t *buffer)
{
	return _glprog_run(program, buffer, NULL);
}

void glprog_stop(GLProgram_t *program, GL_Buffer_t *buffer)
{
	glUseProgram(0);
	glBindTexture(buffer->textype, 0);
	glBindTexture(GL_TEXTURE_2D, 0);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

int glprog_setuniform(GLProgram_t *program, GLProgram_Uniform_t *uniform)
{
	if (!uniform->loc)
		uniform->loc = glGetUniformLocation(program->ID, uniform->name);
	switch (uniform->type & ~Uniform_SHARED_e)
	{
	case Uniform_FLOAT_e:
	{
		glUniform1f(uniform->loc, *(GLfloat*)uniform->value);
	}
	break;
	case Uniform_INT_e:
	{
		glUniform1i(uniform->loc, *(GLint*)uniform->value);
	}
	break;
	case Uniform_FVEC2_e:
	{
		glUniform2f(uniform->loc, ((GLfloat*)uniform->value)[0],
						((GLfloat*)uniform->value)[1]);
	}
	break;
	case Uniform_FVEC3_e:
	{
		glUniform3f(uniform->loc, ((GLfloat*)uniform->value)[0],
						((GLfloat*)uniform->value)[1],
						((GLfloat*)uniform->value)[2]);
	}
	break;
	case Uniform_FVEC4_e:
	{
		glUniform4f(uniform->loc, ((GLfloat*)uniform->value)[0],
						((GLfloat*)uniform->value)[1],
						((GLfloat*)uniform->value)[2],
						((GLfloat*)uniform->value)[3]);
	}
	break;
	case Uniform_IVEC2_e:
	{
		glUniform2i(uniform->loc, ((GLint*)uniform->value)[0],
						((GLint*)uniform->value)[1]);
	}
	break;
	case Uniform_IVEC3_e:
	{
		GLint loc = glGetUniformLocation(program->ID, uniform->name);
		glUniform3i(loc, ((GLint*)uniform->value)[0],
						((GLint*)uniform->value)[1],
						((GLint*)uniform->value)[2]);
	}
	break;
	case Uniform_IVEC4_e:
	{
		glUniform4i(uniform->loc, ((GLint*)uniform->value)[0],
						((GLint*)uniform->value)[1],
						((GLint*)uniform->value)[2],
						((GLint*)uniform->value)[3]);
	}
	break;
	case Uniform_MAT2_e:
	{
		glUniformMatrix2fv(uniform->loc, 1, GL_FALSE, uniform->value);
	}
	break;
	case Uniform_MAT3_e:
	{
		glUniformMatrix3fv(uniform->loc, 1, GL_FALSE, uniform->value);
	}
	break;
	case Uniform_MAT4_e:
	{
		glUniformMatrix4fv(uniform->loc, 1, GL_FALSE, uniform->value);
	}
	break;
	case Uniform_FUNC_e:
	{
		GLfloat (*func)(GLProgram_Uniform_t *uniform) = uniform->value;
		glUniform1f(uniform->loc, func(uniform));
	}
	break;
	default:
		err("segl: Uniform type invalid");
		return -1;
	}
	return 0;
}

void glprog_destroy(GLProgram_t *program)
{
	if (program->next)
		return glprog_destroy(program->next);
	if (program->out)
	{
		glbuffer_destroy(program->out);
	}
	free(program->config);
	GLProgram_Uniform_t *next;
	for (GLProgram_Uniform_t *uniform = program->controls; uniform; uniform = next)
	{
		next = uniform->next;
		_glprog_uniform_destroy(uniform);
	}
	if (program->controls_data)
		shmdt(program->controls_data);
	free(program);
}

typedef GLfloat (*GLProgram_Uniform_func_t)(GLProgram_Uniform_t *uniform);
static GLfloat _time(GLProgram_Uniform_t *uniform)
{
	int t = time(NULL);
	int t0 = (int)(long)uniform->data;
	if (t0 == 0)
		uniform->data = (void*)(long)t;
	return (GLfloat)(t - t0);
}

static GLfloat _frame(GLProgram_Uniform_t *uniform)
{
	int f = (int)(long)uniform->data;
	uniform->data = (void*)(long)++f;
	return (GLfloat)f;
}

#ifdef HAVE_JANSSON
#include <jansson.h>

static void _glprog_uniform_setarray(GLProgram_Uniform_t *uniform, json_t *jvalue, unsigned char nbentries, Uniform_Type_e type)
{
	if (!uniform->value && type == Uniform_FLOAT_e)
	{
		err("segl: %s memory allocation error", uniform->name);
		uniform->value = calloc(nbentries, sizeof(GLfloat));
	}
	if (!uniform->value && type == Uniform_INT_e)
	{
		err("segl: %s memory allocation error", uniform->name);
		uniform->value = calloc(nbentries, sizeof(GLint));
	}
	GLfloat *fvalues = uniform->value;
	GLint *ivalues = uniform->value;
	for (int i = 0; i < nbentries; i++)
	{
		json_t *entry = json_array_get(jvalue, i);
		if (type == Uniform_FLOAT_e)
			fvalues[i] = json_real_value(entry);
		if (type == Uniform_INT_e)
			ivalues[i] = json_integer_value(entry);
	}
}

static int _glprog_uniform_size(GLProgram_Uniform_t *uniform)
{
	int ret = -1;
	switch (uniform->type & ~Uniform_SHARED_e)
	{
	case Uniform_INT_e:
		ret = sizeof(GLint);
	break;
	case Uniform_FLOAT_e:
		ret = sizeof(GLfloat);
	break;
	case Uniform_FVEC2_e:
		ret = sizeof(GLfloat) * 2;
	break;
	case Uniform_FVEC3_e:
		ret = sizeof(GLfloat) * 3;
	break;
	case Uniform_FVEC4_e:
		ret = sizeof(GLfloat) * 4;
	break;
	case Uniform_IVEC2_e:
		ret = sizeof(GLint) * 2;
	break;
	case Uniform_IVEC3_e:
		ret = sizeof(GLint) * 3;
	break;
	case Uniform_IVEC4_e:
		ret = sizeof(GLint) * 4;
	break;
	case Uniform_MAT2_e:
		ret = sizeof(GLfloat) * 2 * 2;
	break;
	case Uniform_MAT3_e:
		ret = sizeof(GLfloat) * 3 * 3;
	break;
	case Uniform_MAT4_e:
		ret = sizeof(GLfloat) * 4 * 4;
	break;
	default:
		ret = -1;
	break;
	}
	return ret;
}

static int _glprog_uniform_setvalue(GLProgram_Uniform_t *uniform, json_t *jvalue)
{
	int ret = -1;
	if (!uniform->value)
	{
		err("segl: %s memory allocation error", uniform->name);
		int size = _glprog_uniform_size(uniform);
		if (size > 0)
			uniform->value = malloc(size);
	}
	if (jvalue && json_is_number(jvalue))
	{
		switch (uniform->type & ~Uniform_SHARED_e)
		{
		case Uniform_INT_e:
			*(GLint *)uniform->value = json_integer_value(jvalue);
			ret = 0;
		break;
		case Uniform_FLOAT_e:
			*(GLfloat *)uniform->value = json_real_value(jvalue);
			ret = 0;
		break;
		default:
			err("segl: settings mal formatted");
		}
	}
	if (jvalue && json_is_array(jvalue))
	{
		switch (uniform->type & ~Uniform_SHARED_e)
		{
		case Uniform_FVEC2_e:
			_glprog_uniform_setarray(uniform, jvalue, 2, Uniform_FLOAT_e);
			ret = 0;
		break;
		case Uniform_FVEC3_e:
			_glprog_uniform_setarray(uniform, jvalue, 3, Uniform_FLOAT_e);
			ret = 0;
		break;
		case Uniform_FVEC4_e:
			_glprog_uniform_setarray(uniform, jvalue, 4, Uniform_FLOAT_e);
			ret = 0;
		break;
		case Uniform_IVEC2_e:
			_glprog_uniform_setarray(uniform, jvalue, 2, Uniform_INT_e);
			ret = 0;
		break;
		case Uniform_IVEC3_e:
			_glprog_uniform_setarray(uniform, jvalue, 3, Uniform_INT_e);
			ret = 0;
		break;
		case Uniform_IVEC4_e:
			_glprog_uniform_setarray(uniform, jvalue, 4, Uniform_INT_e);
			ret = 0;
		break;
		case Uniform_MAT2_e:
			_glprog_uniform_setarray(uniform, jvalue, 2 * 2, Uniform_FLOAT_e);
			ret = 0;
		break;
		case Uniform_MAT3_e:
			_glprog_uniform_setarray(uniform, jvalue, 3 * 3, Uniform_FLOAT_e);
			ret = 0;
		break;
		case Uniform_MAT4_e:
			_glprog_uniform_setarray(uniform, jvalue, 4 * 4, Uniform_FLOAT_e);
			ret = 0;
		break;
		default:
		break;
		}
	}
	return ret;
}

static GLProgram_Uniform_t * _glprog_uniform_create(void *setting)
{
	json_t *jsetting = setting;
	GLProgram_Uniform_t *uniform = calloc(1, sizeof(*uniform));
	json_t *jname = json_object_get(jsetting, "name");
	if (jname && json_is_string(jname))
	{
		uniform->name = json_string_value(jname);
	}
	json_t *jtype = json_object_get(jsetting, "type");
	uniform->type = Uniform_UNKNOWN_e;
	if (jtype && json_is_string(jtype))
	{
		const char *value = json_string_value(jtype);
		if (!strcmp(value, "int"))
			uniform->type = Uniform_INT_e;
		else if (!strcmp(value, "float"))
			uniform->type = Uniform_FLOAT_e;
		else if (!strcmp(value, "vec2"))
			uniform->type = Uniform_FVEC2_e;
		else if (!strcmp(value, "vec3"))
			uniform->type = Uniform_FVEC3_e;
		else if (!strcmp(value, "vec4"))
			uniform->type = Uniform_FVEC4_e;
		else if (!strcmp(value, "ivec2"))
			uniform->type = Uniform_IVEC2_e;
		else if (!strcmp(value, "ivec3"))
			uniform->type = Uniform_IVEC3_e;
		else if (!strcmp(value, "ivec4"))
			uniform->type = Uniform_IVEC4_e;
		else if (!strcmp(value, "mat2"))
			uniform->type = Uniform_MAT2_e;
		else if (!strcmp(value, "mat3"))
			uniform->type = Uniform_MAT3_e;
		else if (!strcmp(value, "mat4"))
			uniform->type = Uniform_MAT4_e;
		else if (!strcmp(value, "func"))
		{
			uniform->type = Uniform_FUNC_e;
			/// function are not modifiable with setting
			json_t *jvalue = json_object_get(jsetting, "value");
			if (jvalue && json_is_string(jvalue))
			{
				const char *value = NULL;
				value = json_string_value(jvalue);
				if (!strncasecmp(value, "time", 4))
					uniform->value = _time;
				else if (!strncasecmp(value, "frames", 6))
					uniform->value = _frame;
			}
		}
	}
	if (uniform->type == Uniform_UNKNOWN_e)
	{
		free(uniform);
		uniform = NULL;
	}
	return uniform;
}

static int _glprog_setcontrols(GLProgram_t *program, json_t *jsettings)
{
	if (jsettings && json_is_array(jsettings))
	{
		json_t *jsetting = NULL;
		int i = 0;
		json_array_foreach(jsettings, i, jsetting)
		{
			json_t *jname = json_object_get(jsetting, "name");
			if (!jname || ! json_is_string(jname))
			{
				continue;
			}
			for (GLProgram_Uniform_t *uniform = program->controls; uniform; uniform = uniform->next)
			{
				if (!strcasecmp(json_string_value(jname), uniform->name))
				{
					json_t *jvalue = json_object_get(jsetting, "value");
					_glprog_uniform_setvalue(uniform, jvalue);
				}
			}
		}
	}
	else if (jsettings && json_is_object(jsettings))
	{
		json_t *jname = json_object_get(jsettings, "name");
		if (jname && json_is_string(jname))
		{
			for (GLProgram_Uniform_t *uniform = program->controls; uniform; uniform = uniform->next)
			{
				if (!strcasecmp(json_string_value(jname), uniform->name))
				{
					json_t *jvalue = json_object_get(jsettings, "value");
					_glprog_uniform_setvalue(uniform, jvalue);
				}
			}
		}
	}
	return 0;
}

static int _glprog_loadjsonsetting(GLProgram_t *programs, json_t *jprogram)
{
	int ret = -1;
	for (GLProgram_t *program = programs; program; program = program->next)
	{
		json_t *jname = json_object_get(jprogram, "name");
		const EGLConfig_Program_t *config = program->config;
		if (config && config->name && jname && json_is_string(jname) &&
			strcasecmp(json_string_value(jname), config->name) != 0)
		{
			continue;
		}
		json_t *jcontrols = json_object_get(jprogram, "controls");
		ret = _glprog_setcontrols(program, jcontrols);
	}
	return ret;
}

int glprog_loadjsonsetting(GLProgram_t *programs, void *entry)
{
	int ret = -1;
	json_t *jsetting = entry;
	if (jsetting && json_is_array(jsetting))
	{
		int index;
		json_t *jprogram;
		json_array_foreach(jsetting, index, jprogram)
		{
			_glprog_loadjsonsetting(programs, jprogram);
		}
	}
	if (jsetting && json_is_object(jsetting))
	{
		_glprog_loadjsonsetting(programs, jsetting);
	}
	return ret;
}

static int _glprog_loadjsontexture(EGLConfig_Program_t *config, json_t *texture)
{
	json_t *source = NULL;
	if (texture && json_is_object(texture))
	{
		texture = json_object_get(texture, "name");
		source = json_object_get(texture, "src");
	}
	if (source && json_is_string(source))
	{
		const char *value = json_string_value(source);
		config->input.src = value;
	}
	if (texture && json_is_string(texture))
	{
		const char *value = json_string_value(texture);
		config->input.name = value;
		if (!source)
			config->input.src = value;
	}
	return 0;
}

static int _glprog_loadjsonconfiguration(EGLConfig_Program_t *config, json_t *jconfig)
{
	json_t *disable = json_object_get(jconfig, "disable");
	if (disable && json_is_boolean(disable) && json_is_true(disable))
	{
		return -1;
	}
	json_t *name = json_object_get(jconfig, "name");
	if (name && json_is_string(name))
	{
		const char *value = json_string_value(name);
		config->name = value;
	}
	json_t *texture = json_object_get(jconfig, "texture");
	if (texture)
		_glprog_loadjsontexture(config, texture);
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
	json_t *controls = json_object_get(jconfig, "controls");
	if (controls && json_is_array(controls))
	{
		json_t *control = NULL;
		int i = 0;
		json_array_foreach(controls, i, control)
		{
			GLProgram_Uniform_t *uniform = _glprog_uniform_create(control);
			if (uniform)
			{
				uniform->next = config->controls;
				config->controls = uniform;
			}
		}
	}
	else if (controls && json_is_object(controls))
	{
		GLProgram_Uniform_t *uniform = _glprog_uniform_create(controls);
		if (uniform)
		{
			uniform->next = config->controls;
			config->controls = uniform;
		}
	}
	return 0;
}

int glprog_loadjsonconfiguration(void *arg, void *entry)
{
	// This will inverse the list of programs before usage
	EGLConfig_Program_t *first = NULL;
	EGLConfig_Program_t *previous = NULL;
	json_t *jconfig = entry;
	if (jconfig && json_is_array(jconfig))
	{
		json_t *jfield = NULL;
		int i = 0;
		json_array_foreach(jconfig, i, jfield)
		{
			EGLConfig_Program_t *config = calloc(1, sizeof(*config));
			if (_glprog_loadjsonconfiguration(config, jfield))
			{
				free(config);
				continue;
			}
			if (first == NULL)
				first = config;
			if (previous)
				previous->next = config;
			previous = config;
			config->type = _gles2_ops.name;
		}
	}
	else if (jconfig && json_is_object(jconfig))
	{
		EGLConfig_Program_t *config = calloc(1, sizeof(*first));
		if(_glprog_loadjsonconfiguration(config, jconfig))
		{
			free(config);
		}
		else
		{
			first = config;
			config->name = _gles2_ops.name;
		}
	}
	if (arg != NULL)
	{
		EGLConfig_Program_t **prgconfig = arg;
		*prgconfig = first;
	}
	return 0;
}
#endif

static void _glprog_uniform_destroy(GLProgram_Uniform_t *uniform)
{
	if (!(uniform->type & Uniform_SHARED_e))
	{
		switch (uniform->type)
		{
			case Uniform_FUNC_e:
			break;
			default:
				free(uniform->value);
		}
	}
	free(uniform);
}

static EGLProg_ops_t _gles2_ops = {
	.name = "gles2",
	.create = glprog_create,
	.create_controler = glprog_create_controler,
	.setup = glprog_setup,
	.buffer = {
		.create = gltexture_create,
		.attach = gltexture_attach,
		.id = gltexture_id,
		.destroy = gltexture_destroy,
	},
	.run = glprog_run,
	.stop = glprog_stop,
	.setuniform = glprog_setuniform,
	.destroy = glprog_destroy,
	.loadjsonsetting = glprog_loadjsonsetting,
	.loadjsonconfiguration = glprog_loadjsonconfiguration,
};

#include <dlfcn.h>

static void __attribute__ ((constructor)) segl_init()
{
	_egl_initprototypes();

	segl_program_ops_append_t _segl_program_ops_append;
	void *hdl = dlopen(NULL, RTLD_NOW);
	_segl_program_ops_append = dlsym(hdl, "segl_program_ops_append");
	if (_segl_program_ops_append)
	{
		_segl_program_ops_append(&_gles2_ops);
	}
}
