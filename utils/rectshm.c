#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>

#include <jansson.h>

#include "log.h"
#include "rectshm.h"

/*
 * generic code to use the "data/rectangle.glsl".
 */

#define RECTS_NAME "vRects["
#define MAX_RECTS 32

typedef struct Rect_s Rect_t;
struct Rect_s
{
	int index;
	size_t offset;
};

struct RectShm_s
{
	char *controls;
	Rect_t rects[MAX_RECTS];
	int nrects;
};

static int _json_hasname(json_t *jentry, const char *name)
{
	json_t *jname = json_object_get(jentry, "name");
	if (jname && json_is_string(jname))
		return !strcmp(json_string_value(jname), name);
	if (jname && json_is_array(jname))
	{
		size_t i;
		json_t *jvalue;
		json_array_foreach(jname, i, jvalue)
		{
			if (json_is_string(jvalue) && !strcmp(json_string_value(jvalue), name))
				return 1;
		}
	}
	return 0;
}

static int _control_size(json_t *jcontrol)
{
	static const struct
	{
		const char *type;
		int size;
	} sizes[] = {
		{"int", 4}, {"float", 4},
		{"vec2", 8}, {"vec3", 12}, {"vec4", 16},
		{"ivec2", 8}, {"ivec3", 12}, {"ivec4", 16},
		{"mat2", 16}, {"mat3", 36}, {"mat4", 64},
	};
	json_t *jname = json_object_get(jcontrol, "name");
	json_t *jtype = json_object_get(jcontrol, "type");
	if (!jname || !json_is_string(jname) || !jtype || !json_is_string(jtype))
		return -1;
	for (size_t i = 0; i < sizeof(sizes) / sizeof(*sizes); i++)
	{
		if (!strcmp(json_string_value(jtype), sizes[i].type))
			return sizes[i].size;
	}
	return -1;
}

static ssize_t _layout(json_t *jroot, const char *device, const char *program, Rect_t rects[MAX_RECTS], int *nrects)
{
	json_t *jdevice = NULL;
	size_t i;
	json_t *jentry;
	json_array_foreach(jroot, i, jentry)
	{
		if (_json_hasname(jentry, device))
		{
			jdevice = jentry;
			break;
		}
	}
	if (jdevice == NULL)
	{
		err("rectshm: device %s not found", device);
		return -1;
	}
	json_t *jprogram = NULL;
	json_t *jprograms = json_object_get(jdevice, "programs");
	if (jprograms && json_is_object(jprograms) && _json_hasname(jprograms, program))
		jprogram = jprograms;
	json_array_foreach(jprograms, i, jentry)
	{
		if (_json_hasname(jentry, program))
		{
			jprogram = jentry;
			break;
		}
	}
	if (jprogram == NULL)
	{
		err("rectshm: program %s not found into %s", program, device);
		return -1;
	}
	json_t *jcontrols = json_object_get(jprogram, "controls");
	json_t *jsingle = NULL;
	if (jcontrols && json_is_object(jcontrols))
	{
		jsingle = json_array();
		json_array_append(jsingle, jcontrols);
		jcontrols = jsingle;
	}
	if (!jcontrols || !json_is_array(jcontrols))
	{
		err("rectshm: program %s has no controls", program);
		return -1;
	}

	/// the slots follow the reverse order of the JSON array
	ssize_t size = 0;
	*nrects = 0;
	for (ssize_t j = json_array_size(jcontrols) - 1; j >= 0; j--)
	{
		json_t *jcontrol = json_array_get(jcontrols, j);
		int csize = _control_size(jcontrol);
		if (csize < 0)
			continue;
		const char *name = json_string_value(json_object_get(jcontrol, "name"));
		int index;
		if (csize == 16 && !strncmp(name, RECTS_NAME, sizeof(RECTS_NAME) - 1) &&
			!strcmp(json_string_value(json_object_get(jcontrol, "type")), "vec4") &&
			sscanf(name + sizeof(RECTS_NAME) - 1, "%d]", &index) == 1 &&
			*nrects < MAX_RECTS)
		{
			int k = *nrects;
			/// insertion sorted by index
			while (k > 0 && rects[k - 1].index > index)
			{
				rects[k] = rects[k - 1];
				k--;
			}
			rects[k].index = index;
			rects[k].offset = size;
			(*nrects)++;
		}
		size += csize;
	}
	if (jsingle)
		json_decref(jsingle);
	if (*nrects == 0)
	{
		err("rectshm: no vec4 %sN] control into program %s", RECTS_NAME, program);
		return -1;
	}
	return size;
}

