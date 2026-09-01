subdir-$(CLIENTS)+=fastsetting-cli.mk

hostbin-y+=mpegts-sync
mpegts-sync_SOURCES+=mpegts-sync.c
mpegts-sync_SOURCES+=$(srcdir)src/client.c
mpegts-sync_CFLAGS+=-I$(srcdir)src
mpegts-sync_LDFLAGS+=-L$(builddir)src

hostbin-y+=unixclient
unixclient_SOURCES+=unixclient.c
unixclient_SOURCES+=$(srcdir)src/client.c
unixclient_CFLAGS+=-I$(srcdir)src
unixclient_LDFLAGS+=-L$(builddir)src

hostbin-y+=unixproxy
unixproxy_SOURCES+=unixproxy.c
unixproxy_SOURCES+=$(srcdir)src/client.c
unixproxy_CFLAGS+=-I$(srcdir)src
unixproxy_LDFLAGS+=-L$(builddir)src

data-y+=rpi3cam-setup.sh
data-y+=rpi5cam-setup.sh

ifneq ($(wildcard $(SYSROOT)/usr/include/linux/bcm2835-isp.h),)
  RPIVC4=y
endif
bin-$(RPIVC4)+=rpivc4_alg
rpivc4_alg_SOURCES=rpivc4_alg.c
rpivc4_alg_SOURCES+=$(srcdir)src/client.c
rpivc4_alg_SOURCES+=$(srcdir)src/daemonize.c
rpivc4_alg_CFLAGS+=-I$(srcdir)src
rpivc4_alg_LDFLAGS+=-L$(builddir)src

bin-$(HAVE_DRM)+=hdmi_connected
hdmi_connected_SOURCES+=hdmi_connected.c
hdmi_connected_CFLAGS+=-I$(SYSROOT)/usr/include/libdrm
hdmi_connected_LIBRARY+=libdrm

ifneq ($(wildcard $(SYSROOT)/usr/include/gpiod.h),)
  HAVE_GPIOD=y
endif
bin-$(HAVE_GPIOD)+=shoot
shoot_SOURCES+=shoot.c
shoot_SOURCES+=$(srcdir)src/daemonize.c
shoot_LIBRARY+=libgpiod
shoot_CFLAGS+=-I$(srcdir)src
shoot_LDFLAGS+=-L$(builddir)src
