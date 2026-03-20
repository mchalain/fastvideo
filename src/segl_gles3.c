#include <string.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES3/gl3ext.h>

#include "segl.h"
#include "segl_engine.h"
#include "log.h"

#define segl_dbg(...)

/// allow to write output into a external framebuffer
#define GLES3_EXTERNALOUTPUT 0
/// allow to make a daisy chain of several programs
#define GLES3_MULTIPROGRAMS 0

struct GL_Buffer_s
{
	const char *name;
	GLint id;
	GLint loc;
	EGLImageKHR image;
	GLuint fbo;
	GLuint texture;
	int current;
	GLenum textype;
	EGLint egltarget;
};

static EGLProg_ops_t gles3_ops;

typedef struct GLProgram_s GLProgram_t;
struct GLProgram_s
{
	GLProgram_t *next;
	EGLConfig_Program_t *config;
	GLuint ID;
	GLuint vertexArrayID;
	GLuint vertexBufferObject[3];
	const char *in_texturename;
	GL_Buffer_t *out;
	GLuint fbo;
	uint32_t width;
	uint32_t height;
	uint32_t fourcc;
	GLProgram_Uniform_t *controls;
};

static const GLchar *_defaulttexturename = "vTexture";
#if GLES3_EXTERNALOUTPUT
static const GLchar *_defaultout = "out";
#endif

/* Vertex shader GLSL 3.00 es */
static const GLchar defaultvertex[] =
"#version 300 es\n"
"layout(location = 0) in vec3 vPosition;\n"
"out vec2 texUV;\n"
"\n"
"void main (void)\n"
"{\n"
"	gl_Position = vec4(vPosition, 1);\n"
"	texUV = vec2(0.5 - vPosition.x / 2.0, 0.5 - vPosition.y / 2.0);\n"
"}\n";

/* Fragment shader GLSL 3.00 es — uses GL_OES_EGL_image_external_essl3 */
static const GLchar defaultfragment[] =
"#version 300 es\n"
"#extension GL_OES_EGL_image_external_essl3 : require\n"
"precision mediump float;\n"
"uniform samplerExternalOES vTexture;\n"
"in vec2 texUV;\n"
"out vec4 fragColor;\n"
"\n"
"void main() {\n"
"	fragColor = texture(vTexture, texUV);\n"
"}\n";

#ifndef GL_TEXTURE_EXTERNAL_OES
#define GL_TEXTURE_EXTERNAL_OES 0x8D65
#endif

/* glEGLImageTargetTexture2DOES is an OES extension — must be loaded manually */
#ifndef GL_OES_EGL_image
typedef void *GLeglImageOES;
#endif
typedef void (*PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)(GLenum target, GLeglImageOES image);

#ifndef EGL_EGLEXT_PROTOTYPES
/* glBindVertexArray / glGenVertexArrays are core in ES3 — no OES suffix needed */
static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES = NULL;

static int _egl_initprototypes(void)
{
	glEGLImageTargetTexture2DOES = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)
		eglGetProcAddress("glEGLImageTargetTexture2DOES");
	if (glEGLImageTargetTexture2DOES == NULL)
	{
		err("segl: glEGLImageTargetTexture2DOES not available");
		return -1;
	}
	return 0;
}
#else
#define _egl_initprototypes(...)
#endif

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
		segl_dbg("selg: load dynamic shader:\n%s<=", shaderSource);
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

	GLuint vertexID = loadShader(GL_VERTEX_SHADER, vertex, defaultvertex);
	if ( vertexID == 0)
	{
		err("segl: vertex shader compilation error");
		return 0;
	}

	GLuint fragmentID = 0;
	if (fragments == NULL)
		fragmentID = loadShader(GL_FRAGMENT_SHADER, NULL, defaultfragment);
	else if (fragments[1] == NULL)
		fragmentID = loadShader(GL_FRAGMENT_SHADER, fragments[0], defaultfragment);
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

