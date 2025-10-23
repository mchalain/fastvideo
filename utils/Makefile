subdir-y+=fastsetting-cli.mk

bin-y+=mpegts-sync
mpegts-sync_SOURCES+=mpegts-sync.c
mpegts-sync_SOURCES+=$(srcdir)src/client.c
mpegts-sync_CFLAGS+=-I$(srcdir)src
mpegts-sync_LDFLAGS+=-L$(builddir)src

data-y+=rpi3cam-setup.sh
data-y+=rpi5cam-setup.sh
