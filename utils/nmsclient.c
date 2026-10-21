#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "log.h"
#include "daemonize.h"
#include "rectshm.h"

/*
 * Reads the NMS frames of a YOLO model (e.g. yolov8) run by the shailo
 * device (fastvideo/src/shailo.c) from a fifo, and draws the detections
 * over the video with the rectangles.glsl program.
 */

#define DEFAULT_WORKDIR "/tmp/fastvideo.segl/"
#define DEFAULT_CONFIG "/etc/fastvideo/raspicam.json"
#define DEFAULT_DEVICE "gpu"
#define DEFAULT_PROGRAM "rectangles"
#define DEFAULT_CLASSES 80
#define DEFAULT_BBOXES 100
#define DEFAULT_SCORE 0.5

/// hailo_bbox_float32_t
typedef struct __attribute__((packed))
{
	float y_min;
	float x_min;
	float y_max;
	float x_max;
	float score;
} NmsBox_t;

/// hailo_detection_t
typedef struct __attribute__((packed))
{
	float y_min;
	float x_min;
	float y_max;
	float x_max;
	float score;
	uint16_t class_id;
} NmsDetection_t;

_Static_assert(sizeof(NmsBox_t) == 20, "hailo_bbox_float32_t is 20 bytes");
_Static_assert(sizeof(NmsDetection_t) == 22, "hailo_detection_t is 22 bytes");

typedef struct Detection_s Detection_t;
struct Detection_s
{
	int class_id;
	float score;
	float x1, y1, x2, y2;
};

typedef enum
{
	Layout_ByClass_e,
	Layout_ByScore_e,
} Layout_e;

typedef struct Filter_s Filter_t;
struct Filter_s
{
	float score;
	int class_id;
};

static volatile sig_atomic_t g_run = 1;

static void _stop(int signum)
{
	(void)signum;
	g_run = 0;
}

static void help(const char *name)
{
	fprintf(stderr, "Usage: %s -f <fifo> [OPTION]...\n", name);
	fprintf(stderr, "\n");
	fprintf(stderr, "  -h            display this message\n");
	fprintf(stderr, "  -f <fifo>     fifo written by fastvideo (created if missing)\n");
	fprintf(stderr, "  -m <layout>   NMS layout: \"byclass\" (default) or \"byscore\"\n");
	fprintf(stderr, "  -c <classes>  number of classes of the model (default %d)\n", DEFAULT_CLASSES);
	fprintf(stderr, "  -b <bboxes>   max boxes per class (byclass) or in total (byscore) (default %d per class)\n", DEFAULT_BBOXES);
	fprintf(stderr, "  -S <size>     frame size in bytes (default: computed from -m, -c and -b)\n");
	fprintf(stderr, "  -s <score>    minimum score (default %.2f)\n", DEFAULT_SCORE);
	fprintf(stderr, "  -k <class>    keep only this class id (default: all)\n");
	fprintf(stderr, "  -r <x,y,w,h>  region of the image seen by the model (default 0,0,1,1)\n");
	fprintf(stderr, "  -N <slots>    max rectangles to draw (default: all the vRects slots)\n");
	fprintf(stderr, "  -n            do not draw, only print (with -v)\n");
	fprintf(stderr, "  -v            print the detections of each frame as a JSON line\n");
	fprintf(stderr, "  -j <json>     fastvideo configuration file (default %s)\n", DEFAULT_CONFIG);
	fprintf(stderr, "  -d <device>   name of the gpu device into the configuration (default %s)\n", DEFAULT_DEVICE);
	fprintf(stderr, "  -p <program>  name of the GLSL program of this device (default %s)\n", DEFAULT_PROGRAM);
	fprintf(stderr, "  -W <path>     segl working directory (default %s)\n", DEFAULT_WORKDIR);
	fprintf(stderr, "  -D            daemonize (fork to background)\n");
	fprintf(stderr, "  -L <logfile>  redirect stdout/stderr to this file\n");
	fprintf(stderr, "  -P <pidfile>  write the daemon pid to this file\n");
}

static int _keep(Filter_t *filter, int class_id, float score)
{
	return score >= filter->score && (filter->class_id < 0 || filter->class_id == class_id);
}

/// returns the number of detections, -1 if the frame is not a valid one
static int _parse_byclass(const uint8_t *frame, size_t size, int nclasses, int nbboxes,
			Filter_t *filter, Detection_t *dets)
{
	const uint8_t *ptr = frame;
	const uint8_t *end = frame + size;
	int ndets = 0;
	for (int class_id = 0; class_id < nclasses; class_id++)
	{
		float countf;
		if ((size_t)(end - ptr) < sizeof(countf))
			return -1;
		memcpy(&countf, ptr, sizeof(countf));
		ptr += sizeof(countf);
		if (!(countf >= 0.0 && countf <= nbboxes))
			return -1;
		uint32_t count = (uint32_t)countf;
		if ((size_t)(end - ptr) < count * sizeof(NmsBox_t))
			return -1;
		for (uint32_t i = 0; i < count; i++, ptr += sizeof(NmsBox_t))
		{
			NmsBox_t box;
			memcpy(&box, ptr, sizeof(box));
			if (!_keep(filter, class_id, box.score))
				continue;
			dets[ndets++] = (Detection_t){ class_id, box.score, box.x_min, box.y_min, box.x_max, box.y_max };
		}
	}
	return ndets;
}