static GLProgram_t *glprog_create(EGLConfig_Program_t *config, uint32_t width, uint32_t height)
{
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

	GLProgram_t *program = calloc(1, sizeof(*program));
	program->ID = programID;
	program->in_texturename = _defaulttexturename;
	program->config = config;
	if (config)
		program->controls = config->controls;
	if (config && config->input.name)
		program->in_texturename = config->input.name;

	program->width = width;
	program->height = height;

	glViewport(0, 0, width, height);
	glUseProgram(program->ID);

	/* glGenVertexArrays / glBindVertexArray are core in ES3 — no OES suffix */
	glGenVertexArrays(1, &program->vertexArrayID);
	glBindVertexArray(program->vertexArrayID);

	glGenBuffers(1, program->vertexBufferObject);

	GLfloat vertices[] = {
		-1.0f,  1.0f,  0.0f, // top left
		-1.0f, -1.0f,  0.0f, // bottom left
		 1.0f,  1.0f,  0.0f, // top right
		 1.0f, -1.0f,  0.0f, // bottom right
	};
	glBindBuffer(GL_ARRAY_BUFFER, program->vertexBufferObject[0]);
	glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

	GLint pos = glGetAttribLocation(program->ID, "vPosition");
	glEnableVertexAttribArray(pos);
	glVertexAttribPointer(pos, 3, GL_FLOAT, GL_FALSE, 0, 0);

	GLint texMap = glGetUniformLocation(program->ID, program->in_texturename);
	glUniform1i(texMap, 0); // GL_TEXTURE0
	glActiveTexture(GL_TEXTURE0);

	GLuint resolutionID = glGetUniformLocation(program->ID, "vResolution");
	glUniform4f(resolutionID, (GLfloat)program->width, (GLfloat)program->height, 1 / (GLfloat)program->width, 1 / (GLfloat)program->height);

	GLProgram_Uniform_t *uniform = program->controls;
	while (uniform)
	{
		GLProgram_Uniform_t *next = uniform->next;
		glprog_setuniform(program, uniform);
		uniform = next;
	}

	glBindVertexArray(0);
#if GLES3_MULTIPROGRAMS
	if (config && config->next)
	{
		program->next = glprog_create(config->next, width, height);
	}
#endif
	return program;
}

