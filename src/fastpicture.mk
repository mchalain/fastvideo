bin-y+=fastpicture
fastpicture_SOURCES+=fastpicture.c
fastpicture_SOURCES-$(HAVE_JANSSON)+=config.c
fastpicture_LIBS+=fastvideo
fastpicture_LIBRARY+=jansson
fastpicture_CFLAGS-$(SANITIZER)+=-fsanitize=address
fastpicture_LDFLAGS-$(SANITIZER)+=-fsanitize=address -static-libasan
