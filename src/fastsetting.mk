bin-y+=fastsetting
fastsetting_SOURCES+=fastsetting.c
fastsetting_SOURCES+=server.c
fastsetting_SOURCES+=client.c
fastsetting_SOURCES+=daemonize.c
fastsetting_SOURCES-$(HAVE_JANSSON)+=config.c
fastsetting_LIBS+=fastvideo
fastsetting_LIBRARY+=jansson
fastsetting_CFLAGS-$(SANITIZER)+=-fsanitize=address
fastsetting_LDFLAGS-$(SANITIZER)+=-fsanitize=address -static-libasan
