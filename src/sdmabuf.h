#ifndef __SDMABUF_H__
#define __SDMABUF_H__

const char *scpu_queryextensions();
int scpu_checkextension(const char *ext);
#define SCPU_NEON 0
#define SCPU_VFPV4 1
int scpu_check(int ext);

int sdmabuf_create(const char *name, size_t size);
int sdmabuf_sync(int dmafd, int start);
void *sdmabuf_map(int dmafd, size_t size, int write);
void sdmabuf_unmap(void *mem, size_t size);
void sdmabuf_destroy(int dmafd);
#endif
