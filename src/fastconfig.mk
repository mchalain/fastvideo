bin-$(HAVE_JANSSON)+=fastconfig
fastconfig_SOURCES+=fastconfig.c
fastconfig_LIBS+=fastvideo
fastconfig_LIBRARY+=jansson
fastconfig_CFLAGS-$(SANITIZER)+=-fsanitize=address
fastconfig_LDFLAGS-$(SANITIZER)+=-fsanitize=address -static-libasan