static int _parse_byscore(const uint8_t *frame, size_t size, Filter_t *filter, Detection_t *dets)
{
	uint16_t count;
	if (size < sizeof(count))
		return -1;
	memcpy(&count, frame, sizeof(count));
	if (count > (size - sizeof(count)) / sizeof(NmsDetection_t))
		return -1;
	const uint8_t *ptr = frame + sizeof(count);
	int ndets = 0;
	for (uint16_t i = 0; i < count; i++, ptr += sizeof(NmsDetection_t))
	{
		NmsDetection_t d;
		memcpy(&d, ptr, sizeof(d));
		if (!_keep(filter, d.class_id, d.score))
			continue;
		dets[ndets++] = (Detection_t){ d.class_id, d.score, d.x_min, d.y_min, d.x_max, d.y_max };
	}
	return ndets;
}

static int _byscore(const void *a, const void *b)
{
	float sa = ((const Detection_t *)a)->score;
	float sb = ((const Detection_t *)b)->score;
	return (sa < sb) - (sa > sb);
}

static float _clamp(float v)
{
	return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
}

/// returns 1 on a full frame, 0 at the end of the stream, -1 on error or signal
static int _readframe(int fd, uint8_t *frame, size_t size)
{
	size_t length = 0;
	while (length < size)
	{
		ssize_t ret = read(fd, frame + length, size - length);
		if (ret < 0)
		{
			if (errno == EINTR && g_run)
				continue;
			if (errno != EINTR)
				err("nmsclient: fifo read error %m");
			return -1;
		}
		if (ret == 0)
		{
			if (length > 0)
				warn("nmsclient: stream closed in the middle of a frame (%zu/%zu bytes)", length, size);
			return 0;
		}
		length += ret;
	}
	return 1;
}

