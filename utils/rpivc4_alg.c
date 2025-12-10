#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>

#include <linux/bcm2835-isp.h>

#include "log.h"
#include "daemonize.h"

#define MODE_DAEMONIZE 0x01
#define MODE_KILLDAEMON 0x02

static void *_control_open(int atfd, const char *name)
{
	return NULL;
}

static int _control_gain(void * arg, int gain)
{
	dbg("awb gain %d", (uint32_t)gain);
	return 0;
}

static void _control_close(void * arg)
{
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
		static int analog_gain = 852;
		static uint64_t previous = 0;
		static int recompute = 1;

		struct bcm2835_isp_stats_region *region = &stats[GAIN_AWB_REGION_MAIN];
		if (region->counted == 0)
			return 0;
		uint64_t gain = (region->r_sum + region->g_sum + region->b_sum) / region->counted;
		gain /= GAIN_RATIO;
		if (previous == 0)
			previous = gain;
		if (recompute && (previous != gain))
		{
			analog_gain += gain - previous;
			if (analog_gain > 1023)
				analog_gain = 1023;
			if (analog_gain < 13)
				analog_gain = 13;
			previous = gain;
			recompute = 0;
			_control_gain(controlfd, gain);
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
		(sb.st_mode & S_IFMT != S_IFIFO))
	{
		err("sfile: file %s is not a named pipe", name);
		return NULL;
	}
	mode = O_RDONLY;
	if (faccessat(atfd, name, R_OK, 0) < 0)
		fd = mkfifoat(atfd, name , access);
	else
		fd = openat(atfd, name, mode, access);
	if (fd <= 0)
		return NULL;
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
	fprintf(stderr, "  -C <file>   set the configuration file\n");
}

int main(int argc, char *const argv[])
{
	const char *logfile = "-";
	const char *rootfs = NULL;
	const char *pidfile= NULL;
	const char *owner= NULL;
	const char *configfile = NULL;
	const char *statistics = "/tmp/camera/statistics";
	const char *control = "/tmp/camera/control";
	int mode = 0;

	int opt;
	do
	{
		opt = getopt(argc, argv, "hL:W:DKP:U:C:s:");
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
			case 'C':
				configfile = optarg;
			break;
			case 's':
				statistics = optarg;
			break;
			case 'c':
				control = optarg;
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
