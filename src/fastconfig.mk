bin-$(HAVE_JANSSON)+=fastconfig
fastconfig_SOURCES+=fastconfig.c
fastconfig_SOURCES-$(HAVE_JANSSON)+=config.c
fastconfig_LIBS+=fastvideo
fastconfig_LIBRARY+=jansson
