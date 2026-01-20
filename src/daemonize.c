/*****************************************************************************
 * deamonize.c
 * this file is part of https://github.com/mchalain
 *****************************************************************************
 * Copyright (C) 2016-2024
 *
 * Authors: Marc Chalain <marc.chalain@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject
 * to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *****************************************************************************/
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <pwd.h>
#if _POSIX_C_SOURCE >= 199309L
# define HAVE_SIGACTION
#endif

#include "log.h"

static char _run = 0;
#ifdef HAVE_SIGACTION
static void _handler(int sig, siginfo_t *UNUSED(si), void *UNUSED(arg))
#else
static void _handler(int sig)
#endif
{
	err("main: signal %d", sig);
	if (sig == SIGSEGV)
	{
#ifdef BACKTRACE
		void *array[10];
		size_t size;

		// get void*'s for all entries on the stack
		size = backtrace(array, 10);

		// print out all the frames to stderr
		backtrace_symbols_fd(array, size, STDERR_FILENO);
#endif
		exit(1);
	}
	_run = 'k';
}

unsigned char isrunning()
{
	return _run == 'r';
}

static int _pidfd = -1;
static int _setpidfile(const char *pidfile)
{
	if (pidfile[0] != '\0')
	{
		_pidfd = open(pidfile,O_WRONLY|O_CREAT|O_TRUNC,0644);
		if (_pidfd > 0)
		{
			char buffer[12];
			ssize_t length;
			pid_t pid = 1;

			struct flock fl;
			memset(&fl, 0, sizeof(fl));
			fl.l_type = F_WRLCK;
			fl.l_whence = SEEK_SET;
			fl.l_start = 0;
			fl.l_len = 0;
			fl.l_pid = 0;
			if (fcntl(_pidfd, F_SETLK, &fl) == -1) {
				err("server already running");
				close(_pidfd);
				exit(1);
			}

			pid = getpid();
			length = snprintf(buffer, 12, "%.10d\n", pid);
			ssize_t len = write(_pidfd, buffer, length);
			if (len != length)
				err("pid file error %s", strerror(errno));
			fsync(_pidfd);
			/**
			 * the file must be open while the process is running
			close pidfd
			 */
		}
		else
		{
			err("pid file error %s", strerror(errno));
			pidfile = NULL;
			return -1;
		}
	}
	return 0;
}

static int _setowner(const char *user)
{
	int ret = -1;
	struct passwd *pw;
	pw = getpwnam(user);
	if (pw != NULL)
	{
		uid_t uid;
		uid = getuid();
		//only "saved set-uid", "uid" and "euid" may be set
		//first step: set the "saved set-uid" (root)
		if (seteuid(uid) < 0)
			warn("not enought rights to change user");
		//second step: set the new "euid"
		else if (setegid(pw->pw_gid) < 0)
			warn("not enought rights to change group");
		else if (seteuid(pw->pw_uid) < 0)
			warn("not enought rights to change user");
		else
			ret = 0;
	}
	return ret;
}

static int _chroot(const char *rootfs)
{
	if (chroot(rootfs) == 0)
	{
		warn("daemon runs insizde a sandbix");
	}
	else if (chdir(rootfs) != 0)
	{
		err("%s directory not accessible", rootfs);
		return -1;
	}
	return 0;
}

int daemonize(unsigned char onoff, const char *pidfile, const char *owner, const char *rootfs)
{
	pid_t pid = -1;
	pid_t sid = -1;
	if ( getppid() == 1 )
	{
		return -1;
	}
	if (onoff && (pid = fork()) == (pid_t)-1)
	{
		err("process may not be daemonized");
		return -1;
	}
	if (pid > 0)
	{
		dbg("start daemon on pid %d", pid);
		exit(0);
	}
	if (pid == 0 && (sid = setsid()) == (pid_t)-1)
	{
		err("process may not be owner group");
		return -1;
	}

	if (pidfile != NULL && _setpidfile(pidfile))
		return -1;

	if (owner != NULL && _setowner(owner))
		return -1;

	if (rootfs != NULL && _chroot(rootfs))
		return -1;

#ifdef HAVE_SIGACTION
	struct sigaction action;
	action.sa_flags = SA_SIGINFO;
	sigemptyset(&action.sa_mask);
	action.sa_sigaction = _handler;
	sigaction(SIGTERM, &action, NULL);
	sigaction(SIGINT, &action, NULL);
# ifdef BACKTRACE
	sigaction(SIGSEGV, &action, NULL);
# endif
#else
	signal(SIGTERM, handler);
	signal(SIGINT, handler);
# ifdef BACKTRACE
	signal(SIGSEGV, handler);
# endif
#endif
	_run = 'r';

	return 0;
}

void killdaemon(const char *pidfile)
{
	if (_pidfd > 0)
	{
		close(_pidfd);
		_pidfd = -1;
		_run = 's';
	}
	else if (pidfile != NULL)
	{
		_pidfd = open(pidfile,O_RDWR);
		if (_pidfd > 0)
		{
			struct flock fl;
			memset(&fl, 0, sizeof(fl));
			fl.l_type = F_WRLCK;
			fl.l_whence = SEEK_SET;
			fl.l_start = 0;
			fl.l_len = 0;
			fl.l_pid = 0;
			if (fcntl(_pidfd, F_GETLK, &fl) == -1)
				err("lock error %s", strerror(errno));
			else if (fl.l_type == F_UNLCK)
				err("server not running");
			else if (getpid() != fl.l_pid)
				kill(fl.l_pid, SIGTERM);
			close(_pidfd);
		}
	}
	else
		_run = 's';
	if (pidfile && !access(pidfile, W_OK))
		unlink(pidfile);
}
