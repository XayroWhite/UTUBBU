TARGET = utubbu
PSP_FW_VERSION = 371
OBJS = main.o ui_font.o clock_probe.o catalog.o network.o youtube.o yvid_player.o mp4_player.o osk.o entropy_psp.o tls_trace.o media_engine.o compat_ffmpeg.o cooleyes_bridge_blob.o cooleyesBridge.o mpeg_new.o

CFLAGS = -O2 -G0 -Wall -Wextra
CXXFLAGS = $(CFLAGS) -fno-exceptions -fno-rtti
ASFLAGS = $(CFLAGS)

INCDIR += ../vendor/ffmpeg-psp/include
LIBDIR += ../vendor/ffmpeg-psp/lib

ifdef PSP_MODERN
INCDIR += $(PSP_MODERN)/psp/sdk/include $(PSP_MODERN)/psp/include
LIBDIR += $(PSP_MODERN)/psp/sdk/lib $(PSP_MODERN)/psp/lib
endif

ifdef PSP_MODERN_LIB
INCDIR += $(PSP_MODERN_LIB)/psp/include/curl
LIBDIR += $(PSP_MODERN_LIB)/psp/sdk/lib $(PSP_MODERN_LIB)/psp/lib
endif

ifdef LEGACY_HTTP
LIBS = -lcurl -lz -lpspjpeg -lpspaudio -lpsppower -lpspwlan -lpspgu
else
LIBS = -Wl,--wrap=mbedtls_x509_crt_init,--wrap=mbedtls_x509_crl_init,--wrap=mbedtls_pk_init \
       -Wl,--wrap=mbedtls_ssl_config_init,--wrap=mbedtls_ssl_init \
       -Wl,--start-group -lcurl -lmbedtls -lmbedx509 -lmbedcrypto -lavcodec -lavutil -lz \
       -lm \
       -lpspjpeg -lpspaudio -lpspaudiocodec -lpspmpegfull -lpspmpegbase -lpspkubridge -lpsppower -lpspwlan -lpspgu \
       -lc -lpthreadglue -lpthread -lcglue -lpspsdk -lpspminicrt -Wl,--end-group
endif

ifdef PSP_SHIM_LIB
LIBDIR += $(PSP_SHIM_LIB)
endif

EXTRA_TARGETS = EBOOT.PBP
PSP_EBOOT_TITLE = UTUBBU
PSP_EBOOT_ICON = ../assets/ICON0.PNG
PSP_EBOOT_PIC1 = ../assets/PIC1.PNG

PSPSDK := $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak
