bin-y+=fastvideo
fastvideo_SOURCES+=fastvideo.c
fastvideo_SOURCES+=daemonize.c
fastvideo_SOURCES-$(HAVE_JANSSON)+=config.c
fastvideo_LIBS+=fastvideo
fastvideo_LIBRARY+=jansson
fastvideo_CFLAGS+=-fPIC -rdynamic
fastvideo_LDFLAGS+=-fPIC -rdynamic
fastvideo_CFLAGS-$(SANITIZER)+=-fsanitize=address
fastvideo_LDFLAGS-$(SANITIZER)+=-fsanitize=address -static-libasan
