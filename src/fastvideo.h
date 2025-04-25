#ifndef __FASTVIDEO_DEVICE_H__
#define __FASTVIDEO_DEVICE_H__

typedef struct FastVideoList_s FastVideoList_t;

FastVideoList_t *fastvideolist_append(FastVideoList_t *list, void *device);
FastVideoList_t *fastvideolist_insert(FastVideoList_t *list, void *device);
FastVideoList_t *fastvideolist_last(FastVideoList_t *list);
void *fastvideolist_next(FastVideoList_t *list);
void *fastvideolist_previous(FastVideoList_t *list);
int fastvideolist_islast(FastVideoList_t *list, void *entity);
int fastvideolist_isfirst(FastVideoList_t *list, void *entity);

#endif
