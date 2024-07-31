#include <string.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include "segl.h"
#include "log.h"

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
} Uniform_Type_e;

typedef struct GLProgram_Uniform_s GLProgram_Uniform_t;
struct GLProgram_Uniform_s
{
	const char *name;
	Uniform_Type_e type;
	void *value;
	GLProgram_Uniform_t *next;
};

static GLProgram_Uniform_t * _glprog_uniform_create(void *setting);
static void _glprog_uniform_destroy(GLProgram_Uniform_t *uniform);

typedef struct GLProgram_s GLProgram_t;
struct GLProgram_s
{
	GLProgram_t *next;
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
	GLProgram_Uniform_t *controls;
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
"\n""	texUV = vec2(0.5 - vPosition.x / 2.0, 0.5 - vPosition.y / 2.0);"
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
	if (config)
		program->controls = config->controls;
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

	if (config && config->next)
	{
		program->next = glprog_create(config->next);
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

	GLint texMap = glGetUniformLocation(program->ID, program->in_texturename);
	glUniform1i(texMap, 0); // GL_TEXTURE0
	glActiveTexture(GL_TEXTURE0);

	GLuint resolutionID = glGetUniformLocation(program->ID, "vResolution");
	glUniform2f(resolutionID, program->width, program->height);

	GLProgram_Uniform_t *uniform = program->controls;
	while (uniform)
	{
		GLProgram_Uniform_t *next = uniform->next;
		glprog_setuniform(program, uniform);
		if (program->next == NULL)
			_glprog_uniform_destroy(uniform);
		uniform = next;
	}
	if (program->next == NULL)
		program->controls = NULL;

	glBindVertexArrayOES(0);
	if (program->next)
		return glprog_setup(program->next, width, height);

	return 0;
}

GLBuffer_t *glprog_getouttexture(GLProgram_t *program, GLuint nbtex)
{
	if (program->out_textures[0].dma_texture)
	{
		return program->out_textures;
	}
	glGenFramebuffers(1, &program->fbID);
	if (program->fbID == 0)
	{
		err("segl: framebuffer unsupported");
		return NULL;
	}
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
	if (program->next)
	{
		GLBuffer_t *textures;
		textures = glprog_getouttexture(program, nbtex);
		if (textures == NULL)
			return -1;
		return glprog_setintexture(program->next, GL_TEXTURE_2D, nbtex, textures);
	}
	return 0;
}

int glprog_run(GLProgram_t *program, int bufid)
{
	GLenum err = 0;
	if (program->fbID)
	{
		glBindFramebuffer(GL_FRAMEBUFFER, program->fbID);
		glBindTexture(GL_TEXTURE_2D, program->out_textures[bufid].dma_texture);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
					program->out_textures[bufid].dma_texture, 0);
		err = glGetError();
		if (err != GL_NO_ERROR)
		{
			err("segl: Framebuffer access error %#X %#X", err, GL_INVALID_FRAMEBUFFER_OPERATION);
		}
	}
	else
		glClear(GL_COLOR_BUFFER_BIT);

	glBindVertexArrayOES(program->vertexArrayID);
	glUseProgram(program->ID);

	glActiveTexture(GL_TEXTURE0);
	glBindTexture(program->in_textype, program->in_textures[bufid].dma_texture);
	GLProgram_Uniform_t *uniform = program->controls;
	while (uniform)
	{
		GLProgram_Uniform_t *next = uniform->next;
		glprog_setuniform(program, uniform);
		if (program->next == NULL)
			_glprog_uniform_destroy(uniform);
		uniform = next;
	}
	if (program->next == NULL)
		program->controls = NULL;

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
	if (program->next)
		return glprog_run(program->next, bufid);
	return 0;
}

