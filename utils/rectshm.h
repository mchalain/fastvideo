#ifndef __RECTSHM_H__
#define __RECTSHM_H__

typedef struct RectShm_s RectShm_t;

RectShm_t *rectshm_open(const char *configfile, const char *device, const char *program, const char *workdir);
void rectshm_close(RectShm_t *shm);

/// number of vRects[N] slots, sorted by N
int rectshm_count(RectShm_t *shm);
/// N of the vRects[N] uniform of a slot
int rectshm_index(RectShm_t *shm, int slot);

int rectshm_isempty(RectShm_t *shm, int slot);
void rectshm_get(RectShm_t *shm, int slot, float rect[4]);
void rectshm_set(RectShm_t *shm, int slot, float x1, float y1, float x2, float y2);
void rectshm_clear(RectShm_t *shm, int slot);

#endif
