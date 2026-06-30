#!/bin/bash
# FuriLabs phoc 0.44 on the wlroots 0.17.4 DRM backend via the hybris shims.
PHOC=/home/furios/repos/furilabs-phoc/build/src/phoc
unset WAYLAND_DISPLAY
export XDG_RUNTIME_DIR=/run/user/$(id -u)
export XDG_SEAT=seat0 XDG_VTNR=7
export XDG_DATA_DIRS=/usr/local/share:/usr/share
# wlroots 0.17.4 (/usr/local) first, then the phoc build dir
export LD_LIBRARY_PATH=/usr/local/lib/aarch64-linux-gnu:$(dirname "$PHOC")
export WLR_BACKENDS=drm,libinput WLR_RENDERER=gles2 WLR_DRM_DEVICES=/dev/dri/card0
export GBM_BACKEND=hybris GBM_BACKENDS_PATH=/usr/lib/aarch64-linux-gnu/gbm HYBRIS_EGLPLATFORM=drmadapter
export HYBRIS_WLROOTS=1
export __EGL_VENDOR_LIBRARY_FILENAMES=/usr/share/glvnd/egl_vendor.d/10_libhybris.json
export LD_PRELOAD=/usr/lib/aarch64-linux-gnu/libdrm-hybris.so:/usr/local/lib/wlegl_server.so
exec "$PHOC" -C /etc/phosh/phoc.ini "$@"
