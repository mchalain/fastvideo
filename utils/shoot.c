#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>

#include <gpiod.h>

#include "log.h"
#include "daemonize.h"
#include "spassthrough.h"

#define DEFAULT_WORKDIR "/tmp/fastvideo.spassthrough/"
#define DEFAULT_CHIP 0
#define CONSUMER "shoot"

static void help(const char *name)
{
	fprintf(stderr, "Usage: %s -g <gpio> -c <shmname> [-W <workdir>] [-D] [-L <logfile>] [-P <pidfile>]\n", name);
	fprintf(stderr, "\n");
	fprintf(stderr, "  -h            display this message\n");
	fprintf(stderr, "  -g <gpio>     gpiochip%d line number to watch (pull-up, idle high)\n", DEFAULT_CHIP);
	fprintf(stderr, "  -W <path>     spassthrough working directory (default %s)\n", DEFAULT_WORKDIR);
	fprintf(stderr, "  -c <name>     shared memory file name (the spassthrough device's own JSON name)\n");
	fprintf(stderr, "  -p <periodic> set the frames number to repeat the shot (default 0)\n");
	fprintf(stderr, "  -D            daemonize (fork to background)\n");
	fprintf(stderr, "  -L <logfile>  redirect stdout/stderr to this file\n");
	fprintf(stderr, "  -P <pidfile>  write the daemon pid to this file\n");
}

/*
 * Same mkdir/openat/ftok/shmget sequence as fastcontrols_create()
 * (fastvideo/src/sfastvideo.c), run from the same working directory, so
 * this attaches to the exact shm segment a running spassthrough device
 * already created (or creates it, if shoot is started first).
 */
static Passthrough_Control_t *_controls_attach(const char *dir, const char *keyname)
{
	int curdir = open(".", O_DIRECTORY);
	if (mkdir(dir, 0755) && errno != EEXIST)
		err("shoot: %s directory creation error %m", dir);
	int rootfd = open(dir, O_DIRECTORY);
	if (rootfd == -1)
	{
		rootfd = AT_FDCWD;
		err("shoot: run inside current directory %m");
	}
	int fd = openat(rootfd, keyname, O_CREAT | O_RDWR, 0644);
	if (fd < 0)
		err("shoot: shm file error %m");
	else
		close(fd);

	if (rootfd != AT_FDCWD)
		fchdir(rootfd);
	key_t key = ftok(keyname, 'R');
	fchdir(curdir);
	close(curdir);
	if (rootfd != AT_FDCWD)
		close(rootfd);

	if (key == -1)
	{
		err("shoot: shm token error %m");
		return NULL;
	}
	int shmid = shmget(key, sizeof(Passthrough_Control_t), IPC_CREAT | 0644);
	if (shmid < 0)
	{
		err("shoot: shm segment error %m");
		return NULL;
	}
	void *controls = shmat(shmid, NULL, 0);
	if (controls == (void *)-1)
	{
		err("shoot: shm attach error %m");
		return NULL;
	}
	warn("shoot: key=0x%x shmid=%d", key, shmid);
	return (Passthrough_Control_t *)controls;
}

int main(int argc, char *const argv[])
{
	int gpio = -1;
	const char *workdir = DEFAULT_WORKDIR;
	const char *shmname = NULL;
	const char *logfile = NULL;
	const char *pidfile = NULL;
	int do_daemonize = 0;
	uint32_t periodic = 0;

	int opt;
	while ((opt = getopt(argc, argv, "hg:W:c:DL:P:p:")) != -1)
	{
		switch (opt)
		{
			case 'g':
				gpio = atoi(optarg);
			break;
			case 'W':
				workdir = optarg;
			break;
			case 'c':
				shmname = optarg;
			break;
			case 'D':
				do_daemonize = 1;
			break;
			case 'L':
				logfile = optarg;
			break;
			case 'P':
				pidfile = optarg;
			break;
			case 'p':
				periodic = strtoull(optarg, NULL, 10);
			break;
			case 'h':
			default:
				help(argv[0]);
				return 1;
		}
	}
	if (gpio < 0 || shmname == NULL)
	{
		help(argv[0]);
		return 1;
	}

	if (logfile)
		daemon_setlogfile(logfile);

	struct gpiod_chip *chip = gpiod_chip_open_by_number(DEFAULT_CHIP);
	if (chip == NULL)
	{
		err("shoot: gpiochip%d open error %m", DEFAULT_CHIP);
		return 1;
	}
	struct gpiod_line *line = gpiod_chip_get_line(chip, gpio);
	if (line == NULL)
	{
		err("shoot: gpiochip%d line %d error %m", DEFAULT_CHIP, gpio);
		gpiod_chip_close(chip);
		return 1;
	}
	if (gpiod_line_request_both_edges_events_flags(line, CONSUMER, GPIOD_LINE_REQUEST_FLAG_BIAS_PULL_UP) < 0)
	{
		err("shoot: gpio %d request error %m", gpio);
		gpiod_chip_close(chip);
		return 1;
	}

	Passthrough_Control_t *controls = _controls_attach(workdir, shmname);
	if (controls == NULL)
		return 1;

	daemonize(do_daemonize, NULL, pidfile, NULL, NULL);

	warn("shoot: watching gpio %d, shooting into %s%s", gpio, workdir, shmname);

	/* idle is HIGH (pull-up); a shot is a HIGH->LOW->HIGH transition */
	int armed = 0;
	while (isrunning())
	{
		for (int retries = 3; (controls->state & STATE_SHOOT) && retries > 0; retries--)
		{
			struct timespec delay = {.tv_sec = 0, .tv_nsec = 100000000L};
			nanosleep(&delay, NULL);
		}
		if (controls->state & STATE_SHOOT)
		{
			err("shoot: STATE_SHOOT still set after 300ms, is a spassthrough device consuming %s%s?", workdir, shmname);
			shmdt(controls);
			return 1;
		}

		struct timespec timeout = {.tv_sec = 1, .tv_nsec = 0};
		int ret = gpiod_line_event_wait(line, &timeout);
		if (ret < 0)
		{
			if (errno == EINTR)
				continue;
			err("shoot: gpio %d event wait error %m", gpio);
			break;
		}
		if (ret == 0)
			continue; /* timeout, loop back to let isrunning() catch a signal */

		struct gpiod_line_event event;
		if (gpiod_line_event_read(line, &event) < 0)
		{
			err("shoot: gpio %d event read error %m", gpio);
			continue;
		}
		if (event.event_type == GPIOD_LINE_EVENT_FALLING_EDGE)
		{
			armed = 1;
		}
		else if (event.event_type == GPIOD_LINE_EVENT_RISING_EDGE && armed)
		{
			armed = 0;
			dbg("shoot: gpio %d high->low->high, shooting", gpio);
			controls->state |= STATE_SHOOT;
			controls->periodic = periodic;
		}
	}

	gpiod_line_release(line);
	gpiod_chip_close(chip);
	shmdt(controls);

	killdaemon(pidfile);
	warn("shoot: end");
	return 0;
}
