#ifndef __FASTSETTING_CLI_H__
#define __FASTSETTING_CLI_H__
typedef struct control_s control_t;
struct control_s
{
	char name[32];
	long unsigned id;
	enum {
		CTRL_UNKONWN,
		CTRL_INTEGER,
		CTRL_BOOLEAN,
	} type;
	union {
		int integer;
		int boolean;
	} value;
	control_t *next;
	control_t *previous;
};

typedef struct device_s device_t;
struct device_s
{
	char name[36];
	char type[36];
	int id;
	unsigned long width;
	unsigned long height;
	char fourcc[5];
	control_t *controls;
	device_t *next;
	device_t *previous;
};

typedef struct fastsetting_s fastsetting_t;

fastsetting_t *fastsetting_create(const char *serverpath);
device_t *fastsetting_device(fastsetting_t *data, int index);
int fastsetting_capabilities(fastsetting_t *data, device_t *device);
int fastsetting_change(fastsetting_t *data, device_t *device, control_t *control);
void fastsetting_destroy(fastsetting_t *data);

#endif
