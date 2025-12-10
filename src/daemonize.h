#ifndef __DEAMONIZE_H__
#define __DEAMONIZE_H__

int daemonize(unsigned char onoff, const char *pidfile, const char *owner, const char *rootfs);
void killdaemon(const char *pidfile);
unsigned char isrunning();

#endif
