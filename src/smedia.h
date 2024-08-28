#ifndef __SMEDIA_H__
#define __SMEDIA_H__

#include <linux/media.h>

typedef struct Media_s Media_t;

Media_t *smedia_create(const char *path);
Media_t *smedia_create2(int fd, const char *name);
const char *smedia_name(Media_t *media);
int smedia_enumpads(Media_t *media, struct media_entity_desc *entity, int (*pad)(void *arg, struct media_pad_desc *pad), void *cbarg);
int smedia_enumlinks(Media_t *media, struct media_entity_desc *entity, int (*link)(void *arg, struct media_link_desc *link), void *cbarg);
struct media_pad_desc *smedia_pad(Media_t *media, struct media_entity_desc *entity, int id);
int smedia_enumentities(Media_t *media, int (*entity)(void *arg, Media_t *media, struct media_entity_desc *entity), void *cbarg);
void smedia_destroy(Media_t *media);

#endif