static void *_controls_attach(const char *dir, const char *keyname, size_t size)
{
	int curdir = open(".", O_DIRECTORY);
	if (chdir(dir) != 0)
	{
		err("rectshm: %s not found, is fastvideo running? %m", dir);
		close(curdir);
		return NULL;
	}
	key_t key = ftok(keyname, 'R');
	if (fchdir(curdir) != 0)
		err("rectshm: working directory error %m");
	close(curdir);
	if (key == -1)
	{
		err("rectshm: %s%s not found, is fastvideo running? %m", dir, keyname);
		return NULL;
	}
	int shmid = shmget(key, size, 0);
	if (shmid < 0)
	{
		err("rectshm: shared memory of %s (%zu bytes) not found: %m", keyname, size);
		return NULL;
	}
	void *controls = shmat(shmid, NULL, 0);
	if (controls == (void *)-1)
	{
		err("rectshm: shared memory attach error %m");
		return NULL;
	}
	return controls;
}

RectShm_t *rectshm_open(const char *configfile, const char *device, const char *program, const char *workdir)
{
	json_error_t error;
	json_t *jroot = json_load_file(configfile, 0, &error);
	if (jroot == NULL || !json_is_array(jroot))
	{
		err("rectshm: %s: %s (line %d)", configfile, jroot ? "not an array" : error.text, error.line);
		if (jroot)
			json_decref(jroot);
		return NULL;
	}
	RectShm_t *shm = calloc(1, sizeof(*shm));
	ssize_t size = _layout(jroot, device, program, shm->rects, &shm->nrects);
	json_decref(jroot);
	if (size > 0)
		shm->controls = _controls_attach(workdir, program, size);
	if (shm->controls == NULL)
	{
		free(shm);
		return NULL;
	}
	return shm;
}

void rectshm_close(RectShm_t *shm)
{
	shmdt(shm->controls);
	free(shm);
}

int rectshm_count(RectShm_t *shm)
{
	return shm->nrects;
}

int rectshm_index(RectShm_t *shm, int slot)
{
	return shm->rects[slot].index;
}

static volatile float *_rect(RectShm_t *shm, int slot)
{
	return (volatile float *)(shm->controls + shm->rects[slot].offset);
}

int rectshm_isempty(RectShm_t *shm, int slot)
{
	volatile float *rect = _rect(shm, slot);
	return rect[0] == 0.0 && rect[1] == 0.0 && rect[2] == 0.0 && rect[3] == 0.0;
}

void rectshm_get(RectShm_t *shm, int slot, float values[4])
{
	volatile float *rect = _rect(shm, slot);
	for (int i = 0; i < 4; i++)
		values[i] = rect[i];
}

void rectshm_set(RectShm_t *shm, int slot, float x1, float y1, float x2, float y2)
{
	volatile float *rect = _rect(shm, slot);
	rect[2] = rect[0];
	rect[0] = x1;
	rect[1] = y1;
	rect[3] = y2;
	rect[2] = x2;
}

void rectshm_clear(RectShm_t *shm, int slot)
{
	volatile float *rect = _rect(shm, slot);
	rect[2] = rect[0];
	rect[3] = 0.0;
	rect[1] = 0.0;
	rect[0] = 0.0;
	rect[2] = 0.0;
}
