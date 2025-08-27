#include <stdlib.h>

#include "fastvideo.h"
#include "log.h"

struct FastVideoList_s
{
	void *entity;
	FastVideoList_t *next;
	FastVideoList_t *previous;
	FastVideoList_t *last;
	FastVideoList_t *iterator;
};

FastVideoList_t *fastvideolist_append(FastVideoList_t *list, void *device)
{
	FastVideoList_t *entry = NULL;
	entry = calloc(1, sizeof(*entry));
	entry->entity = device;
	entry->next = list;
	if (list == NULL)
		entry->last = entry;
	else
	{
		list->previous = entry;
		entry->last = list->last;
	}
	return entry;
}

FastVideoList_t *fastvideolist_insert(FastVideoList_t *list, void *device)
{
	FastVideoList_t *entry = NULL;
	entry = calloc(1, sizeof(*entry));
	entry->entity = device;
	if (list)
	{
		entry->previous = list->last;
		list->last->next = entry;
	}
	else
	{
		list = entry;
	}
	list->last = entry;
	return list;
}

void fastvideolist_reset(FastVideoList_t *list)
{
	list->iterator = NULL;
}

FastVideoList_t *fastvideolist_last(FastVideoList_t *list)
{
	return list->last;
}

FastVideoList_t *fastvideolist_first(FastVideoList_t *list)
{
	list->iterator = NULL;
	return list;
}

int fastvideolist_islast(FastVideoList_t *list, void *entity)
{
	if (list->last->entity == entity)
		return 1;
	return 0;
}

int fastvideolist_isfirst(FastVideoList_t *list, void *entity)
{
	if (list->entity == entity)
		return 1;
	return 0;
}

void *fastvideolist_next(FastVideoList_t *list)
{
	if (list == NULL)
		return NULL;

	void *entity = NULL;
	FastVideoList_t *entry = NULL;
	if (list->iterator == NULL)
		entry = list;
	else
		entry = list->iterator->next;

	if (entry)
		entity = entry->entity;
	list->iterator = entry;

	return entity;
}

void *fastvideolist_previous(FastVideoList_t *list)
{
	if (list == NULL)
		return NULL;

	void *entity = NULL;
	FastVideoList_t *entry = NULL;
	if (list->iterator == NULL)
		entry = list->last;
	else
		entry = list->iterator->previous;

	if (entry)
		entity = entry->entity;
	list->iterator = entry;

	return entity;
}

void fastvideolist_destroy(FastVideoList_t *list, void(*destroy)(void *))
{
	if (list == NULL)
		return;

	void *entity = NULL;
	FastVideoList_t *previous = NULL;
	for (FastVideoList_t *entry = list->last; entry != NULL; entry = previous)
	{
		previous = entry->previous;
		if (destroy)
			destroy(entry->entity);
		free(entry);
	}
}

static FastVideoList_t *g_FastVideoDevice_ops = NULL;

void fastvideodevice_ops_append(FastVideoDevice_ops_t *ops)
{
	g_FastVideoDevice_ops = fastvideolist_append(g_FastVideoDevice_ops, ops);
}

FastVideoDevice_ops_t *fastvideodevice_ops_next(FastVideoDevice_ops_t *ops)
{
	if (ops == NULL)
		g_FastVideoDevice_ops->iterator = NULL;
	ops = fastvideolist_next(g_FastVideoDevice_ops);
	return ops;
}