#if GLES3_EXTERNALOUTPUT
static GL_Buffer_t *glprog_outtexture(GLProgram_t *program, const char *name)
{
	GLenum textype = GL_TEXTURE_2D;
	GLuint fbo;
	glGenFramebuffers(1, &fbo);
	if (fbo == 0)
	{
		err("segl: framebuffer unsupported");
		return NULL;
	}
	glBindFramebuffer(GL_FRAMEBUFFER, program->fbo);
	/* glEnable(textype) is not valid for texture targets in ES3 — removed */
	GLuint texture = 0;
	glGenTextures(1, &texture);
	glBindTexture(textype, texture);
	// The format must be RGB. RGBA generate error during the texture attachment to the frambuffer (glprog_run)
	glTexImage2D(textype, 0, GL_RGB, program->width, program->height, 0, GL_RGB,  GL_UNSIGNED_BYTE, NULL);
	glTexParameteri(textype, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(textype, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameterf(textype, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameterf(textype, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	GL_Buffer_t *out = calloc(1, sizeof(*out));
	program->out->texture = texture;
	program->out->textype = textype;
	program->out->fbo = fbo;
	out->egltarget = EGL_GL_TEXTURE_2D;
	out->name = name;

	return out;
}
#endif

static int glprog_setup(GLProgram_t *program, GL_Buffer_t *out)
{
#if GLES3_MULTIPROGRAMS
	if (program->next)
	{
		if (program->config)
			program->out = glprog_outtexture(program, program->config->name);
		else
			program->out = glprog_outtexture(program, _defaultout);
		if (program->out)
			return -1;
		return glprog_setup(program->next, out);
	}
#endif
	glBindVertexArray(program->vertexArrayID);
	if (out)
	{
		program->out = out;
	}
	return 0;
}

static GL_Buffer_t *gltexture_create(GLProgram_t *program, const char *name, const char *src)
{
	GLenum textype = GL_TEXTURE_EXTERNAL_OES;
	GLuint texture;
	glGenTextures(1, &texture);

	glBindTexture(textype, texture);

	glTexParameteri(textype, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(textype, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri(textype, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(textype, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	GL_Buffer_t *glbuffer = calloc(1, sizeof(*glbuffer));
	glbuffer->texture = texture;
	glbuffer->textype = textype;
	glbuffer->name = name;

	if (program->config && program->config->input.name)
		glbuffer->name = program->config->input.name;
	glbuffer->loc = glGetUniformLocation(program->ID, glbuffer->name);
	glbuffer->id = 0;
	glUniform1i(glbuffer->loc, glbuffer->id);

	return glbuffer;
}

static void gltexture_attach(GL_Buffer_t *glbuffer, EGLImageKHR image)
{
	glEGLImageTargetTexture2DOES(glbuffer->textype, image);
}

static uint32_t gltexture_id(GL_Buffer_t *glbuffer)
{
	return glbuffer->texture;
}

static void gltexture_destroy(GL_Buffer_t *glbuffer)
{
	free(glbuffer);
}

static int glprog_run(GLProgram_t *program, GL_Buffer_t *buffer)
{
#if GLES3_EXTERNALOUTPUT
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	if (program->out)
	{
		glBindFramebuffer(GL_FRAMEBUFFER, program->fbo);
		glBindTexture(program->out->textype, program->out->texture);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, program->out->textype,
					program->out->texture, 0);
		err = glGetError();
		if (err != GL_NO_ERROR)
		{
			err("segl: program[%s] Framebuffer access error %#x", program->config->name, err);
		}
	}
	else
#endif
		glClear(GL_COLOR_BUFFER_BIT);

	glUseProgram(program->ID);

	glBindTexture(buffer->textype, buffer->texture);

	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

#if GLES3_EXTERNALOUTPUT
	if (program->out)
	{
		GLint status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
		if (status != GL_FRAMEBUFFER_COMPLETE)
		{
			err("framebuffer %u incomplet %#x", program->out->fbo, status);
			//return -1;
		}
		//glFramebufferTexture2D to disable the texture is an invalid operation
	}
#if GLES3_MULTIPROGRAMS
	if (program->next)
	{
		return glprog_run(program->next, program->out);
	}
#endif
#endif
	return 0;
}

static void glprog_stop(GLProgram_t *program, GL_Buffer_t *buffer)
{
	glUseProgram(0);
	glBindTexture(buffer->textype, 0);
	glBindTexture(GL_TEXTURE_2D, 0);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

static void _glprog_uniform_destroy(GLProgram_Uniform_t *uniform)
{
	switch (uniform->type)
	{
		default:
			free(uniform->value);
	}
	free(uniform);
}

static void glprog_destroy(GLProgram_t *program)
{
#if GLES3_EXTERNALOUTPUT
#if GLES3_MULTIPROGRAMS
	if (program->next)
		return glprog_destroy(program->next);
#endif
	if (program->out)
	{
		glDeleteFramebuffers(1, &program->out->fbo);
		glDeleteTextures(1, &program->out->texture);
		free(program->out);
	}
#endif
	free(program->config);
	GLProgram_Uniform_t *uniform = program->controls;
	while (uniform)
	{
		GLProgram_Uniform_t *next = uniform->next;
		_glprog_uniform_destroy(uniform);
		uniform = next;
	}
	free(program->config);
	free(program);
}

static EGLProg_ops_t gles3_ops = {
	.name = "gles3",
	.create = glprog_create,
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
	.loadjsonsetting = NULL,
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
		_segl_program_ops_append(&gles3_ops);
	}
}
