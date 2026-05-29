CC      = gcc
CFLAGS  = -O2 -fPIC -I/usr/include -I/usr/include/android \
          $(shell pkg-config --cflags glib-2.0)
LDFLAGS = -shared -Wl,-soname,eglplatform_drmadapter.so
LIBS    = -ldl -lhybris-common -lEGL \
          -lhybris-eglplatformcommon -lhybris-hwcomposerwindow -lhwc2 \
          $(shell pkg-config --libs glib-2.0)

all: eglplatform_drmadapter.so

eglplatform_drmadapter.so: src/eglplatform_drmadapter.c src/hwc2_shared.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LIBS)

install: eglplatform_drmadapter.so
	install -Dm755 eglplatform_drmadapter.so \
	    $(DESTDIR)/usr/lib/aarch64-linux-gnu/libhybris/eglplatform_drmadapter.so

clean:
	rm -f eglplatform_drmadapter.so
