#ifndef __SPASSTROUGHT_H__
#define __SPASSTROUGHT_H__

typedef struct Passthrough_s Passthrough_t;

DeviceConf_t * spassthrough_createconfig(void);
void *spassthrough_create(const char *devicename, DeviceConf_t *config);
void *spassthrough_duplicate(Passthrough_t *dev);
int spassthrough_loadsettings(Passthrough_t *dev, void *configentry);
int spassthrough_requestbuffer(Passthrough_t *dev, enum buf_type_e t, ...);
int spassthrough_fd(Passthrough_t *dev);
int spassthrough_start(Passthrough_t *dev);
int spassthrough_stop(Passthrough_t *dev);
int spassthrough_dequeue(Passthrough_t *dev, void **mem, size_t *bytesused);
int spassthrough_queue(Passthrough_t *dev, int index, size_t bytesused);
void spassthrough_destroy(Passthrough_t *dev);

#endif
