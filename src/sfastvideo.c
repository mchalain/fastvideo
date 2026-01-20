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

FastVideoList_t *fastvideolist_push(FastVideoList_t *list, FastVideoList_t *newentry)
{
	FastVideoList_t *entry = NULL;
	for (entry = list; entry != NULL; entry = entry->next)
	{
		if (entry->entity == newentry->entity)
			return list;
	}
	if (list)
	{
		newentry->last = list->last;
		list->previous = newentry;
	}
	newentry->next = list;
	return newentry;
}

FastVideoList_t *fastvideolist_poplast(FastVideoList_t *list, FastVideoList_t **entry)
{
	if (!list)
		return NULL;
	list->iterator = list->last;
	return fastvideolist_pop(list, entry);
}

FastVideoList_t *fastvideolist_pop(FastVideoList_t *list, FastVideoList_t **entry)
{
	if (list == NULL)
		return NULL;
	FastVideoList_t *current = list->iterator;
	if (current == NULL)
		current = list;
	if (entry)
		*entry = current;
	if (list->iterator->previous)
		list->iterator->previous->next = current->next;
	if (list->iterator->next)
		list->iterator->next->previous = current->previous;
	list->iterator = list->iterator->next;
	if (current == list->last)
		list->last = current->previous;
	if (entry)
		(*entry)->previous = (*entry)->next = NULL;
	if (current == list)
		list = NULL;
	return list;
}

void fastvideolist_reset(FastVideoList_t *list)
{
	if (list)
		list->iterator = NULL;
}

FastVideoList_t *fastvideolist_last(FastVideoList_t *list)
{
	if (!list)
		return NULL;
	return list->last;
}

FastVideoList_t *fastvideolist_first(FastVideoList_t *list)
{
	if (list)
		list->iterator = NULL;
	return list;
}

FastVideoList_t *fastvideolist_current(FastVideoList_t *list)
{
	if (list)
		return list->iterator;
	return NULL;
}

FastVideoList_t *fastvideolist_up(FastVideoList_t *list)
{
	if (list)
	{
		if (list->iterator)
			list->iterator = list->iterator->next;
		else
			list->iterator = list;
		return list->iterator;
	}
	return NULL;
}

FastVideoList_t *fastvideolist_down(FastVideoList_t *list)
{
	if (list)
	{
		if (list->iterator)
			list->iterator = list->iterator->previous;
		else
			list->iterator = list->last;
		return list->iterator;
	}
	return NULL;
}

int fastvideolist_islast(FastVideoList_t *list, void *entity)
{
	if (!list)
		return 0;
	if (list->last->entity == entity)
		return 1;
	return 0;
}

int fastvideolist_isfirst(FastVideoList_t *list, void *entity)
{
	if (!list)
		return 0;
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

void fastvideodevice_ops_append(const FastVideoDevice_ops_t *ops)
{
	g_FastVideoDevice_ops = fastvideolist_append(g_FastVideoDevice_ops, (void *)ops);
}

FastVideoDevice_ops_t *fastvideodevice_ops_next(FastVideoDevice_ops_t *ops)
{
	if (ops == NULL)
		g_FastVideoDevice_ops->iterator = NULL;
	ops = fastvideolist_next(g_FastVideoDevice_ops);
	return ops;
}

const Proto_t * _protos[10] = {0};
void fastvideo_proto_append(const Proto_t *proto)
{
	int i = 0;
	for (; _protos[i] && i < sizeof(_protos) / sizeof(*_protos); i++);
	if (i < sizeof(_protos)/sizeof(*_protos))
		_protos[i] = proto;
}

