#include <stdlib.h>
#include <errno.h>

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
		(*entry)->previous = (*entry)->next = (*entry)->last = (*entry)->iterator = NULL;
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

void *fastvideolist_get(FastVideoList_t *list)
{
	if (list == NULL)
		return NULL;

	return list->entity;
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

#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/shm.h>

static int _fastcontrols_prepare(const char *dir, const char *keyname, int clean)
{
	if (mkdir(dir, 0755) && errno != EEXIST)
		err("sfastvideo: programs directory creation error %m");
	int rootfd = open(dir, O_DIRECTORY);
	if (rootfd == -1)
	{
		rootfd = AT_FDCWD;
		err("sfastvideo: run inside current directory %m");
	}
	int ret = faccessat(rootfd, keyname, F_OK, AT_EACCESS);
	if (!ret && clean)
	{
		ret = unlinkat(rootfd, keyname, 0);
		if (ret)
		{
			err("sfastvideo: shm file access error %m");
		}
		else
			ret = rootfd;
	}
	else
		ret = rootfd;
	return ret;
}

void *fastcontrols_create(const char *dir, const char *keyname, unsigned int size)
{
	int ret = _fastcontrols_prepare(dir, keyname, 0);
	if (ret < 0)
		return NULL;
	int rootfd = ret;
	int curdir = open(".", O_DIRECTORY);
	int fd = openat(rootfd, keyname, O_CREAT|O_RDWR, 0644);
	if (fd < 0)
		err("sfastvideo: shm file error %m");
	close(fd);
	int shmid = 0;
	key_t key;
	if (rootfd != AT_FDCWD)
		fchdir(rootfd);
	key = ftok(keyname, 'R');
	fchdir(curdir);
	close(curdir);
	close(rootfd);
	if (key == -1)
		err("sfastvideo: shm token error %m");
	void *controls = (void *)-1;
	if (key != -1)
		shmid = shmget(key, size, IPC_CREAT| 0644);
	if (shmid > 0)
	{
		controls = shmat(shmid, NULL, 0);
	}
	warn("key=0x%x shmid=%d", key, shmid);
	if (controls == (void *)-1)
	{
		err("sfastvideo: share memory allocation error %m");
		size = 0;
		controls = NULL;
	}
	return controls;
}

void fastcontrols_destroy(void *controls)
{
	shmdt(controls);
}

void fastclean(const char *dir, const char *keyname)
{
	_fastcontrols_prepare(dir, keyname, 1);
}
