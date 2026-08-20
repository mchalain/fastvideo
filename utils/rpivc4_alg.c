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
		err("rpivc4_alg: fastsetting not found");
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
		err("rpivc4_alg: fastsetting reject \"auto exposure\" control");
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
		err("rpivc4_alg: fastsetting reject \"Analogue Gain\" control");
	return 0;
}

static void _control_close(void * arg)
{
	client_destroy(arg);
}

/*
 * region index into the 16x12 AWB region grid (AWB_REGIONS, rows 0-11 x
 * cols 0-15 - see linux/bcm2835-isp.h) used as the exposure/gain metering
 * point. row 6 (of 12) / col 8 (of 16) is the closest grid cell to true
 * center (12 and 16 are both even, so there's no single exact center
 * cell). Kept as a file-global variable, not a #define, so it's easy to
 * retarget/tune while iterating on hardware without touching the
 * algorithm itself.
 */
#define AGC_REGION_DEFAULT (6 * 16 + 8)
static int agc_region_main = AGC_REGION_DEFAULT;

#define GAIN_AGC_PERIOD 30
/* divides the raw region brightness sum into a gain-scale value (see
 * _algo_agc() below) - there is no separate explicit "target brightness"
 * setpoint in this algorithm, this ratio is what implicitly determines
 * it. Kept as a file-global variable, settable via -r, for the same
 * on-hardware tuning reason as agc_region_main above. */
#define GAIN_RATIO_DEFAULT 30
static int gain_ratio = GAIN_RATIO_DEFAULT;

/* Stabilization parameters */
#define AGC_EMA_ALPHA       0.15f   /* EMA filter coefficient (0 < α ≤ 1, smaller = smoother) */
#define AGC_DEADBAND        8       /* Deadband: no adjustment if |error| < deadband */
#define AGC_STEP_DIVISOR    3       /* Step proportional to the error (error / divisor) */
#define AGC_MAX_STEP        80      /* Safety cap on the step (avoids too abrupt a jump) */
#define AGC_SETTLE_CYCLES   3       /* Number of wait cycles after an adjustment */
#define AGC_GAIN_MIN        13
#define AGC_GAIN_MAX        (1023 - 13)

static int _algo_agc(struct bcm2835_isp_stats_region *stats, int nbregions, void *controlfd)
{
	static int gain_counter = 0;
	static float ema_gain = 0.0f;
	static int ema_initialized = 0;
	static int applied_gain = -1;
	static int settle_counter = 0;

	gain_counter++;
	if (gain_counter < GAIN_AGC_PERIOD)
		return 0;
	gain_counter = 0;

	if (settle_counter > 0)
	{
		settle_counter--;
		return 0;
	}

	if (agc_region_main < 0 || agc_region_main >= nbregions)
	{
		err("rpivc4_alg: agc region %d out of range (0-%d)", agc_region_main, nbregions - 1);
		return -1;
	}
	struct bcm2835_isp_stats_region *region = &stats[agc_region_main];

	int32_t target_gain;
	if (region->counted == 0)
	{
		target_gain = AGC_GAIN_MIN;
		warn("rpivc4_alg: region %d starved (counted=0) - driving for max gain", agc_region_main);
	}
	else
	{
		int32_t raw_gain = (int32_t)((region->r_sum + region->g_sum + region->b_sum) / region->counted);
		raw_gain /= gain_ratio;
		if (region->counted < 100)
			raw_gain *= 2;

		if (!ema_initialized)
		{
			ema_gain = (float)raw_gain;
			ema_initialized = 1;
		}
		else
		{
			ema_gain = AGC_EMA_ALPHA * (float)raw_gain + (1.0f - AGC_EMA_ALPHA) * ema_gain;
		}

		target_gain = (int32_t)(ema_gain + 0.5f);
	}

	/* Clamp the target within the limits */
	if (target_gain < AGC_GAIN_MIN)
		target_gain = AGC_GAIN_MIN;
	if (target_gain > AGC_GAIN_MAX)
		target_gain = AGC_GAIN_MAX;

	if (applied_gain < 0)
	{
		applied_gain = target_gain;
		_control_gain(controlfd, 1023 - applied_gain);
		warn("rpivc4_alg: region %d counted=%u target_gain=%d applied_gain=%d (init)",
			agc_region_main, region->counted, target_gain, applied_gain);
		settle_counter = AGC_SETTLE_CYCLES;
		return 0;
	}

	int32_t error = target_gain - applied_gain;
	if (abs(error) <= AGC_DEADBAND)
		return 0;

	int32_t step = error / AGC_STEP_DIVISOR;
	if (step > AGC_MAX_STEP)
		step = AGC_MAX_STEP;
	else if (step < -AGC_MAX_STEP)
		step = -AGC_MAX_STEP;

	applied_gain += step;

	if (applied_gain < AGC_GAIN_MIN)
		applied_gain = AGC_GAIN_MIN;
	if (applied_gain > AGC_GAIN_MAX)
		applied_gain = AGC_GAIN_MAX;

	_control_gain(controlfd, 1023 - applied_gain);
	warn("rpivc4_alg: region %d counted=%u target_gain=%d applied_gain=%d step=%d",
		agc_region_main, region->counted, target_gain, applied_gain, step);

	settle_counter = AGC_SETTLE_CYCLES;

	return 0;
}

