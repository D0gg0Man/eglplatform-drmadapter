#!/bin/bash
# Launch phoc 0.55 on the wlroots DRM backend via the hybris/HWC2 shims.
# Adjust PHOC and the uid/runtime dir for your device.
PHOC="${PHOC:-$HOME/repos/phoc-0.55/build/src/phoc}"
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
export XDG_SEAT=seat0 XDG_VTNR=7
export XDG_DATA_DIRS=/usr/local/share:/usr/share
export LD_LIBRARY_PATH=/usr/local/lib/aarch64-linux-gnu:$(dirname "$PHOC")
export WLR_DRM_DEVICES=/dev/dri/card0 WLR_RENDERER=gles2     # do NOT set WLR_BACKENDS
export GBM_BACKEND=hybris GBM_BACKENDS_PATH=/usr/lib/aarch64-linux-gnu/gbm HYBRIS_EGLPLATFORM=drmadapter
export HYBRIS_WLROOTS=1
export __EGL_VENDOR_LIBRARY_FILENAMES=/usr/share/glvnd/egl_vendor.d/10_libhybris.json
export LD_PRELOAD=/usr/lib/aarch64-linux-gnu/libdrm-hybris.so:/usr/local/lib/wlegl_server.so
exec "$PHOC" -v -C /etc/phosh/phoc.ini
