#ifndef __SDMABUF_H__
#define __SDMABUF_H__

int sdmabuf_create(const char *name, size_t size);
int sdmabuf_sync(int dmafd, int start);
void *sdmabuf_map(int dmafd, size_t size, int write);
void *sdmabuf_unmap(void *mem, size_t size);
void sdmabuf_destroy(int dmafd);
#endif
