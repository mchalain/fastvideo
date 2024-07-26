#include <string.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include "segl.h"
#include "log.h"

typedef struct GLProgram_s GLProgram_t;
struct GLProgram_s
{
	EGLConfig_Program_t *config;
	GLuint ID;
	GLuint vertexArrayID;
	GLuint vertexBufferObject[3];
	const char *in_texturename;
	GLenum in_textype;
	GLBuffer_t *in_textures;
	GLBuffer_t out_textures[MAX_BUFFERS];
	GLuint fbID;
	GLfloat width;
	GLfloat height;
};

const GLchar *defaulttexturename = "vTexture";

//#define GLSLV300

#ifdef GLSLV300
static const GLchar defaultvertex[] = "#version 300 es \n\
layout(location = 0) in vec3 vPosition;\n\
out vec2 texUV;\n\
\n\
void main (void)\n\
{\n\
	gl_Position = vec4(vPosition, 1);\n\
	texUV = (vec2(0.5, 0.5) - vPosition.xy / 2.0);\n\
}\n\
";
static const GLchar defaultfragment[] = "#version 300 es\n\
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
static const GLchar defaultvertex[] = ""
"\n""attribute vec3 vPosition;"
"\n""varying vec2 texUV;"
"\n"
"\n""void main (void)"
"\n""{"
"\n""	texUV = vec2(0.5 + vPosition.x / 2.0, 0.5 - vPosition.y / 2.0);"
"\n""	gl_Position = vec4(vPosition,1.);"
"\n""}"
"\n";
static const GLchar defaultfragment[] = ""
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

PFNGLBINDVERTEXARRAYOESPROC glBindVertexArrayOES = NULL;
PFNGLGENVERTEXARRAYSOESPROC glGenVertexArraysOES = NULL;

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

static GLuint loadShader(GLenum shadertype, const char *shaderfile, const char *defaultshader)
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
	if (shaderSource != defaultshader)
		free(shaderSource);

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

static GLuint buildProgramm(const char *vertex, const char *fragments[MAX_SHADERS])
{
	GLchar* vertexSource = NULL;
	GLchar* fragmentSource = NULL;
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

GLProgram_t *glprog_create(EGLConfig_Program_t *config)
{
	GLuint programID = 0;
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
	program->in_texturename = defaulttexturename;
	program->config = config;
	if (config && config->tex_name)
		program->in_texturename = config->tex_name;

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

	return program;
}

int glprog_setup(GLProgram_t *program, GLuint width, GLuint height)
{
	program->width = width;
	program->height = height;

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

	GLuint texid;
	GLint texMap = glGetUniformLocation(program->ID, program->in_texturename);
	glUniform1i(texMap, 0); // GL_TEXTURE0
	glActiveTexture(GL_TEXTURE0);

	GLuint resolutionID = glGetUniformLocation(program->ID, "vResolution");
	glUniform2f(resolutionID, program->width, program->height);

	glBindVertexArrayOES(0);
	return 0;
}

GLBuffer_t *glprog_getouttexture(GLProgram_t *program, GLuint nbtex)
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

int glprog_setintexture(GLProgram_t *program, GLenum type, GLuint nbtex, GLBuffer_t *in_textures)
{
	program->in_textures = in_textures;
	program->in_textype = type;
}

int glprog_run(GLProgram_t *program, int bufid)
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
			err("framebuffer %u incomplet %#x", program->fbID, status);
			//return -1;
		}
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, 0, 0, 0);
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
	}

	return 0;
}

void glprog_destroy(GLProgram_t *program)
{
	if (program->fbID)
	{
		glDeleteFramebuffers(1, &program->fbID);
		for (int i = 0; i < MAX_BUFFERS; i++)
		{
			glDeleteTextures(1, &program->out_textures[i].dma_texture);
		}
	}
	free(program->config);
	free(program);
}

#ifdef HAVE_JANSSON
#include <jansson.h>

int _glprog_loadjsonconfiguration(EGLConfig_Program_t *config, json_t *jconfig)
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
			if (first == NULL)
				first = config;
			if (previous)
				previous->next = config;
			previous = config;
			_glprog_loadjsonconfiguration(config, jfield);
		}
	}
	else if (jconfig && json_is_object(jconfig))
	{
		first = calloc(1, sizeof(*first));
		_glprog_loadjsonconfiguration(first, jconfig);
	}
	if (arg != NULL)
	{
		EGLConfig_Program_t **prgconfig = arg;
		*prgconfig = first;
	}
	return 0;
}
#endif
