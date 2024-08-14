bin-y+=fastvideo
fastvideo_SOURCES+=fastvideo.c
fastvideo_SOURCES+=daemonize.c
fastvideo_SOURCES-$(HAVE_JANSSON)+=config.c
fastvideo_LIBS+=fastvideo
fastvideo_LIBRARY+=jansson