int main(int argc, char *const argv[])
{
	const char *fifo = NULL;
	const char *workdir = DEFAULT_WORKDIR;
	const char *configfile = DEFAULT_CONFIG;
	const char *device = DEFAULT_DEVICE;
	const char *program = DEFAULT_PROGRAM;
	Layout_e layout = Layout_ByClass_e;
	int nclasses = DEFAULT_CLASSES;
	int nbboxes = DEFAULT_BBOXES;
	size_t framesize = 0;
	Filter_t filter = { .score = DEFAULT_SCORE, .class_id = -1 };
	float region[4] = { 0.0, 0.0, 1.0, 1.0 };
	int maxslots = -1;
	int draw = 1;
	int verbose = 0;
	int do_daemonize = 0;
	const char *logfile = NULL;
	const char *pidfile = NULL;

	int opt;
	while ((opt = getopt(argc, argv, "hf:m:c:b:S:s:k:r:N:nvj:d:p:W:DL:P:")) != -1)
	{
		switch (opt)
		{
			case 'f':
				fifo = optarg;
			break;
			case 'm':
				if (!strcmp(optarg, "byclass"))
					layout = Layout_ByClass_e;
				else if (!strcmp(optarg, "byscore"))
					layout = Layout_ByScore_e;
				else
				{
					err("nmsclient: unknown layout %s", optarg);
					return 1;
				}
			break;
			case 'c':
				nclasses = strtol(optarg, NULL, 0);
			break;
			case 'b':
				nbboxes = strtol(optarg, NULL, 0);
			break;
			case 'S':
				framesize = strtoul(optarg, NULL, 0);
			break;
			case 's':
				filter.score = strtof(optarg, NULL);
			break;
			case 'k':
				filter.class_id = strtol(optarg, NULL, 0);
			break;
			case 'r':
				if (sscanf(optarg, "%f,%f,%f,%f", &region[0], &region[1], &region[2], &region[3]) != 4 ||
					region[2] <= 0.0 || region[3] <= 0.0)
				{
					err("nmsclient: %s: expected x,y,w,h with w > 0 and h > 0", optarg);
					return 1;
				}
			break;
			case 'N':
				maxslots = strtol(optarg, NULL, 0);
			break;
			case 'n':
				draw = 0;
			break;
			case 'v':
				verbose = 1;
			break;
			case 'j':
				configfile = optarg;
			break;
			case 'd':
				device = optarg;
			break;
			case 'p':
				program = optarg;
			break;
			case 'W':
				workdir = optarg;
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
			case 'h':
			default:
				help(argv[0]);
				return 1;
		}
	}
	if (fifo == NULL || nclasses <= 0 || nbboxes <= 0)
	{
		help(argv[0]);
		return 1;
	}

	/// worst-case frame size of the output vstream (hailo_get_vstream_frame_size)
	size_t maxdets;
	if (layout == Layout_ByClass_e)
	{
		maxdets = (size_t)nclasses * nbboxes;
		if (framesize == 0)
			framesize = nclasses * (sizeof(float) + nbboxes * sizeof(NmsBox_t));
	}
	else
	{
		if (framesize == 0)
			framesize = sizeof(uint16_t) + (size_t)nclasses * nbboxes * sizeof(NmsDetection_t);
		maxdets = (framesize - sizeof(uint16_t)) / sizeof(NmsDetection_t);
	}

	RectShm_t *shm = NULL;
	int nslots = 0;
	if (draw)
	{
		shm = rectshm_open(configfile, device, program, workdir);
		if (shm == NULL)
			return 1;
		nslots = rectshm_count(shm);
		if (maxslots >= 0 && maxslots < nslots)
			nslots = maxslots;
	}

	struct stat st;
	if (stat(fifo, &st) != 0 && mkfifo(fifo, 0664) != 0)
	{
		err("nmsclient: fifo %s creation error %m", fifo);
		return 1;
	}

	if (logfile)
		daemon_setlogfile(logfile);
	/// after the configuration checks, to report their errors on the terminal
	daemonize(do_daemonize, NULL, pidfile, NULL, NULL);

	/// no SA_RESTART: a signal must interrupt the blocking open() and read()
	struct sigaction action = { .sa_handler = _stop };
	sigaction(SIGINT, &action, NULL);
	sigaction(SIGTERM, &action, NULL);

	uint8_t *frame = malloc(framesize);
	Detection_t *dets = malloc(maxdets * sizeof(*dets));
	if (frame == NULL || dets == NULL)
	{
		err("nmsclient: memory allocation error");
		return 1;
	}
	warn("nmsclient: read %zu bytes frames from %s", framesize, fifo);

	unsigned long nframe = 0;
	int ret = 0;
	while (g_run)
	{
		/// the writer may restart: reopen the fifo at each end of stream
		int fd = open(fifo, O_RDONLY);
		if (fd < 0)
		{
			if (errno != EINTR)
			{
				err("nmsclient: fifo %s open error %m", fifo);
				ret = 1;
			}
			break;
		}
		int rret = 0;
		while (g_run && (rret = _readframe(fd, frame, framesize)) > 0)
		{
			int ndets;
			if (layout == Layout_ByClass_e)
				ndets = _parse_byclass(frame, framesize, nclasses, nbboxes, &filter, dets);
			else
				ndets = _parse_byscore(frame, framesize, &filter, dets);
			nframe++;
			if (ndets < 0)
			{
				err("nmsclient: frame %lu is not a %s NMS frame of %zu bytes, check -m, -c, -b and -S",
					nframe, layout == Layout_ByClass_e ? "byclass" : "byscore", framesize);
				continue;
			}
			qsort(dets, ndets, sizeof(*dets), _byscore);
			for (int i = 0; i < ndets; i++)
			{
				dets[i].x1 = _clamp(region[0] + dets[i].x1 * region[2]);
				dets[i].y1 = _clamp(region[1] + dets[i].y1 * region[3]);
				dets[i].x2 = _clamp(region[0] + dets[i].x2 * region[2]);
				dets[i].y2 = _clamp(region[1] + dets[i].y2 * region[3]);
			}
			for (int i = 0; i < nslots; i++)
			{
				if (i < ndets)
					rectshm_set(shm, i, dets[i].x1, dets[i].y1, dets[i].x2, dets[i].y2);
				else if (!rectshm_isempty(shm, i))
					rectshm_clear(shm, i);
			}
			if (verbose)
			{
				printf("{\"frame\":%lu,\"detections\":[", nframe);
				for (int i = 0; i < ndets; i++)
					printf("%s{\"class\":%d,\"score\":%.3f,\"x1\":%.4f,\"y1\":%.4f,\"x2\":%.4f,\"y2\":%.4f}",
						i ? "," : "", dets[i].class_id, dets[i].score,
						dets[i].x1, dets[i].y1, dets[i].x2, dets[i].y2);
				printf("]}\n");
				fflush(stdout);
			}
		}
		close(fd);
		if (rret < 0)
			break;
	}

	/// do not leave the last detections on the screen
	for (int i = 0; i < nslots; i++)
		rectshm_clear(shm, i);
	if (shm)
		rectshm_close(shm);
	free(dets);
	free(frame);
	warn("nmsclient: %lu frames read", nframe);
	killdaemon(pidfile);
	return ret;
}
