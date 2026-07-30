ifeq ($(ARCH),arm)
ARM=y
ARMV7=y
endif
ifeq ($(ARCH),aarch64)
ARMV8=y
endif
lib-y+=fastconvert
fastconvert_SOURCES+=sconvert_BG10toR16.c
fastconvert_SOURCES-$(ARMV7)+=sconvert_BG10toR16_armv7.c
fastconvert_CFLAGS-$(ARMV7)+=-DBG10toR16_CONVERTLINE=y
fastconvert_SOURCES-$(ARMV8)+=sconvert_BG10toR16_armv8.c
fastconvert_CFLAGS-$(ARMV8)+=-DBG10toR16_CONVERTLINE=y
fastconvert_SOURCES+=sconvert_NV12toR8.c
fastconvert_LIBS+=dl