static void *_fifo_open(int atfd, const char *name, int access)
{
	int fd = -1;
	int mode = 0;
	if (!strcmp(name, "-"))
		return NULL;
	if (faccessat(atfd, name, R_OK| W_OK | F_OK, 0) < 0 &&
		mkfifoat(atfd, name , 0664))
	{
		err("rpivc4_alg: %s creation error", name);
	}
	struct stat sb;
	if (fstatat(atfd, name, &sb, 0) &&
		((sb.st_mode & S_IFMT) != S_IFIFO))
	{
		err("rpivc4_alg: file %s is not a named pipe", name);
		return NULL;
	}
	mode = O_RDONLY;

	warn("rpivc4_alg: waiting access to %s", name);
	fd = openat(atfd, name, mode, access);
	if (fd <= 0)
	{
		err("rpivc4_alg: opening %s error: %m", name);
		return NULL;
	}
	warn("rpivc4_alg: %s opened", name);
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
		{
			size_t remaining = statistics.size - sizeof(uint32_t) * 2;
			if (statistics.size > sizeof(statistics))
			{
				err("rpivc4_alg: stats size %u exceeds local struct size %zu, clamping",
					statistics.size, sizeof(statistics));
				remaining = sizeof(statistics) - sizeof(uint32_t) * 2;
			}
			ret = read(fd, (unsigned char *)(&statistics) + sizeof(uint32_t) * 2, remaining);
		}
		if (ret > 0)
			ret = _algo_agc(statistics.awb_stats, AWB_REGIONS, controlfd);
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
	fprintf(stderr, "  -r <ratio>  gain ratio divisor (default %d)\n", GAIN_RATIO_DEFAULT);
}

int main(int argc, char *const argv[])
{
	const char *logfile = NULL;
	const char *rootfs = NULL;
	const char *pidfile= NULL;
	const char *owner= NULL;
	const char *statistics = "/tmp/statistics";
	const char *control = FASTSETTING_DEFAULT_SERVER;
	int mode = 0;

	int opt;
	do
	{
		opt = getopt(argc, argv, "hL:W:DKP:U:s:c:gr:");
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
			case 'r':
				gain_ratio = atoi(optarg);
			break;
		}
	} while(opt != -1);

	if (mode & MODE_KILLDAEMON)
	{
		killdaemon(pidfile);
		exit(0);
	}

	daemonize((mode & MODE_DAEMONIZE) == MODE_DAEMONIZE, logfile, pidfile, owner, rootfs);

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
	dbg("rpivc4_alg: process died");
	return 0;
}
