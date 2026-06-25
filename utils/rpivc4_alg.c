#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>

#include <linux/bcm2835-isp.h>

#include "log.h"
#include "daemonize.h"
#include "unixsocket.h"

#define MODE_DAEMONIZE 0x01
#define MODE_KILLDAEMON 0x02
#define MODE_AUTOGAIN 0x04

static ssize_t _client_receive(void *arg, client_t *clt, const char *buffer, size_t length)
{
	return 0;
}

static void *_control_open(int atfd, const char *name)
{
	void * client = NULL;
	client = client_create(name);
	if (client == NULL)
	{
		err("fastsetting not found");
		return NULL;
	}
	client_attach_receive(client, _client_receive, NULL);
	return client;
}

static int _control_autogain(void *client, int autogain)
{
	if (client == NULL)
		return -1;
	char request[1024] = {0};
	size_t length = snprintf(request, sizeof(request) - 1, "\
	{\"cmd\":\"loadsetting\", \
	 \"data\":{ \
	  \"name\":\"unicam-image\", \
	  \"controls\":[ \
	   {\"id\":9963794,\"value\":%s} \
	  ] \
	 } \
	}", autogain?"true":"false");

#if RPIVC4_ALG_REQUEST == y
	int ret = client_request(client, (void*)request, length);
#else
	int ret = client_send(client, (void*)request, length);
#endif
	if (ret < 0)
		err("fastsetting reject \"auto exposure\" control");
	return 0;
}

static int _control_gain(void * client, int gain)
{
	if (client == NULL)
		return -1;
	char request[1024] = {0};
	size_t length = snprintf(request, sizeof(request) - 1,
"{ \
 \"cmd\":\"loadsetting\",\
 \"data\":{ \
  \"name\":\"unicam-image\", \
  \"controls\":[ \
   {\"id\":10356995,\"value\":%d} \
  ] \
 } \
}"
	, gain);
#if RPIVC4_ALG_REQUEST == y
	int ret = client_request(client, (void*)request, length);
#else
	int ret = client_send(client, (void*)request, length);
#endif
	if (ret < 0)
		err("fastsetting reject \"Analogue Gain\" control");
	return 0;
}

static void _control_close(void * arg)
{
	client_destroy(arg);
}

#define GAIN_AWB_REGION_MAIN 7
#define GAIN_AWB_PERIOD 30
#define GAIN_RATIO 25
static int _algo_awb(struct bcm2835_isp_stats_region *stats, int nbregions, void *controlfd)
{
	static int gain_counter = 0;
	gain_counter++;
	if (gain_counter >= GAIN_AWB_PERIOD)
	{
		static int32_t previous = 0;
		static int recompute = 1;

		struct bcm2835_isp_stats_region *region = &stats[GAIN_AWB_REGION_MAIN];
		if (region->counted == 0)
			return 0;
		int32_t gain = (region->r_sum + region->g_sum + region->b_sum) / region->counted;
		gain /= GAIN_RATIO;
		if (region->counted < 100)
			gain *= 2;
		if (previous == 0)
			previous = gain;
		if (recompute && (previous != gain))
		{
			if ((previous - gain) > 10)
			{
				gain += (previous - gain) / 2;
				gain_counter = GAIN_AWB_PERIOD;
			}
			if ((gain - previous) > 10)
			{
				gain -= (gain - previous) / 2;
				gain_counter = GAIN_AWB_PERIOD;
			}
			previous = gain;
			if (gain > (1023 - 13))
				gain = (1023 - 13);
			recompute = 0;
			_control_gain(controlfd, 1023 - gain);
		}
		if (previous != gain)
		{
			recompute = 1;
		}
	}
	gain_counter %= GAIN_AWB_PERIOD;
	return 0;
}

static void *_fifo_open(int atfd, const char *name, int access)
{
	int fd = -1;
	int mode = 0;
	if (!strcmp(name, "-"))
		return NULL;
	struct stat sb;
	if (fstatat(atfd, name, &sb, 0) &&
		((sb.st_mode & S_IFMT) != S_IFIFO))
	{
		err("sfile: file %s is not a named pipe", name);
		return NULL;
	}
	mode = O_RDONLY;
	if (faccessat(atfd, name, R_OK, 0) < 0)
		mkfifoat(atfd, name , access);

	warn("waiting access to %s", name);
	fd = openat(atfd, name, mode, access);
	if (fd <= 0)
	{
		err("fifo: opening %s error: %m", name);
		return NULL;
	}
	warn("fifo: %s opened", name);
	return (void*)(long)fd;
}

