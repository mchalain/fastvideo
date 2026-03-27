#ifndef __SEGL_ENGINE_H__
#define __SEGL_ENGINE_H__

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
	Uniform_SAMPLER_e,
} Uniform_Type_e;

#define	Uniform_SHARED_e	0x1000
#define	Uniform_FUNC_e		0x2000

typedef struct GLProgram_Uniform_s GLProgram_Uniform_t;
struct GLProgram_Uniform_s
{
	const char *name;
	void *config;
	EGLint loc;
	Uniform_Type_e type;
	void *value;
	void *data;
	GLProgram_Uniform_t *next;
};

typedef struct GLProgram_Input_s GLProgram_Input_t;
struct GLProgram_Input_s {
	const char *name;
	const char *src;
};

typedef struct EGLConfig_Program_s EGLConfig_Program_t;
struct EGLConfig_Program_s
{
	int rootfd;
	const char *name;
	void *entry;
	const char *vertex;
	const char *fragments[MAX_SHADERS];
	GLProgram_Input_t input;
	EGLConfig_Program_t *next;
	GLProgram_Uniform_t *controls;
};

#ifdef HAVE_JANSSON
int glprog_loadjsonconfiguration(void *arg, void *entry);
int glprog_setuniform(GLProgram_t *program, GLProgram_Uniform_t *uniform);
#endif
#endif
