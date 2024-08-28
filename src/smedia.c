#include <fcntl.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <sys/types.h>

#include "log.h"
#include "smedia.h"

typedef struct Media_s Media_t;
struct Media_s
{
	int fd;
	const char *name;
	/** cf media_pad **/
	struct media_links_enum *current;
};

Media_t *smedia_create(const char *path)
{
	int fd = open(path, O_RDWR);
	if (fd == -1)
		return NULL;
	const char *name = path;
	path = strrchr(path, '/');
	if (path)
		name = path + 1;
	Media_t *media = smedia_create2(fd, name);
	if (media == NULL)
		close(fd);
	return media;
}

Media_t *smedia_create2(int fd, const char *name)
{
	Media_t *media = calloc(1, sizeof(*media));
	media->fd = fd;
	media->name = name;
	return media;
}

const char *smedia_name(Media_t *media)
{
	return media->name;
}

static int _media_links(Media_t *media, struct media_links_enum *current, int (*links)(void *arg, struct media_links_enum *links_enum), void *cbarg)
{
	/** cf media_pad **/
	media->current = current;
	if (ioctl(media->fd, MEDIA_IOC_ENUM_LINKS, media->current))
	{
		err("media %d links not found %m", media->current->entity);
		media->current = NULL;
		return -1;
	}

	if (links)
		links(cbarg, media->current);

	media->current = NULL;
	return 0;
}

typedef struct _Media_Links_s
{
	int nb;
	int (*cb)(void *arg, void *pad);
	void *cbarg;
} _Media_Links_t;

static int _media_padscb(void *arg, struct media_links_enum *links_enum)
{
	_Media_Links_t *pads = (_Media_Links_t *)arg;
	for (int i = 0; i < pads->nb; i++)
	{
		if (pads->cb)
		{
			struct media_pad_desc *pad = &(links_enum->pads[i]);
			pads->cb(pads->cbarg, (void*)pad);
		}
	}
	return 0;
}

int smedia_enumpads(Media_t *media, struct media_entity_desc *entity, int (*pad)(void *arg, struct media_pad_desc *pad), void *cbarg)
{
	_Media_Links_t arg = {0};
	arg.cb = (int (*)(void *arg, void *pad)) pad;
	arg.cbarg = cbarg;
	arg.nb = entity->pads;

	struct media_links_enum current = {0};
	current.entity = entity->id;
	current.pads = calloc(entity->pads, sizeof(struct media_pad_desc));
	current.links = calloc(entity->links, sizeof(struct media_link_desc));

	int ret  = _media_links(media, &current, _media_padscb, &arg);

	free(current.pads);
	free(current.links);
	return ret;
}

static int _media_linkscb(void *arg, struct media_links_enum *links_enum)
{
	_Media_Links_t *links = (_Media_Links_t *)arg;
	for (int i = 0; i < links->nb; i++)
	{
		if (links->cb)
			links->cb(links->cbarg, &(links_enum->links[i]));
	}
	return 0;
}

int smedia_enumlinks(Media_t *media, struct media_entity_desc *entity, int (*link)(void *arg, struct media_link_desc *link), void *cbarg)
{
	_Media_Links_t arg = {0};
	arg.cb = (int (*)(void *arg, void *pad)) link;
	arg.cbarg = cbarg;
	arg.nb = entity->links;

	struct media_links_enum current = {0};
	current.entity = entity->id;
	current.pads = calloc(entity->pads, sizeof(struct media_pad_desc));
	current.links = calloc(entity->links, sizeof(struct media_link_desc));

	int ret = _media_links(media, &current, _media_linkscb, &arg);

	free(current.pads);
	free(current.links);
	return ret;
}

struct media_pad_desc *smedia_pad(Media_t *media, struct media_entity_desc *entity, int id)
{
	/**
	 * media_pad may be call from the cb of media_enumlinks
	 * it is useless to enum the links again.
	 * But if the function is directly called,
	 * the links must be enumerated.
	 */
	struct media_links_enum *current = media->current;
	struct media_links_enum constant;
	if (media->current == NULL)
	{
		constant.entity = entity->id;
		constant.pads = calloc(entity->pads, sizeof(struct media_pad_desc));
		constant.links = calloc(entity->links, sizeof(struct media_link_desc));

		_media_links(media, &constant, NULL, NULL);
		current = &constant;

	}
	struct media_pad_desc *pad = calloc(1, sizeof(*pad));
	memcpy(pad, &current->pads[id], sizeof(*pad));
	if (current == &constant)
	{
		free(constant.pads);
		free(constant.links);
	}
	return pad;
}

int smedia_enumentities(Media_t *media, int (*entity)(void *arg, Media_t *media, struct media_entity_desc *entity), void *cbarg)
{
	struct media_entity_desc entity_desc = {0};
	entity_desc.id = MEDIA_ENT_ID_FLAG_NEXT;

	int i;
	int ret;
	for (i = 0; (ret = ioctl(media->fd, MEDIA_IOC_ENUM_ENTITIES, &entity_desc)) == 0; i++)
	{
		if (entity)
			entity(cbarg, media, &entity_desc);
		entity_desc.id |= MEDIA_ENT_ID_FLAG_NEXT;
	}
	return i;
}

void smedia_destroy(Media_t *media)
{
	if (media->current != NULL)
	{
		free(media->current->pads);
		free(media->current->links);
	}
	close(media->fd);
	free(media);
}