void _fifo_close(void *arg)
{
	int fd = (long)arg;
	close(fd);
}

int _fifo_receive(void *arg, void * controlfd)
{
	int fd = (long)arg;
	int maxfd = fd;
	fd_set rfds = {0};
	FD_ZERO(&rfds);
	FD_SET(fd, &rfds);
	int ret = 0;

	ret = select(maxfd + 1, &rfds, NULL, NULL, NULL);
	if (ret && FD_ISSET(fd, &rfds))
	{
		struct bcm2835_isp_stats statistics = {0};
		ret = read(fd, &statistics, sizeof(uint32_t) * 2);
		if (ret > 0)
			ret = read(fd, (unsigned char *)(&statistics) + sizeof(uint32_t) * 2,
					statistics.size - sizeof(uint32_t) * 2);
		if (ret > 0)
			ret = _algo_awb(statistics.awb_stats, AWB_REGIONS, controlfd);
	}
	return ret;
}

void help(void)
{
	fprintf(stderr, "Usage: $s [OPTION]...\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "  -h          display this message\n");
	fprintf(stderr, "  -W <path>   change working directory\n");
	fprintf(stderr, "  -D          daemonize the process\n");
	fprintf(stderr, "  -P <file>   set the daemonized pid into file\n");
	fprintf(stderr, "  -U <user>   set the owner of the daemonized process\n");
	fprintf(stderr, "  -s <fifo>   set the statistic fifo path\n");
	fprintf(stderr, "  -c <fifo>   set the control fifo path\n");
	fprintf(stderr, "  -g          disable autogain at startup\n");
}

int main(int argc, char *const argv[])
{
	const char *logfile = "-";
	const char *rootfs = NULL;
	const char *pidfile= NULL;
	const char *owner= NULL;
	const char *statistics = "/tmp/statistics";
	const char *control = FASTSETTING_DEFAULT_SERVER;
	int mode = 0;

	int opt;
	do
	{
		opt = getopt(argc, argv, "hL:W:DKP:U:s:c:g");
		switch (opt)
		{
			case 'h':
				help();
				exit(-1);
			break;
			case 'D':
				mode |= MODE_DAEMONIZE;
			break;
			case 'K':
				mode |= MODE_KILLDAEMON;
			break;
			case 'P':
				pidfile = optarg;
			break;
			case 'U':
				owner = optarg;
			break;
			case 'L':
				logfile = optarg;
			break;
			case 'W':
				rootfs = optarg;
			break;
			case 's':
				statistics = optarg;
			break;
			case 'c':
				control = optarg;
			break;
			case 'g':
				mode |= MODE_AUTOGAIN;
			break;
		}
	} while(opt != -1);

	if (mode & MODE_KILLDAEMON)
	{
		killdaemon(pidfile);
		exit(0);
	}

	if (strcmp(logfile,"-"))
	{
		int logfd = open(logfile, O_WRONLY | O_CREAT | O_TRUNC, 00644);
		if (logfd > 0)
		{
			dup2(logfd, 1);
			dup2(logfd, 2);
			close(logfd);
		}
		else
			err("log file error %m");
	}
	daemonize((mode & MODE_DAEMONIZE) == MODE_DAEMONIZE, pidfile, owner, rootfs);

	void *statisticsfd = NULL;
	statisticsfd = _fifo_open(AT_FDCWD, statistics, 0644);

	void * controlfd = NULL;
	controlfd = _control_open(AT_FDCWD, control);
	if (mode & MODE_AUTOGAIN)
		_control_autogain(controlfd, 0);

	while (isrunning())
	{
		_fifo_receive(statisticsfd, controlfd);
	}
	_fifo_close(statisticsfd);
	_control_close(controlfd);

	killdaemon(pidfile);
	dbg("process died");
	return 0;
}