int glprog_setuniform(GLProgram_t *program, GLProgram_Uniform_t *uniform)
{
	switch (uniform->type)
	{
	case Uniform_FLOAT_e:
	{
		GLint loc = glGetUniformLocation(program->ID, uniform->name);
		glUniform1f(loc, *(GLfloat*)uniform->value);
	}
	break;
	case Uniform_INT_e:
	{
		GLint loc = glGetUniformLocation(program->ID, uniform->name);
		glUniform1i(loc, *(GLint*)uniform->value);
	}
	break;
	case Uniform_FVEC2_e:
	{
		GLint loc = glGetUniformLocation(program->ID, uniform->name);
		glUniform2fv(loc, 2, uniform->value);
	}
	break;
	case Uniform_FVEC3_e:
	{
		GLint loc = glGetUniformLocation(program->ID, uniform->name);
		glUniform2fv(loc, 3, uniform->value);
	}
	break;
	case Uniform_FVEC4_e:
	{
		GLint loc = glGetUniformLocation(program->ID, uniform->name);
		glUniform2fv(loc, 4, uniform->value);
	}
	break;
	case Uniform_IVEC2_e:
	{
		GLint loc = glGetUniformLocation(program->ID, uniform->name);
		glUniform2iv(loc, 2, uniform->value);
	}
	break;
	case Uniform_IVEC3_e:
	{
		GLint loc = glGetUniformLocation(program->ID, uniform->name);
		glUniform2iv(loc, 3, uniform->value);
	}
	break;
	case Uniform_IVEC4_e:
	{
		GLint loc = glGetUniformLocation(program->ID, uniform->name);
		glUniform2iv(loc, 4, uniform->value);
	}
	break;
	case Uniform_MAT2_e:
	{
		GLint loc = glGetUniformLocation(program->ID, uniform->name);
		glUniformMatrix2fv(loc, 1, GL_FALSE, uniform->value);
	}
	break;
	case Uniform_MAT3_e:
	{
		GLint loc = glGetUniformLocation(program->ID, uniform->name);
		glUniformMatrix3fv(loc, 1, GL_FALSE, uniform->value);
	}
	break;
	case Uniform_MAT4_e:
	{
		GLint loc = glGetUniformLocation(program->ID, uniform->name);
		glUniformMatrix4fv(loc, 1, GL_FALSE, uniform->value);
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

static void _glprog_uniform_destroy(GLProgram_Uniform_t *uniform)
{
	free(uniform->value);
	free(uniform);
}

#ifdef HAVE_JANSSON
#include <jansson.h>

static void _glprog_uniform_setvalue(GLProgram_Uniform_t *uniform, json_t *jvalue, unsigned char nbentries, Uniform_Type_e type)
{
	if (type == Uniform_FLOAT_e)
		uniform->value = calloc(nbentries, sizeof(GLfloat));
	if (type == Uniform_INT_e)
		uniform->value = calloc(nbentries, sizeof(GLint));
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
	}
	json_t *jvalue = json_object_get(jsetting, "value");
	if (jvalue && json_is_number(jvalue))
	{
		switch (uniform->type)
		{
		case Uniform_INT_e:
			uniform->value = malloc(sizeof(GLint));
			*(GLint *)uniform->value = json_integer_value(jvalue);
		break;
		case Uniform_FLOAT_e:
			uniform->value = malloc(sizeof(GLfloat));
			*(GLfloat *)uniform->value = json_real_value(jvalue);
		break;
		default:
			err("segl: settings mal formatted");
			uniform->type = Uniform_UNKNOWN_e;
		}
	}
	if (jvalue && json_is_array(jvalue))
	{
		switch (uniform->type)
		{
		case Uniform_FVEC2_e:
			_glprog_uniform_setvalue(uniform, jvalue, 2, Uniform_FLOAT_e);
		break;
		case Uniform_FVEC3_e:
			_glprog_uniform_setvalue(uniform, jvalue, 3, Uniform_FLOAT_e);
		break;
		case Uniform_FVEC4_e:
			_glprog_uniform_setvalue(uniform, jvalue, 4, Uniform_FLOAT_e);
		break;
		case Uniform_IVEC2_e:
			_glprog_uniform_setvalue(uniform, jvalue, 2, Uniform_INT_e);
		break;
		case Uniform_IVEC3_e:
			_glprog_uniform_setvalue(uniform, jvalue, 3, Uniform_INT_e);
		break;
		case Uniform_IVEC4_e:
			_glprog_uniform_setvalue(uniform, jvalue, 4, Uniform_INT_e);
		break;
		case Uniform_MAT2_e:
			_glprog_uniform_setvalue(uniform, jvalue, 2 * 2, Uniform_FLOAT_e);
		break;
		case Uniform_MAT3_e:
			_glprog_uniform_setvalue(uniform, jvalue, 3 * 3, Uniform_FLOAT_e);
		break;
		case Uniform_MAT4_e:
			_glprog_uniform_setvalue(uniform, jvalue, 4 * 4, Uniform_FLOAT_e);
		break;
		}
	}
	if (uniform->type == Uniform_UNKNOWN_e)
	{
		free(uniform);
		uniform = NULL;
	}
	return uniform;
}

int glprog_loadjsonsetting(GLProgram_t *program, void *entry)
{
	json_t *jsettings = entry;
	if (jsettings && json_is_array(jsettings))
	{
		json_t *jsetting = NULL;
		int i = 0;
		json_array_foreach(jsettings, i, jsetting)
		{
			GLProgram_Uniform_t *uniform = _glprog_uniform_create(jsetting);
			if (uniform)
			{
				uniform->next = program->controls;
				program->controls = uniform;
			}
		}
	}
	else if (jsettings && json_is_object(jsettings))
	{
		GLProgram_Uniform_t *uniform = _glprog_uniform_create(jsettings);
		if (uniform)
		{
			while (program)
			{
				uniform->next = program->controls;
				program->controls = uniform;
				program = program->next;
			}
		}
	}
	return 0;
}

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
